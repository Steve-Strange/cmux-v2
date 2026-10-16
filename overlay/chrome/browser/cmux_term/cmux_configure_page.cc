// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_configure_page.h"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/stringprintf.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "chrome/browser/cmux_term/cmux_keymap.h"
#include "chrome/browser/cmux_term/cmux_layout_config.h"
#include "chrome/browser/cmux_term/cmux_theme_ghostty.h"
#include "chrome/browser/cmux_term/cmux_views.h"
#include "chrome/browser/platform_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/pref_names.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_MAC)
#include "chrome/browser/cmux_term/cmux_app_icon.h"
#endif
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "content/public/browser/webui_config.h"
#include "content/public/browser/webui_config_map.h"
#include "content/public/common/url_constants.h"
#include "components/prefs/pref_service.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"

namespace cmux {

WEB_UI_CONTROLLER_TYPE_IMPL(CmuxConfigureUI)

namespace {

constexpr char kIndexHtml[] = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>cmux settings</title>
  <link rel="stylesheet" href="configure.css">
</head>
<body>
  <main>
    <header>
      <div class="mark" aria-hidden="true">c</div>
      <div>
        <p class="eyebrow">CMUX SETTINGS</p>
        <h1>Make cmux yours.</h1>
        <p class="lede">Tune cmux's appearance, shortcuts, and browser setup.</p>
      </div>
    </header>

    <nav class="settings-tabs" role="tablist" aria-label="Settings sections">
      <button class="settings-tab active" type="button" role="tab"
        aria-selected="true" data-panel="appearance">Appearance</button>
      <button class="settings-tab" type="button" role="tab"
        aria-selected="false" data-panel="toolbar">Toolbar</button>
      <button class="settings-tab" type="button" role="tab"
        aria-selected="false" data-panel="details">Keyboard</button>
      <button class="settings-tab" type="button" role="tab"
        aria-selected="false" data-panel="browser-setup">Browser</button>
    </nav>

    <section id="appearance" class="panel settings-panel" role="tabpanel">
      <div class="section-heading">
        <div><p class="eyebrow">GHOSTTY THEME</p><h2>One theme, everywhere</h2></div>
        <span id="theme-count" class="native-pill">Loading…</span>
      </div>
      <p class="muted">Choose any theme bundled with Ghostty. The terminal,
        sidebar, tabs, toolbar, omnibox, menus, and New Tab page update
        together. Choose “Follow Ghostty config” to use your Ghostty files.</p>
      <div class="theme-picker">
        <label for="ghostty-theme">Theme</label>
        <input id="ghostty-theme" list="ghostty-theme-options"
          autocomplete="off" placeholder="Follow Ghostty config">
        <datalist id="ghostty-theme-options"></datalist>
        <button id="clear-theme" class="file-button" type="button">Use Ghostty config</button>
      </div>
      <div id="customization-status" class="inline-status"
        role="status" aria-live="polite"></div>

      <div class="panel-divider"></div>
      <div class="section-heading">
        <div><p class="eyebrow">CONTENT FRAME</p><h2>Window surfaces</h2></div>
        <span class="native-pill">Helium</span>
      </div>
      <p class="muted">Give terminal and browser content Helium's compact
        rounded frame. Turn it off to remove both the frame and its content
        padding. The frame also stays out of the way in fullscreen.</p>
      <label class="toggle-row" for="rounded-frame-toggle">
        <span><strong>Show rounded content frame</strong>
          <small>On: 3 px inset, 8 px corners, and a subtle 1 px outline.
            Off: no inset, corner mask, or outline.</small>
        </span>
        <span class="switch">
          <input id="rounded-frame-toggle" type="checkbox" role="switch" disabled>
          <span class="switch-track" aria-hidden="true"></span>
        </span>
      </label>
      <div id="rounded-frame-status" class="inline-status"
        role="status" aria-live="polite"></div>

      <div class="panel-divider"></div>
      <div class="section-heading">
        <div><p class="eyebrow">APP ICON</p><h2>Dock appearance</h2></div>
        <span class="native-pill">macOS</span>
      </div>
      <p class="muted">Follow the system automatically, or keep a specific
        cmux icon in the Dock while the app is running.</p>
      <fieldset id="icon-mode-fieldset" class="icon-mode-grid" disabled>
        <legend>App icon appearance</legend>
        <label class="icon-mode-card">
          <input type="radio" name="app-icon-mode" value="automatic">
          <span class="icon-preview automatic" aria-hidden="true"></span>
          <span><strong>Automatic</strong><small>Match the current system appearance.</small></span>
        </label>
        <label class="icon-mode-card">
          <input type="radio" name="app-icon-mode" value="light">
          <span class="icon-preview light" aria-hidden="true"></span>
          <span><strong>Light</strong><small>Always use the white cmux icon.</small></span>
        </label>
        <label class="icon-mode-card">
          <input type="radio" name="app-icon-mode" value="dark">
          <span class="icon-preview dark" aria-hidden="true"></span>
          <span><strong>Dark</strong><small>Always use the black cmux icon.</small></span>
        </label>
      </fieldset>
      <div id="icon-status" class="inline-status" role="status" aria-live="polite"></div>
    </section>

    <div id="status" role="status" aria-live="polite">Loading shortcuts…</div>

    <section id="toolbar" class="panel settings-panel" role="tabpanel" hidden>
      <div class="section-heading">
        <div><p class="eyebrow">TOOLBAR</p><h2>Choose what stays within reach</h2></div>
        <span class="native-pill">Live</span>
      </div>
      <p class="muted">These are cmux’s permanent pane controls and apply to
        every web pane immediately. New Tab → Edit → Toolbar opens Helium’s
        complete native set of pinnable browser actions.</p>
      <div class="toggle-grid" aria-label="Toolbar controls">
        <label class="toggle-card">
          <span><strong>Back</strong><small>Return through page history.</small></span>
          <input type="checkbox" data-toolbar="back">
        </label>
        <label class="toggle-card">
          <span><strong>Forward</strong><small>Move forward through page history.</small></span>
          <input type="checkbox" data-toolbar="forward">
        </label>
        <label class="toggle-card">
          <span><strong>Reload</strong><small>Reload or stop the current page.</small></span>
          <input type="checkbox" data-toolbar="reload">
        </label>
        <label class="toggle-card">
          <span><strong>Home</strong><small>Open your configured home page.</small></span>
          <input type="checkbox" data-toolbar="home">
        </label>
        <label class="toggle-card">
          <span><strong>Extensions</strong><small>Pinned actions and the extensions menu.</small></span>
          <input type="checkbox" data-toolbar="extensions">
        </label>
        <label class="toggle-card">
          <span><strong>Downloads</strong><small>Recent and active downloads.</small></span>
          <input type="checkbox" data-toolbar="downloads">
        </label>
        <label class="toggle-card">
          <span><strong>Global media controls</strong><small>Playback controls shown while media is active.</small></span>
          <input type="checkbox" data-toolbar="media">
        </label>
        <label class="toggle-card">
          <span><strong>Profile</strong><small>Passwords, identity, and profile controls.</small></span>
          <input type="checkbox" data-toolbar="profile">
        </label>
        <label class="toggle-card">
          <span><strong>Main menu</strong><small>Browser settings and commands.</small></span>
          <input type="checkbox" data-toolbar="menu">
        </label>
      </div>
      <div class="toolbar-actions">
        <button id="show-all-toolbar" class="file-button" type="button">Show all</button>
        <button id="reset-customization" class="file-button" type="button">Reset defaults</button>
      </div>
      <div id="toolbar-status" class="inline-status"
        role="status" aria-live="polite"></div>
    </section>

