// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host-compilable unit test for the pure cmux keymap model (no Chromium,
// no gtest). Build + run:
//
//   c++ -std=c++17 -I overlay \
//     overlay/chrome/browser/cmux_term/cmux_keymap.cc \
//     overlay/chrome/browser/cmux_term/cmux_keymap_test.cc \
//     -o /tmp/keymap_test && /tmp/keymap_test

#include "chrome/browser/cmux_term/cmux_keymap.h"

#include <cstdio>
#include <fstream>
#include <optional>
#include <string>

using namespace cmux;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("  FAIL: %s\n", what);
  }
}

KeyChord MustParseChord(const std::string& text) {
  std::string error;
  std::optional<KeyChord> chord = ParseChord(text, &error);
  Check(chord.has_value(), ("parse chord " + text + ": " + error).c_str());
  return chord.value_or(KeyChord());
}

WhenExpression MustParseWhen(const std::string& text) {
  std::string error;
  std::optional<WhenExpression> expression = ParseWhen(text, &error);
  Check(expression.has_value(), ("parse when " + text + ": " + error).c_str());
  return expression.value_or(WhenExpression());
}

void TestChordParser() {
  KeyChord mod_t = MustParseChord("mod+t");
  Check(mod_t.modifiers == kKeyModPrimary && mod_t.key == "t",
        "mod+t parses as primary modifier");

  KeyChord mac_workspace = MustParseChord("cmd+ctrl+[");
  Check(mac_workspace.modifiers == (kKeyModCmd | kKeyModCtrl) &&
            mac_workspace.key == "[",
        "cmd+ctrl+[ parses");

  KeyChord shifted = MustParseChord("cmd+shift+{");
  Check(shifted.modifiers == (kKeyModCmd | kKeyModShift) && shifted.key == "[",
        "shifted punctuation normalizes");

  KeyChord zoom_equal = MustParseChord("mod+=");
  Check(zoom_equal.modifiers == kKeyModPrimary && zoom_equal.key == "=",
        "mod+= parses for zoom in");

  KeyChord zoom_plus = MustParseChord("mod+plus");
  Check(zoom_plus.modifiers == (kKeyModPrimary | kKeyModShift) &&
            zoom_plus.key == "=",
        "mod+plus parses as shifted equals");

  KeyChord zoom_minus = MustParseChord("mod+-");
  Check(zoom_minus.modifiers == kKeyModPrimary && zoom_minus.key == "-",
        "mod+- parses for zoom out");

  KeyChord zoom_reset = MustParseChord("mod+0");
  Check(zoom_reset.modifiers == kKeyModPrimary && zoom_reset.key == "0",
        "mod+0 parses for zoom reset");

  KeyChord tab = MustParseChord("ctrl+shift+tab");
  Check(tab.modifiers == (kKeyModCtrl | kKeyModShift) && tab.key == "tab",
        "ctrl+shift+tab parses");

  KeyChord digit = MustParseChord("mod+9");
  Check(digit.modifiers == kKeyModPrimary && digit.key == "9", "digits parse");

  KeyChord arrow = MustParseChord("cmd+alt+ArrowLeft");
  Check(arrow.modifiers == (kKeyModCmd | kKeyModAlt) && arrow.key == "left",
        "arrow aliases normalize");

  KeyChord f12 = MustParseChord("ctrl+f12");
  Check(f12.modifiers == kKeyModCtrl && f12.key == "f12", "f-key parses");

  KeyChord scan = MustParseChord("cmd+[Slash]");
  Check(scan.modifiers == kKeyModCmd && scan.key == "/",
        "VS Code scan-code spelling parses");

  std::string error;
  Check(!ParseChord("", &error).has_value(), "empty chord fails");
  Check(!ParseChord("cmd+cmd+t", &error).has_value(),
        "duplicate modifier fails");
  Check(!ParseChord("hyper+t", &error).has_value(), "unknown modifier fails");
  Check(!ParseChord("cmd+mouse1", &error).has_value(),
        "unsupported key token fails");
}

