// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_EXTENSIONS_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_EXTENSIONS_H_

class Profile;

namespace cmux {

extern const char kUBlockOriginExtensionId[];
extern const char kBitwardenExtensionId[];

// Historical pre-profile hook retained for startup call sites. Bitwarden
// external registration now runs from RegisterProfileExtensions(), where the
// profile registry can skip redundant warm-profile work.
void RegisterUserExternalExtensions();

// First-run migration for Chrome's extension toolbar pin pref. Writes the
// default cmux preinstall pins only if the pref is still unset/default; an
// explicitly empty user list is preserved.
void EnsureDefaultPinnedExtensions(Profile* profile);

// Loads cmux-bundled unpacked extensions for `profile` and schedules optional
// CMUX_EXT_SELFTEST logging.
void RegisterProfileExtensions(Profile* profile);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_EXTENSIONS_H_