    <section id="details" class="panel settings-panel" role="tabpanel" hidden>
      <div class="section-heading">
        <div><p class="eyebrow">KEYBOARD</p><h2>Tabs and workspaces</h2></div>
        <span class="native-pill">cmux.json</span>
      </div>
      <div class="choice-grid keyboard-actions" aria-label="Keyboard actions">
        <button id="use-defaults" class="choice primary" type="button">
          <span class="choice-title">View shortcuts</span>
          <span>Inspect the active cmux.json keyboard configuration</span>
        </button>
        <button id="configure" class="choice" type="button">
          <span class="choice-title">Open configuration</span>
          <span>Edit shortcuts and advanced browser settings in cmux.json</span>
        </button>
      </div>
      <fieldset id="scheme-fieldset" disabled>
        <legend>Shortcut modifier scheme (edit in cmux.json)</legend>
        <label class="scheme">
          <input type="radio" name="scheme"
            value="command-tabs-control-workspaces">
          <span><strong>Command tabs</strong><small>Command creates, closes, and
            selects tabs. Option selects workspaces and Option-N creates one.
            </small></span>
        </label>
        <label class="scheme">
          <input type="radio" name="scheme"
            value="control-tabs-command-workspaces">
          <span><strong>Control tabs</strong><small>Control creates, closes, and
            selects tabs. Command selects workspaces; Command-T still creates
            a web tab.</small></span>
        </label>
      </fieldset>
      <div class="shortcut-tools">
        <label class="search-label" for="shortcut-search">Filter shortcuts</label>
        <input id="shortcut-search" type="search"
          placeholder="Search key, command, or when clause">
        <div class="shortcut-actions">
          <span id="shortcut-count" class="muted"></span>
          <button id="open-config" class="file-button" type="button">Open cmux.json</button>
        </div>
      </div>
      <div class="shortcut-table" role="table" aria-label="Active shortcuts">
        <div class="shortcut-row shortcut-header" role="row">
          <span role="columnheader">Key</span>
          <span role="columnheader">Command</span>
          <span role="columnheader">When</span>
          <span role="columnheader">Source</span>
        </div>
        <div id="shortcut-rows"></div>
      </div>
    </section>