void TestWhenParser() {
  WhenExpression expression =
      MustParseWhen("!terminalFocused && webFocused || dragActive");

  KeyContext context;
  context.web_focused = true;
  Check(EvalWhen(expression, context), "! binds before &&");

  context.terminal_focused = true;
  Check(!EvalWhen(expression, context), "&& binds before ||");

  context.drag_active = true;
  Check(EvalWhen(expression, context), "|| fallback evaluates");

  WhenExpression pane_count = MustParseWhen("paneCount>1 && sidebarVisible");
  KeyContext pane_context;
  pane_context.pane_count_gt_one = true;
  pane_context.pane_count = 3;
  pane_context.sidebar_visible = true;
  Check(EvalWhen(pane_count, pane_context),
        "comparison-like context names are bare flags");

  pane_context.surface_kind = "terminal";
  pane_context.is_mac = true;
  WhenExpression typed = MustParseWhen(
      "isMac && paneCount >= 2 && surfaceKind == terminal");
  Check(EvalWhen(typed, pane_context),
        "typed VS Code comparisons evaluate against context values");
  WhenExpression regex = MustParseWhen("surfaceKind =~ /term.*/");
  Check(EvalWhen(regex, pane_context), "regex context comparison evaluates");

  std::string error;
  Check(!ParseWhen("omniboxFocused &&", &error).has_value(),
        "trailing operator fails");
  WhenExpression grouped = MustParseWhen(
      "(omniboxFocused || terminalFocused) && !dragActive");
  pane_context.terminal_focused = true;
  Check(EvalWhen(grouped, pane_context),
        "parenthesized VS Code expressions evaluate");
  pane_context.drag_active = true;
  Check(!EvalWhen(grouped, pane_context),
        "negation composes with parenthesized expressions");
  Check(!ParseWhen("(omniboxFocused", &error).has_value(),
        "unterminated parenthesis fails");
}

