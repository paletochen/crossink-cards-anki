#include "AnkiActivity.h"

#include "../../anki/AnkiFirmwareVersion.h"
#include <EpdFontData.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Utf8.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cctype>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/anki/AnkiSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/util/ConfirmationActivity.h"
#include "anki/AnkiMdns.h"
#include "anki/AnkiSyncClient.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Default card fonts (Latin + modern/polytonic Greek + romanization):
//   small  → UI_12 @ 1×
//   medium → UI_18 @ 1× (native ~1.5× of UI_12 — sharp, no soft 1.5× bitmap scale)
//   large  → UI_12 @ 2×
// Optional: CrossPoint reader font (Noto / SD fonts via Settings → Font / Fonts).
int cardFontId() {
  if (ANKI_STORE.usesReaderFont()) {
    const int reader = SETTINGS.getReaderFontId();
    if (reader != 0) return reader;
  }
  // Medium uses UI_12 (or UI_18 if available on platform)
#ifdef UI_18_FONT_ID
  if (ANKI_STORE.getFontScale() == 2) return UI_18_FONT_ID;
#endif
  return UI_12_FONT_ID;
}

size_t nextUtf8Boundary(const std::string& text, const size_t position) {
  if (position >= text.size()) return text.size();
  const unsigned char lead = static_cast<unsigned char>(text[position]);
  size_t bytes = 1;
  if ((lead & 0xE0) == 0xC0)
    bytes = 2;
  else if ((lead & 0xF0) == 0xE0)
    bytes = 3;
  else if ((lead & 0xF8) == 0xF0)
    bytes = 4;
  return std::min(text.size(), position + bytes);
}

const char* staticMenuLabel(const AnkiActivity::MenuAction action) {
  switch (action) {
    case AnkiActivity::MenuAction::LEARN:
      return tr(STR_ANKI_LEARN);
    case AnkiActivity::MenuAction::DOWNLOAD:
      return tr(STR_ANKI_DOWNLOAD);
    case AnkiActivity::MenuAction::UPLOAD:
      return tr(STR_ANKI_UPLOAD);
    case AnkiActivity::MenuAction::CONFIGURE:
      return tr(STR_ANKI_SETTINGS);
    case AnkiActivity::MenuAction::DECK:
      // Real titles come from AnkiStore in renderMenu(); this is only a last-resort label.
      return tr(STR_ANKI_LEARN);
  }
  return tr(STR_ANKI);
}

EpdFontFamily::Style cardStyle(const bool bold) { return bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR; }

// XFD Phase B: STX/ETX mark bold on/off (from Anki converter). Visible ** toggles
// bold as a fallback when markers were not applied (raw Markdown on the wire).
constexpr char kBoldOn = '\x02';
constexpr char kBoldOff = '\x03';
// Vector table block (must match xteink_sync/textutil.py). Not splitlines-safe chars
// on the Mac side — device treats them as zero-width controls.
//   \x04table c=N r=M f=FLAGS\n
//   cell\x06cell\x06...\n
//   \x04/table\n
constexpr char kTableRs = '\x04';
constexpr char kCellUs = '\x06';

int baseTextWidth(const GfxRenderer& renderer, const char* text, const bool bold) {
  return renderer.getTextWidth(cardFontId(), text, cardStyle(bold));
}

// Defined later (scaled glyph path); needed by drawStyledRange.
// scaleHalf: 2 = 1.0× (draw at native font size), 4 = 2.0× block upscale of UI_12.
// Medium no longer uses 1.5× upscale — it switches to native UI_18 instead.
int drawScaledText(const GfxRenderer& renderer, const int x, const int y, const char* text, const int scaleHalf,
                   const bool bold);

// fontScale 1/2/3 → half-units. Medium (2) is native UI_18 at 1× (scaleHalf 2).
int fontScaleToHalf(const uint8_t fontScale) {
  if (fontScale <= 1) return 2;  // UI_12 @ 1×
  if (fontScale == 2) return 2;  // UI_18 @ 1× (native medium)
  return 4;                     // UI_12 @ 2× large
}

// Standard gap between card areas (EN↔DE, lemma↔memory, memory↔table, …).
// Chosen from layout mocks (15/20/25); 25px is compact without feeling double-spaced.
constexpr int kCardAreaGapPx = 25;

int scalePx(const int value, const int scaleHalf) {
  return (value * scaleHalf) / 2;
}

// Forward decls — used by vector-table layout before their definitions.
bool boldStateAt(const std::string& text, size_t position);
int styledRangeWidth(const GfxRenderer& renderer, const std::string& text, size_t start, size_t end, bool bold);
int drawStyledRange(const GfxRenderer& renderer, int penX, int y, const std::string& text, size_t start, size_t end,
                    int scaleHalf, bool bold);

bool isStyleMarkerAt(const std::string& text, const size_t position) {
  if (position >= text.size()) return false;
  const unsigned char c = static_cast<unsigned char>(text[position]);
  if (c == static_cast<unsigned char>(kBoldOn) || c == static_cast<unsigned char>(kBoldOff)) return true;
  // Table controls are zero-width (should not appear in normal runs).
  if (c == static_cast<unsigned char>(kTableRs) || c == static_cast<unsigned char>(kCellUs)) return true;
  return c == '*' && position + 1 < text.size() && text[position + 1] == '*';
}

// Advance past one style marker and update bold; returns new index (unchanged if none).
size_t consumeStyleMarker(const std::string& text, size_t position, bool& bold) {
  if (position >= text.size()) return position;
  const unsigned char c = static_cast<unsigned char>(text[position]);
  if (c == static_cast<unsigned char>(kBoldOn)) {
    bold = true;
    return position + 1;
  }
  if (c == static_cast<unsigned char>(kBoldOff)) {
    bold = false;
    return position + 1;
  }
  if (c == static_cast<unsigned char>(kTableRs) || c == static_cast<unsigned char>(kCellUs)) {
    // Zero-width skip; bold unchanged.
    return position + 1;
  }
  if (c == '*' && position + 1 < text.size() && text[position + 1] == '*') {
    bold = !bold;
    return position + 2;
  }
  return position;
}

size_t lineEndPos(const std::string& text, size_t position) {
  while (position < text.size() && text[position] != '\n' && text[position] != '\r') position++;
  return position;
}

size_t nextLineStartPos(const std::string& text, size_t position) {
  position = lineEndPos(text, position);
  if (position < text.size() && text[position] == '\r') position++;
  if (position < text.size() && text[position] == '\n') position++;
  return position;
}

bool startsWithTableHeader(const std::string& text, const size_t position) {
  // "\x04table " — 7 bytes
  constexpr char kPrefix[] = "\x04table ";
  constexpr size_t kLen = 7;
  if (position + kLen > text.size()) return false;
  return text.compare(position, kLen, kPrefix) == 0;
}

bool startsWithTableEnd(const std::string& text, const size_t position) {
  // "\x04/table" — 7 bytes (no trailing space)
  constexpr char kPrefix[] = "\x04/table";
  constexpr size_t kLen = 7;
  if (position + kLen > text.size()) return false;
  return text.compare(position, kLen, kPrefix) == 0;
}

// Locate \x04table header that encloses position (or is at position).
size_t findTableHeaderPos(const std::string& text, size_t position) {
  if (startsWithTableHeader(text, position)) return position;
  size_t searchEnd = position;
  while (searchEnd > 0) {
    const size_t p = text.rfind(kTableRs, searchEnd - 1);
    if (p == std::string::npos) return std::string::npos;
    if (startsWithTableHeader(text, p)) return p;
    if (startsWithTableEnd(text, p)) return std::string::npos;
    searchEnd = p;
  }
  return std::string::npos;
}

struct TableBlock {
  size_t headerPos = 0;
  size_t dataStart = 0;
  size_t endPos = 0;    // at \x04/table
  size_t afterEnd = 0;  // after end line + newline
  uint8_t cols = 0;
  uint8_t rows = 0;
  bool flagC = false;  // conjugation: no outer box
  bool flagH = false;  // header row (underline under row 0)
  bool flagB = false;  // simple box
  uint8_t startRow = 0;
};

bool parseTableBlock(const std::string& text, const size_t position, TableBlock& out) {
  const size_t headerPos = findTableHeaderPos(text, position);
  if (headerPos == std::string::npos) return false;

  // Header line: \x04table c=N r=M f=FLAGS
  const size_t headerEnd = lineEndPos(text, headerPos);
  // Skip "\x04table "
  size_t i = headerPos + 7;
  int cols = 0;
  int rows = 0;
  char flagBuf[8] = {};
  // Manual parse (no heap): keys c= r= f=
  while (i < headerEnd) {
    while (i < headerEnd && (text[i] == ' ' || text[i] == '\t')) i++;
    if (i >= headerEnd) break;
    if (text[i] == 'c' && i + 1 < headerEnd && text[i + 1] == '=') {
      i += 2;
      cols = 0;
      while (i < headerEnd && text[i] >= '0' && text[i] <= '9') {
        cols = cols * 10 + (text[i] - '0');
        i++;
      }
    } else if (text[i] == 'r' && i + 1 < headerEnd && text[i + 1] == '=') {
      i += 2;
      rows = 0;
      while (i < headerEnd && text[i] >= '0' && text[i] <= '9') {
        rows = rows * 10 + (text[i] - '0');
        i++;
      }
    } else if (text[i] == 'f' && i + 1 < headerEnd && text[i + 1] == '=') {
      i += 2;
      size_t fi = 0;
      while (i < headerEnd && text[i] != ' ' && text[i] != '\t' && fi + 1 < sizeof(flagBuf)) {
        flagBuf[fi++] = text[i++];
      }
      flagBuf[fi] = '\0';
    } else {
      i++;
    }
  }
  if (cols < 1 || cols > 4 || rows < 1 || rows > 12) return false;

  out.headerPos = headerPos;
  out.dataStart = nextLineStartPos(text, headerPos);
  out.cols = static_cast<uint8_t>(cols);
  out.rows = static_cast<uint8_t>(rows);
  out.flagC = false;
  out.flagH = false;
  out.flagB = false;
  for (const char* f = flagBuf; *f; ++f) {
    if (*f == 'C') out.flagC = true;
    if (*f == 'H') out.flagH = true;
    if (*f == 'B') out.flagB = true;
  }

  // Walk data rows to end marker.
  size_t rp = out.dataStart;
  for (uint8_t r = 0; r < out.rows; r++) {
    if (rp >= text.size() || startsWithTableEnd(text, rp)) return false;
    rp = nextLineStartPos(text, rp);
  }
  out.endPos = rp;
  if (!startsWithTableEnd(text, out.endPos)) {
    // Tolerant: accept missing end if we consumed declared rows.
    out.afterEnd = rp;
  } else {
    out.afterEnd = nextLineStartPos(text, out.endPos);
  }

  // Which row does position land on?
  out.startRow = 0;
  if (position <= out.dataStart) {
    out.startRow = 0;
  } else if (position >= out.endPos) {
    out.startRow = out.rows;  // past data
  } else {
    size_t p = out.dataStart;
    uint8_t idx = 0;
    while (p < position && idx < out.rows) {
      const size_t next = nextLineStartPos(text, p);
      if (position < next) break;
      p = next;
      idx++;
    }
    out.startRow = idx;
  }
  return true;
}

