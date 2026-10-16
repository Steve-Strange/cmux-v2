// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_KEYMAP_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_KEYMAP_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cmux {

// The two macOS shortcut namespaces users can choose between. The historical
// name of the first scheme is retained for config-file compatibility; its
// workspace modifier is Option (Chrome-first), not Control. This is kept in
// the pure keymap model so cmux://configure can read/write it without knowing
// about Cocoa events or Views accelerators.
enum class ShortcutModifierScheme {
  kCommandTabsControlWorkspaces,
  kControlTabsCommandWorkspaces,
};

inline constexpr ShortcutModifierScheme kDefaultShortcutModifierScheme =
    ShortcutModifierScheme::kCommandTabsControlWorkspaces;

std::string_view ShortcutModifierSchemeToString(
    ShortcutModifierScheme scheme);
std::optional<ShortcutModifierScheme> ParseShortcutModifierScheme(
    std::string_view text);

enum KeyModifier {
  kKeyModCmd = 1 << 0,
  kKeyModCtrl = 1 << 1,
  kKeyModAlt = 1 << 2,
  kKeyModShift = 1 << 3,
  // VSCode-style "mod": cmd on macOS, ctrl elsewhere. Kept separate until
  // binding resolution so user/default rules can stay platform-neutral.
  kKeyModPrimary = 1 << 4,
};

struct KeyChord {
  int modifiers = 0;
  std::string key;

  KeyChord();
  KeyChord(const KeyChord&);
  KeyChord& operator=(const KeyChord&);
  ~KeyChord();
};

struct KeyContext {
  bool omnibox_focused = false;
  bool terminal_focused = false;
  bool web_focused = false;
  bool drag_active = false;
  bool sidebar_visible = false;
  bool pane_count_gt_one = false;
  bool text_input_focused = false;
  bool is_mac = false;
  bool is_linux = false;
  bool is_windows = false;
  int pane_count = 0;
  int tab_count = 0;
  int workspace_count = 0;
  std::string surface_kind;

  KeyContext();
  KeyContext(const KeyContext&);
  KeyContext& operator=(const KeyContext&);
  ~KeyContext();
};

struct WhenTerm {
  std::string flag;
  bool negated = false;

  WhenTerm();
  WhenTerm(const WhenTerm&);
  WhenTerm& operator=(const WhenTerm&);
  ~WhenTerm();
};

struct WhenConjunction {
  std::vector<WhenTerm> terms;

  WhenConjunction();
  WhenConjunction(const WhenConjunction&);
  WhenConjunction& operator=(const WhenConjunction&);
  ~WhenConjunction();
};

struct WhenExpression {
  // Original VS Code-style expression. It is retained so grouping can be
  // evaluated without flattening away parentheses. Empty means "always true".
  std::string source;
  // Retained for source compatibility with the original simple parser.
  std::vector<WhenConjunction> clauses;

  WhenExpression();
  WhenExpression(const WhenExpression&);
  WhenExpression& operator=(const WhenExpression&);
  ~WhenExpression();

  bool empty() const { return source.empty() && clauses.empty(); }
};

struct KeyRule {
  // `chord` is the first stroke and remains available to accelerator
  // registration. `sequence` contains every stroke for VS Code-style chords.
  KeyChord chord;
  std::vector<KeyChord> sequence;
  std::string command;
  std::optional<WhenExpression> when;
  std::string when_text;
  // Canonical JSON for the optional VS Code `args` value. Kept opaque in the
  // pure keymap model and decoded by the command dispatcher.
  std::string args_json;

  KeyRule();
  KeyRule(const KeyRule&);
  KeyRule& operator=(const KeyRule&);
  ~KeyRule();
};

struct KeymapLoadResult {
  bool file_found = false;
  bool valid = true;
  std::optional<ShortcutModifierScheme> modifier_scheme;
  std::vector<KeyRule> rules;
  std::vector<std::string> warnings;

  KeymapLoadResult();
  KeymapLoadResult(const KeymapLoadResult&);
  KeymapLoadResult& operator=(const KeymapLoadResult&);
  ~KeymapLoadResult();
};

struct ResolvedKeybinding {
  std::string command;
  std::string args_json;

  ResolvedKeybinding();
  ResolvedKeybinding(std::string command, std::string args_json);
  ResolvedKeybinding(const ResolvedKeybinding&);
  ResolvedKeybinding& operator=(const ResolvedKeybinding&);
  ~ResolvedKeybinding();
};

class Keymap {
 public:
  Keymap();
  explicit Keymap(std::vector<KeyRule> rules);
  Keymap(const Keymap&);
  Keymap& operator=(const Keymap&);
  ~Keymap();

  const std::vector<KeyRule>& rules() const { return rules_; }
  void AddRule(const KeyRule& rule);
  void AppendRules(const std::vector<KeyRule>& rules);

  // Returns the resolved command id for `pressed`, or none. Rules are ordered
  // defaults first, user rules last; the last matching rule wins.
  std::optional<std::string> Resolve(const KeyChord& pressed,
                                     const KeyContext& context,
                                     bool is_mac) const;
  std::optional<ResolvedKeybinding> ResolveSequence(
      const std::vector<KeyChord>& pressed,
      const KeyContext& context,
      bool is_mac) const;
  bool HasChordPrefix(const std::vector<KeyChord>& pressed,
                      const KeyContext& context,
                      bool is_mac) const;
  std::optional<std::vector<KeyChord>> PrimaryBindingForCommand(
      std::string_view command,
      const KeyContext& context,
      bool is_mac) const;

 private:
  std::vector<KeyRule> rules_;
};

std::optional<std::string> NormalizeKeyToken(std::string_view token,
                                             std::string* error = nullptr);
std::optional<KeyChord> ParseChord(std::string_view text,
                                   std::string* error = nullptr);
std::optional<std::vector<KeyChord>> ParseKeySequence(
    std::string_view text,
    std::string* error = nullptr);

std::optional<WhenExpression> ParseWhen(std::string_view text,
                                        std::string* error = nullptr);
bool EvalWhen(const WhenExpression& expression, const KeyContext& context);
bool KeyContextValue(const KeyContext& context, std::string_view flag);

int ResolveKeyModifiers(int modifiers, bool is_mac);
bool ChordsEqual(const KeyChord& rule_chord,
                 const KeyChord& pressed_chord,
                 bool is_mac);
std::string FormatChord(const KeyChord& chord, bool is_mac);
std::string FormatKeySequence(const std::vector<KeyChord>& sequence,
                              bool is_mac);

bool IsUnbindCommand(std::string_view command);

std::vector<KeyRule> DefaultKeymapRules(bool is_mac);
std::vector<KeyRule> DefaultKeymapRules(bool is_mac,
                                        ShortcutModifierScheme scheme);
Keymap DefaultKeymap(bool is_mac);
Keymap DefaultKeymap(bool is_mac, ShortcutModifierScheme scheme);

// Parses the JSONC cmux configuration object. User rules live in the
// VS Code-compatible top-level `keybindings` array and the optional default
// modifier preference lives at `keyboard.modifierScheme`.
KeymapLoadResult ParseKeymapJson(std::string_view json);
std::string SerializeKeymapConfig(ShortcutModifierScheme scheme,
                                  const std::vector<KeyRule>& rules);
// Applies the selected default scheme first, then appends individual user
// rules so existing last-match-wins customization remains intact.
Keymap KeymapFromConfig(bool is_mac, const KeymapLoadResult& config);
KeymapLoadResult LoadKeymapFile(const std::string& path);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_KEYMAP_H_
