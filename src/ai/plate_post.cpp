#include "plate_post.h"

#include <cstdint>
#include <string>
#include <vector>

namespace uvnvr {
namespace {

// Decodes one UTF-8 code point starting at `pos` (which must be < text.size()).
// Invalid sequences are returned as a single replacement-ish code point and
// advance one byte, which keeps the caller making progress.
uint32_t DecodeUtf8(const std::string& text, size_t pos, size_t* length) {
  const unsigned char c = static_cast<unsigned char>(text[pos]);
  if (c < 0x80) {
    *length = 1;
    return c;
  }
  size_t count = 0;
  uint32_t value = 0;
  if ((c & 0xE0) == 0xC0) {
    count = 2;
    value = c & 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    count = 3;
    value = c & 0x0F;
  } else if ((c & 0xF8) == 0xF0) {
    count = 4;
    value = c & 0x07;
  } else {
    *length = 1;
    return c;
  }
  if (pos + count > text.size()) {
    *length = 1;
    return c;
  }
  for (size_t i = 1; i < count; ++i) {
    const unsigned char next = static_cast<unsigned char>(text[pos + i]);
    if ((next & 0xC0) != 0x80) {
      *length = 1;
      return c;
    }
    value = (value << 6) | (next & 0x3F);
  }
  *length = count;
  return value;
}

void AppendUtf8(uint32_t code, std::string* out) {
  if (code < 0x80) {
    out->push_back(static_cast<char>(code));
  } else if (code < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (code >> 6)));
    out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else if (code < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (code >> 12)));
    out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (code >> 18)));
    out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
  }
}

bool IsIgnorable(uint32_t code) {
  switch (code) {
  case ' ':
  case '\t':
  case '\r':
  case '\n':
  case '-':
  case 0x00B7: // · MIDDLE DOT
  case 0x2022: // • BULLET
  case 0x2010: // ‐ HYPHEN
  case 0x2011: // ‑ NON-BREAKING HYPHEN
  case 0x2013: // – EN DASH
  case 0x2014: // — EM DASH
  case 0x3000: // ideographic space
    return true;
  default:
    return false;
  }
}

bool IsAsciiLetter(uint32_t code) {
  return (code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z');
}

bool IsAsciiDigit(uint32_t code) {
  return code >= '0' && code <= '9';
}

char UpperAscii(uint32_t code) {
  if (code >= 'a' && code <= 'z')
    return static_cast<char>(code - 'a' + 'A');
  return static_cast<char>(code);
}

// Folds full-width ASCII (U+FF01..U+FF5E) and returns a vector of UTF-8 units.
// Each unit is one code point; the province character stays a multi-byte unit.
std::vector<std::string> FoldUnits(const std::string& raw) {
  std::vector<std::string> units;
  size_t pos = 0;
  while (pos < raw.size()) {
    size_t length = 0;
    uint32_t code = DecodeUtf8(raw, pos, &length);
    pos += length;
    if (code >= 0xFF01 && code <= 0xFF5E)
      code -= 0xFEE0;
    if (IsIgnorable(code))
      continue;
    std::string unit;
    AppendUtf8(code, &unit);
    units.push_back(std::move(unit));
  }
  return units;
}

bool IsProvince(const std::string& unit) {
  static const char* const kProvinces[] = {
      "京", "津", "冀", "晋", "蒙", "辽", "吉", "黑", "沪", "苏", "浙",
      "皖", "闽", "赣", "鲁", "豫", "鄂", "湘", "粤", "桂", "琼", "渝",
      "川", "贵", "云", "藏", "陕", "甘", "青", "宁", "新",
  };
  for (const char* province : kProvinces) {
    if (unit == province)
      return true;
  }
  return false;
}

} // namespace

std::string NormalizePlate(const std::string& raw) {
  const std::vector<std::string> units = FoldUnits(raw);
  if (units.empty())
    return "";

  std::string out;
  for (size_t i = 0; i < units.size(); ++i) {
    // The first unit is the province character and the second (when present)
    // is the issuing-authority letter. Only the serial segment maps I/O.
    const bool serial = i >= 2;
    const std::string& unit = units[i];
    if (unit.size() == 1) {
      const unsigned char c = static_cast<unsigned char>(unit[0]);
      if (IsAsciiLetter(c)) {
        char letter = UpperAscii(c);
        if (serial) {
          if (letter == 'I')
            letter = '1';
          else if (letter == 'O')
            letter = '0';
        }
        out.push_back(letter);
        continue;
      }
      if (IsAsciiDigit(c)) {
        out.push_back(static_cast<char>(c));
        continue;
      }
    }
    out += unit;
  }
  return out;
}

bool IsValidPlate(const std::string& plate) {
  const std::string normalized = NormalizePlate(plate);
  const std::vector<std::string> units = FoldUnits(normalized);
  if (units.size() != 7 && units.size() != 8)
    return false;
  if (!IsProvince(units[0]))
    return false;
  if (units[1].size() != 1)
    return false;
  const unsigned char authority = static_cast<unsigned char>(units[1][0]);
  if (authority < 'A' || authority > 'Z' || authority == 'I')
    return false;
  for (size_t i = 2; i < units.size(); ++i) {
    if (units[i].size() != 1)
      return false;
    const unsigned char c = static_cast<unsigned char>(units[i][0]);
    if (!IsAsciiDigit(c) && (c < 'A' || c > 'Z'))
      return false;
  }
  return true;
}

} // namespace uvnvr