// Draw a vector table (or remaining rows). Stack-only: colW[4] + colX[5].
// Returns new text position; sets rowsDrawn to display lines consumed.
size_t drawVectorTable(const GfxRenderer& renderer, const std::string& text, size_t position, const int x,
                       const int y, const int width, const int maxRows, const int lineHeight, const int scaleHalf,
                       int& rowsDrawn) {
  rowsDrawn = 0;
  TableBlock table;
  if (!parseTableBlock(text, position, table) || table.startRow >= table.rows) {
    if (startsWithTableHeader(text, position) || startsWithTableEnd(text, position)) {
      return nextLineStartPos(text, position);
    }
    return nextLineStartPos(text, position);
  }

  int16_t colW[4] = {0, 0, 0, 0};

  // Pass 1: measure all cells (full table) so column edges stay stable across pages.
  size_t rp = table.dataStart;
  for (uint8_t r = 0; r < table.rows; r++) {
    const size_t le = lineEndPos(text, rp);
    size_t cellStart = rp;
    uint8_t ci = 0;
    for (size_t i = rp; i <= le && ci < table.cols; i++) {
      const bool atEnd = (i == le);
      if (atEnd || text[i] == kCellUs) {
        const bool bold = boldStateAt(text, cellStart);
        const int w = scalePx(styledRangeWidth(renderer, text, cellStart, i, bold), scaleHalf);
        if (w > colW[ci]) colW[ci] = static_cast<int16_t>(w);
        ci++;
        cellStart = i + (atEnd ? 0 : 1);
        if (atEnd) break;
      }
    }
    rp = nextLineStartPos(text, rp);
  }
  for (uint8_t c = 0; c < table.cols; c++) {
    if (colW[c] < 4) colW[c] = 4;
  }

  constexpr int kPad = 3;
  constexpr int kVLine = 1;
  auto totalWidth = [&]() -> int {
    int sum = kVLine;
    for (uint8_t c = 0; c < table.cols; c++) sum += kPad + colW[c] + kPad + kVLine;
    return sum;
  };
  // Shrink widest columns until the grid fits (floor ~8px).
  while (totalWidth() > width) {
    int widest = 0;
    for (uint8_t c = 1; c < table.cols; c++) {
      if (colW[c] > colW[widest]) widest = c;
    }
    if (colW[widest] <= 8) break;
    colW[widest]--;
  }

  int16_t colX[5] = {0, 0, 0, 0, 0};
  colX[0] = static_cast<int16_t>(x);
  for (uint8_t c = 0; c < table.cols; c++) {
    colX[c + 1] = static_cast<int16_t>(colX[c] + kVLine + kPad + colW[c] + kPad);
  }

  // Pass 2: draw from startRow.
  rp = table.dataStart;
  for (uint8_t r = 0; r < table.startRow; r++) rp = nextLineStartPos(text, rp);

  const int asc = scalePx(renderer.getFontAscenderSize(cardFontId()), scaleHalf);
  const bool drawOuter = !table.flagC;  // H/B get outer box; C only internal rules

  while (table.startRow + static_cast<uint8_t>(rowsDrawn) < table.rows && rowsDrawn < maxRows) {
    const size_t le = lineEndPos(text, rp);
    const int baseline = y + rowsDrawn * lineHeight + asc;
    size_t cellStart = rp;
    uint8_t ci = 0;
    for (size_t i = rp; i <= le && ci < table.cols; i++) {
      const bool atEnd = (i == le);
      if (atEnd || text[i] == kCellUs) {
        // Truncate cell to colW[ci] by walking end backward (keeps bold markers).
        size_t cellEnd = i;
        const bool bold0 = boldStateAt(text, cellStart);
        while (cellEnd > cellStart &&
               scalePx(styledRangeWidth(renderer, text, cellStart, cellEnd, bold0), scaleHalf) > colW[ci]) {
          // Step back one UTF-8 scalar or one control byte.
          if (cellEnd > cellStart && isStyleMarkerAt(text, cellEnd - 1)) {
            cellEnd--;
            continue;
          }
          // Find previous UTF-8 start.
          size_t back = cellEnd;
          if (back > cellStart) back--;
          while (back > cellStart && (static_cast<unsigned char>(text[back]) & 0xC0) == 0x80) back--;
          if (back < cellStart) back = cellStart;
          if (back == cellEnd) break;
          cellEnd = back;
        }
        const int cellX = colX[ci] + kVLine + kPad;
        drawStyledRange(renderer, cellX, baseline, text, cellStart, cellEnd, scaleHalf, bold0);
        ci++;
        cellStart = i + (atEnd ? 0 : 1);
        if (atEnd) break;
      }
    }

    // Header underline (full width of grid).
    if (table.flagH && table.startRow + static_cast<uint8_t>(rowsDrawn) == 0) {
      const int ruleY = y + (rowsDrawn + 1) * lineHeight - 2;
      renderer.drawLine(colX[0], ruleY, colX[table.cols], ruleY, true);
    }

    rowsDrawn++;
    rp = nextLineStartPos(text, rp);
  }

  if (rowsDrawn > 0) {
    const int y0 = y;
    const int y1 = y + rowsDrawn * lineHeight - 1;
    if (drawOuter) {
      renderer.drawLine(colX[0], y0, colX[table.cols], y0, true);
      renderer.drawLine(colX[0], y1, colX[table.cols], y1, true);
      for (uint8_t v = 0; v <= table.cols; v++) {
        renderer.drawLine(colX[v], y0, colX[v], y1, true);
      }
    } else {
      // Conjugation: light verticals between columns only.
      for (uint8_t v = 1; v < table.cols; v++) {
        renderer.drawLine(colX[v], y0, colX[v], y1, true);
      }
    }
  }

  if (table.startRow + static_cast<uint8_t>(rowsDrawn) >= table.rows) return table.afterEnd;
  return rp;
}

// --- Vector figures (stem / stress / timeline) — same \x04 block family as tables ---

bool startsWithFigHeader(const std::string& text, const size_t position) {
  // "\x04fig " — 5 bytes
  constexpr char kPrefix[] = "\x04fig ";
  constexpr size_t kLen = 5;
  if (position + kLen > text.size()) return false;
  return text.compare(position, kLen, kPrefix) == 0;
}

bool startsWithFigEnd(const std::string& text, const size_t position) {
  // "\x04/fig" — 5 bytes
  constexpr char kPrefix[] = "\x04/fig";
  constexpr size_t kLen = 5;
  if (position + kLen > text.size()) return false;
  return text.compare(position, kLen, kPrefix) == 0;
}

size_t findFigHeaderPos(const std::string& text, size_t position) {
  if (startsWithFigHeader(text, position)) return position;
  size_t searchEnd = position;
  while (searchEnd > 0) {
    const size_t p = text.rfind(kTableRs, searchEnd - 1);
    if (p == std::string::npos) return std::string::npos;
    if (startsWithFigHeader(text, p)) return p;
    if (startsWithFigEnd(text, p) || startsWithTableEnd(text, p) || startsWithTableHeader(text, p)) {
      return std::string::npos;
    }
    searchEnd = p;
  }
  return std::string::npos;
}

enum class FigType : uint8_t { STEM = 0, STRESS = 1, TIMELINE = 2 };

struct FigBlock {
  size_t headerPos = 0;
  size_t dataStart = 0;
  size_t endPos = 0;
  size_t afterEnd = 0;
  FigType type = FigType::STEM;
  uint8_t idx = 0;         // stress syllable or timeline marker (0-based)
  uint8_t dataLines = 0;   // 1–2 content lines before /fig
  uint8_t heightRows = 3;  // virtual line rows for paging budget
};

