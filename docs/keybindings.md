# Keybindings

cmux reads shortcuts from the JSONC file `~/.config/cmux/cmux.json`, or the
path selected by an absolute `CMUX_CONFIG`. Defaults are loaded first, then the
top-level
`keybindings` rules are appended and evaluated from bottom to top, matching VS
Code's resolution order.

```jsonc
{
  "$schema": "https://raw.githubusercontent.com/manaflow-ai/cmux-browser/main/docs/cmux.schema.json",
  "keyboard": {
    "modifierScheme": "command-tabs-control-workspaces"
  },
  "keybindings": [
    { "key": "cmd+k cmd+r", "command": "keymap.reload" },
    {
      "key": "cmd+ctrl+t",
      "command": "tab.newTerminal",
      "when": "isMac && !textInputFocus",
      "args": { "title": "Dev", "command": "zsh" }
    }
  ]
}
```

## Modifier Scheme

On macOS, `keyboard.modifierScheme` accepts either:

- `command-tabs-control-workspaces` (default, legacy config spelling): Command
  creates, closes, cycles, and selects tabs; Option creates, cycles, and
  selects workspaces. The spelling is retained so existing JSON keeps working.
- `control-tabs-command-workspaces`: Control creates, closes, and cycles tabs;
  Command selects workspaces by number and toggles workspace layout. `Cmd+T`
  still creates a web tab.

`Ctrl+Tab` and `Ctrl+Shift+Tab` always cycle tabs because macOS reserves
`Cmd+Tab` for switching applications. Browser, pane, column, and app shortcuts
keep their native Command bindings. Linux and Windows keep their existing
Control defaults regardless of this macOS preference.

In the default mode, `Option+[` / `Option+]` cycle workspaces. In the alternate
mode, `Command+Option+[` / `Command+Option+]` do so. `Option+N` creates a new
workspace in both modes.

Use a VS Code removal rule such as `{"key":"cmd+t","command":"-tab.newWeb"}`
to remove a particular default. An empty command consumes a shortcut without
running anything.

## Keys

Modifiers are `cmd`, `ctrl`, `alt`, `shift`, and `mod`. `mod` means `cmd` on
macOS and `ctrl` on Linux/Windows. Explicit `cmd` and `ctrl` are accepted.

