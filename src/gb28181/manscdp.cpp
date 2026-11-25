// GB/T 28181 MANSCDP XML codec and catalog mapping.
//
// The XML reader is intentionally small: MANSCDP documents are a flat tree of
// elements with text and a few attributes, so a full XML library would add a
// dependency (and a licence review) for no benefit. The reader handles the
// constructs a platform actually sends: the XML declaration, comments, CDATA,
// character/entity references and nested elements.

#include "gb28181/manscdp.h"
#include "gb28181/time.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace uvnvr::gb28181 {
namespace {

// ---------------------------------------------------------------------------
// Small text helpers
// ---------------------------------------------------------------------------

bool IsAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view TrimAscii(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size() && IsAsciiSpace(text[begin]))
    ++begin;
  std::size_t end = text.size();
  while (end > begin && IsAsciiSpace(text[end - 1]))
    --end;
  return text.substr(begin, end - begin);
}

std::string ToLowerAscii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text)
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return out;
}

bool EqualIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

bool IsAsciiDigit(char c) {
  return c >= '0' && c <= '9';
}

bool AllAsciiDigits(std::string_view text) {
  if (text.empty())
    return false;
  for (char c : text) {
    if (!IsAsciiDigit(c))
      return false;
  }
  return true;
}

constexpr std::string_view kXmlHeader = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n";

std::string EscapeXml(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char c : text) {
    switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    case '\'':
      out += "&apos;";
      break;
    default:
      out.push_back(c);
      break;
    }
  }
  return out;
}

bool AppendUtf8(std::uint32_t code_point, std::string* out) {
  if (code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF))
    return false;
  if (code_point <= 0x7F) {
    out->push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FF) {
    out->push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point <= 0xFFFF) {
    out->push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
  return true;
}

bool DecodeEntities(std::string_view raw, std::string* out, std::string* error) {
  out->reserve(out->size() + raw.size());
  for (std::size_t i = 0; i < raw.size();) {
    if (raw[i] != '&') {
      out->push_back(raw[i++]);
      continue;
    }
    const std::size_t semicolon = raw.find(';', i + 1);
    if (semicolon == std::string_view::npos || semicolon - i > 12) {
      if (error)
        *error = "unterminated XML entity reference";
      return false;
    }
    const std::string_view entity = raw.substr(i + 1, semicolon - i - 1);
    if (entity == "amp") {
      out->push_back('&');
    } else if (entity == "lt") {
      out->push_back('<');
    } else if (entity == "gt") {
      out->push_back('>');
    } else if (entity == "quot") {
      out->push_back('"');
    } else if (entity == "apos") {
      out->push_back('\'');
    } else if (!entity.empty() && entity[0] == '#') {
      std::string_view digits = entity.substr(1);
      int base = 10;
      if (!digits.empty() && (digits[0] == 'x' || digits[0] == 'X')) {
        base = 16;
        digits.remove_prefix(1);
      }
      if (digits.empty()) {
        if (error)
          *error = "empty numeric XML entity";
        return false;
      }
      std::uint32_t code_point = 0;
      const auto result =
          std::from_chars(digits.data(), digits.data() + digits.size(), code_point, base);
      if (result.ec != std::errc() || result.ptr != digits.data() + digits.size() ||
          !AppendUtf8(code_point, out)) {
        if (error)
          *error = "invalid numeric XML entity";
        return false;
      }
    } else {
      if (error)
        *error = "unknown XML entity '&" + std::string(entity) + ";'";
      return false;
    }
    i = semicolon + 1;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Minimal XML tree reader
// ---------------------------------------------------------------------------

struct XmlNode {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::vector<XmlNode> children;
  std::string text;

  const XmlNode* Child(std::string_view child_name) const {
    for (const XmlNode& child : children) {
      if (child.name == child_name)
        return &child;
    }
    return nullptr;
  }

  std::string ChildText(std::string_view child_name) const {
    const XmlNode* child = Child(child_name);
    if (child == nullptr)
      return {};
    return std::string(TrimAscii(child->text));
  }

  std::string Attribute(std::string_view attribute_name) const {
    for (const auto& [name, value] : attributes) {
      if (name == attribute_name)
        return value;
    }
    return {};
  }
};

bool IsNameStart(char c) {
  const auto ch = static_cast<unsigned char>(c);
  return std::isalpha(ch) != 0 || c == '_' || c == ':';
}

bool IsNameChar(char c) {
  const auto ch = static_cast<unsigned char>(c);
  return std::isalnum(ch) != 0 || c == '_' || c == '-' || c == '.' || c == ':';
}

class XmlParser {
public:
  explicit XmlParser(std::string_view input) : input_(input) {}

  bool ParseDocument(XmlNode* root, std::string* error) {
    // Skip a UTF-8 BOM when a platform or editor emitted one.
    if (input_.size() >= 3 && static_cast<unsigned char>(input_[0]) == 0xEF &&
        static_cast<unsigned char>(input_[1]) == 0xBB &&
        static_cast<unsigned char>(input_[2]) == 0xBF) {
      pos_ = 3;
    }
    if (!SkipMisc(error))
      return false;
    if (pos_ >= input_.size() || input_[pos_] != '<')
      return Fail("expected the root element", error);
    if (!ParseElement(root, error))
      return false;
    if (!SkipMisc(error))
      return false;
    if (pos_ != input_.size())
      return Fail("trailing content after the root element", error);
    return true;
  }

private:
  bool Fail(const std::string& message, std::string* error) {
    if (error != nullptr)
      *error = message + " (at byte " + std::to_string(pos_) + ")";
    return false;
  }

  bool StartsWith(std::string_view prefix) const {
    return input_.substr(pos_, prefix.size()) == prefix;
  }

  void SkipWhitespace() {
    while (pos_ < input_.size() && IsAsciiSpace(input_[pos_]))
      ++pos_;
  }

  bool SkipUntil(std::string_view terminator, const char* what, std::string* error) {
    const std::size_t end = input_.find(terminator, pos_);
    if (end == std::string_view::npos)
      return Fail(std::string("unterminated ") + what, error);
    pos_ = end + terminator.size();
    return true;
  }

  bool SkipDeclaration(std::string* error) {
    // <!DOCTYPE ...> may carry an internal subset in brackets.
    int bracket_depth = 0;
    char quote = '\0';
    pos_ += 2; // "<!"
    while (pos_ < input_.size()) {
      const char c = input_[pos_++];
      if (quote != '\0') {
        if (c == quote)
          quote = '\0';
        continue;
      }
      if (c == '"' || c == '\'') {
        quote = c;
      } else if (c == '[') {
        ++bracket_depth;
      } else if (c == ']') {
        if (bracket_depth > 0)
          --bracket_depth;
      } else if (c == '>' && bracket_depth == 0) {
        return true;
      }
    }
    return Fail("unterminated XML declaration", error);
  }

  bool SkipMisc(std::string* error) {
    for (;;) {
      SkipWhitespace();
      if (StartsWith("<?")) {
        if (!SkipUntil("?>", "processing instruction", error))
          return false;
        continue;
      }
      if (StartsWith("<!--")) {
        if (!SkipUntil("-->", "comment", error))
          return false;
        continue;
      }
      if (StartsWith("<!")) {
        if (!SkipDeclaration(error))
          return false;
        continue;
      }
      return true;
    }
  }

  bool ParseName(std::string* name, std::string* error) {
    if (pos_ >= input_.size() || !IsNameStart(input_[pos_]))
      return Fail("expected an element or attribute name", error);
    const std::size_t begin = pos_;
    while (pos_ < input_.size() && IsNameChar(input_[pos_]))
      ++pos_;
    name->assign(input_.substr(begin, pos_ - begin));
    return true;
  }

  bool ParseElement(XmlNode* node, std::string* error) {
    if (pos_ >= input_.size() || input_[pos_] != '<')
      return Fail("expected '<'", error);
    ++pos_;
    if (!ParseName(&node->name, error))
      return false;

    for (;;) {
      SkipWhitespace();
      if (pos_ >= input_.size())
        return Fail("unterminated start tag <" + node->name + ">", error);
      if (StartsWith("/>")) {
        pos_ += 2;
        return true;
      }
      if (input_[pos_] == '>') {
        ++pos_;
        break;
      }
      std::string attribute_name;
      if (!ParseName(&attribute_name, error))
        return false;
      SkipWhitespace();
      if (pos_ >= input_.size() || input_[pos_] != '=')
        return Fail("expected '=' after attribute '" + attribute_name + "'", error);
      ++pos_;
      SkipWhitespace();
      if (pos_ >= input_.size() || (input_[pos_] != '"' && input_[pos_] != '\''))
        return Fail("expected a quoted attribute value", error);
      const char quote = input_[pos_++];
      const std::size_t end = input_.find(quote, pos_);
      if (end == std::string_view::npos)
        return Fail("unterminated attribute value", error);
      std::string value;
      if (!DecodeEntities(input_.substr(pos_, end - pos_), &value, error))
        return false;
      node->attributes.emplace_back(std::move(attribute_name), std::move(value));
      pos_ = end + 1;
    }

    std::string text;
    for (;;) {
      if (pos_ >= input_.size())
        return Fail("unterminated element <" + node->name + ">", error);
      if (StartsWith("</")) {
        pos_ += 2;
        std::string closing_name;
        if (!ParseName(&closing_name, error))
          return false;
        if (closing_name != node->name)
          return Fail("mismatched closing tag </" + closing_name + "> for <" + node->name + ">",
                      error);
        SkipWhitespace();
        if (pos_ >= input_.size() || input_[pos_] != '>')
          return Fail("expected '>' in closing tag", error);
        ++pos_;
        node->text = std::move(text);
        return true;
      }
      if (StartsWith("<!--")) {
        if (!SkipUntil("-->", "comment", error))
          return false;
        continue;
      }
      if (StartsWith("<?")) {
        if (!SkipUntil("?>", "processing instruction", error))
          return false;
        continue;
      }
      if (StartsWith("<![CDATA[")) {
        pos_ += 9;
        const std::size_t end = input_.find("]]>", pos_);
        if (end == std::string_view::npos)
          return Fail("unterminated CDATA section", error);
        text.append(input_.substr(pos_, end - pos_));
        pos_ = end + 3;
        continue;
      }
      if (input_[pos_] == '<') {
        XmlNode child;
        if (!ParseElement(&child, error))
          return false;
        node->children.push_back(std::move(child));
        continue;
      }
      const std::size_t next = input_.find('<', pos_);
      if (next == std::string_view::npos)
        return Fail("unterminated element <" + node->name + ">", error);
      if (!DecodeEntities(input_.substr(pos_, next - pos_), &text, error))
        return false;
      pos_ = next;
    }
  }

  std::string_view input_;
  std::size_t pos_ = 0;
};

bool ParseXmlDocument(std::string_view xml, XmlNode* root, std::string* error) {
  XmlParser parser(xml);
  return parser.ParseDocument(root, error);
}

// ---------------------------------------------------------------------------
// Element helpers used by the builders and parsers
// ---------------------------------------------------------------------------

void AppendElement(std::string* out, std::string_view indent, std::string_view name,
                   std::string_view value) {
  out->append(indent);
  out->push_back('<');
  out->append(name);
  out->push_back('>');
  out->append(EscapeXml(value));
  out->append("</");
  out->append(name);
  out->append(">\r\n");
}

void AppendUintElement(std::string* out, std::string_view indent, std::string_view name,
                       std::uint64_t value) {
  AppendElement(out, indent, name, std::to_string(value));
}

bool ParseUintText(std::string_view text, std::uint64_t* value, std::string_view field,
                   std::string* error) {
  const std::string_view trimmed = TrimAscii(text);
  if (trimmed.empty()) {
    if (error != nullptr)
      *error = "<" + std::string(field) + "> is empty";
    return false;
  }
  std::uint64_t parsed = 0;
  const auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed);
  if (result.ec != std::errc() || result.ptr != trimmed.data() + trimmed.size()) {
    if (error != nullptr)
      *error = "<" + std::string(field) + "> is not an unsigned integer";
    return false;
  }
  *value = parsed;
  return true;
}

bool ParseRequiredUint(const XmlNode& root, std::string_view name, std::uint64_t* value,
                       std::string* error) {
  const XmlNode* child = root.Child(name);
  if (child == nullptr) {
    if (error != nullptr)
      *error = "missing <" + std::string(name) + ">";
    return false;
  }
  return ParseUintText(child->text, value, name, error);
}

bool ParseOptionalInt(const XmlNode& root, std::string_view name, int default_value, int* value,
                      std::string* error) {
  const XmlNode* child = root.Child(name);
  if (child == nullptr) {
    *value = default_value;
    return true;
  }
  const std::string_view trimmed = TrimAscii(child->text);
  if (trimmed.empty()) {
    *value = default_value;
    return true;
  }
  std::uint64_t parsed = 0;
  const auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed);
  if (result.ec != std::errc() || result.ptr != trimmed.data() + trimmed.size() ||
      parsed > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    if (error != nullptr)
      *error = "<" + std::string(name) + "> is not a valid integer";
    return false;
  }
  *value = static_cast<int>(parsed);
  return true;
}