bool parseFigBlock(const std::string& text, const size_t position, FigBlock& out) {
  const size_t headerPos = findFigHeaderPos(text, position);
  if (headerPos == std::string::npos) return false;

  const size_t headerEnd = lineEndPos(text, headerPos);
  // Skip "\x04fig "
  size_t i = headerPos + 5;
  char typeBuf[12] = {};
  int idx = 0;
  bool haveIdx = false;
  while (i < headerEnd) {
    while (i < headerEnd && (text[i] == ' ' || text[i] == '\t')) i++;
    if (i >= headerEnd) break;
    if (text[i] == 't' && i + 1 < headerEnd && text[i + 1] == '=') {
      i += 2;
      size_t ti = 0;
      while (i < headerEnd && text[i] != ' ' && text[i] != '\t' && ti + 1 < sizeof(typeBuf)) {
        typeBuf[ti++] = text[i++];
      }
      typeBuf[ti] = '\0';
    } else if ((text[i] == 's' || text[i] == 'm') && i + 1 < headerEnd && text[i + 1] == '=') {
      i += 2;
      idx = 0;
      haveIdx = true;
      while (i < headerEnd && text[i] >= '0' && text[i] <= '9') {
        idx = idx * 10 + (text[i] - '0');
        i++;
      }
    } else {
      i++;
    }
  }

  FigType type = FigType::STEM;
  if (typeBuf[0] == 's' && typeBuf[1] == 't' && typeBuf[2] == 'r') {
    type = FigType::STRESS;
  } else if (typeBuf[0] == 't' && typeBuf[1] == 'i') {
    type = FigType::TIMELINE;
  } else if (typeBuf[0] == 's' && typeBuf[1] == 't' && typeBuf[2] == 'e') {
    type = FigType::STEM;
  } else if (typeBuf[0] == '\0') {
    return false;
  }

  out.headerPos = headerPos;
  out.dataStart = nextLineStartPos(text, headerPos);
  out.type = type;
  out.idx = static_cast<uint8_t>(haveIdx ? (idx < 0 ? 0 : (idx > 7 ? 7 : idx)) : 0);

  // Count data lines until \x04/fig (max 2).
  size_t rp = out.dataStart;
  uint8_t n = 0;
  while (rp < text.size() && n < 2 && !startsWithFigEnd(text, rp)) {
    rp = nextLineStartPos(text, rp);
    n++;
  }
  if (n < 1) return false;
  out.dataLines = n;
  out.endPos = rp;
  if (startsWithFigEnd(text, out.endPos)) {
    out.afterEnd = nextLineStartPos(text, out.endPos);
  } else {
    out.afterEnd = rp;
  }

  // Virtual height for page budget (whole fig, not mid-split).
  if (type == FigType::STEM) {
    out.heightRows = 4;
  } else if (type == FigType::STRESS) {
    out.heightRows = 3;
  } else {
    out.heightRows = 3;
  }
  return true;
}

// Split one data line into cells at \x06. Stack-only: max 8 cells, offsets into text.
struct FigCells {
  size_t start[8];
  size_t end[8];
  uint8_t n = 0;
};

void splitFigCells(const std::string& text, size_t lineStart, size_t lineEnd, FigCells& out) {
  out.n = 0;
  size_t cellStart = lineStart;
  for (size_t i = lineStart; i <= lineEnd && out.n < 8; i++) {
    const bool atEnd = (i == lineEnd);
    if (atEnd || text[i] == kCellUs) {
      out.start[out.n] = cellStart;
      out.end[out.n] = i;
      out.n++;
      cellStart = i + (atEnd ? 0 : 1);
      if (atEnd) break;
    }
  }
}

// Draw whole figure; never mid-splits. rowsDrawn = heightRows on success.
size_t drawVectorFig(const GfxRenderer& renderer, const std::string& text, size_t position, const int x,
                     const int y, const int width, const int maxRows, const int lineHeight, const int scaleHalf,
                     int& rowsDrawn) {
  rowsDrawn = 0;
  FigBlock fig;
  if (!parseFigBlock(text, position, fig)) {
    if (startsWithFigHeader(text, position) || startsWithFigEnd(text, position)) {
      return nextLineStartPos(text, position);
    }
    return nextLineStartPos(text, position);
  }
  // Only draw when we are at the start of the fig (no mid-fig paging).
  if (position > fig.dataStart && position < fig.afterEnd && !startsWithFigHeader(text, position)) {
    // Continuation mid-block should not happen; skip to end.
    return fig.afterEnd;
  }
  if (position != fig.headerPos && !startsWithFigHeader(text, position)) {
    // If position is dataStart after partial consume — treat as full draw from header path only.
  }
  if (maxRows < static_cast<int>(fig.heightRows)) {
    // Not enough room: caller should page; signal 0 drawn and stay at header.
    if (position == fig.headerPos || startsWithFigHeader(text, position)) {
      rowsDrawn = 0;
      return position;
    }
  }

  const int asc = scalePx(renderer.getFontAscenderSize(cardFontId()), scaleHalf);
  const int pad = 4;
  const size_t line1Start = fig.dataStart;
  const size_t line1End = lineEndPos(text, line1Start);
  FigCells cells{};
  splitFigCells(text, line1Start, line1End, cells);
  if (cells.n < 1) return fig.afterEnd;

  auto cellWidth = [&](uint8_t ci) -> int {
    if (ci >= cells.n) return 0;
    const bool bold = boldStateAt(text, cells.start[ci]);
    return scalePx(styledRangeWidth(renderer, text, cells.start[ci], cells.end[ci], bold), scaleHalf);
  };
  auto drawCell = [&](uint8_t ci, int penX, int baseline) {
    if (ci >= cells.n) return;
    const bool bold = boldStateAt(text, cells.start[ci]);
    drawStyledRange(renderer, penX, baseline, text, cells.start[ci], cells.end[ci], scaleHalf, bold);
  };

  if (fig.type == FigType::STEM) {
    // [stem] + [end]  then arrow  then [result]
    const int boxH = lineHeight;
    const int gap = 8;
    int w0 = cellWidth(0) + 2 * pad;
    int w1 = (cells.n > 1 ? cellWidth(1) : 0) + 2 * pad;
    int w2 = (cells.n > 2 ? cellWidth(2) : 0) + 2 * pad;
    if (w0 < 24) w0 = 24;
    if (w1 < 24) w1 = 24;
    if (w2 < 28) w2 = 28;
    const int plusW = scalePx(baseTextWidth(renderer, "+", false), scaleHalf) + 4;
    int rowW = w0 + gap + plusW + gap + w1;
    if (rowW > width) {
      // Shrink boxes proportionally.
      const int over = rowW - width;
      w0 = std::max(20, w0 - over / 2);
      w1 = std::max(20, w1 - over / 2);
      rowW = w0 + gap + plusW + gap + w1;
    }
    int x0 = x + std::max(0, (width - rowW) / 2);
    const int y0 = y;
    renderer.drawRect(x0, y0, w0, boxH);
    drawCell(0, x0 + pad, y0 + asc);
    const int plusX = x0 + w0 + gap;
    drawScaledText(renderer, plusX, y0 + asc, "+", scaleHalf, false);
    const int x1 = plusX + plusW + gap;
    renderer.drawRect(x1, y0, w1, boxH);
    if (cells.n > 1) drawCell(1, x1 + pad, y0 + asc);

    // Arrow down (lines only).
    const int midX = x0 + rowW / 2;
    const int arrowTop = y0 + boxH + 4;
    const int arrowBot = y0 + boxH + lineHeight - 4;
    renderer.drawLine(midX, arrowTop, midX, arrowBot, true);
    renderer.drawLine(midX, arrowBot, midX - 5, arrowBot - 7, true);
    renderer.drawLine(midX, arrowBot, midX + 5, arrowBot - 7, true);

    // Result box
    const int yRes = y0 + 2 * lineHeight;
    int resW = w2;
    if (resW > width) resW = width;
    const int xRes = x + std::max(0, (width - resW) / 2);
    renderer.drawRect(xRes, yRes, resW, boxH);
    if (cells.n > 2) drawCell(2, xRes + pad, yRes + asc);

    rowsDrawn = fig.heightRows;
    return fig.afterEnd;
  }

  if (fig.type == FigType::STRESS) {
    // Syllable boxes in a row; stress bar above idx.
    const int n = cells.n;
    const int boxH = lineHeight;
    const int gap = 6;
    int16_t cw[8] = {};
    int total = gap * (n > 0 ? n - 1 : 0);
    for (uint8_t i = 0; i < n; i++) {
      cw[i] = static_cast<int16_t>(std::max(20, cellWidth(i) + 2 * pad));
      total += cw[i];
    }
    while (total > width && n > 0) {
      int widest = 0;
      for (uint8_t i = 1; i < n; i++) {
        if (cw[i] > cw[widest]) widest = i;
      }
      if (cw[widest] <= 16) break;
      cw[widest]--;
      total--;
    }
    int pen = x + std::max(0, (width - total) / 2);
    const int yBox = y + lineHeight;  // leave room for stress mark
    const uint8_t stress = fig.idx < n ? fig.idx : 0;
    for (uint8_t i = 0; i < n; i++) {
      renderer.drawRect(pen, yBox, cw[i], boxH);
      drawCell(i, pen + pad, yBox + asc);
      if (i == stress) {
        const int markY = yBox - 6;
        renderer.drawLine(pen + 4, markY, pen + cw[i] - 4, markY, true);
        renderer.drawLine(pen + 4, markY, pen + 4, markY + 3, true);
        renderer.drawLine(pen + cw[i] - 4, markY, pen + cw[i] - 4, markY + 3, true);
      }
      pen += cw[i] + gap;
    }
    rowsDrawn = fig.heightRows;
    return fig.afterEnd;
  }

  // TIMELINE
  {
    const int n = cells.n;
    if (n < 2) {
      rowsDrawn = 0;
      return fig.afterEnd;
    }
    const int lineY = y + lineHeight + lineHeight / 2;
    const int x0 = x + 8;
    const int x1 = x + width - 8;
    renderer.drawLine(x0, lineY, x1, lineY, true);
    const uint8_t mark = fig.idx < n ? fig.idx : 0;
    for (uint8_t i = 0; i < n; i++) {
      const int tx = x0 + (n == 1 ? 0 : ((x1 - x0) * i) / (n - 1));
      renderer.drawLine(tx, lineY - 6, tx, lineY + 6, true);
      const int tw = cellWidth(i);
      const int lx = tx - tw / 2;
      drawCell(i, std::max(x, lx), lineY + 8 + asc / 2);
      if (i == mark) {
        // diamond
        renderer.drawLine(tx, lineY - 8, tx + 5, lineY, true);
        renderer.drawLine(tx + 5, lineY, tx, lineY + 8, true);
        renderer.drawLine(tx, lineY + 8, tx - 5, lineY, true);
        renderer.drawLine(tx - 5, lineY, tx, lineY - 8, true);
      }
    }
    // Optional event label on second data line
    if (fig.dataLines >= 2) {
      const size_t l2 = nextLineStartPos(text, line1Start);
      if (l2 < fig.endPos) {
        const size_t l2e = lineEndPos(text, l2);
        const bool bold = boldStateAt(text, l2);
        const int ew = scalePx(styledRangeWidth(renderer, text, l2, l2e, bold), scaleHalf);
        const int markX = x0 + (n == 1 ? 0 : ((x1 - x0) * mark) / (n - 1));
        const int ex = std::max(x, markX - ew / 2);
        drawStyledRange(renderer, ex, y + asc, text, l2, l2e, scaleHalf, bold);
      }
    }
    rowsDrawn = fig.heightRows;
    return fig.afterEnd;
  }
}

