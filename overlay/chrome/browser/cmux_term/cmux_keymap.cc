// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_keymap.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <regex>
#include <utility>

#if __has_include("base/memory/raw_ptr.h") && \
    __has_include("third_party/re2/src/re2/re2.h")
#define CMUX_KEYMAP_CHROMIUM_BUILD 1
#include "base/memory/raw_ptr.h"
#include "third_party/re2/src/re2/re2.h"
#endif

namespace cmux {

namespace {

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool IsDigit(char c) {
  return c >= '0' && c <= '9';
}

char LowerAscii(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string LowerAscii(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(LowerAscii(c));
  }
  return out;
}

std::string Trim(std::string_view s) {
  size_t begin = 0;
  size_t end = s.size();
  while (begin < end && IsSpace(s[begin])) {
    ++begin;
  }
  while (end > begin && IsSpace(s[end - 1])) {
    --end;
  }
  return std::string(s.substr(begin, end - begin));
}

void SetError(std::string* error, std::string_view message) {
  if (error) {
    *error = std::string(message);
  }
}

bool ParsePositiveInt(std::string_view text, int* out) {
  if (!out || text.empty()) {
    return false;
  }
  int value = 0;
  for (char c : text) {
    if (!IsDigit(c)) {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  *out = value;
  return true;
}

std::optional<char> UnshiftedAscii(char c) {
  switch (c) {
    case '!':
      return '1';
    case '@':
      return '2';
    case '#':
      return '3';
    case '$':
      return '4';
    case '%':
      return '5';
    case '^':
      return '6';
    case '&':
      return '7';
    case '*':
      return '8';
    case '(':
      return '9';
    case ')':
      return '0';
    case '_':
      return '-';
    case '+':
      return '=';
    case '{':
      return '[';
    case '}':
      return ']';
    case '|':
      return '\\';
    case ':':
      return ';';
    case '"':
      return '\'';
    case '<':
      return ',';
    case '>':
      return '.';
    case '?':
      return '/';
    case '~':
      return '`';
    default:
      return std::nullopt;
  }
}

bool IsSupportedSingleCharKey(char c) {
  if ((c >= 'a' && c <= 'z') || IsDigit(c)) {
    return true;
  }
  switch (c) {
    case '[':
    case ']':
    case '\\':
    case ';':
    case '\'':
    case ',':
    case '.':
    case '/':
    case '-':
    case '=':
    case '`':
    case ' ':
      return true;
    default:
      return false;
  }
}

std::optional<int> ModifierForToken(std::string_view token) {
  const std::string t = LowerAscii(Trim(token));
  if (t == "cmd" || t == "command" || t == "meta") {
    return kKeyModCmd;
  }
  if (t == "ctrl" || t == "control") {
    return kKeyModCtrl;
  }
  if (t == "alt" || t == "option" || t == "opt") {
    return kKeyModAlt;
  }
  if (t == "shift") {
    return kKeyModShift;
  }
  if (t == "mod") {
    return kKeyModPrimary;
  }
  return std::nullopt;
}

bool AddParsedRule(std::vector<KeyRule>* rules,
                   std::string_view key,
                   std::string_view command,
                   std::string_view when = std::string_view()) {
  std::string error;
  std::optional<std::vector<KeyChord>> sequence =
      ParseKeySequence(key, &error);
  if (!sequence || sequence->empty()) {
    return false;
  }
  KeyRule rule;
  rule.sequence = *sequence;
  rule.chord = rule.sequence.front();
  rule.command = std::string(command);
  rule.when_text = std::string(when);
  if (!Trim(when).empty()) {
    std::optional<WhenExpression> expression = ParseWhen(when, &error);
    if (!expression) {
      return false;
    }
    rule.when = *expression;
  }
  rules->push_back(rule);
  return true;
}

void AddDefault(std::vector<KeyRule>* rules,
                std::string_view key,
                std::string_view command) {
  AddParsedRule(rules, key, command);
}

void AddDefault(std::vector<KeyRule>* rules,
                std::string_view key,
                std::string_view command,
                std::string_view when) {
  AddParsedRule(rules, key, command, when);
}

std::string WithModifier(std::string_view modifier, std::string_view key) {
  return std::string(modifier) + "+" + std::string(key);
}

std::string EscapeJson(std::string_view text) {
  std::string out;
  for (char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          const unsigned char value = static_cast<unsigned char>(c);
          const auto hex_digit = [](unsigned char nibble) {
            return static_cast<char>(nibble < 10 ? '0' + nibble
                                                 : 'a' + nibble - 10);
          };
          out += "\\u00";
          out.push_back(hex_digit(value >> 4));
          out.push_back(hex_digit(value & 0x0f));
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  return out;
}

std::string FormatConfigChord(const KeyChord& chord) {
  std::vector<std::string_view> parts;
  if ((chord.modifiers & kKeyModPrimary) != 0) {
    parts.push_back("mod");
  }
  if ((chord.modifiers & kKeyModCmd) != 0) {
    parts.push_back("cmd");
  }
  if ((chord.modifiers & kKeyModCtrl) != 0) {
    parts.push_back("ctrl");
  }
  if ((chord.modifiers & kKeyModAlt) != 0) {
    parts.push_back("alt");
  }
  if ((chord.modifiers & kKeyModShift) != 0) {
    parts.push_back("shift");
  }
  std::string out;
  for (std::string_view part : parts) {
    if (!out.empty()) {
      out += "+";
    }
    out += part;
  }
  if (!out.empty()) {
    out += "+";
  }
  out += chord.key;
  return out;
}

class WhenParser {
 public:
  explicit WhenParser(std::string_view text,
                      const KeyContext* context = nullptr)
      : text_(text), context_(context) {}

  std::optional<WhenExpression> Parse(std::string* error) {
    SkipSpaces();
    WhenExpression expression;
    if (AtEnd()) {
      return expression;
    }
    if (!ParseOr(error)) {
      return std::nullopt;
    }
    SkipSpaces();
    if (!AtEnd()) {
      SetError(error, "expected && or ||");
      return std::nullopt;
    }
    expression.source = Trim(text_);
    return expression;
  }

  std::optional<bool> Evaluate(std::string* error) {
    SkipSpaces();
    if (AtEnd()) {
      return true;
    }
    std::optional<bool> value = ParseOr(error);
    SkipSpaces();
    if (!value || !AtEnd()) {
      if (value) {
        SetError(error, "expected && or ||");
      }
      return std::nullopt;
    }
    return value;
  }

 private:
  bool AtEnd() const { return pos_ >= text_.size(); }

  void SkipSpaces() {
    while (!AtEnd() && IsSpace(text_[pos_])) {
      ++pos_;
    }
  }

  bool Consume(std::string_view token) {
    SkipSpaces();
    if (text_.substr(pos_, token.size()) != token) {
      return false;
    }
    pos_ += token.size();
    return true;
  }

  std::optional<bool> ParseOr(std::string* error) {
    std::optional<bool> value = ParseAnd(error);
    if (!value) {
      return std::nullopt;
    }
    while (true) {
      if (!Consume("||")) {
        return value;
      }
      std::optional<bool> rhs = ParseAnd(error);
      if (!rhs) {
        return std::nullopt;
      }
      *value = *value || *rhs;
    }
  }

  std::optional<bool> ParseAnd(std::string* error) {
    std::optional<bool> value = ParseUnary(error);
    if (!value) {
      return std::nullopt;
    }
    while (true) {
      if (!Consume("&&")) {
        return value;
      }
      std::optional<bool> rhs = ParseUnary(error);
      if (!rhs) {
        return std::nullopt;
      }
      *value = *value && *rhs;
    }
  }

  std::optional<bool> ParseUnary(std::string* error) {
    SkipSpaces();
    if (!AtEnd() && text_[pos_] == '!' &&
        text_.substr(pos_, 2) != "!=") {
      ++pos_;
      std::optional<bool> value = ParseUnary(error);
      if (!value) {
        return std::nullopt;
      }
      return !*value;
    }
    if (Consume("(")) {
      std::optional<bool> value = ParseOr(error);
      if (!value || !Consume(")")) {
        SetError(error, "expected closing parenthesis");
        return std::nullopt;
      }
      return value;
    }
    return ParseAtom(error);
  }

  std::optional<bool> ParseAtom(std::string* error) {
    SkipSpaces();
    const size_t begin = pos_;
    char quote = 0;
    bool in_regex = false;
    while (!AtEnd()) {
      const char current = text_[pos_];
      if (quote != 0) {
        if (current == '\\' && pos_ + 1 < text_.size()) {
          pos_ += 2;
          continue;
        }
        if (current == quote) {
          quote = 0;
        }
        ++pos_;
        continue;
      }
      if (current == '\'' || current == '"') {
        quote = current;
        ++pos_;
        continue;
      }
      if (current == '/') {
        in_regex = !in_regex;
        ++pos_;
        continue;
      }
      if (!in_regex && (current == ')' || text_.substr(pos_, 2) == "&&" ||
                        text_.substr(pos_, 2) == "||")) {
        break;
      }
      ++pos_;
    }
    const std::string atom = Trim(text_.substr(begin, pos_ - begin));
    if (atom.empty() || quote != 0 || in_regex) {
      SetError(error, atom.empty() ? "expected context flag"
                                   : "unterminated literal");
      return std::nullopt;
    }
    return context_ ? KeyContextValue(*context_, atom) : true;
  }

  std::string_view text_;
#if defined(CMUX_KEYMAP_CHROMIUM_BUILD)
  raw_ptr<const KeyContext> context_ = nullptr;
#else
  const KeyContext* context_ = nullptr;
#endif
  size_t pos_ = 0;
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view text) : text_(text) {}

  bool ParseConfig(KeymapLoadResult* result) {
    SkipSpaces();
    if (!AtEnd() && text_[pos_] == '{') {
      const bool ok = ParseConfigObject(result) && !syntax_error_;
      result->valid = ok;
      return ok;
    }
    result->warnings.push_back("cmux config must be a JSON object");
    result->valid = false;
    return false;
  }

 private:
  bool ParseRuleArray(KeymapLoadResult* result, bool require_end) {
    if (!Consume('[')) {
      result->warnings.push_back("bindings must be a JSON array");
      return false;
    }
    SkipSpaces();
    if (Consume(']')) {
      SkipSpaces();
      if (require_end && !AtEnd()) {
        result->warnings.push_back("unexpected content after keybindings");
        return false;
      }
      return true;
    }
    int index = 0;
    while (true) {
      std::vector<std::pair<std::string, std::string>> object;
      if (!ParseObject(&object)) {
        result->warnings.push_back("malformed rule at index " +
                                   std::to_string(index));
        return false;
      }
      AddObjectRule(object, index, result);
      ++index;
      SkipSpaces();
      if (Consume(',')) {
        SkipSpaces();
        if (Consume(']')) {
          SkipSpaces();
          return !require_end || AtEnd();
        }
        continue;
      }
      if (Consume(']')) {
        SkipSpaces();
        if (require_end && !AtEnd()) {
          result->warnings.push_back("unexpected content after keybindings");
          return false;
        }
        return true;
      }
      result->warnings.push_back("expected comma or ] after rule at index " +
                                 std::to_string(index - 1));
      return false;
    }
  }

  bool ParseConfigObject(KeymapLoadResult* result) {
    if (!Consume('{')) {
      return false;
    }
    SkipSpaces();
    if (Consume('}')) {
      return true;
    }
    while (true) {
      std::string field;
      if (!ParseString(&field) || !Consume(':')) {
        result->warnings.push_back("malformed keybindings config object");
        return false;
      }
      if (field == "keyboard") {
        std::vector<std::pair<std::string, std::string>> keyboard;
        if (!ParseObject(&keyboard)) {
          result->warnings.push_back("keyboard must be an object");
          return false;
        }
        if (std::optional<std::string> value =
                FindString(keyboard, "modifierScheme")) {
          std::optional<ShortcutModifierScheme> scheme =
              ParseShortcutModifierScheme(*value);
          if (scheme) {
            result->modifier_scheme = *scheme;
          } else {
            result->warnings.push_back("unknown keyboard.modifierScheme '" +
                                       *value + "'");
          }
        }
      } else if (field == "keybindings") {
        if (!ParseRuleArray(result, /*require_end=*/false)) {
          return false;
        }
      } else if (!SkipValue()) {
        result->warnings.push_back("invalid value for config field '" + field +
                                   "'");
        return false;
      }
      SkipSpaces();
      if (Consume(',')) {
        SkipSpaces();
        if (Consume('}')) {
          SkipSpaces();
          return AtEnd();
        }
        continue;
      }
      if (!Consume('}')) {
        result->warnings.push_back(
            "expected comma or } after keybindings config field");
        return false;
      }
      SkipSpaces();
      if (!AtEnd()) {
        result->warnings.push_back("unexpected content after keybindings");
        return false;
      }
      return true;
    }
  }

  bool AtEnd() const { return pos_ >= text_.size(); }

  void SkipSpaces() {
    while (!AtEnd()) {
      if (IsSpace(text_[pos_])) {
        ++pos_;
        continue;
      }
      if (text_.substr(pos_, 2) == "//") {
        pos_ += 2;
        while (!AtEnd() && text_[pos_] != '\n') {
          ++pos_;
        }
        continue;
      }
      if (text_.substr(pos_, 2) == "/*") {
        const size_t end = text_.find("*/", pos_ + 2);
        if (end == std::string_view::npos) {
          syntax_error_ = true;
          pos_ = text_.size();
          return;
        }
        pos_ = end + 2;
        continue;
      }
      break;
    }
  }

  bool Consume(char c) {
    SkipSpaces();
    if (AtEnd() || text_[pos_] != c) {
      return false;
    }
    ++pos_;
    return true;
  }

  bool ParseObject(std::vector<std::pair<std::string, std::string>>* object) {
    if (!Consume('{')) {
      return false;
    }
    SkipSpaces();
    if (Consume('}')) {
      return true;
    }
    while (true) {
      std::string key;
      if (!ParseString(&key) || !Consume(':')) {
        return false;
      }
      SkipSpaces();
      if (!AtEnd() && text_[pos_] == '"') {
        const size_t value_begin = pos_;
        std::string value;
        if (!ParseString(&value)) {
          return false;
        }
        if (key == "args") {
          value = std::string(text_.substr(value_begin, pos_ - value_begin));
        }
        object->push_back(std::make_pair(std::move(key), std::move(value)));
      } else {
        const size_t value_begin = pos_;
        if (!SkipValue()) {
          return false;
        }
        object->push_back(std::make_pair(
            std::move(key), Trim(text_.substr(value_begin, pos_ - value_begin))));
      }
      SkipSpaces();
      if (Consume(',')) {
        SkipSpaces();
        if (Consume('}')) {
          return true;
        }
        continue;
      }
      if (Consume('}')) {
        return true;
      }
      return false;
    }
  }

  bool ParseString(std::string* out) {
    if (!Consume('"')) {
      return false;
    }
    std::string value;
    while (!AtEnd()) {
      char c = text_[pos_++];
      if (c == '"') {
        *out = std::move(value);
        return true;
      }
      if (c != '\\') {
        value.push_back(c);
        continue;
      }
      if (AtEnd()) {
        return false;
      }
      const char escaped = text_[pos_++];
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          value.push_back(escaped);
          break;
        case 'b':
          value.push_back('\b');
          break;
        case 'f':
          value.push_back('\f');
          break;
        case 'n':
          value.push_back('\n');
          break;
        case 'r':
          value.push_back('\r');
          break;
        case 't':
          value.push_back('\t');
          break;
        case 'u':
          for (int i = 0; i < 4; ++i) {
            if (AtEnd() ||
                (!IsDigit(text_[pos_]) && !(LowerAscii(text_[pos_]) >= 'a' &&
                                            LowerAscii(text_[pos_]) <= 'f'))) {
              return false;
            }
            ++pos_;
          }
          value.push_back('?');
          break;
        default:
          return false;
      }
    }
    return false;
  }

  bool SkipValue() {
    SkipSpaces();
    if (AtEnd()) {
      return false;
    }
    if (text_[pos_] == '"') {
      std::string ignored;
      return ParseString(&ignored);
    }
    if (text_[pos_] == '{') {
      std::vector<std::pair<std::string, std::string>> ignored;
      return ParseObject(&ignored);
    }
    if (text_[pos_] == '[') {
      ++pos_;
      SkipSpaces();
      if (Consume(']')) {
        return true;
      }
      while (true) {
        if (!SkipValue()) {
          return false;
        }
        if (Consume(',')) {
          SkipSpaces();
          if (Consume(']')) {
            return true;
          }
          continue;
        }
        return Consume(']');
      }
    }
    constexpr std::string_view literals[] = {"true", "false", "null"};
    for (std::string_view literal : literals) {
      if (text_.substr(pos_, literal.size()) == literal) {
        pos_ += literal.size();
        return true;
      }
    }
    const size_t begin = pos_;
    if (text_[pos_] == '-') {
      ++pos_;
    }
    const size_t integer_begin = pos_;
    while (!AtEnd() && IsDigit(text_[pos_])) {
      ++pos_;
    }
    if (integer_begin == pos_) {
      pos_ = begin;
      return false;
    }
    if (!AtEnd() && text_[pos_] == '.') {
      ++pos_;
      const size_t fraction_begin = pos_;
      while (!AtEnd() && IsDigit(text_[pos_])) {
        ++pos_;
      }
      if (fraction_begin == pos_) {
        pos_ = begin;
        return false;
      }
    }
    if (!AtEnd() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (!AtEnd() && (text_[pos_] == '+' || text_[pos_] == '-')) {
        ++pos_;
      }
      const size_t exponent_begin = pos_;
      while (!AtEnd() && IsDigit(text_[pos_])) {
        ++pos_;
      }
      if (exponent_begin == pos_) {
        pos_ = begin;
        return false;
      }
    }
    return true;
  }

  static std::optional<std::string> FindString(
      const std::vector<std::pair<std::string, std::string>>& object,
      std::string_view field) {
    for (const auto& item : object) {
      if (item.first == field) {
        return item.second;
      }
    }
    return std::nullopt;
  }

  static void AddObjectRule(
      const std::vector<std::pair<std::string, std::string>>& object,
      int index,
      KeymapLoadResult* result) {
    std::optional<std::string> key = FindString(object, "key");
    std::optional<std::string> command = FindString(object, "command");
    if (!key || !command) {
      result->warnings.push_back("rule " + std::to_string(index) +
                                 " missing key or command");
      return;
    }
    std::string error;
    std::optional<std::vector<KeyChord>> sequence =
        ParseKeySequence(*key, &error);
    if (!sequence || sequence->empty()) {
      result->warnings.push_back("rule " + std::to_string(index) +
                                 " has invalid key '" + *key + "': " + error);
      return;
    }
    KeyRule rule;
    rule.sequence = *sequence;
    rule.chord = rule.sequence.front();
    rule.command = *command;
    if (std::optional<std::string> args = FindString(object, "args")) {
      rule.args_json = *args;
    }
    if (std::optional<std::string> when = FindString(object, "when")) {
      rule.when_text = *when;
      std::optional<WhenExpression> expression = ParseWhen(*when, &error);
      if (!expression) {
        result->warnings.push_back("rule " + std::to_string(index) +
                                   " has invalid when '" + *when +
                                   "': " + error);
        return;
      }
      if (!expression->empty()) {
        rule.when = *expression;
      }
    }
    result->rules.push_back(rule);
  }

  std::string_view text_;
  size_t pos_ = 0;
  bool syntax_error_ = false;
};

}  // namespace

std::string_view ShortcutModifierSchemeToString(
    ShortcutModifierScheme scheme) {
  switch (scheme) {
    case ShortcutModifierScheme::kCommandTabsControlWorkspaces:
      return "command-tabs-control-workspaces";
    case ShortcutModifierScheme::kControlTabsCommandWorkspaces:
      return "control-tabs-command-workspaces";
  }
  return "command-tabs-control-workspaces";
}

std::optional<ShortcutModifierScheme> ParseShortcutModifierScheme(
    std::string_view text) {
  const std::string value = LowerAscii(Trim(text));
  if (value == "command-tabs-control-workspaces" ||
      value == "cmd-tabs-ctrl-workspaces") {
    return ShortcutModifierScheme::kCommandTabsControlWorkspaces;
  }
  if (value == "control-tabs-command-workspaces" ||
      value == "ctrl-tabs-cmd-workspaces") {
    return ShortcutModifierScheme::kControlTabsCommandWorkspaces;
  }
  return std::nullopt;
}

KeyChord::KeyChord() = default;
KeyChord::KeyChord(const KeyChord&) = default;
KeyChord& KeyChord::operator=(const KeyChord&) = default;
KeyChord::~KeyChord() = default;

KeyContext::KeyContext() = default;
KeyContext::KeyContext(const KeyContext&) = default;
KeyContext& KeyContext::operator=(const KeyContext&) = default;
KeyContext::~KeyContext() = default;

WhenTerm::WhenTerm() = default;
WhenTerm::WhenTerm(const WhenTerm&) = default;
WhenTerm& WhenTerm::operator=(const WhenTerm&) = default;
WhenTerm::~WhenTerm() = default;

WhenConjunction::WhenConjunction() = default;
WhenConjunction::WhenConjunction(const WhenConjunction&) = default;
WhenConjunction& WhenConjunction::operator=(const WhenConjunction&) = default;
WhenConjunction::~WhenConjunction() = default;

WhenExpression::WhenExpression() = default;
WhenExpression::WhenExpression(const WhenExpression&) = default;
WhenExpression& WhenExpression::operator=(const WhenExpression&) = default;
WhenExpression::~WhenExpression() = default;

KeyRule::KeyRule() = default;
KeyRule::KeyRule(const KeyRule&) = default;
KeyRule& KeyRule::operator=(const KeyRule&) = default;
KeyRule::~KeyRule() = default;

KeymapLoadResult::KeymapLoadResult() = default;
KeymapLoadResult::KeymapLoadResult(const KeymapLoadResult&) = default;
KeymapLoadResult& KeymapLoadResult::operator=(const KeymapLoadResult&) =
    default;
KeymapLoadResult::~KeymapLoadResult() = default;

ResolvedKeybinding::ResolvedKeybinding() = default;
ResolvedKeybinding::ResolvedKeybinding(std::string command,
                                       std::string args_json)
    : command(std::move(command)), args_json(std::move(args_json)) {}
ResolvedKeybinding::ResolvedKeybinding(const ResolvedKeybinding&) = default;
ResolvedKeybinding& ResolvedKeybinding::operator=(
    const ResolvedKeybinding&) = default;
ResolvedKeybinding::~ResolvedKeybinding() = default;

Keymap::Keymap() = default;
Keymap::Keymap(std::vector<KeyRule> rules) : rules_(std::move(rules)) {}
Keymap::Keymap(const Keymap&) = default;
Keymap& Keymap::operator=(const Keymap&) = default;
Keymap::~Keymap() = default;

void Keymap::AddRule(const KeyRule& rule) {
  rules_.push_back(rule);
}

void Keymap::AppendRules(const std::vector<KeyRule>& rules) {
  for (const KeyRule& rule : rules) {
    AddRule(rule);
  }
}

std::optional<std::string> Keymap::Resolve(const KeyChord& pressed,
                                           const KeyContext& context,
                                           bool is_mac) const {
  std::optional<ResolvedKeybinding> binding =
      ResolveSequence({pressed}, context, is_mac);
  return binding && !binding->command.empty()
             ? std::optional<std::string>(binding->command)
             : std::nullopt;
}

std::optional<ResolvedKeybinding> Keymap::ResolveSequence(
    const std::vector<KeyChord>& pressed,
    const KeyContext& context,
    bool is_mac) const {
  std::vector<std::string> removed_commands;
  for (auto it = rules_.rbegin(); it != rules_.rend(); ++it) {
    const KeyRule& rule = *it;
    const std::vector<KeyChord> fallback{rule.chord};
    const std::vector<KeyChord>& sequence =
        rule.sequence.empty() ? fallback : rule.sequence;
    if (sequence.size() != pressed.size()) {
      continue;
    }
    bool keys_match = true;
    for (size_t i = 0; i < sequence.size(); ++i) {
      keys_match = keys_match && ChordsEqual(sequence[i], pressed[i], is_mac);
    }
    if (!keys_match) {
      continue;
    }
    if (rule.when && !EvalWhen(*rule.when, context)) {
      continue;
    }
    if (rule.command.size() > 1 && rule.command[0] == '-') {
      removed_commands.push_back(rule.command.substr(1));
      continue;
    }
    if (std::find(removed_commands.begin(), removed_commands.end(),
                  rule.command) != removed_commands.end()) {
      continue;
    }
    if (IsUnbindCommand(rule.command)) {
      return ResolvedKeybinding{"", rule.args_json};
    }
    return ResolvedKeybinding{rule.command, rule.args_json};
  }
  return std::nullopt;
}

bool Keymap::HasChordPrefix(const std::vector<KeyChord>& pressed,
                            const KeyContext& context,
                            bool is_mac) const {
  for (auto it = rules_.rbegin(); it != rules_.rend(); ++it) {
    const KeyRule& rule = *it;
    const std::vector<KeyChord> fallback{rule.chord};
    const std::vector<KeyChord>& sequence =
        rule.sequence.empty() ? fallback : rule.sequence;
    if (sequence.size() <= pressed.size() ||
        (rule.when && !EvalWhen(*rule.when, context))) {
      continue;
    }
    bool matches = true;
    for (size_t i = 0; i < pressed.size(); ++i) {
      matches = matches && ChordsEqual(sequence[i], pressed[i], is_mac);
    }
    if (matches && ResolveSequence(sequence, context, is_mac).has_value()) {
      return true;
    }
  }
  return false;
}

std::optional<std::vector<KeyChord>> Keymap::PrimaryBindingForCommand(
    std::string_view command,
    const KeyContext& context,
    bool is_mac) const {
  for (auto it = rules_.rbegin(); it != rules_.rend(); ++it) {
    const KeyRule& rule = *it;
    if (rule.command != command) {
      continue;
    }
    const std::vector<KeyChord> sequence =
        rule.sequence.empty() ? std::vector<KeyChord>{rule.chord}
                              : rule.sequence;
    std::optional<ResolvedKeybinding> resolved =
        ResolveSequence(sequence, context, is_mac);
    if (resolved && resolved->command == command) {
      return sequence;
    }
  }
  return std::nullopt;
}

std::optional<std::string> NormalizeKeyToken(std::string_view token,
                                             std::string* error) {
  std::string key = LowerAscii(Trim(token));
  if (key.empty()) {
    SetError(error, "missing key token");
    return std::nullopt;
  }
  if (key == "\t") {
    return std::string("tab");
  }
  if (key == "\r" || key == "\n") {
    return std::string("enter");
  }
  if (key.size() == 1 && key[0] == 27) {
    return std::string("escape");
  }
  // VS Code scan-code spelling. cmux's event adapters already report the
  // corresponding physical virtual key, so normalize both forms together.
  if (key.size() > 2 && key.front() == '[' && key.back() == ']') {
    const std::string scan = key.substr(1, key.size() - 2);
    if (scan.size() == 4 && scan.substr(0, 3) == "key" &&
        scan[3] >= 'a' && scan[3] <= 'z') {
      return std::string(1, scan[3]);
    }
    if (scan.size() == 6 && scan.substr(0, 5) == "digit" &&
        IsDigit(scan[5])) {
      return std::string(1, scan[5]);
    }
    const std::pair<std::string_view, std::string_view> scans[] = {
        {"slash", "/"},       {"backslash", "\\"},
        {"semicolon", ";"},   {"quote", "'"},
        {"comma", ","},       {"period", "."},
        {"minus", "-"},       {"equal", "="},
        {"backquote", "`"},   {"bracketleft", "["},
        {"bracketright", "]"}, {"arrowleft", "left"},
        {"arrowright", "right"}, {"arrowup", "up"},
        {"arrowdown", "down"}, {"tab", "tab"},
        {"enter", "enter"},   {"escape", "escape"},
        {"space", "space"},   {"backspace", "backspace"},
        {"delete", "delete"}};
    for (const auto& [name, normalized] : scans) {
      if (scan == name) {
        return std::string(normalized);
      }
    }
  }
  if (key.size() == 1) {
    char c = key[0];
    if (std::optional<char> unshifted = UnshiftedAscii(c)) {
      c = *unshifted;
    }
    c = LowerAscii(c);
    if (IsSupportedSingleCharKey(c)) {
      if (c == ' ') {
        return std::string("space");
      }
      return std::string(1, c);
    }
  }
  if (key == "esc") {
    return std::string("escape");
  }
  if (key == "return") {
    return std::string("enter");
  }
  if (key == "arrowleft") {
    return std::string("left");
  }
  if (key == "arrowright") {
    return std::string("right");
  }
  if (key == "arrowup") {
    return std::string("up");
  }
  if (key == "arrowdown") {
    return std::string("down");
  }
  if (key == "plus") {
    return std::string("=");
  }
  if (key == "minus") {
    return std::string("-");
  }
  if (key == "comma") {
    return std::string(",");
  }
  if (key == "period") {
    return std::string(".");
  }
  if (key == "slash") {
    return std::string("/");
  }
  if (key == "semicolon") {
    return std::string(";");
  }
  if (key == "quote") {
    return std::string("'");
  }
  if (key == "backslash") {
    return std::string("\\");
  }
  if (key == "backquote") {
    return std::string("`");
  }
  if (key == "left" || key == "right" || key == "up" || key == "down" ||
      key == "tab" || key == "escape" || key == "enter" || key == "space" ||
      key == "backspace" || key == "delete" || key == "home" || key == "end" ||
      key == "pageup" || key == "pagedown") {
    return key;
  }
  if (key.size() >= 2 && key[0] == 'f') {
    int fkey = 0;
    if (ParsePositiveInt(std::string_view(key).substr(1), &fkey) && fkey >= 1 &&
        fkey <= 24) {
      return key;
    }
  }
  SetError(error, "unsupported key token");
  return std::nullopt;
}

std::optional<KeyChord> ParseChord(std::string_view text, std::string* error) {
  std::vector<std::string_view> parts;
  size_t begin = 0;
  while (begin <= text.size()) {
    const size_t plus = text.find('+', begin);
    if (plus == std::string_view::npos) {
      parts.push_back(text.substr(begin));
      break;
    }
    parts.push_back(text.substr(begin, plus - begin));
    begin = plus + 1;
  }
  if (parts.empty()) {
    SetError(error, "empty chord");
    return std::nullopt;
  }
  KeyChord chord;
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    std::optional<int> modifier = ModifierForToken(parts[i]);
    if (!modifier) {
      SetError(error, "unknown modifier '" + Trim(parts[i]) + "'");
      return std::nullopt;
    }
    if ((chord.modifiers & *modifier) != 0) {
      SetError(error, "duplicate modifier '" + Trim(parts[i]) + "'");
      return std::nullopt;
    }
    chord.modifiers |= *modifier;
  }
  const std::string raw_key = LowerAscii(Trim(parts.back()));
  std::optional<std::string> key = NormalizeKeyToken(parts.back(), error);
  if (!key) {
    return std::nullopt;
  }
  if (raw_key == "plus") {
    chord.modifiers |= kKeyModShift;
  }
  chord.key = *key;
  return chord;
}

std::optional<std::vector<KeyChord>> ParseKeySequence(std::string_view text,
                                                       std::string* error) {
  std::vector<KeyChord> sequence;
  size_t pos = 0;
  while (pos < text.size()) {
    while (pos < text.size() && IsSpace(text[pos])) {
      ++pos;
    }
    if (pos == text.size()) {
      break;
    }
    size_t end = pos;
    while (end < text.size() && !IsSpace(text[end])) {
      ++end;
    }
    std::optional<KeyChord> chord = ParseChord(text.substr(pos, end - pos), error);
    if (!chord) {
      return std::nullopt;
    }
    sequence.push_back(*chord);
    pos = end;
  }
  if (sequence.empty()) {
    SetError(error, "empty key sequence");
    return std::nullopt;
  }
  if (sequence.size() > 2) {
    SetError(error, "key sequences may contain at most two chords");
    return std::nullopt;
  }
  return sequence;
}

std::optional<WhenExpression> ParseWhen(std::string_view text,
                                        std::string* error) {
  WhenParser parser(text);
  return parser.Parse(error);
}

bool EvalWhen(const WhenExpression& expression, const KeyContext& context) {
  if (expression.empty()) {
    return true;
  }
  if (!expression.source.empty()) {
    WhenParser parser(expression.source, &context);
    return parser.Evaluate(/*error=*/nullptr).value_or(false);
  }
  for (const WhenConjunction& conjunction : expression.clauses) {
    bool conjunction_value = true;
    for (const WhenTerm& term : conjunction.terms) {
      bool value = KeyContextValue(context, term.flag);
      if (term.negated) {
        value = !value;
      }
      conjunction_value = conjunction_value && value;
    }
    if (conjunction_value) {
      return true;
    }
  }
  return false;
}

bool KeyContextValue(const KeyContext& context, std::string_view flag) {
  const std::string expression = Trim(flag);
  auto bool_value = [&](std::string_view key) -> std::optional<bool> {
    if (key == "omniboxFocused" || key == "omniboxFocus") {
      return context.omnibox_focused;
    }
    if (key == "terminalFocused" || key == "terminalFocus") {
      return context.terminal_focused;
    }
    if (key == "webFocused" || key == "webFocus") {
      return context.web_focused;
    }
    if (key == "dragActive") {
      return context.drag_active;
    }
    if (key == "sidebarVisible") {
      return context.sidebar_visible;
    }
    if (key == "textInputFocus") {
      return context.text_input_focused;
    }
    if (key == "isMac") {
      return context.is_mac;
    }
    if (key == "isLinux") {
      return context.is_linux;
    }
    if (key == "isWindows") {
      return context.is_windows;
    }
    if (key == "paneCountGt1") {
      return context.pane_count_gt_one;
    }
    return std::nullopt;
  };
  auto number_value = [&](std::string_view key) -> std::optional<int> {
    if (key == "paneCount") {
      return context.pane_count;
    }
    if (key == "tabCount") {
      return context.tab_count;
    }
    if (key == "workspaceCount") {
      return context.workspace_count;
    }
    return std::nullopt;
  };
  auto unquote = [](std::string value) {
    if (value.size() >= 2 &&
        ((value.front() == '\'' && value.back() == '\'') ||
         (value.front() == '"' && value.back() == '"'))) {
      return value.substr(1, value.size() - 2);
    }
    return value;
  };
  struct Operator {
    std::string_view token;
  };
  constexpr Operator operators[] = {{"!=="}, {"==="}, {"!="}, {"=="},
                                    {">="},  {"<="},  {"=~"}, {">"},
                                    {"<"}};
  for (const Operator& op : operators) {
    const size_t at = expression.find(op.token);
    if (at == std::string::npos) {
      continue;
    }
    const std::string lhs = Trim(expression.substr(0, at));
    const std::string rhs =
        unquote(Trim(expression.substr(at + op.token.size())));
    if (std::optional<int> left = number_value(lhs)) {
      int right = 0;
      if (!ParsePositiveInt(rhs, &right)) {
        return false;
      }
      if (op.token == ">") return *left > right;
      if (op.token == ">=") return *left >= right;
      if (op.token == "<") return *left < right;
      if (op.token == "<=") return *left <= right;
      if (op.token == "==" || op.token == "===") return *left == right;
      if (op.token == "!=" || op.token == "!==") return *left != right;
      return false;
    }
    std::string left_string;
    if (lhs == "surfaceKind") {
      left_string = context.surface_kind;
    } else if (std::optional<bool> left = bool_value(lhs)) {
      left_string = *left ? "true" : "false";
    } else {
      return false;
    }
    if (op.token == "==" || op.token == "===") {
      return left_string == rhs;
    }
    if (op.token == "!=" || op.token == "!==") {
      return left_string != rhs;
    }
    if (op.token == "=~") {
      std::string pattern = rhs;
      if (pattern.size() >= 2 && pattern.front() == '/' &&
          pattern.rfind('/') > 0) {
        pattern = pattern.substr(1, pattern.rfind('/') - 1);
      }
#if defined(CMUX_KEYMAP_CHROMIUM_BUILD)
      const re2::RE2 regex(pattern);
      return regex.ok() && re2::RE2::PartialMatch(left_string, regex);
#else
      try {
        return std::regex_search(left_string, std::regex(pattern));
      } catch (const std::regex_error&) {
        return false;
      }
#endif
    }
    return false;
  }
  if (std::optional<bool> value = bool_value(expression)) {
    return *value;
  }
  if (expression == "paneCount>1" || expression == "paneCount>1?") {
    return context.pane_count_gt_one;
  }
  return false;
}

int ResolveKeyModifiers(int modifiers, bool is_mac) {
  int resolved = modifiers;
  if ((resolved & kKeyModPrimary) != 0) {
    resolved &= ~kKeyModPrimary;
    resolved |= is_mac ? kKeyModCmd : kKeyModCtrl;
  }
  return resolved;
}

bool ChordsEqual(const KeyChord& rule_chord,
                 const KeyChord& pressed_chord,
                 bool is_mac) {
  return rule_chord.key == pressed_chord.key &&
         ResolveKeyModifiers(rule_chord.modifiers, is_mac) ==
             ResolveKeyModifiers(pressed_chord.modifiers, is_mac);
}

std::string FormatChord(const KeyChord& chord, bool is_mac) {
  std::vector<std::string> parts;
  const int resolved = ResolveKeyModifiers(chord.modifiers, is_mac);
  if ((resolved & kKeyModCmd) != 0) {
    parts.push_back("cmd");
  }
  if ((resolved & kKeyModCtrl) != 0) {
    parts.push_back("ctrl");
  }
  if ((resolved & kKeyModAlt) != 0) {
    parts.push_back("alt");
  }
  if ((resolved & kKeyModShift) != 0) {
    parts.push_back("shift");
  }
  parts.push_back(chord.key);
  std::string out;
  for (const std::string& part : parts) {
    if (!out.empty()) {
      out += "+";
    }
    out += part;
  }
  return out;
}

std::string FormatKeySequence(const std::vector<KeyChord>& sequence,
                              bool is_mac) {
  std::string out;
  for (const KeyChord& chord : sequence) {
    if (!out.empty()) {
      out += " ";
    }
    out += FormatChord(chord, is_mac);
  }
  return out;
}

bool IsUnbindCommand(std::string_view command) {
  return command.empty() || command == "-" || command == "unbind";
}

std::vector<KeyRule> DefaultKeymapRules(bool is_mac,
                                        ShortcutModifierScheme scheme) {
  std::vector<KeyRule> rules;
  // Linux and Windows keep their established Ctrl defaults. macOS has two
  // intentionally simple modes:
  //   Chrome-first: Command owns tabs and Option owns workspaces.
  //   cmux-first:   Control owns tabs and Command owns workspaces.
  // Page, pane, column, and app commands retain their native Command
  // shortcuts. Cmd+T is a browser invariant in both modes.
  const std::string_view tab_modifier =
      is_mac &&
              scheme ==
                  ShortcutModifierScheme::kControlTabsCommandWorkspaces
          ? "ctrl"
          : "mod";
  const std::string_view workspace_modifier =
      is_mac &&
              scheme ==
                  ShortcutModifierScheme::kCommandTabsControlWorkspaces
          ? "alt"
          : "mod";

  AddDefault(&rules, "mod+b", "sidebar.toggle");
  AddDefault(&rules, "mod+z", "edit.undo");
  AddDefault(&rules, "mod+shift+z", "edit.redo");
  AddDefault(&rules, "mod+x", "edit.cut");
  AddDefault(&rules, "mod+c", "edit.copy");
  AddDefault(&rules, "mod+v", "edit.paste");
  AddDefault(&rules, "mod+a", "edit.selectAll");
  AddDefault(&rules, "mod+shift+n", "window.new");
  AddDefault(&rules, "mod+shift+w", "window.close");
  AddDefault(&rules, "mod+shift+enter", "window.toggleFullscreen");
  AddDefault(&rules, WithModifier(tab_modifier, "t"), "tab.newWeb");
  if (is_mac && tab_modifier != "mod") {
    AddDefault(&rules, "cmd+t", "tab.newWeb");
  }
  // Keep the browser-native reopen shortcut available. Terminal creation is
  // deliberately adjacent to Cmd+T on macOS without taking over Cmd+Shift+T.
  if (is_mac) {
    AddDefault(&rules, "cmd+shift+t", "tab.restore");
    AddDefault(&rules, "cmd+ctrl+t", "tab.newTerminal");
  } else {
    AddDefault(&rules, "ctrl+shift+t", "tab.restore");
    AddDefault(&rules, "ctrl+alt+t", "tab.newTerminal");
  }
  AddDefault(&rules, "mod+d", "pane.splitRight");
  AddDefault(&rules, "mod+shift+d", "pane.splitDown");
  AddDefault(&rules, "mod+n", "column.new");
  AddDefault(&rules, WithModifier(workspace_modifier, "shift+m"),
             "workspace.toggleLayout");
  AddDefault(&rules, WithModifier(tab_modifier, "w"), "tab.close");
  AddDefault(&rules, "mod+l", "omnibox.focus");
  AddDefault(&rules, "mod+r", "page.reload");
  AddDefault(&rules, "mod+[", "page.back");
  AddDefault(&rules, "mod+]", "page.forward");
  AddDefault(&rules, "mod+=", "zoom.in", "!terminalFocused");
  AddDefault(&rules, "mod+plus", "zoom.in", "!terminalFocused");
  AddDefault(&rules, "mod+-", "zoom.out", "!terminalFocused");
  AddDefault(&rules, "mod+0", "zoom.reset", "!terminalFocused");
  AddDefault(&rules, WithModifier(tab_modifier, "shift+["), "tab.prev");
  AddDefault(&rules, WithModifier(tab_modifier, "shift+]"), "tab.next");
  AddDefault(&rules, "alt+n", "workspace.new");

  // Ctrl+Tab is a platform convention on all desktops. In particular, never
  // synthesize Cmd+Tab on macOS: the OS owns it for application switching.
  AddDefault(&rules, "ctrl+tab", "tab.next");
  AddDefault(&rules, "ctrl+shift+tab", "tab.prev");

  for (int i = 1; i <= 9; ++i) {
    AddDefault(&rules,
               WithModifier(tab_modifier,
                            std::string(1, static_cast<char>('0' + i))),
               std::string("tab.jump") + static_cast<char>('0' + i));
    AddDefault(&rules,
               WithModifier(workspace_modifier,
                            std::string(1, static_cast<char>('0' + i))),
               std::string("workspace.jump") + static_cast<char>('0' + i));
  }
  // Option is already the complete workspace namespace in Chrome-first mode;
  // do not generate the invalid duplicate chord `alt+alt+[`.
  const bool option_workspaces =
      is_mac &&
      scheme == ShortcutModifierScheme::kCommandTabsControlWorkspaces;
  AddDefault(&rules,
             WithModifier(workspace_modifier,
                          option_workspaces ? "[" : "alt+["),
             "workspace.prev");
  AddDefault(&rules,
             WithModifier(workspace_modifier,
                          option_workspaces ? "]" : "alt+]"),
             "workspace.next");

  if (is_mac) {
    AddDefault(&rules, "cmd+ctrl+r", "column.cycleWidth");
    AddDefault(&rules, "cmd+ctrl+h", "pane.focusLeft");
    AddDefault(&rules, "cmd+ctrl+l", "pane.focusRight");
    AddDefault(&rules, "cmd+ctrl+k", "pane.focusUp");
    AddDefault(&rules, "cmd+ctrl+j", "pane.focusDown");
    AddDefault(&rules, "cmd+alt+left", "pane.focusLeft");
    AddDefault(&rules, "cmd+alt+right", "pane.focusRight");
    AddDefault(&rules, "cmd+alt+up", "pane.focusUp");
    AddDefault(&rules, "cmd+alt+down", "pane.focusDown");
    AddDefault(&rules, "cmd+alt+i", "devtools.toggle");
  } else {
    AddDefault(&rules, "ctrl+alt+r", "column.cycleWidth");
    AddDefault(&rules, "ctrl+left", "pane.focusLeft");
    AddDefault(&rules, "ctrl+right", "pane.focusRight");
    AddDefault(&rules, "ctrl+up", "pane.focusUp");
    AddDefault(&rules, "ctrl+down", "pane.focusDown");
  }

  AddDefault(&rules, "escape", "omnibox.escape",
             "omniboxFocused || dragActive");
  return rules;
}

Keymap DefaultKeymap(bool is_mac) {
  return DefaultKeymap(is_mac, kDefaultShortcutModifierScheme);
}

std::vector<KeyRule> DefaultKeymapRules(bool is_mac) {
  return DefaultKeymapRules(is_mac, kDefaultShortcutModifierScheme);
}

Keymap DefaultKeymap(bool is_mac, ShortcutModifierScheme scheme) {
  return Keymap(DefaultKeymapRules(is_mac, scheme));
}

KeymapLoadResult ParseKeymapJson(std::string_view json) {
  KeymapLoadResult result;
  result.file_found = true;
  JsonParser parser(json);
  parser.ParseConfig(&result);
  return result;
}

std::string SerializeKeymapConfig(ShortcutModifierScheme scheme,
                                  const std::vector<KeyRule>& rules) {
  std::string out = "{\n  \"keyboard\": {\n    \"modifierScheme\": \"";
  out += ShortcutModifierSchemeToString(scheme);
  out += "\"\n  },\n  \"keybindings\": [";
  for (size_t i = 0; i < rules.size(); ++i) {
    const KeyRule& rule = rules[i];
    out += i == 0 ? "\n" : ",\n";
    out += "    {\"key\": \"";
    const std::vector<KeyChord> fallback{rule.chord};
    const std::vector<KeyChord>& sequence =
        rule.sequence.empty() ? fallback : rule.sequence;
    std::string formatted_sequence;
    for (const KeyChord& chord : sequence) {
      if (!formatted_sequence.empty()) {
        formatted_sequence += " ";
      }
      formatted_sequence += FormatConfigChord(chord);
    }
    out += EscapeJson(formatted_sequence);
    out += "\", \"command\": \"";
    out += EscapeJson(rule.command);
    out += "\"";
    if (!rule.when_text.empty()) {
      out += ", \"when\": \"";
      out += EscapeJson(rule.when_text);
      out += "\"";
    }
    if (!rule.args_json.empty()) {
      out += ", \"args\": ";
      out += rule.args_json;
    }
    out += "}";
  }
  if (!rules.empty()) {
    out += "\n  ";
  }
  out += "]\n}\n";
  return out;
}

Keymap KeymapFromConfig(bool is_mac, const KeymapLoadResult& config) {
  Keymap keymap = DefaultKeymap(
      is_mac,
      config.modifier_scheme.value_or(kDefaultShortcutModifierScheme));
  keymap.AppendRules(config.rules);
  return keymap;
}

KeymapLoadResult LoadKeymapFile(const std::string& path) {
  KeymapLoadResult result;
  std::ifstream in(path);
  if (!in) {
    return result;
  }
  result.file_found = true;
  std::string json((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  KeymapLoadResult parsed = ParseKeymapJson(json);
  parsed.file_found = true;
  return parsed;
}

}  // namespace cmux