bool ParseOptionalUint32(const XmlNode& root, std::string_view name, std::uint32_t default_value,
                         std::uint32_t* value, std::string* error) {
  const XmlNode* child = root.Child(name);
  if (child == nullptr) {
    *value = default_value;
    return true;
  }
  const std::string_view trimmed = TrimAscii(child->text);
  if (trimmed.empty()) {
    *value = default_value;
    return true;
  }
  std::uint64_t parsed = 0;
  const auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed);
  if (result.ec != std::errc() || result.ptr != trimmed.data() + trimmed.size() ||
      parsed > std::numeric_limits<std::uint32_t>::max()) {
    if (error != nullptr)
      *error = "<" + std::string(name) + "> is not a valid 32-bit unsigned integer";
    return false;
  }
  *value = static_cast<std::uint32_t>(parsed);
  return true;
}

bool RequireRoot(const XmlNode& root, std::string_view name, std::string* error) {
  if (root.name == name)
    return true;
  if (error != nullptr)
    *error = "expected <" + std::string(name) + "> as the root element, got <" + root.name + ">";
  return false;
}

bool RequireCmdType(const XmlNode& root, ManscdpCommand expected, std::string* error) {
  const std::string cmd_type = root.ChildText("CmdType");
  if (cmd_type.empty()) {
    if (error != nullptr)
      *error = "missing <CmdType>";
    return false;
  }
  if (ParseManscdpCommand(cmd_type) != expected) {
    if (error != nullptr)
      *error = "unexpected <CmdType>" + cmd_type + "</CmdType>";
    return false;
  }
  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// GB/T 28181 identifiers
// ---------------------------------------------------------------------------

std::string_view Gb28181IdKindName(Gb28181IdKind kind) {
  switch (kind) {
  case Gb28181IdKind::kDevice:
    return "device";
  case Gb28181IdKind::kPlatform:
    return "platform";
  case Gb28181IdKind::kBusinessGroup:
    return "business-group";
  case Gb28181IdKind::kVirtualOrganization:
    return "virtual-organization";
  case Gb28181IdKind::kUnknown:
    break;
  }
  return "unknown";
}

bool ParseGb28181Id(std::string_view id, Gb28181Id* parsed, std::string* error) {
  if (id.size() != 20) {
    if (error != nullptr)
      *error = "GB/T 28181 ID must be exactly 20 digits, got " + std::to_string(id.size()) +
               " characters";
    return false;
  }
  if (!AllAsciiDigits(id)) {
    if (error != nullptr)
      *error = "GB/T 28181 ID must contain only ASCII digits";
    return false;
  }

  Gb28181Id value;
  value.center_code.assign(id.substr(0, 8));
  value.industry_code.assign(id.substr(8, 2));
  value.type_code.assign(id.substr(10, 3));
  std::uint32_t sequence = 0;
  for (std::size_t i = 13; i < 20; ++i)
    sequence = sequence * 10 + static_cast<std::uint32_t>(id[i] - '0');
  value.sequence = sequence;

  if (value.center_code == "00000000") {
    if (error != nullptr)
      *error = "GB/T 28181 center code must not be all zeroes";
    return false;
  }
  if (sequence == 0) {
    if (error != nullptr)
      *error = "GB/T 28181 sequence number must be 1..9999999";
    return false;
  }
  if (value.type_code[0] < '1' || value.type_code[0] > '4') {
    if (error != nullptr)
      *error = "GB/T 28181 type code must start with 1 (device), 2 (platform), "
               "3 (business group) or 4 (virtual organization)";
    return false;
  }

  if (parsed != nullptr)
    *parsed = std::move(value);
  return true;
}

bool IsValidGb28181Id(std::string_view id, std::string* error) {
  return ParseGb28181Id(id, nullptr, error);
}

Gb28181IdKind Gb28181IdKindOf(std::string_view id) {
  Gb28181Id parsed;
  if (!ParseGb28181Id(id, &parsed, nullptr))
    return Gb28181IdKind::kUnknown;
  switch (parsed.type_code[0]) {
  case '1':
    return Gb28181IdKind::kDevice;
  case '2':
    return Gb28181IdKind::kPlatform;
  case '3':
    return Gb28181IdKind::kBusinessGroup;
  case '4':
    return Gb28181IdKind::kVirtualOrganization;
  default:
    return Gb28181IdKind::kUnknown;
  }
}

std::string FormatGb28181Id(const Gb28181Id& id) {
  if (id.center_code.size() != 8 || id.industry_code.size() != 2 || id.type_code.size() != 3)
    return {};
  if (!AllAsciiDigits(id.center_code) || !AllAsciiDigits(id.industry_code) ||
      !AllAsciiDigits(id.type_code))
    return {};
  if (id.center_code == "00000000" || id.sequence == 0 || id.sequence > 9999999)
    return {};
  if (id.type_code[0] < '1' || id.type_code[0] > '4')
    return {};

  char sequence[8] = {};
  std::snprintf(sequence, sizeof(sequence), "%07u", static_cast<unsigned>(id.sequence));
  std::string out = id.center_code;
  out += id.industry_code;
  out += id.type_code;
  out += sequence;
  return out;
}

std::string MakeChannelGb28181Id(std::string_view center_code, std::uint32_t sequence,
                                 std::string_view type_code, std::string_view industry_code) {
  return FormatGb28181Id(Gb28181Id{std::string(center_code), std::string(industry_code),
                                   std::string(type_code), sequence});
}

std::string MakeGroupGb28181Id(std::string_view center_code, std::uint32_t sequence,
                               std::string_view type_code, std::string_view industry_code) {
  return FormatGb28181Id(Gb28181Id{std::string(center_code), std::string(industry_code),
                                   std::string(type_code), sequence});
}

// ---------------------------------------------------------------------------
// MANSCDP command model
// ---------------------------------------------------------------------------

std::string_view ManscdpCommandName(ManscdpCommand command) {
  switch (command) {
  case ManscdpCommand::kCatalog:
    return "Catalog";
  case ManscdpCommand::kDeviceInfo:
    return "DeviceInfo";
  case ManscdpCommand::kDeviceStatus:
    return "DeviceStatus";
  case ManscdpCommand::kRecordInfo:
    return "RecordInfo";
  case ManscdpCommand::kKeepalive:
    return "Keepalive";
  case ManscdpCommand::kAlarm:
    return "Alarm";
  case ManscdpCommand::kDeviceControl:
    return "DeviceControl";
  case ManscdpCommand::kPlayback:
    return "Playback";
  case ManscdpCommand::kUnknown:
    break;
  }
  return "Unknown";
}

ManscdpCommand ParseManscdpCommand(std::string_view name) {
  const std::string_view trimmed = TrimAscii(name);
  if (EqualIgnoreCase(trimmed, "Catalog"))
    return ManscdpCommand::kCatalog;
  if (EqualIgnoreCase(trimmed, "DeviceInfo"))
    return ManscdpCommand::kDeviceInfo;
  if (EqualIgnoreCase(trimmed, "DeviceStatus"))
    return ManscdpCommand::kDeviceStatus;
  if (EqualIgnoreCase(trimmed, "RecordInfo"))
    return ManscdpCommand::kRecordInfo;
  if (EqualIgnoreCase(trimmed, "Keepalive"))
    return ManscdpCommand::kKeepalive;
  if (EqualIgnoreCase(trimmed, "Alarm"))
    return ManscdpCommand::kAlarm;
  if (EqualIgnoreCase(trimmed, "DeviceControl"))
    return ManscdpCommand::kDeviceControl;
  if (EqualIgnoreCase(trimmed, "Playback"))
    return ManscdpCommand::kPlayback;
  return ManscdpCommand::kUnknown;
}

ManscdpDocumentKind ManscdpDocumentKindOf(std::string_view root_name) {
  const std::string_view trimmed = TrimAscii(root_name);
  if (EqualIgnoreCase(trimmed, "Query"))
    return ManscdpDocumentKind::kQuery;
  if (EqualIgnoreCase(trimmed, "Response"))
    return ManscdpDocumentKind::kResponse;
  if (EqualIgnoreCase(trimmed, "Notify"))
    return ManscdpDocumentKind::kNotify;
  if (EqualIgnoreCase(trimmed, "Control"))
    return ManscdpDocumentKind::kControl;
  return ManscdpDocumentKind::kUnknown;
}

bool ParseManscdpControl(std::string_view xml, ManscdpControl* control, std::string* error) {
  XmlNode root;
  if (!ParseXmlDocument(xml, &root, error))
    return false;

  ManscdpControl parsed;
  parsed.document = ManscdpDocumentKindOf(root.name);
  if (parsed.document == ManscdpDocumentKind::kUnknown) {
    if (error != nullptr)
      *error = "unsupported MANSCDP root element <" + root.name + ">";
    return false;
  }
  const std::string cmd_type = root.ChildText("CmdType");
  if (cmd_type.empty()) {
    if (error != nullptr)
      *error = "missing <CmdType>";
    return false;
  }
  parsed.command = ParseManscdpCommand(cmd_type);
  if (parsed.command == ManscdpCommand::kUnknown) {
    if (error != nullptr)
      *error = "unsupported MANSCDP CmdType '" + cmd_type + "'";
    return false;
  }
  std::uint64_t sn = 0;
  if (!ParseRequiredUint(root, "SN", &sn, error))
    return false;
  parsed.sn = sn;
  parsed.device_id = root.ChildText("DeviceID");

  if (control != nullptr)
    *control = std::move(parsed);
  return true;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::string BuildManscdpQuery(ManscdpCommand command, const ManscdpQuery& query) {
  const std::string_view name = ManscdpCommandName(command);
  if (name == "Unknown")
    return {};
  std::string out(kXmlHeader);
  out += "<Query>\r\n";
  AppendElement(&out, "  ", "CmdType", name);
  AppendUintElement(&out, "  ", "SN", query.sn);
  if (!query.device_id.empty())
    AppendElement(&out, "  ", "DeviceID", query.device_id);
  out += "</Query>\r\n";
  return out;
}

bool ParseManscdpQuery(std::string_view xml, ManscdpQuery* query, ManscdpCommand* command,
                       std::string* error) {
  XmlNode root;
  if (!ParseXmlDocument(xml, &root, error))
    return false;
  if (!RequireRoot(root, "Query", error))
    return false;

  const std::string cmd_type = root.ChildText("CmdType");
  const ManscdpCommand parsed_command = ParseManscdpCommand(cmd_type);
  if (parsed_command != ManscdpCommand::kCatalog && parsed_command != ManscdpCommand::kDeviceInfo &&
      parsed_command != ManscdpCommand::kDeviceStatus) {
    if (error != nullptr)
      *error = "MANSCDP query has unsupported CmdType '" + cmd_type + "'";
    return false;
  }

  std::uint64_t sn = 0;
  if (!ParseRequiredUint(root, "SN", &sn, error))
    return false;

  ManscdpQuery parsed;
  parsed.sn = sn;
  parsed.device_id = root.ChildText("DeviceID");
  if (query != nullptr)
    *query = std::move(parsed);
  if (command != nullptr)
    *command = parsed_command;
  return true;
}

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------

std::string BuildCatalogResponse(const CatalogResponse& response) {
  std::string out(kXmlHeader);
  out += "<Response>\r\n";
  AppendElement(&out, "  ", "CmdType", "Catalog");
  AppendUintElement(&out, "  ", "SN", response.sn);
  AppendElement(&out, "  ", "DeviceID", response.device_id);
  AppendUintElement(&out, "  ", "SumNum", response.devices.size());
  out += "  <DeviceList Num=\"" + std::to_string(response.devices.size()) + "\">\r\n";
  for (const CatalogItem& item : response.devices) {
    out += "    <Item>\r\n";
    AppendElement(&out, "      ", "DeviceID", item.device_id);
    AppendElement(&out, "      ", "Name", item.name);
    AppendElement(&out, "      ", "Manufacturer", item.manufacturer);
    AppendElement(&out, "      ", "Model", item.model);
    AppendElement(&out, "      ", "Owner", item.owner);
    AppendElement(&out, "      ", "CivilCode", item.civil_code);
    AppendElement(&out, "      ", "Address", item.address);
    AppendUintElement(&out, "      ", "Parental", static_cast<std::uint64_t>(item.parental));
    AppendElement(&out, "      ", "ParentID", item.parent_id);
    AppendUintElement(&out, "      ", "SafetyWay", static_cast<std::uint64_t>(item.safety_way));
    AppendUintElement(&out, "      ", "RegisterWay", static_cast<std::uint64_t>(item.register_way));
    AppendUintElement(&out, "      ", "Secrecy", static_cast<std::uint64_t>(item.secrecy));
    AppendElement(&out, "      ", "Status", item.status);
    if (!item.longitude.empty())
      AppendElement(&out, "      ", "Longitude", item.longitude);
    if (!item.latitude.empty())
      AppendElement(&out, "      ", "Latitude", item.latitude);
    out += "    </Item>\r\n";
  }
  out += "  </DeviceList>\r\n";
  out += "</Response>\r\n";
  return out;
}

bool ParseCatalogResponse(std::string_view xml, CatalogResponse* response, std::string* error) {
  XmlNode root;
  if (!ParseXmlDocument(xml, &root, error))
    return false;
  if (!RequireRoot(root, "Response", error))
    return false;
  if (!RequireCmdType(root, ManscdpCommand::kCatalog, error))
    return false;

  CatalogResponse parsed;
  if (!ParseRequiredUint(root, "SN", &parsed.sn, error))
    return false;
  parsed.device_id = root.ChildText("DeviceID");
  std::uint32_t sum_num = 0;
  if (!ParseOptionalUint32(root, "SumNum", 0, &sum_num, error))
    return false;
  parsed.sum_num = sum_num;

  const XmlNode* list = root.Child("DeviceList");
  if (list != nullptr) {
    for (const XmlNode& item_node : list->children) {
      if (item_node.name != "Item")
        continue;
      CatalogItem item;
      item.device_id = item_node.ChildText("DeviceID");
      if (item.device_id.empty()) {
        if (error != nullptr)
          *error = "Catalog <Item> is missing <DeviceID>";
        return false;
      }
      item.name = item_node.ChildText("Name");
      item.manufacturer = item_node.ChildText("Manufacturer");
      item.model = item_node.ChildText("Model");
      item.owner = item_node.ChildText("Owner");
      item.civil_code = item_node.ChildText("CivilCode");
      item.address = item_node.ChildText("Address");
      item.parent_id = item_node.ChildText("ParentID");
      item.status = item_node.ChildText("Status");
      if (item.status.empty())
        item.status = "ON";
      item.longitude = item_node.ChildText("Longitude");
      item.latitude = item_node.ChildText("Latitude");
      if (!ParseOptionalInt(item_node, "Parental", 0, &item.parental, error) ||
          !ParseOptionalInt(item_node, "SafetyWay", 0, &item.safety_way, error) ||
          !ParseOptionalInt(item_node, "RegisterWay", 1, &item.register_way, error) ||
          !ParseOptionalInt(item_node, "Secrecy", 0, &item.secrecy, error)) {
        return false;
      }
      parsed.devices.push_back(std::move(item));
    }
  }

  if (response != nullptr)
    *response = std::move(parsed);
  return true;
}

// ---------------------------------------------------------------------------
// DeviceInfo
// ---------------------------------------------------------------------------

std::string BuildDeviceInfoResponse(const DeviceInfoResponse& response) {
  std::string out(kXmlHeader);
  out += "<Response>\r\n";
  AppendElement(&out, "  ", "CmdType", "DeviceInfo");
  AppendUintElement(&out, "  ", "SN", response.sn);
  AppendElement(&out, "  ", "DeviceID", response.info.device_id);
  AppendElement(&out, "  ", "Result", response.info.result.empty() ? "OK" : response.info.result);
  AppendElement(&out, "  ", "DeviceName", response.info.device_name);
  AppendElement(&out, "  ", "DeviceType", response.info.device_type);
  AppendElement(&out, "  ", "Manufacturer", response.info.manufacturer);
  AppendElement(&out, "  ", "Model", response.info.model);
  AppendElement(&out, "  ", "Firmware", response.info.firmware);
  AppendUintElement(&out, "  ", "Channel", response.info.channel);
  out += "</Response>\r\n";
  return out;
}

bool ParseDeviceInfoResponse(std::string_view xml, DeviceInfoResponse* response,
                             std::string* error) {
  XmlNode root;
  if (!ParseXmlDocument(xml, &root, error))
    return false;
  if (!RequireRoot(root, "Response", error))
    return false;
  if (!RequireCmdType(root, ManscdpCommand::kDeviceInfo, error))
    return false;

  DeviceInfoResponse parsed;
  if (!ParseRequiredUint(root, "SN", &parsed.sn, error))
    return false;
  parsed.info.device_id = root.ChildText("DeviceID");
  parsed.info.result = root.ChildText("Result");
  if (parsed.info.result.empty())
    parsed.info.result = "OK";
  parsed.info.device_name = root.ChildText("DeviceName");
  parsed.info.device_type = root.ChildText("DeviceType");
  parsed.info.manufacturer = root.ChildText("Manufacturer");
  parsed.info.model = root.ChildText("Model");
  parsed.info.firmware = root.ChildText("Firmware");
  if (!ParseOptionalUint32(root, "Channel", 0, &parsed.info.channel, error))
    return false;

  if (response != nullptr)
    *response = std::move(parsed);
  return true;
}

// ---------------------------------------------------------------------------
// DeviceStatus
// ---------------------------------------------------------------------------

std::string BuildDeviceStatusResponse(const DeviceStatusResponse& response) {
  std::string out(kXmlHeader);
  out += "<Response>\r\n";
  AppendElement(&out, "  ", "CmdType", "DeviceStatus");
  AppendUintElement(&out, "  ", "SN", response.sn);
  AppendElement(&out, "  ", "DeviceID", response.status.device_id);
  AppendElement(&out, "  ", "Result",
                response.status.result.empty() ? "OK" : response.status.result);
  AppendElement(&out, "  ", "Online", response.status.online);
  AppendElement(&out, "  ", "Status", response.status.status);
  AppendElement(&out, "  ", "Encode", response.status.encode);
  AppendElement(&out, "  ", "Record", response.status.record);
  AppendElement(&out, "  ", "DeviceTime", response.status.device_time);
  out += "</Response>\r\n";
  return out;
}

bool ParseDeviceStatusResponse(std::string_view xml, DeviceStatusResponse* response,
                               std::string* error) {
  XmlNode root;
  if (!ParseXmlDocument(xml, &root, error))
    return false;
  if (!RequireRoot(root, "Response", error))
    return false;
  if (!RequireCmdType(root, ManscdpCommand::kDeviceStatus, error))
    return false;

  DeviceStatusResponse parsed;
  if (!ParseRequiredUint(root, "SN", &parsed.sn, error))
    return false;
  parsed.status.device_id = root.ChildText("DeviceID");
  parsed.status.result = root.ChildText("Result");
  parsed.status.online = root.ChildText("Online");
  parsed.status.status = root.ChildText("Status");
  parsed.status.encode = root.ChildText("Encode");
  parsed.status.record = root.ChildText("Record");
  parsed.status.device_time = root.ChildText("DeviceTime");
  if (parsed.status.result.empty())
    parsed.status.result = "OK";

  if (response != nullptr)
    *response = std::move(parsed);
  return true;
}

// ---------------------------------------------------------------------------
// RecordInfo
// ---------------------------------------------------------------------------

namespace {

bool ParseRecordType(std::string_view text, std::string* type, std::string* error) {
  const std::string value = ToLowerAscii(TrimAscii(text));
  if (value.empty()) {
    *type = "all";
    return true;
  }
  if (value != "all" && value != "time" && value != "alarm" && value != "manual") {
    if (error != nullptr)
      *error = "unsupported <Type>'" + value + "'</Type>";
    return false;
  }
  *type = value;
  return true;
}

bool ParseOptionalTime(const XmlNode& root, std::string_view name, std::int64_t* value,
                       std::int64_t fallback, std::string* error) {
  const XmlNode* child = root.Child(name);
  if (child == nullptr) {
    *value = fallback;
    return true;
  }
  const std::string_view text = TrimAscii(child->text);
  if (text.empty()) {
    *value = fallback;
    return true;
  }
  return ParseGbTime(text, value, error);
}

} // namespace

std::string BuildRecordInfoQuery(const RecordInfoQuery& query) {
  std::string out(kXmlHeader);
  out += "<Query>\r\n";
  AppendElement(&out, "  ", "CmdType", "RecordInfo");
  AppendUintElement(&out, "  ", "SN", query.sn);
  AppendElement(&out, "  ", "DeviceID", query.device_id);
  AppendElement(&out, "  ", "StartTime", FormatGbTime(query.start_ms));
  AppendElement(&out, "  ", "EndTime", FormatGbTime(query.end_ms));
  if (!query.file_path.empty())
    AppendElement(&out, "  ", "FilePath", query.file_path);
  if (!query.address.empty())
    AppendElement(&out, "  ", "Address", query.address);
  AppendUintElement(&out, "  ", "Secrecy", static_cast<std::uint64_t>(query.secrecy));
  AppendElement(&out, "  ", "Type", query.type.empty() ? "all" : query.type);
  out += "</Query>\r\n";
  return out;
}

bool ParseRecordInfoQuery(std::string_view xml, RecordInfoQuery* query, std::string* error) {
  XmlNode root;
  if (!ParseXmlDocument(xml, &root, error))
    return false;
  if (!RequireRoot(root, "Query", error))
    return false;
  if (!RequireCmdType(root, ManscdpCommand::kRecordInfo, error))
    return false;

  RecordInfoQuery parsed;
  if (!ParseRequiredUint(root, "SN", &parsed.sn, error))
    return false;
  parsed.device_id = root.ChildText("DeviceID");
  if (parsed.device_id.empty()) {
    if (error != nullptr)
      *error = "missing <DeviceID>";
    return false;
  }
  if (!ParseOptionalTime(root, "StartTime", &parsed.start_ms, 0, error))
    return false;
  if (!ParseOptionalTime(root, "EndTime", &parsed.end_ms,
                         std::numeric_limits<std::int64_t>::max(), error))
    return false;
  if (parsed.end_ms < parsed.start_ms) {
    if (error != nullptr)
      *error = "EndTime is before StartTime";
    return false;
  }
  parsed.file_path = root.ChildText("FilePath");
  parsed.address = root.ChildText("Address");
  if (!ParseOptionalInt(root, "Secrecy", 0, &parsed.secrecy, error))
    return false;
  if (!ParseRecordType(root.ChildText("Type"), &parsed.type, error))
    return false;

  if (query != nullptr)
    *query = std::move(parsed);
  return true;
}

std::string BuildRecordInfoResponse(const RecordInfoResponse& response) {
  std::string out(kXmlHeader);
  out += "<Response>\r\n";
  AppendElement(&out, "  ", "CmdType", "RecordInfo");
  AppendUintElement(&out, "  ", "SN", response.sn);
  AppendElement(&out, "  ", "DeviceID", response.device_id);
  if (!response.name.empty())
    AppendElement(&out, "  ", "Name", response.name);
  AppendUintElement(&out, "  ", "SumNum", response.sum_num);
  out += "  <RecordList Num=\"";
  out += std::to_string(response.sum_num);
  out += "\">\r\n";
  for (const RecordItem& item : response.items) {
    out += "    <Item>\r\n";
    AppendElement(&out, "      ", "DeviceID", item.device_id);
    if (!item.name.empty())
      AppendElement(&out, "      ", "Name", item.name);
    AppendElement(&out, "      ", "FilePath", item.file_path);
    if (!item.address.empty())
      AppendElement(&out, "      ", "Address", item.address);
    AppendElement(&out, "      ", "StartTime", FormatGbTime(item.start_ms));
    AppendElement(&out, "      ", "EndTime", FormatGbTime(item.end_ms));
    AppendUintElement(&out, "      ", "Secrecy", static_cast<std::uint64_t>(item.secrecy));
    AppendElement(&out, "      ", "Type", item.type);
    AppendUintElement(&out, "      ", "FileSize", item.file_size);
    out += "    </Item>\r\n";
  }
  out += "  </RecordList>\r\n";
  out += "</Response>\r\n";
  return out;
}

bool ParseRecordInfoResponse(std::string_view xml, RecordInfoResponse* response,
                             std::string* error) {
  XmlNode root;
  if (!ParseXmlDocument(xml, &root, error))
    return false;
  if (!RequireRoot(root, "Response", error))
    return false;
  if (!RequireCmdType(root, ManscdpCommand::kRecordInfo, error))
    return false;

  RecordInfoResponse parsed;
  if (!ParseRequiredUint(root, "SN", &parsed.sn, error))
    return false;
  parsed.device_id = root.ChildText("DeviceID");
  parsed.name = root.ChildText("Name");
  std::uint64_t sum = 0;
  if (!ParseRequiredUint(root, "SumNum", &sum, error))
    return false;
  parsed.sum_num = static_cast<std::uint32_t>(sum);

  const XmlNode* list = root.Child("RecordList");
  if (list == nullptr) {
    if (error != nullptr)
      *error = "missing <RecordList>";
    return false;
  }
  for (const XmlNode& node : list->children) {
    if (node.name != "Item")
      continue;
    RecordItem item;
    item.device_id = node.ChildText("DeviceID");
    item.name = node.ChildText("Name");
    item.file_path = node.ChildText("FilePath");
    item.address = node.ChildText("Address");
    if (!ParseOptionalTime(node, "StartTime", &item.start_ms, 0, error))
      return false;
    if (!ParseOptionalTime(node, "EndTime", &item.end_ms, 0, error))
      return false;
    if (!ParseOptionalInt(node, "Secrecy", 0, &item.secrecy, error))
      return false;
    if (!ParseRecordType(node.ChildText("Type"), &item.type, error))
      return false;
    const XmlNode* size = node.Child("FileSize");
    if (size != nullptr && !TrimAscii(size->text).empty()) {
      std::uint64_t bytes = 0;
      if (!ParseUintText(size->text, &bytes, "FileSize", error))
        return false;
      item.file_size = bytes;
    }
    parsed.items.push_back(std::move(item));
  }
  if (parsed.items.size() != parsed.sum_num && error != nullptr) {
    // SumNum is advisory in the standard, but a mismatch usually means a
    // truncated response; report it without failing the parse.
    *error = "SumNum does not match the number of <Item> elements";
  }

  if (response != nullptr)
    *response = std::move(parsed);
  return true;
}

// ---------------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------------

std::string BuildPlaybackControl(const PlaybackControl& control) {
  std::string out(kXmlHeader);
  out += "<Control>\r\n";
  AppendElement(&out, "  ", "CmdType", "Playback");
  AppendUintElement(&out, "  ", "SN", control.sn);
  AppendElement(&out, "  ", "DeviceID", control.device_id);
  char scale[32];
  std::snprintf(scale, sizeof(scale), "%.2f", control.scale);
  AppendElement(&out, "  ", "Scale", scale);
  if (control.has_range) {
    AppendElement(&out, "  ", "Range", FormatGbTime(control.range_start_ms) + "/" +
                                        FormatGbTime(control.range_end_ms));
  }
  out += "</Control>\r\n";
  return out;
}

bool ParsePlaybackControl(std::string_view xml, PlaybackControl* control, std::string* error) {
  XmlNode root;
  if (!ParseXmlDocument(xml, &root, error))
    return false;
  if (!RequireRoot(root, "Control", error))
    return false;
  if (!RequireCmdType(root, ManscdpCommand::kPlayback, error))
    return false;

  PlaybackControl parsed;
  if (!ParseRequiredUint(root, "SN", &parsed.sn, error))
    return false;
  parsed.device_id = root.ChildText("DeviceID");

  const XmlNode* scale_node = root.Child("Scale");
  if (scale_node != nullptr && !TrimAscii(scale_node->text).empty()) {
    const std::string text(TrimAscii(scale_node->text));
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(value) || std::fabs(value) > 16.0) {
      if (error != nullptr)
        *error = "invalid <Scale> value";
      return false;
    }
    parsed.scale = value;
    parsed.has_scale = true;
  }

  const XmlNode* range_node = root.Child("Range");
  if (range_node != nullptr) {
    const std::string_view range = TrimAscii(range_node->text);
    if (!range.empty()) {
      if (!ParseGbTimeRange(range, &parsed.range_start_ms, &parsed.range_end_ms, error))
        return false;
      if (parsed.range_end_ms != 0 && parsed.range_end_ms < parsed.range_start_ms) {
        if (error != nullptr)
          *error = "Range end is before its start";
        return false;
      }
      parsed.has_range = true;
    }
  }

  if (control != nullptr)
    *control = std::move(parsed);
  return true;
}

// ---------------------------------------------------------------------------
// DeviceControl / PTZCmd
// ---------------------------------------------------------------------------

namespace {

int HexNibble(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

bool ParseHexBytes(std::string_view text, std::uint8_t* out, std::size_t count) {
  if (text.size() != count * 2)
    return false;
  for (std::size_t i = 0; i < count; ++i) {
    const int high = HexNibble(text[i * 2]);
    const int low = HexNibble(text[i * 2 + 1]);
    if (high < 0 || low < 0)
      return false;
    out[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

char HexDigit(std::uint8_t value) {
  return static_cast<char>(value < 10 ? '0' + value : 'A' + (value - 10));
}

std::string FormatHexBytes(const std::uint8_t* data, std::size_t count) {
  std::string out;
  out.reserve(count * 2);
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(HexDigit(static_cast<std::uint8_t>(data[i] >> 4)));
    out.push_back(HexDigit(static_cast<std::uint8_t>(data[i] & 0x0F)));
  }
  return out;
}

std::uint8_t PtzChecksum(const std::uint8_t* bytes) {
  unsigned sum = 0;
  for (std::size_t i = 0; i < 7; ++i)
    sum += bytes[i];
  return static_cast<std::uint8_t>(sum & 0xFF);
}

PtzAction PtzActionFromCode(std::uint8_t code) {
  switch (code) {
  case 0x00:
    return PtzAction::kStop;
  case 0x01:
    return PtzAction::kRight;
  case 0x02:
    return PtzAction::kLeft;
  case 0x03:
    return PtzAction::kUp;
  case 0x04:
    return PtzAction::kDown;
  case 0x05:
    return PtzAction::kUpRight;
  case 0x06:
    return PtzAction::kUpLeft;
  case 0x07:
    return PtzAction::kDownRight;
  case 0x08:
    return PtzAction::kDownLeft;
  case 0x09:
    return PtzAction::kZoomIn;
  case 0x0A:
    return PtzAction::kZoomOut;
  case 0x0B:
    return PtzAction::kFocusNear;
  case 0x0C:
    return PtzAction::kFocusFar;
  case 0x0D:
    return PtzAction::kIrisOpen;
  case 0x0E:
    return PtzAction::kIrisClose;
  case 0x0F:
  case 0x81: // common vendor spelling of "set preset"
    return PtzAction::kSetPreset;
  case 0x10:
  case 0x82: // common vendor spelling of "call preset"
    return PtzAction::kGotoPreset;
  case 0x11:
  case 0x83: // common vendor spelling of "delete preset"
    return PtzAction::kRemovePreset;
  default:
    return PtzAction::kUnknown;
  }
}

std::uint8_t PtzCodeFromAction(PtzAction action) {
  switch (action) {
  case PtzAction::kStop:
    return 0x00;
  case PtzAction::kRight:
    return 0x01;
  case PtzAction::kLeft:
    return 0x02;
  case PtzAction::kUp:
    return 0x03;
  case PtzAction::kDown:
    return 0x04;
  case PtzAction::kUpRight:
    return 0x05;
  case PtzAction::kUpLeft:
    return 0x06;
  case PtzAction::kDownRight:
    return 0x07;
  case PtzAction::kDownLeft:
    return 0x08;
  case PtzAction::kZoomIn:
    return 0x09;
  case PtzAction::kZoomOut:
    return 0x0A;
  case PtzAction::kFocusNear:
    return 0x0B;
  case PtzAction::kFocusFar:
    return 0x0C;
  case PtzAction::kIrisOpen:
    return 0x0D;
  case PtzAction::kIrisClose:
    return 0x0E;
  case PtzAction::kSetPreset:
    return 0x0F;
  case PtzAction::kGotoPreset:
    return 0x10;
  case PtzAction::kRemovePreset:
    return 0x11;
  case PtzAction::kUnknown:
    break;
  }
  return 0xA5; // not a valid action; the builder rejects it
}

bool IsPresetAction(PtzAction action) {
  return action == PtzAction::kSetPreset || action == PtzAction::kGotoPreset ||
         action == PtzAction::kRemovePreset;
}

} // namespace

std::string_view PtzActionName(PtzAction action) {
  switch (action) {
  case PtzAction::kStop:
    return "stop";
  case PtzAction::kRight:
    return "right";
  case PtzAction::kLeft:
    return "left";
  case PtzAction::kUp:
    return "up";
  case PtzAction::kDown:
    return "down";
  case PtzAction::kUpRight:
    return "up-right";
  case PtzAction::kUpLeft:
    return "up-left";
  case PtzAction::kDownRight:
    return "down-right";
  case PtzAction::kDownLeft:
    return "down-left";
  case PtzAction::kZoomIn:
    return "zoom-in";
  case PtzAction::kZoomOut:
    return "zoom-out";
  case PtzAction::kFocusNear:
    return "focus-near";
  case PtzAction::kFocusFar:
    return "focus-far";
  case PtzAction::kIrisOpen:
    return "iris-open";
  case PtzAction::kIrisClose:
    return "iris-close";
  case PtzAction::kSetPreset:
    return "set-preset";
  case PtzAction::kGotoPreset:
    return "goto-preset";
  case PtzAction::kRemovePreset:
    return "remove-preset";
  case PtzAction::kUnknown:
    break;
  }
  return "unknown";
}

bool DecodePtzCmd(std::string_view hex, PtzCmd* command, std::string* error) {
  const std::string_view trimmed = TrimAscii(hex);
  std::uint8_t bytes[8] = {};
  if (!ParseHexBytes(trimmed, bytes, 8)) {
    if (error != nullptr)
      *error = "PTZCmd must be exactly 16 hexadecimal digits";
    return false;
  }
  if (bytes[0] != 0xA5) {
    if (error != nullptr)
      *error = "PTZCmd must start with 0xA5";
    return false;
  }
  if (PtzChecksum(bytes) != bytes[7]) {
    if (error != nullptr)
      *error = "PTZCmd checksum mismatch";
    return false;
  }
  PtzAction action = PtzActionFromCode(bytes[1]);
  if (action == PtzAction::kUnknown) {
    if (error != nullptr)
      *error = "unsupported PTZCmd action 0x" + FormatHexBytes(&bytes[1], 1);
    return false;
  }

  PtzCmd parsed;
  parsed.action = action;
  parsed.action_code = bytes[1];
  parsed.horizontal_speed = bytes[2];
  parsed.vertical_speed = bytes[3];
  parsed.zoom_speed = bytes[4];
  parsed.preset = static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[5]) << 8) | bytes[6]);
  std::memcpy(parsed.bytes.data(), bytes, sizeof(bytes));
  if (IsPresetAction(action) && parsed.preset == 0) {
    if (error != nullptr)
      *error = "PTZCmd preset action has preset number 0";
    return false;
  }
  if (command != nullptr)
    *command = parsed;
  return true;
}

std::string EncodePtzCmd(const PtzCmd& command) {
  if (command.action == PtzAction::kUnknown)
    return {};
  std::uint8_t bytes[8] = {};
  bytes[0] = 0xA5;
  bytes[1] = PtzCodeFromAction(command.action);
  if (bytes[1] == 0xA5)
    return {};
  bytes[2] = command.horizontal_speed;
  bytes[3] = command.vertical_speed;
  bytes[4] = command.zoom_speed;
  if (IsPresetAction(command.action)) {
    if (command.preset == 0)
      return {};
    bytes[5] = static_cast<std::uint8_t>((command.preset >> 8) & 0xFF);
    bytes[6] = static_cast<std::uint8_t>(command.preset & 0xFF);
  }
  bytes[7] = PtzChecksum(bytes);
  return FormatHexBytes(bytes, 8);
}

PtzVelocity PtzVelocityFromCmd(const PtzCmd& command) {
  PtzVelocity velocity;
  velocity.action = command.action;
  velocity.preset = command.preset;
  const double pan_speed = static_cast<double>(command.horizontal_speed) / 255.0;
  const double tilt_speed = static_cast<double>(command.vertical_speed) / 255.0;
  double zoom_speed = static_cast<double>(command.zoom_speed) / 255.0;
  switch (command.action) {
  case PtzAction::kRight:
    velocity.pan = pan_speed;
    break;
  case PtzAction::kLeft:
    velocity.pan = -pan_speed;
    break;
  case PtzAction::kUp:
    velocity.tilt = tilt_speed;
    break;
  case PtzAction::kDown:
    velocity.tilt = -tilt_speed;
    break;
  case PtzAction::kUpRight:
    velocity.pan = pan_speed;
    velocity.tilt = tilt_speed;
    break;
  case PtzAction::kUpLeft:
    velocity.pan = -pan_speed;
    velocity.tilt = tilt_speed;
    break;
  case PtzAction::kDownRight:
    velocity.pan = pan_speed;
    velocity.tilt = -tilt_speed;
    break;
  case PtzAction::kDownLeft:
    velocity.pan = -pan_speed;
    velocity.tilt = -tilt_speed;
    break;
  case PtzAction::kZoomIn:
    // Platforms occasionally leave the zoom-speed byte at zero while still
    // asking for zoom; a zero velocity would be a no-op on ONVIF cameras.
    velocity.zoom = zoom_speed > 0.0 ? zoom_speed : 0.5;
    break;
  case PtzAction::kZoomOut:
    velocity.zoom = -(zoom_speed > 0.0 ? zoom_speed : 0.5);
    break;
  case PtzAction::kStop:
  case PtzAction::kFocusNear:
  case PtzAction::kFocusFar:
  case PtzAction::kIrisOpen:
  case PtzAction::kIrisClose:
  case PtzAction::kSetPreset:
  case PtzAction::kGotoPreset:
  case PtzAction::kRemovePreset:
  case PtzAction::kUnknown:
    break;
  }
  return velocity;
}

std::string BuildDeviceControl(const PtzControl& control) {
  const std::string ptz_cmd = EncodePtzCmd(control.command);
  if (ptz_cmd.empty())
    return {};
  std::string out(kXmlHeader);
  out += "<Control>\r\n";
  AppendElement(&out, "  ", "CmdType", "DeviceControl");
  AppendUintElement(&out, "  ", "SN", control.sn);
  AppendElement(&out, "  ", "DeviceID", control.device_id);
  AppendElement(&out, "  ", "PTZCmd", ptz_cmd);
  out += "  <Info>\r\n";
  AppendUintElement(&out, "    ", "ControlPriority",
                    static_cast<std::uint64_t>(std::max(1, std::min(control.control_priority, 5))));
  out += "  </Info>\r\n";
  out += "</Control>\r\n";
  return out;
}

bool ParseDeviceControl(std::string_view xml, PtzControl* control, std::string* error) {
  XmlNode root;
  if (!ParseXmlDocument(xml, &root, error))
    return false;
  if (!RequireRoot(root, "Control", error))
    return false;
  if (!RequireCmdType(root, ManscdpCommand::kDeviceControl, error))
    return false;

  PtzControl parsed;
  if (!ParseRequiredUint(root, "SN", &parsed.sn, error))
    return false;
  parsed.device_id = root.ChildText("DeviceID");
  if (parsed.device_id.empty()) {
    if (error != nullptr)
      *error = "DeviceControl has no <DeviceID>";
    return false;
  }
  const XmlNode* ptz = root.Child("PTZCmd");
  if (ptz == nullptr) {
    if (error != nullptr)
      *error = "DeviceControl has no <PTZCmd>";
    return false;
  }
  if (!DecodePtzCmd(ptz->text, &parsed.command, error))
    return false;
  if (const XmlNode* info = root.Child("Info")) {
    if (!ParseOptionalInt(*info, "ControlPriority", 1, &parsed.control_priority, error))
      return false;
  }
  if (control != nullptr)
    *control = std::move(parsed);
  return true;
}

// ---------------------------------------------------------------------------
// Alarm notification
// ---------------------------------------------------------------------------

std::string BuildAlarmNotify(const AlarmNotify& alarm) {
  std::string out(kXmlHeader);
  out += "<Notify>\r\n";
  AppendElement(&out, "  ", "CmdType", "Alarm");
  AppendUintElement(&out, "  ", "SN", alarm.sn);
  AppendElement(&out, "  ", "DeviceID", alarm.device_id);
  AppendUintElement(&out, "  ", "AlarmPriority",
                    static_cast<std::uint64_t>(std::max(1, std::min(alarm.priority, 4))));
  AppendUintElement(&out, "  ", "AlarmMethod",
                    static_cast<std::uint64_t>(std::max(1, std::min(alarm.method, 9))));
  AppendElement(&out, "  ", "AlarmTime",
                alarm.time.empty() ? CurrentLocalIso8601() : alarm.time);
  AppendElement(&out, "  ", "AlarmDescription", alarm.description);
  out += "  <AlarmInfo>\r\n";
  AppendElement(&out, "    ", "AlarmType", alarm.alarm_type.empty() ? "1" : alarm.alarm_type);
  if (!alarm.alarm_type_param.empty())
    AppendElement(&out, "    ", "AlarmTypeParam", alarm.alarm_type_param);
  out += "  </AlarmInfo>\r\n";
  out += "</Notify>\r\n";
  return out;
}

bool ParseAlarmNotify(std::string_view xml, AlarmNotify* alarm, std::string* error) {
  XmlNode root;
  if (!ParseXmlDocument(xml, &root, error))
    return false;
  if (root.name != "Notify" && root.name != "Response") {
    if (error != nullptr)
      *error = "expected <Notify> for an Alarm, got <" + root.name + ">";
    return false;
  }
  if (!RequireCmdType(root, ManscdpCommand::kAlarm, error))
    return false;

  AlarmNotify parsed;
  if (!ParseRequiredUint(root, "SN", &parsed.sn, error))
    return false;
  parsed.device_id = root.ChildText("DeviceID");
  if (!ParseOptionalInt(root, "AlarmPriority", 2, &parsed.priority, error))
    return false;
  if (!ParseOptionalInt(root, "AlarmMethod", 5, &parsed.method, error))
    return false;
  if (parsed.priority < 1 || parsed.priority > 4) {
    if (error != nullptr)
      *error = "<AlarmPriority> must be 1..4";
    return false;
  }
  parsed.time = root.ChildText("AlarmTime");
  parsed.description = root.ChildText("AlarmDescription");
  if (const XmlNode* info = root.Child("AlarmInfo")) {
    parsed.alarm_type = info->ChildText("AlarmType");
    if (const XmlNode* param = info->Child("AlarmTypeParam")) {
      parsed.alarm_type_param = param->ChildText("AlarmType");
      if (parsed.alarm_type_param.empty())
        parsed.alarm_type_param = std::string(TrimAscii(param->text));
    }
  }
  if (parsed.alarm_type.empty())
    parsed.alarm_type = "1";
  if (alarm != nullptr)
    *alarm = std::move(parsed);
  return true;
}

// ---------------------------------------------------------------------------
// Keepalive
// ---------------------------------------------------------------------------

std::string BuildKeepalive(const KeepaliveMessage& message) {
  std::string out(kXmlHeader);
  out += "<Notify>\r\n";
  AppendElement(&out, "  ", "CmdType", "Keepalive");
  AppendUintElement(&out, "  ", "SN", message.sn);
  AppendElement(&out, "  ", "DeviceID", message.device_id);
  AppendElement(&out, "  ", "Status", message.status);
  out += "</Notify>\r\n";
  return out;
}

bool ParseKeepalive(std::string_view xml, KeepaliveMessage* message, std::string* error) {
  XmlNode root;
  if (!ParseXmlDocument(xml, &root, error))
    return false;
  if (root.name != "Notify" && root.name != "Response") {
    if (error != nullptr)
      *error = "expected <Notify> or <Response> for Keepalive, got <" + root.name + ">";
    return false;
  }
  if (!RequireCmdType(root, ManscdpCommand::kKeepalive, error))
    return false;

  KeepaliveMessage parsed;
  if (!ParseRequiredUint(root, "SN", &parsed.sn, error))
    return false;
  parsed.device_id = root.ChildText("DeviceID");
  parsed.status = root.ChildText("Status");
  if (parsed.status.empty())
    parsed.status = "OK";

  if (message != nullptr)
    *message = std::move(parsed);
  return true;
}

// ---------------------------------------------------------------------------
// Local channel / group mapping
// ---------------------------------------------------------------------------

namespace {

std::string ResolveCivilCode(const CatalogMapConfig& config) {
  if (!config.civil_code.empty())
    return config.civil_code;
  if (config.device_id.size() >= 6)
    return config.device_id.substr(0, 6);
  return {};
}

std::string ResolveChildCode(const std::string& specific, const std::string& fallback) {
  return specific.empty() ? fallback : specific;
}

} // namespace

CatalogMapResult MapChannelsToCatalog(const CatalogMapConfig& config,
                                      const std::vector<CatalogGroup>& groups,
                                      const std::vector<CatalogChannel>& channels) {
  CatalogMapResult result;
  std::string error;
  if (!IsValidGb28181Id(config.device_id, &error)) {
    result.error = "device_id: " + error;
    return result;
  }

  std::unordered_map<std::int64_t, std::size_t> group_by_local;
  std::unordered_set<std::string> used_ids;
  used_ids.insert(config.device_id); // the NVR itself is not a catalog child
  for (std::size_t i = 0; i < groups.size(); ++i) {
    const CatalogGroup& group = groups[i];
    if (group.local_id == 0) {
      result.error = "group[" + std::to_string(i) + "]: local_id must not be zero";
      return result;
    }
    if (!group_by_local.emplace(group.local_id, i).second) {
      result.error =
          "group[" + std::to_string(i) + "]: duplicate local_id " + std::to_string(group.local_id);
      return result;
    }
    if (!IsValidGb28181Id(group.device_id, &error)) {
      result.error = "group[" + std::to_string(i) + "] device_id: " + error;
      return result;
    }
    const Gb28181IdKind kind = Gb28181IdKindOf(group.device_id);
    if (kind != Gb28181IdKind::kBusinessGroup && kind != Gb28181IdKind::kVirtualOrganization) {
      result.error = "group[" + std::to_string(i) +
                     "]: device_id must be a business group (3xx) or virtual organization (4xx)";
      return result;
    }
    if (!used_ids.insert(group.device_id).second) {
      result.error = "duplicate GB/T 28181 ID " + group.device_id;
      return result;
    }
  }

  for (std::size_t i = 0; i < groups.size(); ++i) {
    const CatalogGroup& group = groups[i];
    if (group.parent_local_id != 0 &&
        group_by_local.find(group.parent_local_id) == group_by_local.end()) {
      result.error = "group[" + std::to_string(i) + "]: parent_local_id " +
                     std::to_string(group.parent_local_id) + " does not exist";
      return result;
    }
  }

  // Reject parent cycles before rendering a tree that the platform cannot use.
  for (const CatalogGroup& group : groups) {
    std::unordered_set<std::int64_t> visited;
    std::int64_t current = group.local_id;
    while (current != 0) {
      if (!visited.insert(current).second) {
        result.error = "group hierarchy contains a cycle at local_id " + std::to_string(current);
        return result;
      }
      const auto found = group_by_local.find(current);
      if (found == group_by_local.end())
        break;
      current = groups[found->second].parent_local_id;
    }
  }

  for (std::size_t i = 0; i < channels.size(); ++i) {
    const CatalogChannel& channel = channels[i];
    if (channel.local_id == 0) {
      result.error = "channel[" + std::to_string(i) + "]: local_id must not be zero";
      return result;
    }
    if (!IsValidGb28181Id(channel.device_id, &error)) {
      result.error = "channel[" + std::to_string(i) + "] device_id: " + error;
      return result;
    }
    if (Gb28181IdKindOf(channel.device_id) != Gb28181IdKind::kDevice) {
      result.error = "channel[" + std::to_string(i) + "]: device_id must be a device (1xx)";
      return result;
    }
    if (!used_ids.insert(channel.device_id).second) {
      result.error = "duplicate GB/T 28181 ID " + channel.device_id;
      return result;
    }
    if (channel.group_local_id != 0 &&
        group_by_local.find(channel.group_local_id) == group_by_local.end()) {
      result.error = "channel[" + std::to_string(i) + "]: group_local_id " +
                     std::to_string(channel.group_local_id) + " does not exist";
      return result;
    }
  }

  const std::string civil_code = ResolveCivilCode(config);
  result.items.reserve(groups.size() + channels.size());

  for (const CatalogGroup& group : groups) {
    CatalogItem item;
    item.device_id = group.device_id;
    item.name = group.name.empty() ? group.device_id : group.name;
    item.manufacturer = config.manufacturer;
    item.model = config.model;
    item.owner = config.owner;
    item.civil_code = civil_code;
    item.address = config.address;
    item.parental = 1;
    item.parent_id = group.parent_local_id == 0
                         ? config.device_id
                         : groups[group_by_local.at(group.parent_local_id)].device_id;
    item.register_way = 1;
    item.secrecy = config.secrecy;
    item.status = "ON";
    result.items.push_back(std::move(item));
  }

  for (const CatalogChannel& channel : channels) {
    if (!channel.enabled && !config.include_disabled_channels)
      continue;
    CatalogItem item;
    item.device_id = channel.device_id;
    item.name = channel.name.empty() ? channel.device_id : channel.name;
    item.manufacturer = ResolveChildCode(channel.manufacturer, config.manufacturer);
    item.model = ResolveChildCode(channel.model, config.model);
    item.owner = ResolveChildCode(channel.owner, config.owner);
    item.civil_code = ResolveChildCode(channel.civil_code, civil_code);
    item.address = ResolveChildCode(channel.address, config.address);
    item.parental = 0;
    item.parent_id = channel.group_local_id == 0
                         ? config.device_id
                         : groups[group_by_local.at(channel.group_local_id)].device_id;
    item.register_way = 1;
    item.secrecy = config.secrecy;
    item.status = (channel.enabled && channel.online) ? "ON" : "OFF";
    result.items.push_back(std::move(item));
  }

  result.ok = true;
  return result;
}

// ---------------------------------------------------------------------------
// MANSCDP responder
// ---------------------------------------------------------------------------

std::string CurrentLocalIso8601() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#ifdef _WIN32
  if (localtime_s(&local, &now) != 0)
    return {};
#else
  if (localtime_r(&now, &local) == nullptr)
    return {};
#endif
  char buffer[32] = {};
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &local) == 0)
    return {};
  return buffer;
}