bool boldStateAt(const std::string& text, const size_t position) {
  bool bold = false;
  size_t i = 0;
  while (i < position && i < text.size()) {
    if (isStyleMarkerAt(text, i)) {
      i = consumeStyleMarker(text, i, bold);
      continue;
    }
    i = nextUtf8Boundary(text, i);
  }
  return bold;
}

// One-line plain preview for the card status strip (answer side: show front / L2 prompt).
// Strips STX/ETX, table controls, and ** markers; collapses whitespace.
// Multi-line Front (EN blank DE) becomes a compact view: "EN / DE" — display only,
// not re-written into card body data. Vector-table blocks are skipped entirely.
std::string plainStatusPreview(const std::string& text) {
  auto cleanLine = [](const std::string& line) -> std::string {
    // Skip vector-table chrome lines.
    if (!line.empty() && line[0] == kTableRs) return "";
    std::string out;
    out.reserve(std::min<size_t>(line.size(), 160));
    bool pendingSpace = false;
    size_t i = 0;
    while (i < line.size()) {
      const char c = line[i];
      if (c == kBoldOn || c == kBoldOff || c == kTableRs || c == kCellUs) {
        i++;
        continue;
      }
      if (c == '*' && i + 1 < line.size() && line[i + 1] == '*') {
        i += 2;
        continue;
      }
      if (c == ' ' || c == '\t') {
        pendingSpace = !out.empty();
        i++;
        continue;
      }
      if (pendingSpace) {
        out.push_back(' ');
        pendingSpace = false;
      }
      const size_t next = nextUtf8Boundary(line, i);
      out.append(line, i, next - i);
      i = next;
    }
    return out;
  };

  // Collect up to two non-empty logical lines → "EN / DE".
  // Skip lines inside a vector-table or vector-fig block.
  std::string line;
  std::string first;
  std::string second;
  bool inTable = false;
  bool inFig = false;
  for (size_t i = 0; i <= text.size(); i++) {
    const bool atEnd = i == text.size();
    const char c = atEnd ? '\n' : text[i];
    if (c == '\n' || c == '\r' || atEnd) {
      if (startsWithTableHeader(line, 0)) inTable = true;
      if (startsWithTableEnd(line, 0)) {
        inTable = false;
        line.clear();
        if (atEnd) break;
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') i++;
        continue;
      }
      if (startsWithFigHeader(line, 0)) inFig = true;
      if (startsWithFigEnd(line, 0)) {
        inFig = false;
        line.clear();
        if (atEnd) break;
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') i++;
        continue;
      }
      if (!inTable && !inFig) {
        const std::string cleaned = cleanLine(line);
        if (!cleaned.empty()) {
          if (first.empty()) {
            first = cleaned;
          } else if (second.empty() && cleaned != first) {
            second = cleaned;
            break;
          }
        }
      }
      line.clear();
      if (atEnd) break;
      // swallow \r\n as one break
      if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') i++;
      continue;
    }
    line.push_back(c);
  }
  if (first.empty()) return "";
  if (second.empty()) return first;
  return first + " / " + second;
}

int styledRangeWidth(const GfxRenderer& renderer, const std::string& text, const size_t start, const size_t end,
                     bool bold) {
  if (start >= end || start >= text.size()) return 0;
  const size_t limit = std::min(end, text.size());
  int width = 0;
  std::string run;
  run.reserve(64);
  auto flush = [&]() {
    if (run.empty()) return;
    width += baseTextWidth(renderer, run.c_str(), bold);
    run.clear();
  };
  size_t i = start;
  while (i < limit) {
    if (isStyleMarkerAt(text, i)) {
      flush();
      i = consumeStyleMarker(text, i, bold);
      continue;
    }
    const size_t next = std::min(nextUtf8Boundary(text, i), limit);
    run.append(text, i, next - i);
    i = next;
  }
  flush();
  return width;
}

int drawStyledRange(const GfxRenderer& renderer, int penX, const int y, const std::string& text, const size_t start,
                    const size_t end, const int scaleHalf, bool bold) {
  if (start >= end || start >= text.size()) return penX;
  const size_t limit = std::min(end, text.size());
  std::string run;
  run.reserve(64);
  auto flush = [&]() {
    if (run.empty()) return;
    penX = drawScaledText(renderer, penX, y, run.c_str(), scaleHalf, bold);
    run.clear();
  };
  size_t i = start;
  while (i < limit) {
    if (isStyleMarkerAt(text, i)) {
      flush();
      i = consumeStyleMarker(text, i, bold);
      continue;
    }
    const size_t next = std::min(nextUtf8Boundary(text, i), limit);
    run.append(text, i, next - i);
    i = next;
  }
  flush();
  return penX;
}

// Anki grade ease: 1=Again, 2=Hard, 3=Good, 4=Easy.
//
// drawButtonHints paints labels in HW slot order BACK, CONFIRM, LEFT, RIGHT at
// increasing portrait X — same order as CrossPoint / ButtonRemap (leftmost = BACK).
// Grade presses and labels share these tables so they stay in lockstep.
//
// RH L→R: Again, Hard, Good, Easy
// LH: independent table (do not derive by reversing RH — keeps current LH behavior)
constexpr int kHwLtr[4] = {
    static_cast<int>(HalGPIO::BTN_BACK),     // leftmost / slot 0
    static_cast<int>(HalGPIO::BTN_CONFIRM),  // slot 1
    static_cast<int>(HalGPIO::BTN_LEFT),     // slot 2
    static_cast<int>(HalGPIO::BTN_RIGHT),    // rightmost / slot 3
};

constexpr uint8_t kRhEaseLtr[4] = {1, 2, 3, 4};  // Again Hard Good Easy
constexpr StrId kRhLabelLtr[4] = {StrId::STR_ANKI_AGAIN, StrId::STR_ANKI_HARD, StrId::STR_ANKI_GOOD,
                                  StrId::STR_ANKI_EASY};

// LH grade row (unchanged vs previous on-device mapping with mirrored RH + reverse).
constexpr uint8_t kLhEaseLtr[4] = {1, 2, 3, 4};  // Again Hard Good Easy
constexpr StrId kLhLabelLtr[4] = {StrId::STR_ANKI_AGAIN, StrId::STR_ANKI_HARD, StrId::STR_ANKI_GOOD,
                                  StrId::STR_ANKI_EASY};

// Returns ease 1..4, or 0 if hw is not a front grade button.
uint8_t gradeEaseForFrontButton(const int hw, const bool leftHanded) {
  const uint8_t* easeLtr = leftHanded ? kLhEaseLtr : kRhEaseLtr;
  for (int i = 0; i < 4; i++) {
    if (hw != kHwLtr[i]) continue;
    return easeLtr[i];
  }
  return 0;
}

void drawGradeButtonHints(GfxRenderer& renderer, const bool leftHanded) {
  const StrId* labelLtr = leftHanded ? kLhLabelLtr : kRhLabelLtr;
  const char* hwLabels[4] = {
      I18n::getInstance().get(labelLtr[0]),
      I18n::getInstance().get(labelLtr[1]),
      I18n::getInstance().get(labelLtr[2]),
      I18n::getInstance().get(labelLtr[3]),
  };
  GUI.drawButtonHints(renderer, hwLabels[0], hwLabels[1], hwLabels[2], hwLabels[3]);
}

void drawScaledGlyph(const GfxRenderer& renderer, const EpdFontFamily& font, const uint32_t cp, const int cursorX,
                     const int baselineY, const int scaleHalf, const bool black, const EpdFontFamily::Style style) {
  const EpdGlyph* glyph = font.getGlyph(cp, style);
  // scaleHalf: 2=1×, 3=1.5×, 4=2× (values below 2 are invalid)
  if (!glyph || scaleHalf < 2) return;

  const EpdFontData* fontData = font.getData(style);
  if (!fontData) return;
  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);
  if (!bitmap) return;

  const int baseX = cursorX + scalePx(glyph->left, scaleHalf);
  const int baseY = baselineY - scalePx(glyph->top, scaleHalf);
  const int srcW = glyph->width;
  const int srcH = glyph->height;

  auto paintSrcPixel = [&](const int sx, const int sy) {
    // Map source pixel [sx,sy] to dest rect; 1.5× uses alternating 1–2 px blocks.
    const int x0 = baseX + (sx * scaleHalf) / 2;
    const int y0 = baseY + (sy * scaleHalf) / 2;
    const int x1 = baseX + ((sx + 1) * scaleHalf) / 2;
    const int y1 = baseY + ((sy + 1) * scaleHalf) / 2;
    for (int py = y0; py < y1; py++) {
      for (int px = x0; px < x1; px++) {
        renderer.drawPixel(px, py, black);
      }
    }
  };

  if (fontData->is2Bit) {
    for (int sy = 0; sy < srcH; sy++) {
      for (int sx = 0; sx < srcW; sx++) {
        const int pos = sy * srcW + sx;
        const uint8_t byte = bitmap[pos >> 2];
        const uint8_t raw = (byte >> ((3 - (pos & 3)) * 2)) & 0x3;
        if (raw < 2) continue;
        paintSrcPixel(sx, sy);
      }
    }
  } else {
    for (int sy = 0; sy < srcH; sy++) {
      for (int sx = 0; sx < srcW; sx++) {
        const int pos = sy * srcW + sx;
        const uint8_t byte = bitmap[pos >> 3];
        const uint8_t bit = 7 - (pos & 7);
        if (((byte >> bit) & 1) == 0) continue;
        paintSrcPixel(sx, sy);
      }
    }
  }
}