    <section id="browser-setup" class="panel settings-panel" role="tabpanel" hidden>
      <div class="section-heading">
        <div><p class="eyebrow">BROWSER SETUP</p><h2>Bring your browser over</h2></div>
        <span class="next-pill">Next steps</span>
      </div>
      <p class="muted">These open Chromium's native settings today. Dedicated
        cmux import and default-browser status will land here next.</p>
      <nav class="links" aria-label="Browser setup links">
        <a href="cmux://settings/search">Default search engine <span>→</span></a>
        <a href="cmux://settings/importData">Import bookmarks and settings <span>→</span></a>
        <a href="cmux://settings/defaultBrowser">Set cmux as default browser <span>→</span></a>
      </nav>
    </section>
  </main>
  <script type="module" src="configure.js"></script>
</body>
</html>)HTML";

constexpr char kCss[] = R"CSS(:root {
  color-scheme: light dark;
  --bg: light-dark(#f7f7f8, #111216);
  --panel: light-dark(#ffffff, #1a1b20);
  --text: light-dark(#17181b, #f3f4f7);
  --muted: light-dark(#686b73, #a6a9b2);
  --line: light-dark(#dedfe3, #34363e);
  --accent: #6c63ff;
  --accent-strong: #5147ef;
  font: 14px -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--text); }
main { width: min(820px, calc(100% - 40px)); margin: 0 auto; padding: 72px 0 96px; }
header { display: grid; grid-template-columns: 56px 1fr; gap: 20px; align-items: start; margin-bottom: 36px; }
.mark { display: grid; place-items: center; width: 56px; height: 56px; border-radius: 17px;
  color: white; background: linear-gradient(145deg, #827bff, #5046e5); font-size: 28px; font-weight: 750; }
.eyebrow { margin: 0 0 7px; color: var(--accent); font-size: 11px; font-weight: 750; letter-spacing: .14em; }
h1, h2 { margin: 0; letter-spacing: -.025em; }
h1 { font-size: clamp(32px, 5vw, 48px); line-height: 1.06; }
h2 { font-size: 22px; }
.lede { max-width: 610px; margin: 13px 0 0; color: var(--muted); font-size: 17px; line-height: 1.55; }
.settings-tabs { display: flex; gap: 4px; padding: 4px; border: 1px solid var(--line);
  border-radius: 13px; background: color-mix(in srgb, var(--panel) 84%, transparent); }
.settings-tab { flex: 1; padding: 10px 14px; border: 0; border-radius: 9px;
  color: var(--muted); background: transparent; cursor: pointer; font-weight: 650; }
.settings-tab:hover { color: var(--text); }
.settings-tab.active { color: var(--text); background: var(--panel); box-shadow: 0 1px 3px #00000016; }
.choice-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 14px; }
.keyboard-actions { margin-bottom: 22px; }
button { font: inherit; }
.choice { min-height: 110px; padding: 20px; border: 1px solid var(--line); border-radius: 16px;
  color: var(--text); background: var(--panel); cursor: pointer; text-align: left; box-shadow: 0 1px 2px #0000000b; }
.choice:hover { border-color: color-mix(in srgb, var(--accent) 65%, var(--line)); transform: translateY(-1px); }
.choice:focus-visible, .file-button:focus-visible, .settings-tab:focus-visible,
a:focus-visible, input:focus-visible { outline: 3px solid color-mix(in srgb, var(--accent) 45%, transparent); outline-offset: 2px; }
.choice.primary { border-color: transparent; color: #fff; background: linear-gradient(145deg, #746cff, var(--accent-strong)); }
.choice span { display: block; opacity: .78; line-height: 1.4; }
.choice-title { margin-bottom: 8px; opacity: 1 !important; font-size: 18px; font-weight: 700; }
#status { min-height: 22px; margin: 16px 3px 0; color: var(--muted); }
#status.error { color: #d34848; }
#status.success { color: light-dark(#16784a, #63d39b); }
.panel { margin-top: 22px; padding: 24px; border: 1px solid var(--line); border-radius: 18px; background: var(--panel); }
.section-heading { display: flex; justify-content: space-between; gap: 18px; align-items: center; margin-bottom: 18px; }
.native-pill, .next-pill { padding: 5px 9px; border-radius: 999px; font-size: 11px; font-weight: 650; white-space: nowrap; }
.native-pill { color: light-dark(#176941, #77d4a5); background: light-dark(#e7f6ee, #153a29); }
.next-pill { color: var(--muted); background: color-mix(in srgb, var(--muted) 12%, transparent); }
fieldset { margin: 0; padding: 0; border: 0; }
legend { position: absolute; width: 1px; height: 1px; overflow: hidden; clip-path: inset(50%); }
.icon-mode-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 12px; margin-top: 20px; }
.icon-mode-grid:disabled { opacity: .65; }
.icon-mode-card { position: relative; display: grid; justify-items: center; gap: 10px;
  padding: 16px 12px; border: 1px solid var(--line); border-radius: 14px;
  cursor: pointer; text-align: center; }
.icon-mode-card:has(input:checked) { border-color: var(--accent);
  box-shadow: 0 0 0 2px color-mix(in srgb, var(--accent) 18%, transparent); }
.icon-mode-card input { position: absolute; top: 10px; left: 10px; accent-color: var(--accent); }
.icon-mode-card strong, .icon-mode-card small { display: block; }
.icon-mode-card small { max-width: 170px; margin-top: 4px; color: var(--muted); line-height: 1.35; }
.icon-preview { position: relative; display: block; width: 70px; height: 70px;
  overflow: hidden; border-radius: 18px; box-shadow: inset 0 0 0 1px #7f84902e, 0 5px 12px #0002; }
.icon-preview.light { background: linear-gradient(#fff, #ededee); }
.icon-preview.dark { background: linear-gradient(#323436, #101112); }
.icon-preview.automatic { background: linear-gradient(135deg, #fff 0 49%, #242628 51% 100%); }
.icon-preview::after { content: ""; position: absolute; inset: 19px 15px;
  background: linear-gradient(135deg, #49d3ee, #5368f3);
  clip-path: polygon(0 0, 100% 50%, 0 100%, 0 72%, 56% 50%, 0 28%);
  filter: drop-shadow(0 2px 4px #269dd788); }
.inline-status { min-height: 20px; margin-top: 12px; color: var(--muted); font-size: 12px; }
.inline-status.error { color: #d34848; }
.inline-status.success { color: light-dark(#16784a, #63d39b); }
.panel-divider { height: 1px; margin: 26px 0; background: var(--line); }
.theme-picker { display: grid; grid-template-columns: 1fr auto; gap: 8px 10px;
  align-items: end; margin-top: 18px; }
.theme-picker label { grid-column: 1 / -1; font-size: 12px; font-weight: 650; }
#ghostty-theme { min-width: 0; padding: 10px 12px; border: 1px solid var(--line);
  border-radius: 10px; color: var(--text); background: var(--bg); font: inherit; }
.toggle-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px;
  margin-top: 20px; }
.toggle-card { display: flex; justify-content: space-between; gap: 16px;
  align-items: center; min-height: 78px; padding: 15px 16px;
  border: 1px solid var(--line); border-radius: 13px; cursor: pointer; }
.toggle-card:hover { border-color: color-mix(in srgb, var(--accent) 55%, var(--line)); }
.toggle-card:has(input:checked) { background: color-mix(in srgb, var(--accent) 7%, var(--panel)); }
.toggle-card strong, .toggle-card small { display: block; }
.toggle-card small { margin-top: 4px; color: var(--muted); line-height: 1.35; }
.toggle-card input { width: 34px; height: 20px; flex: 0 0 auto; accent-color: var(--accent); }
.toolbar-actions { display: flex; justify-content: flex-end; gap: 9px; margin-top: 16px; }
.toggle-row { display: flex; justify-content: space-between; gap: 20px;
  align-items: center; margin-top: 18px; padding: 15px 16px;
  border: 1px solid var(--line); border-radius: 14px; cursor: pointer; }
.toggle-row strong, .toggle-row small { display: block; }
.toggle-row small { margin-top: 4px; color: var(--muted); line-height: 1.4; }
.switch { position: relative; flex: 0 0 auto; width: 42px; height: 24px; }
.switch input { position: absolute; width: 1px; height: 1px; opacity: 0; }
.switch-track { position: absolute; inset: 0; border-radius: 999px;
  background: color-mix(in srgb, var(--muted) 35%, transparent);
  transition: background 120ms ease; }
.switch-track::after { content: ""; position: absolute; top: 3px; left: 3px;
  width: 18px; height: 18px; border-radius: 50%; background: white;
  box-shadow: 0 1px 3px #0004; transition: transform 120ms ease; }
.switch input:checked + .switch-track { background: var(--accent); }
.switch input:checked + .switch-track::after { transform: translateX(18px); }
.switch input:focus-visible + .switch-track {
  outline: 3px solid color-mix(in srgb, var(--accent) 45%, transparent);
  outline-offset: 2px; }
.switch input:disabled + .switch-track { opacity: .6; }
.scheme { display: grid; grid-template-columns: 22px 1fr; gap: 10px; padding: 15px 12px; border-top: 1px solid var(--line); cursor: pointer; }
.scheme:last-child { border-bottom: 1px solid var(--line); }
.scheme input { margin-top: 3px; accent-color: var(--accent); }
.scheme strong, .scheme small { display: block; }
.scheme strong { margin-bottom: 4px; }
.scheme small, .muted { color: var(--muted); line-height: 1.5; }
.shortcut-tools { display: grid; grid-template-columns: 1fr auto; gap: 8px 14px; align-items: center; margin-top: 22px; }
.search-label { grid-column: 1 / -1; font-size: 12px; font-weight: 650; }
#shortcut-search { min-width: 0; padding: 10px 12px; border: 1px solid var(--line); border-radius: 10px;
  color: var(--text); background: var(--bg); font: inherit; }
.shortcut-table { margin-top: 12px; border: 1px solid var(--line); border-radius: 12px; overflow: hidden; }
.shortcut-row { display: grid; grid-template-columns: minmax(130px, .9fr) minmax(170px, 1.25fr) minmax(150px, 1.2fr) 70px;
  gap: 12px; align-items: center; padding: 10px 12px; border-top: 1px solid var(--line); }
.shortcut-row:first-child { border-top: 0; }
.shortcut-header { color: var(--muted); background: color-mix(in srgb, var(--muted) 8%, transparent);
  font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: .06em; }
.shortcut-row code { overflow-wrap: anywhere; font: 12px ui-monospace, SFMono-Regular, Menlo, monospace; }
.shortcut-source { color: var(--muted); font-size: 11px; }
.shortcut-source.overlap { color: light-dark(#a05b00, #f3b55d); }
.shortcut-empty { padding: 22px 12px; color: var(--muted); text-align: center; }
.shortcut-actions { display: flex; gap: 10px; align-items: center; }
.file-button { padding: 8px 10px; border: 1px solid var(--line); border-radius: 9px;
  color: var(--text); background: var(--panel); cursor: pointer; white-space: nowrap; }
.file-button:hover { border-color: var(--accent); }
.file-button:disabled { cursor: default; opacity: .5; }
.links { display: grid; margin-top: 16px; border-top: 1px solid var(--line); }
.links a { display: flex; justify-content: space-between; padding: 14px 4px; border-bottom: 1px solid var(--line); color: var(--text); text-decoration: none; }
.links a:hover { color: var(--accent); }
@media (max-width: 620px) {
  main { width: min(100% - 26px, 820px); padding-top: 38px; }
  header { grid-template-columns: 44px 1fr; gap: 14px; }
  .mark { width: 44px; height: 44px; border-radius: 13px; font-size: 22px; }
  .choice-grid { grid-template-columns: 1fr; }
  .icon-mode-grid { grid-template-columns: 1fr; }
  .theme-picker, .toggle-grid { grid-template-columns: 1fr; }
  .theme-picker label { grid-column: auto; }
  .icon-mode-card { grid-template-columns: 70px 1fr; justify-items: start; align-items: center; text-align: left; }
  .icon-mode-card input { top: 9px; left: 9px; }
  .section-heading { align-items: flex-start; }
  .shortcut-table { overflow-x: auto; }
  .shortcut-row { min-width: 650px; }
}
@media (prefers-reduced-motion: no-preference) {
  .choice { transition: border-color 120ms ease, transform 120ms ease; }
}
)CSS";

constexpr char kJs[] = R"JS(import {sendWithPromise} from 'chrome://resources/js/cr.js';

const status = document.querySelector('#status');
const search = document.querySelector('#shortcut-search');
const shortcutRows = document.querySelector('#shortcut-rows');
const shortcutCount = document.querySelector('#shortcut-count');
const openConfig = document.querySelector('#open-config');
const iconFieldset = document.querySelector('#icon-mode-fieldset');
const iconStatus = document.querySelector('#icon-status');
const themePicker = document.querySelector('#ghostty-theme');
const themeOptions = document.querySelector('#ghostty-theme-options');
const themeCount = document.querySelector('#theme-count');
const customizationStatus = document.querySelector('#customization-status');
const toolbarStatus = document.querySelector('#toolbar-status');
const toolbarToggles = [...document.querySelectorAll('[data-toolbar]')];
const roundedFrameToggle = document.querySelector('#rounded-frame-toggle');
const roundedFrameStatus = document.querySelector('#rounded-frame-status');
const settingsTabs = [...document.querySelectorAll('.settings-tab')];
const settingsPanels = [...document.querySelectorAll('.settings-panel')];
let bindings = [];
let customizationLoaded = false;
let customizationSaveGeneration = 0;

function showPanel(panelId) {
  for (const panel of settingsPanels) panel.hidden = panel.id !== panelId;
  for (const tab of settingsTabs) {
    const active = tab.dataset.panel === panelId;
    tab.classList.toggle('active', active);
    tab.setAttribute('aria-selected', String(active));
    tab.tabIndex = active ? 0 : -1;
  }
  status.hidden = panelId !== 'details';
}

function setStatus(message, kind = '') {
  status.textContent = message;
  status.className = kind;
}

function setIconStatus(message, kind = '') {
  iconStatus.textContent = message;
  iconStatus.className = `inline-status ${kind}`;
}

function setCustomizationStatus(message, kind = '') {
  customizationStatus.textContent = message;
  customizationStatus.className = `inline-status ${kind}`;
}

function setToolbarStatus(message, kind = '') {
  toolbarStatus.textContent = message;
  toolbarStatus.className = `inline-status ${kind}`;
}

function currentToolbarState() {
  return Object.fromEntries(
    toolbarToggles.map(toggle => [toggle.dataset.toolbar, toggle.checked]));
}

function renderPalette(palette) {
  if (!palette) return;
  const root = document.documentElement;
  root.style.colorScheme = palette.isLight ? 'light' : 'dark';
  for (const [property, value] of Object.entries({
    '--bg': palette.background,
    '--panel': palette.panel,
    '--text': palette.text,
    '--muted': palette.muted,
    '--line': palette.line,
    '--accent': palette.accent,
    '--accent-strong': palette.accent,
  })) {
    root.style.setProperty(property, value);
  }
}

function renderCustomization(state, includeThemes = false) {
  if (includeThemes) {
    themeOptions.replaceChildren();
    for (const theme of state.themes || []) {
      const option = document.createElement('option');
      option.value = theme;
      themeOptions.append(option);
    }
    themeCount.textContent = `${(state.themes || []).length} themes`;
  }
  renderPalette(state.palette);
  themePicker.value = state.theme || '';
  for (const toggle of toolbarToggles) {
    toggle.checked = Boolean(state.toolbar?.[toggle.dataset.toolbar]);
  }
}

const delay = milliseconds =>
  new Promise(resolve => setTimeout(resolve, milliseconds));

async function waitForAppliedCustomization(expectedTheme) {
  for (let attempt = 0; attempt < 40; ++attempt) {
    const state = await sendWithPromise('getAppliedCustomization');
    if (!state.ok) {
      throw new Error(state.error || 'Could not read applied customization');
    }
    if (state.theme === expectedTheme && state.palette) return state;
    await delay(50);
  }
  throw new Error('The selected Ghostty theme did not finish applying.');
}

async function saveCustomization() {
  if (!customizationLoaded) return;
  const generation = ++customizationSaveGeneration;
  themePicker.disabled = true;
  toolbarToggles.forEach(toggle => toggle.disabled = true);
  setCustomizationStatus('Applying…');
  setToolbarStatus('Applying…');
  try {
    let state = await sendWithPromise('setCustomization', {
      theme: themePicker.value.trim(),
      toolbar: currentToolbarState(),
    });
    if (!state.ok) throw new Error(state.error || 'Could not save customization');
    if (generation !== customizationSaveGeneration) return;
    if (!state.palette) {
      state = await waitForAppliedCustomization(state.theme || '');
    }
    renderCustomization(state);
    setCustomizationStatus(
      state.theme ? `Using ${state.theme}.` : 'Following your Ghostty configuration.',
      'success');
    setToolbarStatus('Saved. Every web pane has been updated.', 'success');
  } catch (error) {
    setCustomizationStatus(error.message || String(error), 'error');
    setToolbarStatus(error.message || String(error), 'error');
    await loadCustomization();
  } finally {
    if (generation === customizationSaveGeneration) {
      themePicker.disabled = false;
      toolbarToggles.forEach(toggle => toggle.disabled = false);
    }
  }
}

async function loadCustomization() {
  const state = await sendWithPromise('getCustomization');
  if (!state.ok) throw new Error(state.error || 'Could not load customization');
  renderCustomization(state, true);
  customizationLoaded = true;
  themePicker.disabled = false;
  toolbarToggles.forEach(toggle => toggle.disabled = false);
  setCustomizationStatus(
    state.theme ? `Using ${state.theme}.` : 'Following your Ghostty configuration.');
  setToolbarStatus('');
}

function setRoundedFrameStatus(message, kind = '') {
  roundedFrameStatus.textContent = message;
  roundedFrameStatus.className = `inline-status ${kind}`;
}

function renderBindings() {
  const query = search.value.trim().toLocaleLowerCase();
  const visible = bindings.filter(binding =>
    `${binding.key} ${binding.command} ${binding.when} ${binding.source}`
      .toLocaleLowerCase().includes(query));
  shortcutRows.replaceChildren();
  shortcutCount.textContent = `${visible.length} of ${bindings.length}`;
  if (!visible.length) {
    const empty = document.createElement('div');
    empty.className = 'shortcut-empty';
    empty.textContent = 'No matching shortcuts';
    shortcutRows.append(empty);
    return;
  }
  for (const binding of visible) {
    const row = document.createElement('div');
    row.className = 'shortcut-row';
    row.setAttribute('role', 'row');
    for (const value of [binding.key, binding.command, binding.when || 'Always']) {
      const cell = document.createElement('code');
      cell.setAttribute('role', 'cell');
      cell.textContent = value;
      row.append(cell);
    }
    const source = document.createElement('span');
    source.className = 'shortcut-source';
    source.setAttribute('role', 'cell');
    source.textContent = binding.overlapsLater
      ? `${binding.source} · overlaps later rule`
      : binding.source;
    source.classList.toggle('overlap', Boolean(binding.overlapsLater));
    row.append(source);
    shortcutRows.append(row);
  }
}

search.addEventListener('input', renderBindings);
openConfig.addEventListener('click', () => chrome.send('openConfiguration'));
settingsTabs.forEach(tab => tab.addEventListener('click', () => {
  showPanel(tab.dataset.panel);
}));
themePicker.addEventListener('change', saveCustomization);
document.querySelector('#clear-theme').addEventListener('click', () => {
  themePicker.value = '';
  saveCustomization();
});
toolbarToggles.forEach(toggle => {
  toggle.disabled = true;
  toggle.addEventListener('change', saveCustomization);
});
document.querySelector('#show-all-toolbar').addEventListener('click', () => {
  toolbarToggles.forEach(toggle => toggle.checked = true);
  saveCustomization();
});
document.querySelector('#reset-customization').addEventListener('click', async () => {
  themePicker.disabled = true;
  toolbarToggles.forEach(toggle => toggle.disabled = true);
  setCustomizationStatus('Resetting…');
  setToolbarStatus('Resetting…');
  try {
    let state = await sendWithPromise('resetCustomization');
    if (!state.ok) throw new Error(state.error || 'Could not reset customization');
    if (!state.palette) {
      state = await waitForAppliedCustomization(state.theme || '');
    }
    renderCustomization(state);
    setCustomizationStatus('Following your Ghostty configuration.', 'success');
    setToolbarStatus('Default toolbar restored.', 'success');
  } catch (error) {
    setCustomizationStatus(error.message || String(error), 'error');
    setToolbarStatus(error.message || String(error), 'error');
  } finally {
    themePicker.disabled = false;
    toolbarToggles.forEach(toggle => toggle.disabled = false);
  }
});

document.querySelector('#configure').addEventListener('click', () => {
  chrome.send('openConfiguration');
});
document.querySelector('#use-defaults').addEventListener('click', () => {
  search.focus();
});

for (const radio of document.querySelectorAll('input[name="app-icon-mode"]')) {
  radio.addEventListener('change', async () => {
    if (!radio.checked) return;
    iconFieldset.disabled = true;
    setIconStatus('Applying…');
    try {
      const state = await sendWithPromise('setAppIconMode', radio.value);
      if (!state.ok) throw new Error(state.error || 'Could not change the app icon');
      setIconStatus('Saved. The Dock icon updates immediately.', 'success');
    } catch (error) {
      setIconStatus(error.message || String(error), 'error');
      await loadAppIconMode();
    } finally {
      iconFieldset.disabled = false;
    }
  });
}

roundedFrameToggle.addEventListener('change', async () => {
  roundedFrameToggle.disabled = true;
  setRoundedFrameStatus('Applying…');
  try {
    const state = await sendWithPromise(
      'setRoundedFrameEnabled', roundedFrameToggle.checked);
    if (!state.ok) throw new Error(state.error || 'Could not change the frame');
    roundedFrameToggle.checked = state.enabled;
    setRoundedFrameStatus('Saved. Open windows update immediately.', 'success');
  } catch (error) {
    setRoundedFrameStatus(error.message || String(error), 'error');
    await loadRoundedFrame();
  } finally {
    roundedFrameToggle.disabled = false;
  }
});

async function loadRoundedFrame() {
  const state = await sendWithPromise('getRoundedFrameEnabled');
  if (!state.ok) throw new Error(state.error || 'Could not load the frame setting');
  roundedFrameToggle.checked = state.enabled;
  roundedFrameToggle.disabled = false;
  setRoundedFrameStatus('');
}

async function loadAppIconMode() {
  const state = await sendWithPromise('getAppIconMode');
  if (!state.supported) {
    iconFieldset.disabled = true;
    setIconStatus('App icon selection is available on macOS.');
    return;
  }
  const radio = document.querySelector(
    `input[name="app-icon-mode"][value="${state.mode}"]`);
  if (radio) radio.checked = true;
  iconFieldset.disabled = false;
  setIconStatus('');
}

const requestedPanel = new URLSearchParams(location.search).get('section');
showPanel(settingsPanels.some(panel => panel.id === requestedPanel)
  ? requestedPanel : 'appearance');
loadRoundedFrame().catch(error => {
  setRoundedFrameStatus(error.message || String(error), 'error');
});
loadAppIconMode().catch(error => {
  setIconStatus(error.message || String(error), 'error');
});
loadCustomization().catch(error => {
  setCustomizationStatus(error.message || String(error), 'error');
  setToolbarStatus(error.message || String(error), 'error');
  themeCount.textContent = 'Unavailable';
});

try {
  const state = await sendWithPromise('getConfiguration');
  if (!state.ok) throw new Error(state.error || 'Could not load settings');
  const radio = document.querySelector(`input[value="${state.scheme}"]`);
  if (radio) radio.checked = true;
  bindings = state.bindings || [];
  const lastRule = new Map();
  bindings.forEach((binding, index) => {
    const signature = `${binding.key}\u0000${binding.when}`;
    const earlier = lastRule.get(signature);
    if (earlier !== undefined) bindings[earlier].overlapsLater = true;
    lastRule.set(signature, index);
  });
  openConfig.disabled = !state.fileFound;
  renderBindings();
  setStatus(state.fileFound
    ? `Loaded ${state.bindingCount} custom shortcut rule(s) from cmux.json.`
    : 'Using built-in shortcuts; create cmux.json to customize them.');
} catch (error) {
  setStatus(error.message || String(error), 'error');
}
)JS";

enum class Resource { kNone, kHtml, kCss, kJs };

Resource ResourceForPath(const std::string& path) {
  // WebUIDataSource passes the main-document query string through to custom
  // request filters (for example, "?section=appearance"). Match only the
  // resource path so deep links still serve the settings document.
  std::string_view resource_path(path);
  const size_t suffix_start = resource_path.find_first_of("?#");
  if (suffix_start != std::string_view::npos) {
    resource_path = resource_path.substr(0, suffix_start);
  }
  while (!resource_path.empty() && resource_path.front() == '/') {
    resource_path.remove_prefix(1);
  }

  if (resource_path.empty() || resource_path == "index.html") {
    return Resource::kHtml;
  }
  if (resource_path == "configure.css") {
    return Resource::kCss;
  }
  if (resource_path == "configure.js") {
    return Resource::kJs;
  }
  return Resource::kNone;
}

void ServeResource(const std::string& path,
                   content::WebUIDataSource::GotDataCallback callback) {
  std::string data;
  switch (ResourceForPath(path)) {
    case Resource::kHtml:
      data = kIndexHtml;
      break;
    case Resource::kCss:
      data = kCss;
      break;
    case Resource::kJs:
      data = kJs;
      break;
    case Resource::kNone:
      break;
  }
  std::move(callback).Run(
      base::MakeRefCounted<base::RefCountedString>(std::move(data)));
}

struct ConfigResult {
  bool ok = false;
  bool file_found = false;
  ShortcutModifierScheme scheme = kDefaultShortcutModifierScheme;
  int binding_count = 0;
  std::vector<KeyRule> built_in_rules;
  std::vector<KeyRule> user_rules;
  std::string error;
};

constexpr size_t kMaxKeymapConfigBytes = 1024 * 1024;

enum class ConfigReadStatus { kMissing, kRead, kError };

ConfigReadStatus ReadConfigFile(const base::FilePath& path,
                                std::string* json) {
  if (base::ReadFileToStringWithMaxSize(path, json,
                                        kMaxKeymapConfigBytes)) {
    return ConfigReadStatus::kRead;
  }
  // A failed read of an existing file (permissions, directory, or oversized
  // input) must not be treated as a blank config and overwritten.
  return base::PathExists(path) ? ConfigReadStatus::kError
                                : ConfigReadStatus::kMissing;
}

scoped_refptr<base::SequencedTaskRunner> ConfigTaskRunner() {
  static base::NoDestructor<scoped_refptr<base::SequencedTaskRunner>> runner(
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN}));
  return *runner;
}

ConfigResult LoadConfigurationOnWorker() {
  ConfigResult result;
  const base::FilePath path =
      base::FilePath::FromUTF8Unsafe(CmuxConfigPath());
  std::string json;
  const ConfigReadStatus read = ReadConfigFile(path, &json);
  if (read == ConfigReadStatus::kMissing) {
    result.built_in_rules = DefaultKeymapRules(BUILDFLAG(IS_MAC));
    result.ok = true;
    return result;
  }
  if (read == ConfigReadStatus::kError) {
    result.error = "cmux.json could not be read safely.";
    return result;
  }
  result.file_found = true;
  KeymapLoadResult keymap = ParseKeymapJson(json);
  if (!keymap.valid) {
    result.error =
        "cmux.json is not valid JSONC. The active shortcuts were not changed.";
    return result;
  }
  result.scheme = keymap.modifier_scheme.value_or(kDefaultShortcutModifierScheme);
  result.binding_count = static_cast<int>(keymap.rules.size());
  result.built_in_rules =
      DefaultKeymapRules(BUILDFLAG(IS_MAC), result.scheme);
  result.user_rules = keymap.rules;
  result.ok = true;
  return result;
}

base::DictValue ResultToValue(const ConfigResult& result) {
  base::DictValue value;
  value.Set("ok", result.ok);
  value.Set("fileFound", result.file_found);
  value.Set("scheme", ShortcutModifierSchemeToString(result.scheme));
  value.Set("bindingCount", result.binding_count);
  base::ListValue bindings;
  auto append_rules = [&](const std::vector<KeyRule>& rules,
                          std::string_view source) {
    for (const KeyRule& rule : rules) {
      base::DictValue binding;
      const std::vector<KeyChord>& sequence =
          rule.sequence.empty() ? std::vector<KeyChord>{rule.chord}
                                : rule.sequence;
      binding.Set("key", FormatKeySequence(sequence, BUILDFLAG(IS_MAC)));
      binding.Set("command", rule.command);
      binding.Set("when", rule.when_text);
      binding.Set("source", std::string(source));
      bindings.Append(std::move(binding));
    }
  };
  append_rules(result.built_in_rules, "Built-in");
  append_rules(result.user_rules, "cmux.json");
  value.Set("bindings", std::move(bindings));
  if (!result.error.empty()) {
    value.Set("error", result.error);
  }
  return value;
}

base::DictValue CustomizationToValue(
    const LayoutConfig& config,
    const std::vector<std::string>* themes = nullptr) {
  base::DictValue value;
  value.Set("ok", true);
  value.Set("theme", config.ghostty_theme_name);
  if (themes) {
    base::ListValue theme_values;
    for (const std::string& theme : *themes) {
      theme_values.Append(theme);
    }
    value.Set("themes", std::move(theme_values));
  }
  base::DictValue toolbar;
  toolbar.Set("back", config.toolbar_show_back);
  toolbar.Set("forward", config.toolbar_show_forward);
  toolbar.Set("reload", config.toolbar_show_reload);
  toolbar.Set("home", config.toolbar_show_home);
  toolbar.Set("extensions", config.toolbar_show_extensions);
  toolbar.Set("downloads", config.toolbar_show_downloads);
  toolbar.Set("media", config.toolbar_show_media);
  toolbar.Set("profile", config.toolbar_show_profile);
  toolbar.Set("menu", config.toolbar_show_menu);
  value.Set("toolbar", std::move(toolbar));
  const std::optional<CmuxAppliedTheme> applied = GetCmuxAppliedTheme();
  if (applied && applied->name == config.ghostty_theme_name) {
    auto css_color = [](CmuxArgb color) {
      return base::StringPrintf("#%02x%02x%02x", CmuxArgbR(color),
                                CmuxArgbG(color), CmuxArgbB(color));
    };
    base::DictValue palette;
    palette.Set("isLight", applied->palette.is_light);
    palette.Set("background", css_color(applied->palette.window_bg));
    palette.Set("panel", css_color(applied->palette.content_bg));
    palette.Set("text", css_color(applied->palette.tab_active_text));
    palette.Set("muted", css_color(applied->palette.tab_idle_text));
    palette.Set("line", css_color(applied->palette.pane_idle_border));
    palette.Set("accent", css_color(applied->palette.layout_accent));
    value.Set("palette", std::move(palette));
  }
  return value;
}

void ResetCustomization(LayoutConfig* config) {
  if (!config) {
    return;
  }
  const LayoutConfig defaults;
  config->ghostty_theme = defaults.ghostty_theme;
  config->ghostty_theme_name = defaults.ghostty_theme_name;
  config->toolbar_show_back = defaults.toolbar_show_back;
  config->toolbar_show_forward = defaults.toolbar_show_forward;
  config->toolbar_show_reload = defaults.toolbar_show_reload;
  config->toolbar_show_home = defaults.toolbar_show_home;
  config->toolbar_show_extensions = defaults.toolbar_show_extensions;
  config->toolbar_show_downloads = defaults.toolbar_show_downloads;
  config->toolbar_show_media = defaults.toolbar_show_media;
  config->toolbar_show_profile = defaults.toolbar_show_profile;
  config->toolbar_show_menu = defaults.toolbar_show_menu;
}

bool ReadToolbarBoolean(const base::DictValue& toolbar,
                        const char* key,
                        bool* value) {
  std::optional<bool> setting = toolbar.FindBool(key);
  if (!setting) {
    return false;
  }
  *value = *setting;
  return true;
}

void ReloadCustomizationConsumers(bool theme_changed) {
  if (!theme_changed) {
    return;
  }
  ReloadCmuxCustomization();
}

class CmuxConfigureHandler : public content::WebUIMessageHandler {
 public:
  CmuxConfigureHandler() = default;
  CmuxConfigureHandler(const CmuxConfigureHandler&) = delete;
  CmuxConfigureHandler& operator=(const CmuxConfigureHandler&) = delete;
  ~CmuxConfigureHandler() override = default;

  void RegisterMessages() override {
    web_ui()->RegisterMessageCallback(
        "getConfiguration",
        base::BindRepeating(&CmuxConfigureHandler::HandleGetConfiguration,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "openConfiguration",
        base::BindRepeating(&CmuxConfigureHandler::HandleOpenConfiguration,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "getAppIconMode",
        base::BindRepeating(&CmuxConfigureHandler::HandleGetAppIconMode,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "setAppIconMode",
        base::BindRepeating(&CmuxConfigureHandler::HandleSetAppIconMode,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "getCustomization",
        base::BindRepeating(&CmuxConfigureHandler::HandleGetCustomization,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "getAppliedCustomization",
        base::BindRepeating(
            &CmuxConfigureHandler::HandleGetAppliedCustomization,
            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "setCustomization",
        base::BindRepeating(&CmuxConfigureHandler::HandleSetCustomization,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "resetCustomization",
        base::BindRepeating(&CmuxConfigureHandler::HandleResetCustomization,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "getRoundedFrameEnabled",
        base::BindRepeating(
            &CmuxConfigureHandler::HandleGetRoundedFrameEnabled,
            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "setRoundedFrameEnabled",
        base::BindRepeating(
            &CmuxConfigureHandler::HandleSetRoundedFrameEnabled,
            base::Unretained(this)));
  }

  void OnJavascriptDisallowed() override {
    // Cancels replies from the previous document. A later document can obtain
    // fresh weak pointers after calling AllowJavascript().
    weak_factory_.InvalidateWeakPtrs();
  }

 private:
  Profile* profile() {
    return Profile::FromBrowserContext(
        web_ui()->GetWebContents()->GetBrowserContext());
  }

  void HandleGetRoundedFrameEnabled(const base::ListValue& args) {
    if (args.size() != 1 || !args[0].is_string()) {
      return;
    }
    AllowJavascript();
    base::DictValue state;
    state.Set("ok", true);
    state.Set("enabled",
              profile()->GetPrefs()->GetBoolean(prefs::kHeliumRoundedFrame));
    ResolveJavascriptCallback(args[0].Clone(),
                              base::Value(std::move(state)));
  }

  void HandleSetRoundedFrameEnabled(const base::ListValue& args) {
    if (args.size() != 2 || !args[0].is_string() || !args[1].is_bool()) {
      return;
    }
    AllowJavascript();
    const bool enabled = args[1].GetBool();
    profile()->GetPrefs()->SetBoolean(prefs::kHeliumRoundedFrame, enabled);
    base::DictValue state;
    state.Set("ok", true);
    state.Set("enabled", enabled);
    ResolveJavascriptCallback(args[0].Clone(),
                              base::Value(std::move(state)));
  }

  void HandleGetAppliedCustomization(const base::ListValue& args) {
    if (args.size() != 1 || !args[0].is_string()) {
      return;
    }
    AllowJavascript();
    const LayoutConfig config =
        GetPublishedLayoutConfig().value_or(LayoutConfig());
    ResolveJavascriptCallback(
        args[0].Clone(), base::Value(CustomizationToValue(config)));
  }

  void HandleGetCustomization(const base::ListValue& args) {
    if (args.size() != 1 || !args[0].is_string()) {
      return;
    }
    AllowJavascript();
    const LayoutConfig config =
        GetPublishedLayoutConfig().value_or(LayoutConfig());
    ConfigTaskRunner()->PostTaskAndReplyWithResult(
        FROM_HERE, base::BindOnce(&ListGhosttyThemes),
        base::BindOnce(&CmuxConfigureHandler::ReplyCustomization,
                       weak_factory_.GetWeakPtr(), args[0].Clone(), config));
  }

  void HandleSetCustomization(const base::ListValue& args) {
    if (args.size() != 2 || !args[0].is_string() || !args[1].is_dict()) {
      return;
    }
    AllowJavascript();
    LayoutConfig config =
        GetPublishedLayoutConfig().value_or(LayoutConfig());
    const std::string previous_theme = config.ghostty_theme_name;
    const base::DictValue& input = args[1].GetDict();
    const std::string* theme = input.FindString("theme");
    const base::DictValue* toolbar = input.FindDict("toolbar");
    base::DictValue result;
    if (!theme || !toolbar ||
        theme->find_first_of("\r\n") != std::string::npos ||
        (!theme->empty() && *theme != config.ghostty_theme_name &&
         !ghostty_themes_.contains(*theme)) ||
        !ReadToolbarBoolean(*toolbar, "back", &config.toolbar_show_back) ||
        !ReadToolbarBoolean(*toolbar, "forward",
                            &config.toolbar_show_forward) ||
        !ReadToolbarBoolean(*toolbar, "reload",
                            &config.toolbar_show_reload) ||
        !ReadToolbarBoolean(*toolbar, "home", &config.toolbar_show_home) ||
        !ReadToolbarBoolean(*toolbar, "extensions",
                            &config.toolbar_show_extensions) ||
        !ReadToolbarBoolean(*toolbar, "downloads",
                            &config.toolbar_show_downloads) ||
        !ReadToolbarBoolean(*toolbar, "media",
                            &config.toolbar_show_media) ||
        !ReadToolbarBoolean(*toolbar, "profile",
                            &config.toolbar_show_profile) ||
        !ReadToolbarBoolean(*toolbar, "menu", &config.toolbar_show_menu)) {
      result.Set("ok", false);
      result.Set("error", "The customization request was invalid.");
    } else {
      config.ghostty_theme = true;
      config.ghostty_theme_name = *theme;
      SaveLayoutConfig(config);
      ReloadCustomizationConsumers(previous_theme != config.ghostty_theme_name);
      result = CustomizationToValue(config);
    }
    ResolveJavascriptCallback(args[0].Clone(),
                              base::Value(std::move(result)));
  }

  void HandleResetCustomization(const base::ListValue& args) {
    if (args.size() != 1 || !args[0].is_string()) {
      return;
    }
    AllowJavascript();
    LayoutConfig config =
        GetPublishedLayoutConfig().value_or(LayoutConfig());
    const std::string previous_theme = config.ghostty_theme_name;
    ResetCustomization(&config);
    SaveLayoutConfig(config);
    ReloadCustomizationConsumers(previous_theme != config.ghostty_theme_name);
    ResolveJavascriptCallback(
        args[0].Clone(),
        base::Value(CustomizationToValue(config)));
  }

  void HandleGetAppIconMode(const base::ListValue& args) {
    if (args.size() != 1 || !args[0].is_string()) {
      return;
    }
    AllowJavascript();
    base::DictValue state;
    state.Set("ok", true);
#if BUILDFLAG(IS_MAC)
    state.Set("supported", true);
    state.Set("mode", std::string(AppIconModeToString(GetAppIconMode())));
#else
    state.Set("supported", false);
    state.Set("mode", "automatic");
#endif
    ResolveJavascriptCallback(args[0].Clone(),
                              base::Value(std::move(state)));
  }

  void HandleSetAppIconMode(const base::ListValue& args) {
    if (args.size() != 2 || !args[0].is_string() || !args[1].is_string()) {
      return;
    }
    AllowJavascript();
    base::DictValue state;
#if BUILDFLAG(IS_MAC)
    std::optional<AppIconMode> mode =
        AppIconModeFromString(args[1].GetString());
    if (!mode) {
      state.Set("ok", false);
      state.Set("error", "Unknown app icon mode.");
    } else {
      SetAppIconMode(*mode);
      state.Set("ok", true);
      state.Set("mode", std::string(AppIconModeToString(*mode)));
    }
#else
    state.Set("ok", false);
    state.Set("error", "App icon selection is available on macOS.");
#endif
    ResolveJavascriptCallback(args[0].Clone(),
                              base::Value(std::move(state)));
  }

  void HandleOpenConfiguration(const base::ListValue&) {
    platform_util::OpenItem(
        Profile::FromBrowserContext(
            web_ui()->GetWebContents()->GetBrowserContext()),
        base::FilePath::FromUTF8Unsafe(CmuxConfigPath()),
        platform_util::OPEN_FILE, platform_util::OpenOperationCallback());
  }

  void HandleGetConfiguration(const base::ListValue& args) {
    if (args.size() != 1 || !args[0].is_string()) {
      return;
    }
    AllowJavascript();
    ConfigTaskRunner()->PostTaskAndReplyWithResult(
        FROM_HERE, base::BindOnce(&LoadConfigurationOnWorker),
        base::BindOnce(&CmuxConfigureHandler::Reply,
                       weak_factory_.GetWeakPtr(), args[0].Clone()));
  }

  void Reply(base::Value callback_id, ConfigResult result) {
    if (!IsJavascriptAllowed()) {
      return;
    }
    ResolveJavascriptCallback(callback_id,
                              base::Value(ResultToValue(result)));
  }

  void ReplyCustomization(base::Value callback_id,
                          LayoutConfig config,
                          std::vector<std::string> themes) {
    if (!IsJavascriptAllowed()) {
      return;
    }
    ghostty_themes_.clear();
    ghostty_themes_.insert(themes.begin(), themes.end());
    ResolveJavascriptCallback(
        callback_id,
        base::Value(CustomizationToValue(config, &themes)));
  }

  std::set<std::string> ghostty_themes_;
  base::WeakPtrFactory<CmuxConfigureHandler> weak_factory_{this};
};

class CmuxConfigureUIConfig : public content::WebUIConfig {
 public:
  CmuxConfigureUIConfig()
      : WebUIConfig(content::kChromeUIScheme, kCmuxConfigureChromeHost) {}
  ~CmuxConfigureUIConfig() override = default;

  std::unique_ptr<content::WebUIController> CreateWebUIController(
      content::WebUI* web_ui,
      const GURL&) override {
    return std::make_unique<CmuxConfigureUI>(web_ui);
  }
};

}  // namespace

void LoadCmuxThemePickerState(CmuxThemePickerStateCallback callback) {
  const LayoutConfig config =
      GetPublishedLayoutConfig().value_or(LayoutConfig());
  ConfigTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&ListGhosttyThemePreviews),
      base::BindOnce(
          [](CmuxThemePickerStateCallback callback,
             std::string selected_theme,
             std::vector<GhosttyThemePreview> themes) {
            std::move(callback).Run(std::move(themes),
                                    std::move(selected_theme));
          },
          std::move(callback), config.ghostty_theme_name));
}

bool ApplyCmuxGhosttyTheme(const std::string& theme) {
  if (theme.find_first_of("\r\n") != std::string::npos) {
    return false;
  }
  LayoutConfig config =
      GetPublishedLayoutConfig().value_or(LayoutConfig());
  const std::string previous_theme = config.ghostty_theme_name;
  config.ghostty_theme = true;
  config.ghostty_theme_name = theme;
  if (!SaveLayoutConfig(config)) {
    return false;
  }
  ReloadCustomizationConsumers(previous_theme != config.ghostty_theme_name);
  return true;
}

CmuxConfigureUI::CmuxConfigureUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  web_ui->AddMessageHandler(std::make_unique<CmuxConfigureHandler>());
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(), kCmuxConfigureChromeHost);
  source->SetRequestFilter(
      base::BindRepeating(
          [](const std::string& path) {
            return ResourceForPath(path) != Resource::kNone;
          }),
      base::BindRepeating(&ServeResource));
  // Keep the page self-contained. The only cross-origin script is Chromium's
  // reviewed promise bridge; network, object, frame, and inline-script loads
  // remain denied.
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::DefaultSrc, "default-src 'none';");
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::ScriptSrc,
      "script-src 'self' chrome://resources;");
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::StyleSrc, "style-src 'self';");
}

CmuxConfigureUI::~CmuxConfigureUI() = default;

void RegisterCmuxConfigureWebUI() {
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;
  content::WebUIConfigMap::GetInstance().AddWebUIConfig(
      std::make_unique<CmuxConfigureUIConfig>());
}

}  // namespace cmux