void TestResolve() {
  Keymap keymap = DefaultKeymap(/*is_mac=*/true);
  KeyContext context;

  std::optional<std::string> command =
      keymap.Resolve(MustParseChord("cmd+t"), context, /*is_mac=*/true);
  Check(command && *command == "tab.newWeb", "mod resolves to cmd on mac");
  command = keymap.Resolve(MustParseChord("cmd+ctrl+t"), context,
                           /*is_mac=*/true);
  Check(command && *command == "tab.newTerminal",
        "cmd+ctrl+t creates a terminal tab on mac");
  command = keymap.Resolve(MustParseChord("cmd+shift+t"), context,
                           /*is_mac=*/true);
  Check(command && *command == "tab.restore",
        "cmd+shift+t restores a closed tab on mac");
  command = keymap.Resolve(MustParseChord("cmd+c"), context,
                           /*is_mac=*/true);
  Check(command && *command == "edit.copy",
        "native edit shortcuts participate in the keymap");

  Keymap non_mac = DefaultKeymap(/*is_mac=*/false);
  command = non_mac.Resolve(MustParseChord("ctrl+t"), context,
                            /*is_mac=*/false);
  Check(command && *command == "tab.newWeb", "mod resolves to ctrl off mac");
  command = non_mac.Resolve(MustParseChord("ctrl+alt+t"), context,
                            /*is_mac=*/false);
  Check(command && *command == "tab.newTerminal",
        "ctrl+alt+t creates a terminal tab off mac");
  command = non_mac.Resolve(MustParseChord("ctrl+shift+t"), context,
                            /*is_mac=*/false);
  Check(command && *command == "tab.restore",
        "ctrl+shift+t restores a closed tab off mac");

  command =
      keymap.Resolve(MustParseChord("ctrl+tab"), context, /*is_mac=*/true);
  Check(command && *command == "tab.next", "ctrl-tab is real ctrl on mac");

  command = keymap.Resolve(MustParseChord("escape"), context, /*is_mac=*/true);
  Check(!command, "escape is gated by when clause");
  context.omnibox_focused = true;
  command = keymap.Resolve(MustParseChord("escape"), context, /*is_mac=*/true);
  Check(command && *command == "omnibox.escape", "escape resolves in omnibox");

  KeyRule override_rule;
  override_rule.chord = MustParseChord("cmd+t");
  override_rule.command = "workspace.next";
  keymap.AddRule(override_rule);
  command = keymap.Resolve(MustParseChord("cmd+t"), context, /*is_mac=*/true);
  Check(command && *command == "workspace.next", "last matching rule wins");

  KeyRule unbind;
  unbind.chord = MustParseChord("cmd+t");
  unbind.command = "-";
  keymap.AddRule(unbind);
  command = keymap.Resolve(MustParseChord("cmd+t"), context, /*is_mac=*/true);
  Check(!command, "unbind suppresses earlier binding");

  command = keymap.Resolve(MustParseChord("alt+]"), context,
                           /*is_mac=*/true);
  Check(command && *command == "workspace.next",
        "mac workspace cycle default present");
  command = non_mac.Resolve(MustParseChord("ctrl+alt+]"), context,
                            /*is_mac=*/false);
  Check(command && *command == "workspace.next",
        "non-mac workspace cycle default present");
  command =
      non_mac.Resolve(MustParseChord("ctrl+3"), context, /*is_mac=*/false);
  Check(command && *command == "workspace.jump3",
        "workspace digit jump default present");

  command = keymap.Resolve(MustParseChord("cmd+="), context, /*is_mac=*/true);
  Check(command && *command == "zoom.in", "cmd+= zooms in on mac");
  command =
      keymap.Resolve(MustParseChord("cmd+shift+="), context, /*is_mac=*/true);
  Check(command && *command == "zoom.in", "cmd+shift+= zooms in on mac");
  command = keymap.Resolve(MustParseChord("cmd+-"), context, /*is_mac=*/true);
  Check(command && *command == "zoom.out", "cmd+- zooms out on mac");
  command = keymap.Resolve(MustParseChord("cmd+0"), context, /*is_mac=*/true);
  Check(command && *command == "zoom.reset", "cmd+0 resets zoom on mac");
  command = keymap.Resolve(MustParseChord("alt+shift+m"), context,
                           /*is_mac=*/true);
  Check(command && *command == "workspace.toggleLayout",
        "option+shift+m toggles workspace layout in default mac scheme");
  command = non_mac.Resolve(MustParseChord("ctrl+shift+m"), context,
                            /*is_mac=*/false);
  Check(command && *command == "workspace.toggleLayout",
        "ctrl+shift+m toggles workspace layout off mac");
  context.terminal_focused = true;
  context.omnibox_focused = false;
  command = keymap.Resolve(MustParseChord("cmd+="), context, /*is_mac=*/true);
  Check(!command, "zoom defaults are gated away for terminal focus");
}

void TestLinuxTerminalControlKeys() {
  Keymap keymap = DefaultKeymap(false);
  KeyContext context;
  context.is_linux = true;
  context.terminal_focused = true;
  for (const char* chord : {"ctrl+b", "ctrl+d", "ctrl+l", "ctrl+n",
                            "ctrl+r", "ctrl+t", "ctrl+w", "ctrl+[", "ctrl+]"}) {
    Check(!keymap.Resolve(MustParseChord(chord), context, false),
          "Linux terminal Control chord reaches PTY");
  }
  auto split = keymap.Resolve(MustParseChord("ctrl+alt+d"), context, false);
  Check(split && *split == "pane.splitRight", "explicit terminal split shortcut");
  context.terminal_focused = false;
  auto browser_split = keymap.Resolve(MustParseChord("ctrl+d"), context, false);
  Check(browser_split && *browser_split == "pane.splitRight",
        "browser Control shortcuts remain available");
  context.terminal_focused = true;
  KeymapLoadResult overrides = ParseKeymapJson(
      R"({"keybindings":[{"key":"ctrl+d","command":"pane.splitRight"}]})");
  keymap = KeymapFromConfig(false, overrides);
  split = keymap.Resolve(MustParseChord("ctrl+d"), context, false);
  Check(split && *split == "pane.splitRight", "explicit user binding wins");
}