int drawScaledText(const GfxRenderer& renderer, const int x, const int y, const char* text, const int scaleHalf,
                   const bool bold) {
  if (!text || !*text || scaleHalf < 2) return x;
  const int fontId = cardFontId();

  // 1.0× — use the fast built-in text path.
  if (scaleHalf == 2) {
    renderer.drawText(fontId, x, y, text, true, cardStyle(bold));
    return x + renderer.getTextWidth(fontId, text, cardStyle(bold));
  }

  const auto& fontMap = renderer.getFontMap();
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    renderer.drawText(fontId, x, y, text, true, cardStyle(bold));
    return x + renderer.getTextWidth(fontId, text, cardStyle(bold));
  }

  const EpdFontFamily& font = fontIt->second;
  const EpdFontFamily::Style style = cardStyle(bold);
  const char* cursor = text;
  uint32_t cp = 0;
  uint32_t prevCp = 0;
  int penX = x;
  int32_t prevAdvanceFP = 0;
  int lastBaseX = x;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;

  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&cursor)))) {
    if (cp >= 0x0591 && cp <= 0x05C7) continue;

    if (utf8IsCombiningMark(cp)) {
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, style);
      if (!combiningGlyph) continue;
      const auto anchor = combiningMark::anchorFor(cp);
      const int raiseBy =
          combiningMark::raiseAboveBase(anchor, combiningGlyph->top, combiningGlyph->height, lastBaseTop);
      const int combiningX = combiningMark::anchorOver(anchor, lastBaseX, lastBaseLeft, lastBaseWidth,
                                                       combiningGlyph->left, combiningGlyph->width);
      drawScaledGlyph(renderer, font, cp, combiningX, y - scalePx(raiseBy, scaleHalf), scaleHalf, true, style);
      continue;
    }

    cp = font.applyLigatures(cp, cursor, style);
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);
      penX += scalePx(fp4::toPixel(prevAdvanceFP + kernFP), scaleHalf);
    }

    const EpdGlyph* glyph = font.getGlyph(cp, style);
    lastBaseX = penX;
    lastBaseLeft = glyph ? glyph->left : 0;
    lastBaseWidth = glyph ? glyph->width : 0;
    lastBaseTop = glyph ? glyph->top : 0;
    prevAdvanceFP = glyph ? glyph->advanceX : 0;
    drawScaledGlyph(renderer, font, cp, penX, y, scaleHalf, true, style);
    prevCp = cp;
  }

  if (prevCp != 0) penX += scalePx(fp4::toPixel(prevAdvanceFP), scaleHalf);
  return penX;
}
}  // namespace

void AnkiActivity::onEnter() {
  Activity::onEnter();
  applyUiOrientation(false);
  std::string error;
  if (!ANKI_STORE.load(error)) {
    state = State::ERROR;
    message = error;
  } else {
    state = State::MENU;
    rebuildMenu();
  }
  requestUpdate();
}

void AnkiActivity::onExit() {
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  Activity::onExit();
  if (wifiActivated) {
    // WiFi fragments heap on ESP32-C3; reboot like KOReader does, but land in
    // Anki menu so a successful pull/push is not dumped on the home screen.
    WiFi.disconnect(false);
    delay(30);
    silentRestartToAnki();
  }
}

void AnkiActivity::displayCardBuffer() {
  // FAST most of the time; HALF every N paints (N = CrossPoint refresh setting).
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
}

void AnkiActivity::displayUiBuffer() {
  // Same cadence as cards: FAST on menu navigation, HALF only every N paints.
  // Full half-refresh on every selection change was too heavy on e-ink.
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
}

bool AnkiActivity::preventAutoSleep() { return state == State::WORKING; }

void AnkiActivity::applyUiOrientation(const bool forCard) {
  if (forCard && ANKI_STORE.isLandscapeCards()) {
    // Opposite landscape rotation for left-handed so the preferred hand stays
    // near the control edge (mirrors CrossPoint reader CW/CCW choice).
    renderer.setOrientation(ANKI_STORE.isLeftHanded() ? GfxRenderer::Orientation::LandscapeClockwise
                                                     : GfxRenderer::Orientation::LandscapeCounterClockwise);
  } else {
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  }
}

void AnkiActivity::rebuildMenu() {
  menuItems.clear();

  // List every deck from the last pull so each stack is selectable by name.
  const uint16_t deckCount = ANKI_STORE.getDeckCount();
  for (uint16_t i = 0; i < deckCount; i++) {
    menuItems.push_back(MenuItem{MenuAction::DECK, i});
  }

  // Fallback if state has cards but no deck metadata (legacy single-queue session).
  if (deckCount == 0 && ANKI_STORE.hasRemainingCards()) {
    menuItems.push_back(MenuItem{MenuAction::LEARN, 0});
  }

  if (ANKI_STORE.hasPendingUpload()) {
    menuItems.push_back(MenuItem{MenuAction::UPLOAD, 0});
  }
  // Re-download only when no pending reviews/flags, so open grades stay consistent.
  if (!ANKI_STORE.hasPendingUpload()) {
    menuItems.push_back(MenuItem{MenuAction::DOWNLOAD, 0});
  }
  menuItems.push_back(MenuItem{MenuAction::CONFIGURE, 0});
  if (selectedIndex >= static_cast<int>(menuItems.size())) selectedIndex = 0;
}

void AnkiActivity::loop() {
  if (state == State::MENU) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onGoHome(HomeMenuItem::ANKI);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      handleMenuSelection();
      return;
    }
    buttonNavigator.onNext([this] {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems.size()));
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems.size()));
      requestUpdate();
    });
    return;
  }

  if (state == State::QUESTION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      applyUiOrientation(false);
      state = State::MENU;
      rebuildMenu();
      requestUpdate();
      return;
    }
    // Flag before Confirm→answer so Confirm+side can toggle without revealing.
    if (handleCardSideButtons()) return;
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      showAnswer();
      return;
    }
    return;
  }

  if (state == State::ANSWER) {
    // Flag / side paging before grade buttons.
    if (handleCardSideButtons()) return;
    // Grade buttons: physical L→R is Again/Hard/Good/Easy (reversed when left-handed).
    // See gradeEaseForFrontButton() / drawGradeButtonHints().
    const int hw = mappedInput.getPressedFrontButton();
    if (hw >= 0) {
      const uint8_t ease = gradeEaseForFrontButton(hw, ANKI_STORE.isLeftHanded());
      if (ease != 0) {
        grade(ease);
        return;
      }
    }
    return;
  }

  if (state == State::MESSAGE || state == State::ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (restartOnDismiss || wifiActivated) {
        onGoHome(HomeMenuItem::ANKI);
      } else {
        applyUiOrientation(false);
        state = State::MENU;
        rebuildMenu();
        requestUpdate();
      }
    }
  }
}

void AnkiActivity::handleMenuSelection() {
  if (menuItems.empty() || selectedIndex >= static_cast<int>(menuItems.size())) return;
  const MenuItem& item = menuItems[selectedIndex];
  switch (item.action) {
    case MenuAction::LEARN:
      startReview();
      break;
    case MenuAction::DECK:
      startReviewForDeck(item.deckIndex);
      break;
    case MenuAction::DOWNLOAD:
      startNetworkAction(NetworkAction::PULL);
      break;
    case MenuAction::UPLOAD:
      startNetworkAction(NetworkAction::PUSH);
      break;
    case MenuAction::CONFIGURE:
      startActivityForResult(std::make_unique<AnkiSettingsActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) {
                               std::string error;
                               if (!ANKI_STORE.load(error)) {
                                 state = State::ERROR;
                                 message = error;
                               } else {
                                 applyUiOrientation(false);
                                 state = State::MENU;
                                 rebuildMenu();
                               }
                               requestUpdate();
                             });
      break;
  }
}

void AnkiActivity::startNetworkAction(const NetworkAction action) {
  if (!ANKI_STORE.configured()) {
    state = State::MESSAGE;
    message = tr(STR_ANKI_CONFIGURE_FIRST);
    restartOnDismiss = false;
    requestUpdate();
    return;
  }

  networkAction = action;
  discoverOfferedThisAction = false;
  wifiActivated = true;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           onWifiSelectionComplete(!result.isCancelled && WiFi.status() == WL_CONNECTED);
                         });
}

void AnkiActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    state = State::ERROR;
    message = tr(STR_WIFI_CONN_FAILED);
    restartOnDismiss = true;
    requestUpdate();
    return;
  }
  performNetworkAction();
}

