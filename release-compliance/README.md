# Public release compliance inputs

This directory is the fail-closed input boundary for public binary releases.
The release workflow will publish only when both reviewed files below exist
and the repository variable `CMUX_RELEASE_COMPLIANCE_READY` is `true`:

- `THIRD_PARTY_NOTICES.html`
- `third-party-notices.spdx.json`

Do not add placeholders or mechanically convert the source-level
`THIRD_PARTY_NOTICES.md`. The two files must describe the exact target
artifacts after completing the open work recorded there, including Chromium's
generated credits, Ghostty/Rust dependencies, the uBlock package, selected
themes and fonts, GPL source availability, and any LGPL relinking materials.

The human-readable and machine-readable inventories must be reviewed together.
After they land, the release workflow copies them into the artifact set before
provenance and `SHA256SUMS` are generated.