ManscdpResponder::ManscdpResponder(ManscdpDeviceConfig config) : config_(std::move(config)) {}

void ManscdpResponder::set_group_source(GroupSource source) {
  group_source_ = std::move(source);
}

void ManscdpResponder::set_channel_source(ChannelSource source) {
  channel_source_ = std::move(source);
}

void ManscdpResponder::set_status_source(StatusSource source) {
  status_source_ = std::move(source);
}

void ManscdpResponder::set_record_source(RecordSource source) {
  record_source_ = std::move(source);
}

void ManscdpResponder::set_ptz_handler(PtzHandler handler) {
  ptz_handler_ = std::move(handler);
}

bool ManscdpResponder::BuildRecordInfo(const RecordInfoQuery& query,
                                       std::string* response_body,
                                       std::string* error) const {
  if (!record_source_) {
    if (error != nullptr)
      *error = "RecordInfo was requested but no recording index is configured";
    return false;
  }
  RecordInfoResponse response;
  response.sn = query.sn;
  response.device_id = query.device_id;
  response.items = record_source_(query);
  response.sum_num = static_cast<std::uint32_t>(response.items.size());
  if (response_body != nullptr)
    *response_body = BuildRecordInfoResponse(response);
  return true;
}

CatalogMapResult ManscdpResponder::BuildCatalog() const {
  CatalogMapConfig config;
  config.device_id = config_.device_id;
  config.manufacturer = config_.manufacturer;
  config.model = config_.model;
  config.owner = config_.owner;
  config.civil_code = config_.civil_code;
  config.address = config_.address;
  config.secrecy = config_.secrecy;
  config.include_disabled_channels = config_.include_disabled_channels;
  config.device_name = config_.device_name;

  std::vector<CatalogGroup> groups;
  if (group_source_)
    groups = group_source_();
  std::vector<CatalogChannel> channels;
  if (channel_source_)
    channels = channel_source_();
  return MapChannelsToCatalog(config, groups, channels);
}