void AnkiActivity::performNetworkAction() {
  state = State::WORKING;
  message = networkAction == NetworkAction::PULL ? tr(STR_ANKI_PULLING) : tr(STR_ANKI_PUSHING);
  detail.clear();
  requestUpdateAndWait();

  const AnkiSyncClient::Error result =
      networkAction == NetworkAction::PULL ? AnkiSyncClient::pull(detail) : AnkiSyncClient::push(detail);

  // Keep Wi-Fi up briefly if we may offer mDNS search; stop otherwise.
  const bool offerDiscover =
      result == AnkiSyncClient::Error::NETWORK_ERROR && !discoverOfferedThisAction;
  if (!offerDiscover) {
    WiFi.disconnect(false);
    esp_wifi_stop();
  }

  if (result == AnkiSyncClient::Error::OK) {
    std::string loadError;
    if (!ANKI_STORE.load(loadError)) {
      state = State::ERROR;
      message = loadError;
    } else {
      state = State::MESSAGE;
      if (networkAction == NetworkAction::PULL) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s\n%u %s", tr(STR_ANKI_PULL_OK),
                 static_cast<unsigned int>(ANKI_STORE.getDeckCount()), tr(STR_ANKI_DECKS_LOADED));
        message = buf;
      } else {
        message = tr(STR_ANKI_PUSH_OK);
      }
    }
  } else if (result == AnkiSyncClient::Error::PARTIAL_RESPONSE) {
    // Most reviews applied; local session already cleared. Soft success, not red error.
    state = State::MESSAGE;
    message = tr(STR_ANKI_PUSH_PARTIAL);
    if (!detail.empty() && detail.size() < 96) {
      message += "\n";
      message += detail;
    }
  } else if (offerDiscover) {
    // Connection refused / host unreachable — ask: search LAN or Anki is open.
    discoverOfferedThisAction = true;
    offerDiscoverAfterConnectFailure();
    return;
  } else {
    state = State::ERROR;
    if (result == AnkiSyncClient::Error::NETWORK_ERROR) {
      message = tr(STR_ANKI_NETWORK_HINT);
    } else {
      message = AnkiSyncClient::errorString(result);
      if (!detail.empty() && detail.size() < 200) message += "\n" + detail;
    }
  }
  // After a successful pull, stay in Anki so the new deck list is visible immediately.
  const bool pullOk = result == AnkiSyncClient::Error::OK && networkAction == NetworkAction::PULL;
  restartOnDismiss = !pullOk;
  networkAction = NetworkAction::NONE;
  requestUpdate();
}

void AnkiActivity::offerDiscoverAfterConnectFailure() {
  // Wi-Fi may still be up from the failed attempt; dialog first.
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_ANKI_CONN_FAILED),
                                             tr(STR_ANKI_CONN_OFFER_DISCOVER)),
      [this](const ActivityResult& result) { onDiscoverOfferResult(!result.isCancelled); });
}

void AnkiActivity::onDiscoverOfferResult(const bool search) {
  if (!search) {
    // User is sure Anki is running — keep URL, show guidance.
    WiFi.disconnect(false);
    esp_wifi_stop();
    state = State::ERROR;
    message = tr(STR_ANKI_NETWORK_HINT);
    restartOnDismiss = true;
    networkAction = NetworkAction::NONE;
    requestUpdate();
    return;
  }

  // Confirm = search for Mac server on LAN, then retry pull/push.
  if (WiFi.status() == WL_CONNECTED) {
    discoverThenRetry();
    return;
  }
  reconnectWifiThenDiscoverAndRetry();
}

void AnkiActivity::reconnectWifiThenDiscoverAndRetry() {
  wifiActivated = true;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                             WiFi.disconnect(false);
                             esp_wifi_stop();
                             state = State::ERROR;
                             message = tr(STR_ANKI_DISCOVER_WIFI);
                             restartOnDismiss = true;
                             networkAction = NetworkAction::NONE;
                             requestUpdate();
                             return;
                           }
                           discoverThenRetry();
                         });
}

void AnkiActivity::discoverThenRetry() {
  state = State::WORKING;
  message = tr(STR_ANKI_DISCOVERING);
  requestUpdateAndWait();

  std::string url;
  if (!AnkiMdns::discoverServerUrl(url)) {
    WiFi.disconnect(false);
    esp_wifi_stop();
    state = State::ERROR;
    message = tr(STR_ANKI_DISCOVER_NONE);
    restartOnDismiss = true;
    networkAction = NetworkAction::NONE;
    requestUpdate();
    return;
  }

  ANKI_STORE.setServerUrl(url);
  std::string saveError;
  if (!ANKI_STORE.saveConfig(saveError)) {
    WiFi.disconnect(false);
    esp_wifi_stop();
    state = State::ERROR;
    message = saveError.empty() ? tr(STR_ANKI_DISCOVER_NONE) : saveError;
    restartOnDismiss = true;
    networkAction = NetworkAction::NONE;
    requestUpdate();
    return;
  }

  // Retry the original action with the discovered URL (Wi-Fi still up).
  state = State::WORKING;
  message = tr(STR_ANKI_RETRYING);
  requestUpdateAndWait();
  performNetworkAction();
}

void AnkiActivity::startReviewForDeck(const uint16_t deckIndex) {
  if (!ANKI_STORE.setCurrentDeck(deckIndex)) {
    state = State::ERROR;
    message = "Could not select Anki deck";
    restartOnDismiss = false;
    requestUpdate();
    return;
  }
  if (!ANKI_STORE.hasRemainingCards()) {
    state = State::MESSAGE;
    message = tr(STR_ANKI_DECK_DONE);
    restartOnDismiss = false;
    requestUpdate();
    return;
  }
  startReview();
}

void AnkiActivity::startReview() {
  if (!loadCurrentCard()) return;
  applyUiOrientation(true);
  state = State::QUESTION;
  cardStartedAt = millis();
  requestUpdate();
}

bool AnkiActivity::loadCurrentCard() {
  std::string error;
  if (!ANKI_STORE.loadCurrentCard(currentCard, error)) {
    state = State::ERROR;
    message = error;
    restartOnDismiss = false;
    requestUpdate();
    return false;
  }
  if (currentCard.front.empty()) currentCard.front = tr(STR_ANKI_EMPTY_SIDE);
  if (currentCard.back.empty()) currentCard.back = tr(STR_ANKI_EMPTY_SIDE);
  resetPaging();
  return true;
}

void AnkiActivity::showAnswer() {
  state = State::ANSWER;
  resetPaging();
  requestUpdate();
}

void AnkiActivity::grade(const uint8_t ease) {
  const uint32_t elapsed = std::min<uint32_t>(millis() - cardStartedAt, 3600000);
  std::string error;
  if (!ANKI_STORE.recordReview(ease, elapsed, error)) {
    applyUiOrientation(false);
    state = State::ERROR;
    message = error;
    restartOnDismiss = false;
    requestUpdate();
    return;
  }

  if (ANKI_STORE.hasRemainingCards()) {
    if (!loadCurrentCard()) return;
    state = State::QUESTION;
    cardStartedAt = millis();
  } else if (ANKI_STORE.hasAnyRemainingCards()) {
    applyUiOrientation(false);
    state = State::MESSAGE;
    message = tr(STR_ANKI_DECK_DONE);
    restartOnDismiss = false;
  } else {
    applyUiOrientation(false);
    state = State::MESSAGE;
    message = tr(STR_ANKI_OFFLINE_DONE);
    restartOnDismiss = false;
  }
  requestUpdate();
}

void AnkiActivity::resetPaging() {
  pageStart = 0;
  nextPageStart = 0;
  pageHistory.clear();
  pendingSidePageDir = 0;
  flagHoldFired = false;
}

void AnkiActivity::nextPage() {
  const std::string& text = state == State::ANSWER ? currentCard.back : currentCard.front;
  if (nextPageStart > pageStart && nextPageStart < text.size()) {
    pageHistory.push_back(pageStart);
    pageStart = nextPageStart;
    requestUpdate();
  }
}

void AnkiActivity::previousPage() {
  if (pageHistory.empty()) return;
  pageStart = pageHistory.back();
  pageHistory.pop_back();
  requestUpdate();
}

void AnkiActivity::handleCardPaging() {
  // Kept for any legacy call sites; card views use handleCardSideButtons().
  if (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
      mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    previousPage();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::PageForward) ||
             mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    nextPage();
  }
}

bool AnkiActivity::applyFlagToggle() {
  uint8_t newFlag = 0;
  std::string error;
  if (!ANKI_STORE.toggleCurrentCardFlag(newFlag, error)) {
    return true;  // consume gesture even on failure
  }
  currentCard.flag = newFlag;
  requestUpdate();
  return true;
}

bool AnkiActivity::tryToggleFlagGesture() {
  // X4 hardware: BTN_UP and BTN_DOWN share one ADC ladder — only one side button
  // can be detected at a time. Up+Down simultaneous chords are impossible.
  //
  // Working gestures (two ADC channels or timing):
  //   A) Confirm (front/ADC1) + Up or Down (side/ADC2)
  //   B) Long-press Up or Down ≥ kFlagHoldMs (short press pages on release)
  using B = MappedInputManager::Button;
  const bool confHeld = mappedInput.isPressed(B::Confirm);
  const bool confEdge = mappedInput.wasPressed(B::Confirm);
  const bool upEdge = mappedInput.wasPressed(B::Up) || mappedInput.wasPressed(B::PageBack);
  const bool downEdge = mappedInput.wasPressed(B::Down) || mappedInput.wasPressed(B::PageForward);
  const bool upHeld = mappedInput.isPressed(B::Up) || mappedInput.isPressed(B::PageBack);
  const bool downHeld = mappedInput.isPressed(B::Down) || mappedInput.isPressed(B::PageForward);
  const bool sideEdge = upEdge || downEdge;
  const bool sideHeld = upHeld || downHeld;

  // A) Cross-ADC chord: Confirm + either side button.
  if ((confHeld || confEdge) && (sideHeld || sideEdge)) {
    if (confEdge || sideEdge) {
      pendingSidePageDir = 0;
      flagHoldFired = true;
      return applyFlagToggle();
    }
  }

  // B) Long-press side only (single ADC) — fire once per hold.
  if (sideHeld && !confHeld && pendingSidePageDir != 0 && !flagHoldFired) {
    if (mappedInput.getHeldTime() >= kFlagHoldMs) {
      flagHoldFired = true;
      pendingSidePageDir = 0;
      return applyFlagToggle();
    }
  }
  return false;
}