void TestModifierSchemes() {
  using Scheme = ShortcutModifierScheme;

  Check(ShortcutModifierSchemeToString(
            Scheme::kCommandTabsControlWorkspaces) ==
            "command-tabs-control-workspaces",
        "command-tabs scheme has stable config spelling");
  Check(ParseShortcutModifierScheme("cmd-tabs-ctrl-workspaces") ==
            Scheme::kCommandTabsControlWorkspaces,
        "command-tabs shorthand parses");
  Check(ParseShortcutModifierScheme("CONTROL-TABS-COMMAND-WORKSPACES") ==
            Scheme::kControlTabsCommandWorkspaces,
        "scheme parsing is case insensitive");
  Check(!ParseShortcutModifierScheme("swap-sometimes"),
        "unknown scheme is rejected");

  KeyContext context;
  Keymap command_tabs = DefaultKeymap(
      /*is_mac=*/true, Scheme::kCommandTabsControlWorkspaces);
  std::optional<std::string> command = command_tabs.Resolve(
      MustParseChord("cmd+t"), context, /*is_mac=*/true);
  Check(command && *command == "tab.newWeb",
        "command-tabs scheme creates tabs with cmd");
  Check(!command_tabs.Resolve(MustParseChord("ctrl+t"), context,
                              /*is_mac=*/true),
        "command-tabs scheme does not also bind ctrl+t");
  command = command_tabs.Resolve(MustParseChord("alt+3"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "workspace.jump3",
        "command-tabs scheme selects workspaces with option");
  command = command_tabs.Resolve(MustParseChord("cmd+3"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "tab.jump3",
        "command-tabs scheme selects tabs with command digits");
  command = command_tabs.Resolve(MustParseChord("cmd+9"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "tab.jump9",
        "command+9 uses Chrome's last-tab command");
  command = command_tabs.Resolve(MustParseChord("alt+]"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "workspace.next",
        "command-tabs scheme cycles workspaces with option");
  command = command_tabs.Resolve(MustParseChord("alt+n"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "workspace.new",
        "option+n creates a workspace in command-tabs scheme");
  command = command_tabs.Resolve(MustParseChord("cmd+ctrl+t"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "tab.newTerminal",
        "terminal shortcut stays cmd+ctrl+t in command-tabs scheme");
  command = command_tabs.Resolve(MustParseChord("cmd+shift+t"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "tab.restore",
        "restore shortcut stays cmd+shift+t in command-tabs scheme");

  Keymap control_tabs = DefaultKeymap(
      /*is_mac=*/true, Scheme::kControlTabsCommandWorkspaces);
  command = control_tabs.Resolve(MustParseChord("ctrl+t"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "tab.newWeb",
        "control-tabs scheme creates tabs with ctrl");
  command = control_tabs.Resolve(MustParseChord("cmd+t"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "tab.newWeb",
        "cmd+t remains a browser invariant in control-tabs scheme");
  command = control_tabs.Resolve(MustParseChord("cmd+3"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "workspace.jump3",
        "control-tabs scheme selects workspaces with cmd");
  command = control_tabs.Resolve(MustParseChord("ctrl+3"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "tab.jump3",
        "control-tabs scheme selects tabs with control digits");
  command = control_tabs.Resolve(MustParseChord("cmd+alt+]"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "workspace.next",
        "control-tabs scheme cycles workspaces with cmd+alt");
  Check(!control_tabs.Resolve(MustParseChord("ctrl+alt+]"), context,
                              /*is_mac=*/true),
        "control-tabs scheme frees ctrl+alt workspace cycle");
  command = control_tabs.Resolve(MustParseChord("alt+n"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "workspace.new",
        "option+n creates a workspace in control-tabs scheme");
  command = control_tabs.Resolve(MustParseChord("cmd+ctrl+t"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "tab.newTerminal",
        "terminal shortcut stays cmd+ctrl+t when tab modifiers swap");
  command = control_tabs.Resolve(MustParseChord("cmd+shift+t"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "tab.restore",
        "restore shortcut stays cmd+shift+t when tab modifiers swap");

  command = command_tabs.Resolve(MustParseChord("cmd+shift+n"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "window.new",
        "cmd+shift+n creates a native window in command-tabs scheme");
  command = control_tabs.Resolve(MustParseChord("cmd+shift+n"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "window.new",
        "cmd+shift+n remains a native-window shortcut when tab mods swap");
  command = command_tabs.Resolve(MustParseChord("cmd+shift+w"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "window.close",
        "cmd+shift+w closes only the current native window");
  command = control_tabs.Resolve(MustParseChord("cmd+shift+w"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "window.close",
        "native-window close does not change with the tab modifier");

  command = command_tabs.Resolve(MustParseChord("ctrl+tab"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "tab.next",
        "ctrl+tab remains the safe mac tab-cycle convention");
  command = control_tabs.Resolve(MustParseChord("ctrl+tab"), context,
                                 /*is_mac=*/true);
  Check(command && *command == "tab.next",
        "ctrl+tab remains available in control-tabs scheme");

  Keymap non_mac = DefaultKeymap(
      /*is_mac=*/false, Scheme::kControlTabsCommandWorkspaces);
  command = non_mac.Resolve(MustParseChord("ctrl+t"), context,
                            /*is_mac=*/false);
  Check(command && *command == "tab.newWeb",
        "non-mac defaults remain ctrl regardless of mac scheme");
}

void TestVsCodeResolution() {
  Keymap keymap = DefaultKeymap(/*is_mac=*/true);
  KeyContext context;

  KeyRule removal;
  removal.chord = MustParseChord("cmd+shift+t");
  removal.sequence = {removal.chord};
  removal.command = "-tab.restore";
  keymap.AddRule(removal);
  Check(!keymap.Resolve(removal.chord, context, /*is_mac=*/true),
        "minus-prefixed command removes matching default");

  KeyRule replacement;
  replacement.chord = removal.chord;
  replacement.sequence = {replacement.chord};
  replacement.command = "tab.newWeb";
  keymap.AddRule(replacement);
  std::optional<std::string> command =
      keymap.Resolve(removal.chord, context, /*is_mac=*/true);
  Check(command && *command == "tab.newWeb",
        "later user rule replaces removed default");

  KeyRule chord;
  chord.sequence = {MustParseChord("cmd+k"), MustParseChord("cmd+t")};
  chord.chord = chord.sequence.front();
  chord.command = "tab.newTerminal";
  chord.args_json = "{\"title\":\"Chord terminal\"}";
  keymap.AddRule(chord);
  Check(keymap.HasChordPrefix({chord.sequence[0]}, context, /*is_mac=*/true),
        "first stroke enters chord mode");
  std::optional<ResolvedKeybinding> resolved = keymap.ResolveSequence(
      chord.sequence, context, /*is_mac=*/true);
  Check(resolved && resolved->command == "tab.newTerminal" &&
            resolved->args_json.find("Chord terminal") != std::string::npos,
        "two-stroke chord resolves command and args");

  KeyRule remove_chord;
  remove_chord.chord = chord.sequence.front();
  remove_chord.sequence = chord.sequence;
  remove_chord.command = "-tab.newTerminal";
  keymap.AddRule(remove_chord);
  Check(!keymap.HasChordPrefix({chord.sequence[0]}, context,
                               /*is_mac=*/true),
        "removed chord no longer captures its prefix");
}

void TestJson() {
  KeymapLoadResult result = ParseKeymapJson(
      "{// JSONC cmux config\n"
      "\"keyboard\":{\"modifierScheme\":"
      "\"control-tabs-command-workspaces\",},"
      "\"keybindings\":["
      "{\"key\":\"cmd+ctrl+t\",\"command\":\"tab.newTerminal\","
      "\"when\":\"!terminalFocused\"},"
      "{\"key\":\"cmd+k cmd+t\",\"command\":\"runCommands\","
      "\"args\":{\"commands\":[\"tab.newWeb\",\"tab.next\"]}},"
      "{\"key\":\"cmd+1\",\"command\":\"future.command\","
      "\"args\":\"literal\"},"
      "{\"key\":\"cmd+bad\",\"command\":\"tab.close\"},"
      "{\"key\":\"cmd+w\"},"
      "]}");
  Check(result.file_found, "json parse marks file present");
  Check(result.valid, "JSONC cmux config parses");
  Check(result.rules.size() == 3, "json keeps valid rules and skips bad ones");
  Check(result.warnings.size() == 2, "json reports bad rules");
  Check(result.rules[0].command == "tab.newTerminal",
        "json command field parsed");
  Check(result.rules[1].sequence.size() == 2,
        "VS Code chord sequence parsed");
  Check(result.rules[1].args_json.find("commands") != std::string::npos,
        "VS Code args preserved as JSON");
  Check(result.rules[2].args_json == "\"literal\"",
        "string args retain valid JSON quoting");
  Check(result.modifier_scheme ==
            ShortcutModifierScheme::kControlTabsCommandWorkspaces,
        "keyboard modifier scheme parsed");

  result = ParseKeymapJson("{\"keybindings\":{}}");
  Check(result.rules.empty() && !result.warnings.empty(),
        "malformed top-level json warns and yields no rules");

  result = ParseKeymapJson("{\"futureOption\":}");
  Check(!result.valid, "missing unknown-field value invalidates config");
  result = ParseKeymapJson("{/* unterminated");
  Check(!result.valid, "unterminated JSONC comment invalidates config");

  result = ParseKeymapJson(
      "{\"keyboard\":{\"modifierScheme\":"
      "\"control-tabs-command-workspaces\"},"
      "\"keybindings\":[{\"key\":\"ctrl+t\",\"command\":\"tab.newWeb\","
      "\"when\":\"!terminalFocused\"}],\"futureOption\":true}");
  Check(result.modifier_scheme ==
            ShortcutModifierScheme::kControlTabsCommandWorkspaces,
        "object config parses modifier scheme");
  Check(result.rules.size() == 1 && result.warnings.empty(),
        "object config parses bindings and ignores future fields");
  Keymap configured = KeymapFromConfig(/*is_mac=*/true, result);
  std::optional<std::string> configured_command = configured.Resolve(
      MustParseChord("ctrl+w"), KeyContext(), /*is_mac=*/true);
  Check(configured_command && *configured_command == "tab.close",
        "parsed modifier scheme reaches configured defaults");

  result = ParseKeymapJson(
      "{\"keyboard\":{\"modifierScheme\":\"unknown\"},"
      "\"keybindings\":[]}");
  Check(!result.modifier_scheme && result.warnings.size() == 1,
        "unknown modifier scheme warns and falls back");

  KeyRule serialized_rule;
  serialized_rule.chord = MustParseChord("cmd+k");
  serialized_rule.sequence = {serialized_rule.chord,
                              MustParseChord("cmd+ctrl+t")};
  serialized_rule.command = "tab.newTerminal";
  serialized_rule.when_text = "!terminalFocused";
  serialized_rule.when = MustParseWhen(serialized_rule.when_text);
  serialized_rule.args_json = "{\"title\":\"Dev\"}";
  const std::string serialized = SerializeKeymapConfig(
      ShortcutModifierScheme::kControlTabsCommandWorkspaces,
      {serialized_rule});
  result = ParseKeymapJson(serialized);
  Check(result.modifier_scheme ==
            ShortcutModifierScheme::kControlTabsCommandWorkspaces &&
            result.rules.size() == 1,
        "serialized configure object round trips");
  Check(result.rules[0].sequence.size() == 2,
        "serialization preserves chord sequence");
  Check(result.rules[0].when_text == "!terminalFocused",
        "serialization preserves when clause");
  Check(result.rules[0].args_json.find("title") != std::string::npos,
        "serialization preserves args");

  const std::string path = "/tmp/cmux_config_test.json";
  {
    std::ofstream out(path);
    out << "{\"keybindings\":[{\"key\":\"ctrl+n\","
           "\"command\":\"column.new\"}]}";
  }
  result = LoadKeymapFile(path);
  Check(result.file_found && result.rules.size() == 1,
        "LoadKeymapFile reads a valid file");
  std::remove(path.c_str());

  result = LoadKeymapFile("/tmp/cmux_config_test_missing.json");
  Check(!result.file_found && result.rules.empty(),
        "missing keymap file is not an error");
}

}  // namespace

int main() {
  std::printf("cmux_keymap_test\n");
  TestChordParser();
  TestWhenParser();
  TestResolve();
  TestModifierSchemes();
  TestLinuxTerminalControlKeys();
  TestVsCodeResolution();
  TestJson();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
