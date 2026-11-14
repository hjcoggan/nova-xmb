# nova branch

OpenXMB as the console menu of [nova-nix](https://github.com/hjcoggan/nova-nix),
a NixOS-based handheld system for the Retroid Pocket Nova (1280x960, 4:3).

The branch is upstream `phenom64/OpenXMB` at `c4bccc8` (0.3.0-beta.3) plus the
commits above it. It is built by nova-nix; nothing here is specific to Nix.

What the branch adds:

- 4:3 layout: `ui.scale`, overlay canvas for dialogs, choice panel and the
  on-screen keyboard, status bar sizing.
- Console mode: trimmed crossbar categories, hidden empty categories, power
  column, Nintendo-layout button badges.
- Games from `~/Games/<id>/game.json`, launched through `nova-launch`; the menu
  idles while a game runs.
- Pause overlay driven over a Unix socket (`PAUSE <id>`, `RESUME`, `QUIT`),
  drawn transparently over the frozen game.
- Status bar battery, charging and Wi-Fi indicators.
- Settings wired to the hardware: brightness, Wi-Fi, Bluetooth, aeroplane mode.

## Building

Configure with `-DAUROREENGINE_LOCAL=<engine checkout>` where the checkout is
AuroreEngine `52c724e3` with `nova/aurore-engine.patch` applied
(`patch -p1 < nova/aurore-engine.patch`). Other options used by nova-nix:
`-DENABLE_BROWSER=OFF -DENABLE_DISC_MEDIA=OFF -DENABLE_LIBRETRO=OFF
-DOPENXMB_BUILD_NATIVE_TESTS=OFF` and `-DOPENXMB_DEFAULT_UI_SCALE=1.5` in
`CMAKE_CXX_FLAGS`.

## Icons

The crossbar, settings and power icons under `assets/shell/icons` are
[Tabler Icons](https://tabler.io/icons) (MIT), aspect-corrected for the menu.