bool AnkiActivity::handleCardSideButtons() {
  using B = MappedInputManager::Button;
  const bool upEdge = mappedInput.wasPressed(B::Up) || mappedInput.wasPressed(B::PageBack);
  const bool downEdge = mappedInput.wasPressed(B::Down) || mappedInput.wasPressed(B::PageForward);
  const bool upRel = mappedInput.wasReleased(B::Up) || mappedInput.wasReleased(B::PageBack);
  const bool downRel = mappedInput.wasReleased(B::Down) || mappedInput.wasReleased(B::PageForward);
  const bool sideHeld =
      mappedInput.isPressed(B::Up) || mappedInput.isPressed(B::Down) ||
      mappedInput.isPressed(B::PageBack) || mappedInput.isPressed(B::PageForward);

  // Start a short-press page candidate (resolved on release unless long-press flags).
  if (upEdge) {
    pendingSidePageDir = -1;
    flagHoldFired = false;
  } else if (downEdge) {
    pendingSidePageDir = 1;
    flagHoldFired = false;
  }

  if (tryToggleFlagGesture()) return true;

  // Short press: page on release if we never crossed the flag hold threshold.
  if (upRel || downRel) {
    const bool doPage = (pendingSidePageDir != 0 && !flagHoldFired);
    const int8_t dir = pendingSidePageDir;
    pendingSidePageDir = 0;
    flagHoldFired = false;
    if (doPage) {
      if (dir < 0) previousPage();
      else if (dir > 0) nextPage();
      return true;
    }
  }

  // Clear latch when nothing side-related is held.
  if (!sideHeld && pendingSidePageDir == 0) {
    flagHoldFired = false;
  }
  return false;
}

void AnkiActivity::render(RenderLock&&) {
  switch (state) {
    case State::MENU:
      renderMenu();
      break;
    case State::QUESTION:
      renderCard(false);
      break;
    case State::ANSWER:
      renderCard(true);
      break;
    case State::WORKING:
      renderMessage(false);
      break;
    case State::MESSAGE:
      renderMessage(false);
      break;
    case State::ERROR:
      renderMessage(true);
      break;
  }
}

void AnkiActivity::renderMenu() {
  applyUiOrientation(false);
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_ANKI),
                 ankiFirmwareVersionLabel());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(menuItems.size()), selectedIndex,
      [this](const int index) {
        const MenuItem& item = menuItems[index];
        if (item.action == MenuAction::DECK) {
          const AnkiDeckInfo info = ANKI_STORE.getDeckInfo(item.deckIndex);
          // Prefer the real deck title; never fall back to the app name "Anki".
          if (!info.name.empty() && info.name != "Anki") return info.name;
          if (!info.id.empty() && info.id != "default" && info.id != "Anki") {
            if (info.id.rfind("name:", 0) == 0 && info.id.size() > 5) return info.id.substr(5);
            // Numeric Anki deck ids are better than the generic app title.
            if (!(info.id.size() > 0 && info.id[0] >= '0' && info.id[0] <= '9')) return info.id;
          }
          if (!info.name.empty()) return info.name;
          char fallback[28];
          snprintf(fallback, sizeof(fallback), "%s %u", tr(STR_ANKI_LEARN),
                   static_cast<unsigned int>(item.deckIndex + 1));
          return std::string(fallback);
        }
        return std::string(staticMenuLabel(item.action));
      },
      // Subtitle under deck rows: progress for that stack.
      [this](const int index) -> std::string {
        const MenuItem& item = menuItems[index];
        if (item.action != MenuAction::DECK) return {};
        const AnkiDeckInfo info = ANKI_STORE.getDeckInfo(item.deckIndex);
        const uint16_t total = info.total > 0 ? info.total : info.remaining;
        if (total == 0) return tr(STR_ANKI_DECK_FINISHED);
        if (info.remaining == 0) return tr(STR_ANKI_DECK_FINISHED);
        const unsigned pct =
            static_cast<unsigned>((static_cast<uint32_t>(total - info.remaining) * 100u) / total);
        char buf[40];
        snprintf(buf, sizeof(buf), tr(STR_ANKI_PROGRESS_FORMAT), static_cast<unsigned int>(info.remaining),
                 static_cast<unsigned int>(total), pct);
        return buf;
      },
      nullptr,
      [this](const int index) {
        char value[48] = {};
        const MenuItem& item = menuItems[index];
        if (item.action == MenuAction::LEARN) {
          snprintf(value, sizeof(value), tr(STR_ANKI_REMAINING_FORMAT),
                   static_cast<unsigned int>(ANKI_STORE.getRemainingCount()));
        } else if (item.action == MenuAction::DECK) {
          const AnkiDeckInfo info = ANKI_STORE.getDeckInfo(item.deckIndex);
          snprintf(value, sizeof(value), tr(STR_ANKI_REMAINING_FORMAT),
                   static_cast<unsigned int>(info.remaining));
        } else if (item.action == MenuAction::UPLOAD) {
          const unsigned reviews = static_cast<unsigned int>(ANKI_STORE.getReviewCount());
          const unsigned flags = static_cast<unsigned int>(ANKI_STORE.getPendingFlagCount());
          if (flags > 0 && reviews > 0) {
            snprintf(value, sizeof(value), "%u+%u", reviews, flags);
          } else if (flags > 0) {
            snprintf(value, sizeof(value), "F%u", flags);
          } else {
            snprintf(value, sizeof(value), "%u", reviews);
          }
        }
        return std::string(value);
      },
      true,
      // Dim finished decks so open stacks stand out.
      [this](const int index) {
        const MenuItem& item = menuItems[index];
        if (item.action != MenuAction::DECK) return false;
        return ANKI_STORE.getDeckInfo(item.deckIndex).remaining == 0;
      });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  displayUiBuffer();
}

size_t AnkiActivity::drawTextPage(const std::string& text, const size_t start, const int x, const int y,
                                  const int width, const int maxLines, const int lineHeight, const int scale,
                                  const bool /*boldDefault*/) {
  // boldDefault is unused: style comes from STX/ETX markers and optional ** toggles.
  // Vector tables (\x04table …) are drawn as multi-row blocks with 1px grid lines.
  //
  // Vertical layout is pixel-based.
  // Blank lines mark a new area (front EN/DE is consecutive; back uses blanks between
  // lemma / memory / paradigm). Area gap is a fixed 25px on every font size.
  const int blankHeight = kCardAreaGapPx;
  const int pageHeightPx = std::max(1, maxLines * lineHeight);
  const int yLimit = y + pageHeightPx;
  const int asc = scalePx(renderer.getFontAscenderSize(cardFontId()), scale);

  size_t position = std::min(start, text.size());
  int yPos = y;
  while (yPos + std::min(blankHeight, lineHeight) <= yLimit && position < text.size()) {
    while (position < text.size() && (text[position] == ' ' || text[position] == '\t')) position++;
    if (position >= text.size()) break;
    if (text[position] == '\n' || text[position] == '\r') {
      // New area: fixed 25px gap (front & back).
      if (yPos + blankHeight > yLimit) break;
      position = nextLineStartPos(text, position);
      yPos += blankHeight;
      continue;
    }

    // Vector figure block (stem / stress / timeline) — whole fig only, no mid-split.
    if (startsWithFigHeader(text, position) || findFigHeaderPos(text, position) != std::string::npos) {
      FigBlock fprobe;
      if (parseFigBlock(text, position, fprobe) && position < fprobe.afterEnd) {
        const int remainingPx = yLimit - yPos;
        const int remainingRows = std::max(0, remainingPx / lineHeight);
        const int need = static_cast<int>(fprobe.heightRows);
        if (remainingRows < need) {
          if (yPos > y) break;  // next page
          // First line of page but still too short: skip fig rather than hang.
        }
        if (remainingRows <= 0) break;
        int rowsDrawn = 0;
        const size_t before = position;
        position = drawVectorFig(renderer, text, fprobe.headerPos, x, yPos, width, remainingRows, lineHeight, scale,
                                 rowsDrawn);
        if (rowsDrawn <= 0) {
          if (yPos > y) {
            position = before;
            break;
          }
          position = fprobe.afterEnd;  // cannot fit; skip
          continue;
        }
        yPos += rowsDrawn * lineHeight;
        continue;
      }
    }

    // Vector table block (or continuation mid-table after paging).
    if (startsWithTableHeader(text, position) || findTableHeaderPos(text, position) != std::string::npos) {
      TableBlock probe;
      if (parseTableBlock(text, position, probe) && position < probe.afterEnd &&
          (position >= probe.headerPos && position < probe.afterEnd)) {
        const int remainingPx = yLimit - yPos;
        const int remainingRows = std::max(0, remainingPx / lineHeight);
        const int rowsLeft = static_cast<int>(probe.rows - probe.startRow);
        // Prefer pushing a whole table to the next page when it would leave a
        // lonely header strip (table fits the page but not the remaining space).
        if (yPos > y && rowsLeft > remainingRows && rowsLeft * lineHeight <= pageHeightPx) {
          break;  // next page starts at this table
        }
        if (remainingRows <= 0) break;
        int rowsDrawn = 0;
        position =
            drawVectorTable(renderer, text, position, x, yPos, width, remainingRows, lineHeight, scale, rowsDrawn);
        if (rowsDrawn <= 0) {
          position = nextLineStartPos(text, position);
          continue;
        }
        yPos += rowsDrawn * lineHeight;
        continue;
      }
    }

    // Need a full text line slot.
    if (yPos + lineHeight > yLimit) break;

    const size_t lineStart = position;
    const bool lineBold = boldStateAt(text, lineStart);
    size_t scan = position;
    size_t lineEnd = position;
    size_t nextPosition = position;
    size_t lastBreak = std::string::npos;

    while (scan < text.size() && text[scan] != '\n' && text[scan] != '\r') {
      size_t next;
      if (isStyleMarkerAt(text, scan)) {
        bool scratch = false;
        next = consumeStyleMarker(text, scan, scratch);
      } else {
        next = nextUtf8Boundary(text, scan);
        if (text[scan] == ' ' || text[scan] == '\t') lastBreak = next;
      }

      // Width uses the full style timeline from lineStart (markers are zero-width).
      // scale here is half-units (2/3/4); convert with scalePx.
      if (scalePx(styledRangeWidth(renderer, text, lineStart, next, lineBold), scale) > width) {
        if (lastBreak != std::string::npos && lastBreak > lineStart) {
          lineEnd = lastBreak;
          nextPosition = lastBreak;
        } else if (scan > lineStart) {
          lineEnd = scan;
          nextPosition = scan;
        } else {
          // Force progress: at least one glyph or one marker.
          lineEnd = next;
          nextPosition = next;
        }
        break;
      }
      lineEnd = next;
      nextPosition = next;
      scan = next;
    }

    if (scan < text.size() && (text[scan] == '\n' || text[scan] == '\r') && nextPosition == scan) {
      nextPosition = nextLineStartPos(text, scan);
    }

    // Trim trailing spaces/tabs only (keep style markers).
    size_t trimEnd = lineEnd;
    while (trimEnd > lineStart) {
      const char ch = text[trimEnd - 1];
      if (ch == ' ' || ch == '\t') {
        trimEnd--;
        continue;
      }
      break;
    }

    // Ascender offset so the first line is not clipped under the header.
    const int baseline = yPos + asc;
    drawStyledRange(renderer, x, baseline, text, lineStart, trimEnd, scale, lineBold);
    if (nextPosition <= position) {
      if (isStyleMarkerAt(text, position)) {
        bool scratch = lineBold;
        nextPosition = consumeStyleMarker(text, position, scratch);
      } else {
        nextPosition = nextUtf8Boundary(text, position);
      }
    }
    position = nextPosition;
    yPos += lineHeight;
  }
  return position;
}