CatalogMapResult ManscdpResponder::CatalogSnapshot() const {
  return BuildCatalog();
}

std::vector<CatalogItem> ManscdpResponder::FilterCatalog(const std::vector<CatalogItem>& items,
                                                         std::string_view requested_id) const {
  if (requested_id.empty() || requested_id == config_.device_id)
    return items;

  std::unordered_map<std::string, const CatalogItem*> by_id;
  std::unordered_map<std::string, std::vector<const CatalogItem*>> children;
  for (const CatalogItem& item : items) {
    by_id.emplace(item.device_id, &item);
    if (!item.parent_id.empty())
      children[item.parent_id].push_back(&item);
  }

  const auto root = by_id.find(std::string(requested_id));
  if (root == by_id.end())
    return {};

  std::unordered_set<const CatalogItem*> selected;
  std::vector<const CatalogItem*> pending{root->second};
  while (!pending.empty()) {
    const CatalogItem* item = pending.back();
    pending.pop_back();
    if (!selected.insert(item).second)
      continue;
    const auto found = children.find(item->device_id);
    if (found == children.end())
      continue;
    for (const CatalogItem* child : found->second)
      pending.push_back(child);
  }

  std::vector<CatalogItem> filtered;
  for (const CatalogItem& item : items) {
    if (selected.count(&item) != 0)
      filtered.push_back(item);
  }
  return filtered;
}