Keys include letters, digits, `f1` through `f24`, `tab`, `escape`, `enter`,
`left`, `right`, `up`, `down`, `backspace`, `delete`, `home`, `end`, `pageup`,
`pagedown`, and punctuation such as `[`, `]`, `/`, `-`, `=`, and `` ` ``.
Shifted punctuation is normalized, so `cmd+shift+{` is the same chord as
`cmd+shift+[`.

Two-stroke chords are separated by a space. Scan-code spellings such as
`cmd+[Slash]` and `cmd+[KeyK]` are also accepted.

## When Clauses

`when` supports bare context flags, `!`, `&&`, `||`, parentheses,
equality/inequality, numeric comparisons, and regular-expression matching
with `=~`.

Available flags:

- `omniboxFocused`
- `terminalFocused`
- `webFocused`
- `dragActive`
- `sidebarVisible`
- `paneCount>1` (also accepted: `paneCountGt1`)
- `textInputFocus`
- `isMac`, `isLinux`, `isWindows`
- `paneCount`, `tabCount`, `workspaceCount`
- `surfaceKind` (`web` or `terminal`)

Unknown flags evaluate to false.

## Commands

- `tab.newWeb`
- `tab.newTerminal`
- `tab.restore`
- `tab.close`
- `tab.next`
- `tab.prev`
- `tab.jump1` through `tab.jump9` (`tab.jump9` selects the last tab)
- `pane.splitRight`
- `pane.splitDown`
- `pane.focusLeft`
- `pane.focusRight`
- `pane.focusUp`
- `pane.focusDown`
- `column.new`
- `column.cycleWidth`
- `sidebar.toggle`
- `omnibox.focus`
- `omnibox.escape`
- `page.reload`
- `page.back`
- `page.forward`
- `zoom.in`
- `zoom.out`
- `zoom.reset`
- `devtools.toggle`
- `devtools.undock`
- `edit.undo`
- `edit.redo`
- `edit.cut`
- `edit.copy`
- `edit.paste`
- `edit.selectAll`
- `workspace.next`
- `workspace.prev`
- `workspace.new`
- `workspace.toggleLayout`
- `workspace.jump1` through `workspace.jump9`
- `keymap.reload` (unbound by default)
- `theme.reload` (unbound by default)
- `window.new`
- `window.close`
- `window.toggleFullscreen`
- `runCommands`

Installed extension toolbar actions are commands too. Bind an action as
`extension.<extension-id>.action`, replacing `<extension-id>` with the ID on
the extension's details page. It runs against the active workspace.

`args` is passed to the selected command. `tab.newTerminal` accepts optional
`command` and `title` strings. `runCommands` accepts a `commands` array whose
items are command strings or `{ "command": "...", "args": ... }` objects.

`column.cycleWidth` uses `column_width_modes` from `~/.cmux_layout.json`.
The array order and length define the complete shortcut cycle; the default is
100%, 2/3, 1/2, then 1/3:

```json
{
  "column_width_modes": [1.0, 0.6666666667, 0.5, 0.3333333333]
}
```

`sidebar.toggle` cycles by width: completely hidden → icons-only → normal →
completely hidden. The visible widths and current mode persist in
`~/.cmux_layout.json`:

```json
{
  "sidebar_mode": "normal",
  "rail_width": 200,
  "sidebar_icon_width": 42
}
```

Icons-only mode follows Helium's collapsed-strip geometry exactly: a fixed
42-pixel rail containing 30-by-30-pixel targets with 6 pixels on either side.
Older or custom `sidebar_icon_width` values are normalized to 42.

The sidebar edge can be dragged in normal or icons-only mode. Its resize state
matches Helium: proposed widths through 101 pixels snap to the 42-pixel icon
rail; larger widths open at a 160-pixel minimum, grow through 400 pixels, and
snap to the 200-pixel default within Helium's effective 15-pixel range. A drag
released in icons-only mode preserves the previously saved normal width.
Completely hidden is available through `sidebar.toggle`, not by resizing.

The older `"sidebar_collapsed": true` spelling is still accepted and maps to
completely hidden mode. The temporary `"expanded"` and `"compact"` mode names
are also accepted as aliases for `"normal"` and `"icons"`. In `when` clauses,
`sidebarVisible` is true for normal and icons-only modes and false only when
the sidebar is completely hidden.

Workspace icons are unrestricted Unicode strings in `~/.cmux_rail.json`.
Exact workspace titles override the optional `"*"` fallback; use an empty
string to hide an icon intentionally. Multi-codepoint emoji and short text are
kept intact:

```json
{
  "icons": {
    "Work": "💼",
    "Servers": "ssh",
    "Quiet": "",
    "*": "●"
  }
}
```

Workspaces without an override use the first character of their title, so the
icons-only rail never becomes an empty stack of hit targets.

The same layout file controls which edge owns the sidebar. The default is
`"left"`; use `"right"` and restart cmux-browser to move the sidebar and its
resize handle to the other edge:

```json
{
  "sidebar_position": "right"
}
```

You can also change this at runtime by right-clicking a tab or empty tab-strip
chrome, then choosing **Browser Layout → Tabs on Right Side**.

Columns can also be resized freely by dragging the focused column's right
edge. Dividers between split panes are draggable in both directions.

## Defaults

Shared defaults:

| Key | Command | When |
| --- | --- | --- |
| `mod+b` | `sidebar.toggle` | |
| `mod+z` | `edit.undo` | Native responder-chain fallback remains available. |
| `mod+shift+z` | `edit.redo` | Native responder-chain fallback remains available. |
| `mod+x` | `edit.cut` | Native responder-chain fallback remains available. |
| `mod+c` | `edit.copy` | Native responder-chain fallback remains available. |
| `mod+v` | `edit.paste` | Native responder-chain fallback remains available. |
| `mod+a` | `edit.selectAll` | Native responder-chain fallback remains available. |
| `mod+shift+n` | `window.new` | |
| `mod+shift+w` | `window.close` | |
| `mod+shift+enter` | `window.toggleFullscreen` | |
| `mod+t` | `tab.newWeb` | |
| `mod+d` | `pane.splitRight` | Adds a new independently resizable column to the right. |
| `mod+shift+d` | `pane.splitDown` | Splits downward inside the focused column. |
| `mod+n` | `column.new` | |
| `mod+w` | `tab.close` | |
| `mod+l` | `omnibox.focus` | |
| `mod+r` | `page.reload` | |
| `mod+[` | `page.back` | |
| `mod+]` | `page.forward` | |
| `mod+shift+[` | `tab.prev` | |
| `mod+shift+]` | `tab.next` | |
| `ctrl+tab` | `tab.next` | |
| `ctrl+shift+tab` | `tab.prev` | |
| `alt+n` | `workspace.new` | |
| `escape` | `omnibox.escape` | `omniboxFocused || dragActive` |

The configurable tab rows above use Command or Control according to
`keyboard.modifierScheme` on macOS, except `Cmd+T`, which always creates a web tab.
`tab.jump1` through `tab.jump9` use the tab modifier plus the corresponding
digit. `workspace.toggleLayout` uses the workspace modifier plus `Shift+M`, and
`workspace.jump1` through `workspace.jump9` use the workspace modifier plus the
corresponding digit. In the default mode those pairs are Command/Option; in the
alternate mode they are Control/Command.

macOS defaults:

| Key | Command |
| --- | --- |
| `cmd+shift+t` | `tab.restore` |
| `cmd+ctrl+t` | `tab.newTerminal` |
| `cmd+ctrl+r` | `column.cycleWidth` |
| `cmd+ctrl+h` | `pane.focusLeft` |
| `cmd+ctrl+l` | `pane.focusRight` |
| `cmd+ctrl+k` | `pane.focusUp` |
| `cmd+ctrl+j` | `pane.focusDown` |
| `cmd+alt+left` | `pane.focusLeft` |
| `cmd+alt+right` | `pane.focusRight` |
| `cmd+alt+up` | `pane.focusUp` |
| `cmd+alt+down` | `pane.focusDown` |
| `cmd+alt+i` | `devtools.toggle` |

Linux/Windows defaults:

| Key | Command |
| --- | --- |
| `ctrl+shift+t` | `tab.restore` |
| `ctrl+alt+t` | `tab.newTerminal` |
| `ctrl+alt+[` | `workspace.prev` |
| `ctrl+alt+]` | `workspace.next` |
| `ctrl+alt+r` | `column.cycleWidth` |
| `ctrl+left` | `pane.focusLeft` |
| `ctrl+right` | `pane.focusRight` |
| `ctrl+up` | `pane.focusUp` |
| `ctrl+down` | `pane.focusDown` |