void AnkiActivity::renderCard(const bool answer) {
  applyUiOrientation(true);
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const std::string& text = answer ? currentCard.back : currentCard.front;
  // 1→UI_12@1×, 2→UI_18@1× (native medium), 3→UI_12@2×
  const int scaleHalf = fontScaleToHalf(ANKI_STORE.getFontScale());
  const bool landscape = ANKI_STORE.isLandscapeCards();

  // Status strip (UI_10 throughout):
  //   progress bar | [flag column when set]
  //   "12/50 · 76%" | [front / EN+L2] | <deck name>
  //   ────────────────────────────────────
  // Flag layout (approved mocks):
  //   Landscape: right of bar, top = bar bottom edge (dropped under bar line).
  //   Portrait:  right column, bottom-aligned with status block; bar+deck shift left.
  const AnkiDeckInfo deckInfo = ANKI_STORE.getDeckInfo(ANKI_STORE.getCurrentDeckIndex());
  const uint16_t remaining = deckInfo.remaining;
  const uint16_t total = deckInfo.total > 0 ? deckInfo.total : remaining;
  const unsigned percentDone =
      total == 0 ? 100u
                 : static_cast<unsigned>((static_cast<uint32_t>(total - remaining) * 100u) / total);

  const int padX = metrics.contentSidePadding;
  const int statusInnerW = std::max(1, safe.width - 2 * padX);
  const int statsFont = UI_10_FONT_ID;
  const int statsLineH = renderer.getLineHeight(statsFont);
  constexpr int kBarH = 8;
  constexpr int kFlagW = 18;
  constexpr int kFlagH = 16;
  constexpr int kFlagGap = 10;
  const bool showFlag = currentCard.flag != 0;
  // Content column (bar + stats + deck) shrinks when the flag column is reserved.
  const int contentW =
      showFlag ? std::max(40, statusInnerW - kFlagW - kFlagGap) : statusInnerW;
  const int gapAfterHeader = landscape ? 2 : 4;
  const int gapBarStats = 2;
  const int gapStatsSep = landscape ? 2 : 4;
  const int gapSepContent = landscape ? 4 : 8;
  int cursorY = safe.y + gapAfterHeader;

  const int barY = cursorY;
  const int barX = safe.x + padX;
  const int barW = contentW;
  renderer.drawRect(barX, barY, barW, kBarH);
  if (total > 0 && percentDone > 0) {
    const int fillW = std::max(1, static_cast<int>((static_cast<int64_t>(barW - 2) * percentDone) / 100));
    renderer.fillRect(barX + 1, barY + 1, std::min(fillW, barW - 2), kBarH - 2, true);
  }

  cursorY = barY + kBarH + gapBarStats;

  const int statsY = cursorY;
  char stats[40];
  snprintf(stats, sizeof(stats), tr(STR_ANKI_PROGRESS_FORMAT), static_cast<unsigned int>(remaining),
           static_cast<unsigned int>(total), percentDone);
  const int statsW = renderer.getTextWidth(statsFont, stats, EpdFontFamily::REGULAR);
  renderer.drawText(statsFont, safe.x + padX, statsY, stats, true, EpdFontFamily::REGULAR);

  // Right of content column: deck name (flag sits further right when set).
  constexpr int kSideGap = 8;
  int nameW = 0;
  std::string nameFit;
  if (!deckInfo.name.empty()) {
    // Cap deck name so front still gets room (~1/3 of content column).
    const int nameCap = std::max(40, contentW / 3);
    nameFit = renderer.truncatedText(statsFont, deckInfo.name.c_str(), nameCap, EpdFontFamily::BOLD);
    nameW = renderer.getTextWidth(statsFont, nameFit.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(statsFont, safe.x + padX + contentW - nameW, statsY, nameFit.c_str(), true,
                      EpdFontFamily::BOLD);
  }

  // Middle (answer only): front / EN+L2 prompt between progress and deck.
  if (answer) {
    const std::string frontPlain = plainStatusPreview(currentCard.front);
    if (!frontPlain.empty()) {
      const int leftEdge = statsW + kSideGap;
      const int rightEdge = nameW > 0 ? (contentW - nameW - kSideGap) : contentW;
      const int midMaxW = std::max(0, rightEdge - leftEdge);
      if (midMaxW > 16) {
        const std::string frontFit =
            renderer.truncatedText(statsFont, frontPlain.c_str(), midMaxW, EpdFontFamily::REGULAR);
        if (!frontFit.empty()) {
          const int frontW = renderer.getTextWidth(statsFont, frontFit.c_str(), EpdFontFamily::REGULAR);
          // Center within the middle slot (between progress and deck).
          const int midSlotX = safe.x + padX + leftEdge;
          const int frontX = midSlotX + std::max(0, (midMaxW - frontW) / 2);
          renderer.drawText(statsFont, frontX, statsY, frontFit.c_str(), true, EpdFontFamily::REGULAR);
        }
      }
    }
  }

  // Flag column (right of content): orientation-specific vertical placement.
  if (showFlag) {
    const int flagX = safe.x + padX + statusInnerW - kFlagW;
    int flagY = barY;
    if (landscape) {
      // Top of flag sits on the lower progress-bar edge (dropped under the bar).
      flagY = barY + kBarH;
    } else {
      // Portrait: bottom-align with status block (stats line bottom).
      const int statusBottom = statsY + statsLineH;
      flagY = statusBottom - kFlagH;
    }
    // Solid pennant + pole (no internal white line).
    const int poleX = flagX + 2;
    renderer.drawLine(poleX, flagY, poleX, flagY + kFlagH, true);
    renderer.drawLine(poleX + 1, flagY, poleX + 1, flagY + kFlagH, true);
    const int top = flagY + 1;
    const int bot = flagY + (kFlagH * 72) / 100;
    const int midY = (top + bot) / 2;
    for (int y = top; y <= bot; y++) {
      const int t = (y <= midY) ? (y - top) : (bot - y);
      const int span = 4 + (t * (kFlagW - 6)) / std::max(1, midY - top);
      const int x0 = poleX + 2;
      const int x1 = std::min(flagX + kFlagW - 1, x0 + span);
      if (x1 > x0) renderer.drawLine(x0, y, x1, y, true);
    }
  }

  cursorY = statsY + statsLineH + gapStatsSep;

  const int separatorY = cursorY;
  renderer.fillRect(safe.x + padX, separatorY, statusInnerW, 1, true);

  const int x = safe.x + padX + 4;
  const int contentTop = separatorY + gapSepContent;
  // Text line pitch: at least kCardAreaGapPx (25); never below font advance (no overlap).
  // Blank rows between areas always advance exactly kCardAreaGapPx (see drawTextPage).
  const int fontBody = scalePx(renderer.getLineHeight(cardFontId()), scaleHalf);
  const int lineHeight = std::max(kCardAreaGapPx, fontBody + 2);
  const int footerHeight = metrics.buttonHintsHeight + renderer.getLineHeight(SMALL_FONT_ID) + 8;
  // Landscape: button hints sit on the side; safe area already accounts for that.
  const int contentBottom = safe.y + safe.height - (landscape ? 18 : footerHeight);
  const int availableHeight = std::max(1, contentBottom - contentTop);
  const int maxLines = std::max(1, availableHeight / lineHeight);
  const int contentWidth = safe.width - 2 * (metrics.contentSidePadding + 4);

  nextPageStart =
      drawTextPage(text, pageStart, x, contentTop, contentWidth, maxLines, lineHeight, scaleHalf, false);

  std::string paging;
  if (!pageHistory.empty()) paging += "< ";
  paging += tr(STR_ANKI_SIDE_PAGES);
  if (nextPageStart < text.size()) paging += " >";
  const int pagingY = landscape
                          ? safe.y + safe.height - renderer.getLineHeight(SMALL_FONT_ID) - 2
                          : pageHeight - metrics.buttonHintsHeight - renderer.getLineHeight(SMALL_FONT_ID) - 4;
  renderer.drawCenteredText(SMALL_FONT_ID, pagingY, paging.c_str());

  if (answer) {
    drawGradeButtonHints(renderer, ANKI_STORE.isLeftHanded());
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_ANKI_ANSWER), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  (void)pageWidth;
  displayCardBuffer();
}

void AnkiActivity::renderMessage(const bool error) {
  applyUiOrientation(false);
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_ANKI),
                 ankiFirmwareVersionLabel());

  const int x = metrics.contentSidePadding;
  const int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 3;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID) + 4;
  const int maxLines = std::max(1, (pageHeight - y - metrics.buttonHintsHeight - metrics.verticalSpacing) / lineHeight);
  // scaleHalf 2 = 1.0× (UI messages stay unscaled)
  drawTextPage(message, 0, x, y, pageWidth - 2 * x, maxLines, lineHeight, 2, false);

  if (state != State::WORKING) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  displayUiBuffer();
}