DeviceInfo ManscdpResponder::BuildDeviceInfo() const {
  DeviceInfo info;
  info.device_id = config_.device_id;
  info.result = "OK";
  info.device_name = config_.device_name;
  info.device_type = config_.device_type;
  info.manufacturer = config_.manufacturer;
  info.model = config_.model;
  info.firmware = config_.firmware;
  if (channel_source_)
    info.channel = static_cast<std::uint32_t>(channel_source_().size());
  return info;
}

DeviceStatus ManscdpResponder::BuildDeviceStatus() const {
  DeviceStatus status;
  if (status_source_)
    status = status_source_();
  status.device_id = config_.device_id;
  if (status.result.empty())
    status.result = "OK";
  if (status.online.empty())
    status.online = "ONLINE";
  if (status.status.empty())
    status.status = "OK";
  if (status.encode.empty())
    status.encode = "ON";
  if (status.record.empty())
    status.record = "ON";
  if (status.device_time.empty())
    status.device_time = CurrentLocalIso8601();
  return status;
}

bool ManscdpResponder::Handle(std::string_view body, std::string* response_body,
                              std::string* error) const {
  if (response_body != nullptr)
    response_body->clear();

  ManscdpControl control;
  if (!ParseManscdpControl(body, &control, error))
    return false;

  switch (control.command) {
  case ManscdpCommand::kCatalog: {
    const CatalogMapResult mapped = BuildCatalog();
    if (!mapped.ok) {
      if (error != nullptr)
        *error = "cannot build the catalog: " + mapped.error;
      return false;
    }
    CatalogResponse response;
    response.sn = control.sn;
    response.device_id = config_.device_id;
    response.devices = FilterCatalog(mapped.items, control.device_id);
    response.sum_num = static_cast<std::uint32_t>(response.devices.size());
    if (response_body != nullptr)
      *response_body = BuildCatalogResponse(response);
    return true;
  }
  case ManscdpCommand::kDeviceInfo: {
    DeviceInfo info = BuildDeviceInfo();
    const bool match = control.device_id.empty() || control.device_id == config_.device_id;
    info.result = match ? "OK" : "ERROR";
    if (!match)
      info.device_id = control.device_id;
    DeviceInfoResponse response;
    response.sn = control.sn;
    response.info = std::move(info);
    if (response_body != nullptr)
      *response_body = BuildDeviceInfoResponse(response);
    return true;
  }
  case ManscdpCommand::kDeviceStatus: {
    DeviceStatus status = BuildDeviceStatus();
    const bool match = control.device_id.empty() || control.device_id == config_.device_id;
    if (!match) {
      status.device_id = control.device_id;
      status.result = "ERROR";
      status.status = "ERROR";
    }
    DeviceStatusResponse response;
    response.sn = control.sn;
    response.status = std::move(status);
    if (response_body != nullptr)
      *response_body = BuildDeviceStatusResponse(response);
    return true;
  }
  case ManscdpCommand::kRecordInfo: {
    RecordInfoQuery query;
    if (!ParseRecordInfoQuery(body, &query, error))
      return false;
    return BuildRecordInfo(query, response_body, error);
  }
  case ManscdpCommand::kDeviceControl: {
    PtzControl control;
    if (!ParseDeviceControl(body, &control, error))
      return false;
    if (!ptz_handler_) {
      if (error != nullptr)
        *error = "DeviceControl received but no PTZ backend is configured";
      return false;
    }
    // PTZ answers are not MANSCDP documents; the 200 OK sent for the inbound
    // MESSAGE is the acknowledgement. The handler only reports whether the
    // command was accepted by the dispatcher.
    return ptz_handler_(control, error);
  }
  case ManscdpCommand::kKeepalive:
    // A keepalive travels device -> platform; the device does not answer it.
    return true;
  case ManscdpCommand::kPlayback:
    // Playback control arrives in a SIP INFO, not in a MESSAGE query; the
    // media service handles it and answers the INFO directly.
    if (error != nullptr)
      *error = "Playback control must be sent in a SIP INFO";
    return false;
  case ManscdpCommand::kAlarm:
  case ManscdpCommand::kUnknown:
    break;
  }

  if (error != nullptr)
    *error = "unsupported MANSCDP CmdType: " + std::string(ManscdpCommandName(control.command));
  return false;
}

} // namespace uvnvr::gb28181
