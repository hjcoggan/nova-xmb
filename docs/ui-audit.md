# OpenXMB console menu — UI audit

Baseline: nova-nix pin `520b7f2` of nova-xmb, on `heath@nova.lan`
(1280x960 panel, 100 fps frame limit, Interface Scale 1.5, Screen
Brightness 40). All screenshots are grim captures of the panel taken while
walking the menu with synthetic key input. SSIDs, MAC addresses and other
private values have been painted out; the long-title game is a temporary fake
manifest made for this audit.

Screenshots are under [`ui-audit-shots/`](ui-audit-shots/). A row's screenshot
is named after the file linked there.

## Phase 1 — inventory

| # | Screen / state | Screenshot | Problems found |
|---|---|---|---|
| 1 | Crossbar — Games | [00-crossbar-games-baseline.png](ui-audit-shots/00-crossbar-games-baseline.png) | Crossbar icons sit 80 px from the top edge; a sliver of the neighbouring column icon is clipped at x=0 when scrolling. Game label and cover share a focus card whose width grows with the label (see 35). |
| 2 | Crossbar — Android | [01-crossbar-android.png](ui-audit-shots/01-crossbar-android.png) | Same clipped sliver at the left edge. Android column contains one app; no empty state is visible. |
| 3 | Crossbar — Settings | [03-crossbar-settings.png](ui-audit-shots/03-crossbar-settings.png) | Focus card is 715 px wide but the row below ("Display Settings") has no card — inconsistent row shapes. Hint bar shows "A Select" only. |
| 4 | Crossbar — Power | [04-crossbar-power.png](ui-audit-shots/04-crossbar-power.png) | Items Sleep / Restart / Power Off. Crossbar icon spacing jumps between columns as the strip scrolls. |
| 5 | Crossbar — Power, second row | [05-power-restart.png](ui-audit-shots/05-power-restart.png) | Same three destructive actions are repeated in the Power menu overlay (#22) — duplication. |
| 6 | Settings category list | [14-settings-back-level.png](ui-audit-shots/14-settings-back-level.png) | Category icons in the left rail are cut off at x=0 (gear, palette). No B hint at the root even though B returns to the crossbar. |
| 7 | Settings category list, lower | [17-settings-cat-4.png](ui-audit-shots/17-settings-cat-4.png) | "Network Settings" label at the focus size is 340 px wide; the card edge follows it, so cards have different widths on the same list. |
| 8 | Settings category list, end | [21-settings-cat-7b.png](ui-audit-shots/21-settings-cat-7b.png) | List stops at System Settings (no wrap). Consistent, but no end-of-list affordance. |
| 9 | Theme settings sub-page | [07-settings-root.png](ui-audit-shots/07-settings-root.png) | Sub-rail icons clipped at the left edge. The value ("Blue") sits 100 px from the right edge, not aligned to a scale. Description is one line here but wraps on longer text (see 11). |
| 10 | Theme settings, Wave row | [09-settings-down2.png](ui-audit-shots/09-settings-down2.png) | No clipping; used as the aligned reference. |
| 11 | Theme settings, Restore Original Theme | [12-settings-down5.png](ui-audit-shots/12-settings-down5.png) | **Overflow:** description wraps to two lines; "appearance." is drawn below the focus card and overlaps the divider. |
| 12 | Display settings sub-page | [32-display-root.png](ui-audit-shots/32-display-root.png) | Screen Brightness 40, Interface Scale 1.5. Left rail icons clipped. |
| 13 | Display settings, Frame Limit | [33-display-down2.png](ui-audit-shots/33-display-down2.png) | Value "100" right-aligned to a different x than "40" above — no shared value slot. |
| 14 | Value picker (Show FPS) | [35-fps-picker.png](ui-audit-shots/35-fps-picker.png) | Glass sheet is 480 px wide; title is a bare value. Hint bar "A Select / B Back" matches. |
| 15 | System settings sub-page | [22-system-settings.png](ui-audit-shots/22-system-settings.png) | Sub-rail has six icons, all clipped at the left edge; labels start at x=505 with no shared gutter. |
| 16 | System settings, System Information | [25-system-down3.png](ui-audit-shots/25-system-down3.png) | **Overflow:** two-line description escapes the card. Value "OpenXMB 0.?" is a placeholder string. |
| 17 | System settings, Licences | [26-system-down4.png](ui-audit-shots/26-system-down4.png) | **Overflow:** two-line description clipped at card bottom. |
| 18 | System settings, Restore Default Settings | [27-system-down5.png](ui-audit-shots/27-system-down5.png) | **Overflow:** two-line description clipped at card bottom. |
| 19 | Show FPS on — Settings | [38-fps-settings.png](ui-audit-shots/38-fps-settings.png) | Baseline FPS 98.30. FPS text sits flush at (0,0) with no safe-area inset. |
| 20 | Show FPS on — crossbar | [41-fps-crossbar.png](ui-audit-shots/41-fps-crossbar.png) | Baseline FPS 99.43. |
| 21 | Network settings sub-page | [51-network-root.png](ui-audit-shots/51-network-root.png) | Wi-Fi value is the connected SSID — a live network name in a settings value slot with no max width. (SSID painted out here.) |
| 22 | Network settings, Bluetooth Devices | [52-net-down2.png](ui-audit-shots/52-net-down2.png) | Value "Scan & Pair" sits at x=1020, not on a shared value column. |
| 23 | Wi-Fi network list dialog | [54-wifi-list.png](ui-audit-shots/54-wifi-list.png) | Title "Select Wi-Fi Network" wraps raggedly into two lines. Network entry shows raw `[Connected] (psk ****)` debug text on three lines. SSID painted out. |
| 24 | Bluetooth scan — loading | [60-bt-devices.png](ui-audit-shots/60-bt-devices.png) | Loading state is a centred glass card with only "B Back" — no A hint; acceptable but inconsistent with other sheets. |
| 25 | Bluetooth device list dialog | [61-bt-scan-result.png](ui-audit-shots/61-bt-scan-result.png) | **Overflow + privacy:** raw MAC addresses wrap mid-string onto a second line. (MACs painted out here.) |
| 26 | Power menu overlay | [63-power-menu.png](ui-audit-shots/63-power-menu.png) | Duplicates the Power crossbar column. Opens at 0,0-adjacent x=780 with a different radius from other sheets. |
| 27 | Pause overlay over menu | [65-pause-overlay.png](ui-audit-shots/65-pause-overlay.png) | **Two hint bars overlap** at the bottom: the underlying screen's "A Select / B Back" plus the overlay's "B Resume". Placeholder title "Game" when PAUSE has no id. |
| 28 | Pause overlay — Volume focused | [67b-volume-steady.png](ui-audit-shots/67b-volume-steady.png) | Same duplicate hint bar. Slider arrows appear only on focus, so the row's content shifts. |
| 29 | Pause overlay — Brightness focused | [69-brightness-overlay.png](ui-audit-shots/69-brightness-overlay.png) | The left slider arrow collides with the "Brightness" label (`Brightness◀`). Same duplicate hint bar. |
| 30 | Pause overlay over a real game | [81-real-pause.png](ui-audit-shots/81-real-pause.png) | Correct title ("Clover Pit") and one hint bar once the menu background is transparent. Slider value column is aligned. |
| 31 | Game Options (Y) | [72-game-options.png](ui-audit-shots/72-game-options.png) | Clean. Sheet has a different radius from the value picker. |
| 32 | Game Information dialog | [75-game-info.png](ui-audit-shots/75-game-info.png) | Hint reads "A Enter" — an action name, not a verb. Long title/file path would have no max width. |
| 33 | Launch / loading card | [79-launch-card.png](ui-audit-shots/79-launch-card.png) | Clean. No hint bar during loading (expected). |
| 34 | Game running | [80-game-running.png](ui-audit-shots/80-game-running.png) | Menu draws nothing while a game is up (as designed). |
| 35 | Game list — long focused title | [85-gamedown12.png](ui-audit-shots/85-gamedown12.png) | **Critical overflow:** both the label and the focus card extend past the right edge; the title is cut off mid-word and is not scrolled or ellipsised. |
| 36 | Game list — long unfocused title | [85-gamedown11.png](ui-audit-shots/85-gamedown11.png) | Unfocused long title also runs off-screen instead of truncating with an ellipsis. |
| 37 | System Information | [99-system-info.png](ui-audit-shots/99-system-info.png) | Legacy full-screen desktop About page: logo and every line of prose are clipped at both edges; hint bar says "Button 12" instead of A; a stray globe icon overlaps the hints. Does not use glass. |
| 38 | Error state (launch failure) | — | Not reachable on the device with the temporary failing manifest (the library loader skipped it); the `message_overlay` error path exists in code. Verified in code only — see the closing checklist. |
| 39 | Empty game column / empty list | — | No empty column was present on this device. `library_menu` has an empty-list path in code; not verified on device. |

## Cross-cutting problems

1. **No text slots.** Labels, values, descriptions, dialog titles, Wi-Fi/BT
   names and hint text have no max width. Long content grows the glass card
   itself or runs off the panel. No ellipsis, no scroll.
2. **Magic numbers everywhere.** `SettingsSceneMetrics` alone has 25 literal
   measurements; there is no one spacing or radius scale. The same kind of
   container has different padding on different screens.
3. **Radii are hand-picked** per widget (34 px focus lens, 36 px `card_radius`,
   dialog radii that differ), and no continuous-corner path is used.
4. **Type styles are not shared** — sizes are literals in each renderer, with
   two description sizes and two label sizes in the same scene.
5. **Drawing breaks the rules in two places**: the legacy System Information
   page draws its own full-screen background/globe, and the pause overlay
   draws a second hint bar instead of going through `shell::render_hint_bar`.
6. **Controls**: A confirms and B goes back everywhere except System
   Information ("Button 12"). The crossbar, game list and settings lists stop
   at the end; no list wraps. Focus is visible except on the clipped left rail.
7. **States**: loading exists for Bluetooth and update, error exists for
   launch, but empty states are unproven; the Bluetooth dialog title and
   Wi-Fi list are ragged; two screens show a flash of the underlying hint bar.

## Phase 3 — removal candidates (owner-approved, except Sparkles/Wave)

Each line is a setting, entry or screen that does not serve a handheld game
launcher with sleep, pause, resume and quit, or that duplicates another one.
The owner approved these with two changes: Sparkles and Wave stay, and the
AuroreEngine patch is acceptable. The deletions are in the feedback-round
commits below; Sparkles and Wave remain in the Theme group.

| Candidate | One-line reason |
|---|---|
| Legacy **System Information** full-screen page | Clipped legacy desktop About screen, no glass, "Button 12" hints; the same facts are in nova-nix's About and System Settings already; removes a whole non-glass screen. |
| **Licences** settings entry | Static legal text duplicates `LICENSE`/`NOTICE` in both repos and is not part of launching or pausing games. |
| **Restore Default Settings** and **Restore Original Theme** | Destructive reset entries with long clipped descriptions; not needed on a single-user kiosk and duplicate nova-nix's config management. |
| **Startup Intro** setting | Decorative-only startup animation; no function on a console that powers straight into the menu. |
| **Sparkles** (background particles) and **Wave** | Decorative-only animations; the rules forbid decorative-only motion, and they cost frame rate. |
| **Interface Scale**, **Frame Limit**, **Interface Text Size** | Desktop tuning knobs; nova-nix already fixes scale at 1.5 and the frame limit at 100, so these duplicate system defaults. |
| **Show GPU Memory**, **Open Log Folder**, **Decoder Information** | Desktop debug toggles; inaccessible in console mode and of no use from a gamepad. |
| **Power menu overlay** (Sleep/Restart/Power Off) | Exact duplicate of the Power crossbar column; keep the crossbar and keep the `POWER_MENU` socket message mapping onto it. |
| **Launch at Login**, **Window Mode**, **Display**, **Resolution**, **VSync**, **Anti-aliasing**, **Glass Icons** | Desktop-only settings filtered out of console mode; dead rows in the catalog. |
| **Music**, **Browser**, **Network > Friends/PlayStation** categories | Desktop/media/browser features unrelated to launching games; already hidden by nova-nix config. |

Explicitly **not** proposed for removal (system dependencies, leave intact):
`nova-launch`, `nova-home-ctl`, `nova-update`, `nova-powerd` integrations;
the `nova-pause.sock` verbs `PAUSE <id>`, `RESUME`, `QUIT`, `POWER_MENU`;
Volume, Brightness, Wi-Fi, Bluetooth, Aeroplane Mode, Screen Brightness,
Show FPS (needed for the done check), and the language/date/time settings the
tools read.

Dependency grep before proposing removals: 18 references to the
`nova-launch` / `nova-home-ctl` / `nova-update` / `nova-powerd` /
`nova-pause.sock` / `POWER_MENU` interfaces in nova-xmb; none of the candidate
setting keys above are read by nova-nix's `tools/` or `modules/`. Screen
Brightness and Volume *are* used through sysfs and `wpctl` (and `nova-powerd`
enforces a minimum backlight), so they are not candidates.

## Phase 2 — before / after on the device

The menu was rebuilt from this branch with
`nix build .#openxmb --override-input nova-xmb path:<this checkout>`, switched
into `heath`, and the session restarted. Screenshots were taken on the panel
at 1280x960. The long-title game was a temporary fake manifest, removed after
the captures; the SSID/BSSID rows in the Wi-Fi screenshots are painted out.

| Screen | Before | After | What changed |
|---|---|---|---|
| Long game title, focused | [85-gamedown12.png](ui-audit-shots/85-gamedown12.png) | [after-long-game-focused.png](ui-audit-shots/after-long-game-focused.png), [earlier frame](ui-audit-shots/after-long-game-focused-earlier.png) | Card ends inside the panel; title scrolls a whole glyph at a time (two frames show different offsets). |
| Long game title, unfocused | [85-gamedown11.png](ui-audit-shots/85-gamedown11.png) | [after-long-game-unfocused.png](ui-audit-shots/after-long-game-unfocused.png) | Truncates with `...` instead of running off-screen. |
| Settings description | [12-settings-down5.png](ui-audit-shots/12-settings-down5.png) | [after-settings-description.png](ui-audit-shots/after-settings-description.png) | One line, wholly inside the glass card. |
| Wi-Fi picker | [54-wifi-list.png](ui-audit-shots/54-wifi-list.png) | [after-wifi-list.png](ui-audit-shots/after-wifi-list.png) | One-line title, no `(psk ****)` debug text, long names truncate with `...`, focused row scrolls. |
| Bluetooth picker | [61-bt-scan-result.png](ui-audit-shots/61-bt-scan-result.png) | [after-bluetooth-list.png](ui-audit-shots/after-bluetooth-list.png) | No MAC addresses; generic numbered names. |
| Power menu | [63-power-menu.png](ui-audit-shots/63-power-menu.png) | [after-power-menu.png](ui-audit-shots/after-power-menu.png) | Unchanged (no confirmed action; closed with B). |
| Pause / Volume / Brightness | [67b-volume-steady.png](ui-audit-shots/67b-volume-steady.png) | [after-pause-volume-brightness.png](ui-audit-shots/after-pause-volume-brightness.png) | Slider arrows clear of the label; brightness reads 50%. |
| FPS on crossbar | [41-fps-crossbar.png](ui-audit-shots/41-fps-crossbar.png) | [after-fps-crossbar.png](ui-audit-shots/after-fps-crossbar.png) | 99.43 -> 98.44 fps (within sample noise). |
| FPS in Settings | [38-fps-settings.png](ui-audit-shots/38-fps-settings.png) | [after-fps-settings.png](ui-audit-shots/after-fps-settings.png) | 98.30 -> 99.92 fps. Show FPS was turned off again. |

### Device verification record

- Launched Clover Pit from the menu, paused with `nova-home-ctl pause
  cloverpit`, saw the real pause panel over the game, quit with `nova-home-ctl
  quit cloverpit`; the scope was gone afterwards.
- Opened and closed the Power menu with `POWER_MENU` and B; no power action
  was confirmed.
- Opened and closed the pause panel with `PAUSE` and B; exercised the Volume
  and Brightness rows one step at a time.
- Set Screen Brightness to 50 and confirmed the backlight at 1723/3445 and
  the pause panel reading 50%. Left the menu on screen.
- `nova-session` active, `systemctl --failed` empty after every switch.

### Not verified on device

- A long Wi-Fi SSID: iwd would not accept a fake known network without an
  access point. The same scrolling slot is used by the Wi-Fi picker and is
  covered by `openxmb_scrolling_text_tests`; the long game title proves the
  component on the panel.
- The launch-failure error overlay and an empty game column: the failing
  temporary manifest was skipped by the library loader, and this device has
  no empty column. Both remain code-only.
- Aeroplane glyph on the panel (toggling radios would drop SSH); verified in
  code only. Squircle corners were added in the feedback round below.

## Owner feedback round (same branch)

The owner approved the removal list with two changes — keep Sparkles and Wave,
and patch AuroreEngine — plus further work, all implemented here:

| Feedback | Where | Device evidence |
|---|---|---|
| Smooth the overflow scroll | `scrolling_text.hpp` now carries a fractional glyph offset and one spare leading glyph; a contract test asserts a sub-glyph offset. | [frame a](ui-audit-shots/fb-smooth-a.png) / [frame b](ui-audit-shots/fb-smooth-b.png) show different sub-glyph positions on the focused long title. |
| Squircle corners | `nova/aurore-engine.patch`: exponent-4 superellipse in the corner region instead of a circular `roundedBox`. | [crossbar](ui-audit-shots/fb-00-crossbar.png) — glass corners flow continuously. |
| Restore the Switch games list | Cherry-picked the Switch / Windows / Linux library columns commit from the existing `feat/library-switch-windows-linux` branch. | [crossbar](ui-audit-shots/fb-00-crossbar.png) — Switch, Windows and Linux columns are back. |
| Keep Sparkles and Wave | Both settings remain in the Theme group's console rows. | — |
| Status bar icons + Bluetooth/aeroplane | Wi-Fi and battery use the accent material and the clock's hairline; an unblocked Bluetooth radio shows its rune; both radios blocked shows one aeroplane glyph. | [crossbar](ui-audit-shots/fb-00-crossbar.png) — accent battery/Wi-Fi and the Bluetooth rune. Aeroplane glyph was **not** toggled on the device (it would drop SSH); it is code-only. |
| Analog clock toggle, off by default | New Display setting `status.analog.clock`; the renderer only draws the face when enabled. | [off](ui-audit-shots/fb-display-rows.png), [on](ui-audit-shots/fb-clock-on.png). Left off. |
| Console mode only | Removed the approved rows and their actions: Interface Scale, Frame Limit, Restore Original Theme, Startup Intro, System Information, Licences, Restore Default Settings, and the desktop diagnostics. Display now has exactly Screen Brightness, Show FPS and Analog Clock. | [Display rows](ui-audit-shots/fb-display-rows.png) |
| Duplicate power sheet | `POWER_MENU` focuses the existing Power column at Sleep; the second choice overlay and its member/bookkeeping are deleted. | [Power column](ui-audit-shots/fb-power-column.png) |

After the feedback build: `nix build .#openxmb-tests --override-input nova-xmb
path:…` — 52/52 pass. FPS 118.76 on the crossbar with Show FPS on (no worse
than the 99.43 baseline). Show FPS is off again, brightness 50%, `nova-session`
active with no failed units, and the temporary long-title game removed.

## Polish & console-only cleanup round (`5a8ce92`, `1a043ea`, `a2a7e6e`)

Follow-up polish and console-only trimming on `ui-tidy/audit`:

1. **UI polish (`5a8ce92`)**:
   - `status_bar_renderer.cppm`: remove reserved analog-clock gap from `text_right_x` when `status.analog_clock` is `false` so right padding matches left padding.
   - `choice_overlay.cpp` / `.cppm`: lay out title wrapping before sheet sizing (removing the ~100 px dead gap below sheet titles), subtract `swatch_inset` in `ui::fitting_count`, and move focus scroll state into member fields.
   - `message_overlay.cpp`: hint bar `action::ok` reads `"OK"` (single button) or `"Select"` (choices) instead of `"Enter"`.
   - `choice_overlay.cpp`, `message_overlay.cpp`, `progress_overlay.cpp`: replace missing-glyph `…` (U+2026) with `"..."`.
   - `shell.cpp`: inset FPS and GPU memory debug text by `openxmb::xmb::ui::safe_inset`.
2. **Console-only cleanup (`1a043ea`, `a2a7e6e`)**:
   - Dropped unused desktop viewers, media players (`src/music/`, `src/photos/`, `src/documents/`, `src/video*.cpp`, `src/programs/*_viewer.cppm`, `music_player`, `video_player`, `about`, `integration_center`, `news_display`), `src/integrations/*`, `src/modules/{browser,disc,libretro}.cppm`, legacy `src/menu/{files,users,settings}_menu.*`, and their CMake/test targets while keeping `pkgs/openxmb/default.nix` and `tests.nix` compatible.
   - Trimmed `desktop_settings_catalog.hpp` to the 21 console rows, removed desktop-only handlers/`binding_capture` from `desktop_settings.cppm`, made console layout unconditional in `main_menu.cpp` and `settings_scene_renderer.cppm`, and added [`docs/PROVENANCE.md`](PROVENANCE.md).

### Device verification at `a2a7e6e` (1280x960 panel)

| Check | Screenshot | Result |
|---|---|---|
| Status bar right padding (Analog Clock Off) | [polish-01-crossbar-clock-off.png](ui-audit-shots/polish-01-crossbar-clock-off.png) | Right padding matches left padding; Switch crossbar clean. |
| Game Information hint bar | [polish-02-game-info.png](ui-audit-shots/polish-02-game-info.png) | Hint bar shows `A OK  B Back` (no `"Enter"`). |
| Theme > Colour swatches | [polish-03-theme-colour.png](ui-audit-shots/polish-03-theme-colour.png) | Colour swatches and labels align cleanly inside `choice_overlay`. |
| Multi-choice `message_overlay` (System Update) | [polish-04-system-update-prompt.png](ui-audit-shots/polish-04-system-update-prompt.png) | Hint bar shows `A Select  B Back`; cancelled safely with B. |
| Show FPS inset & Analog Clock On | [polish-05-fps-inset-clock-on.png](ui-audit-shots/polish-05-fps-inset-clock-on.png) | `FPS: 118.74` inset by `ui::safe_inset`, clear of `(0,0)` and rounded corners; analog face aligned. Both restored to Off. |
| Wi-Fi picker sheet title & `...` | [polish-06-wifi-sheet.png](ui-audit-shots/polish-06-wifi-sheet.png) | `Wi-Fi Networks` sits snugly above rows with no ~100 px dead gap; long SSID truncates with `...`. (Private SSIDs painted out.) |
| Bluetooth picker sheet title | [polish-07-bluetooth-sheet.png](ui-audit-shots/polish-07-bluetooth-sheet.png) | `Bluetooth Devices` sits snugly above `Bluetooth device 1..4` with no dead gap and no MAC addresses. |
| System Settings (console rows) | [polish-08-system-settings.png](ui-audit-shots/polish-08-system-settings.png) | Shows only `Language` and `Rescan Game Library`. |
| Launch-error `message_overlay` | [polish-09-launch-error.png](ui-audit-shots/polish-09-launch-error.png) | Exercised via synthetic `~/.local/state/nova/last-launch-error.json` (`Sample Game`): renders `Could Not Start Sample Game` with `A OK  B Back`, dismissed with A. |

Closing state on device: Screen Brightness restored to `50` (`1723/3445` sysfs, `~/.local/state/nova/brightness` = `50`), Show FPS Off, Analog Clock Off, menu on Switch crossbar, `nova-session` active, `systemctl --failed` empty. Aeroplane glyph (toggling radios drops SSH) and empty library columns (skipped by design in `main_menu.cpp`) remain code-only.

## Status bar icons, theming consistency, library management & refresh rate controls

Follow-up refinement round on `ui-tidy/audit`:

1. **Tabler status bar icons, sizing & layout (`status_bar.hpp`, `status_bar.cpp`, `status_bar_renderer.cppm`, `shell.cpp`)**:
   - Replaced custom vector battery/Wi-Fi/Bluetooth/aeroplane indicators with 15 crisp white 128x128 Tabler PNG icons (`plane-tilt`, `bluetooth`, `bluetooth-connected`, `battery-exclamation`, `battery-1`..`4`, `battery-charging`, `battery-charging-2`, `wifi-0`..`2`, `wifi`, `wifi-off`) and removed the battery percentage text label.
   - Kept Wi-Fi and Bluetooth icons white (matching the date text), scaled icon glyph heights (`22` logical px visual height) to match the date digit height, and widened spacing (`20` logical px gap, `22` logical px minimum end padding).
   - Added `/sys/class/bluetooth/hciN:handle` connection detection, external power supply online check (`charging` / `battery-charging-2` at 100%), and non-blocking default-route + TCP 443 internet reachability check (`wifi-off`) with socket calls kept in `status_bar.cpp` to avoid Clang C++23 module fortify linkage issues (`desktop_settings_posix.cpp` similarly isolates `::open`).
2. **Active theme consistency & loading screen cleanup (`glass.hpp`, `glass_draw.hpp`, `nova/aurore-engine.patch`, `launch_overlay.cpp`, `pause_overlay.cpp`, `progress_overlay.cpp`, `shell.cpp`)**:
   - Wired active theme colors into `glass::set_theme_colors(...)` and AuroreEngine's glass fragment shader so sheets, buttons, pause/progress overlays, and sliders follow the active device theme (no disconnected blue/grey tint).
   - Updated `launch_overlay` to use the active theme gradient/glow and omit the duplicate game title below cover logo art (retaining it only as fallback when artwork is missing).
3. **Settings column horizontal spring (`settings_scene_renderer.cppm`, `main_menu.cpp`)**:
   - Wired `x_shift`, `alpha_multiplier`, and `lens_focus` (`focus_tracker`) into `SettingsSceneRenderer` so the Settings column slides and bounces consistently with the game columns.
4. **Hide Games & System Settings > Hidden Games (`applications_menu.cpp`, `desktop_settings.cppm`, `desktop_settings_catalog.hpp`)**:
   - Added `Hide` to game Options (`Y`) menu (preserving custom `libraryOrder` slots on subsequent `Move` operations) and `System Settings > Hidden Games` with `Play Game`, `Restore to Library` (restoring original position), and `Cancel`.
5. **Sort Game Library (`applications_menu.cpp`, `desktop_settings.cppm`, `desktop_settings_catalog.hpp`)**:
   - Added `System Settings > Sort Game Library` (`Alphabetically`, `Date Added` via manifest mtime) affecting all four game categories (`switch`, `windows`, `linux`, `android`).
6. **Display Settings > Rearrange Status Bar (`status_bar.hpp`, `status_bar.cpp`, `status_bar_renderer.cppm`, `desktop_settings.cppm`)**:
   - Added `Display Settings > Rearrange Status Bar` to interactively reorder `Battery`, `Wi-Fi`, `Bluetooth`, and `Clock` (plus `Reset to Default`) with live status bar updates and round-trip unit tests in `tests/xmb/status_bar_tests.cpp`.
7. **60 fps launcher cap & Display Settings > In-Game Refresh Rate (`display_refresh.cpp`, `shell.cpp`, `main.cpp`, `utils.cppm`, `desktop_settings.cppm`)**:
   - Capped `XMS.bin` launcher frame rate at `60` fps in `main.cpp` and `shell.cpp` and set launcher Sway output mode to `60 Hz` (`1280x960@59.999Hz`).
   - Added `Display Settings > In-Game Refresh Rate` (`30 Hz`, `60 Hz`, `90 Hz`, `120 Hz`, plus `VRR` only when Sway `adaptive_sync` is supported), switching Sway output mode and passing `NOVA_REFRESH_RATE` / `DXVK_FRAME_RATE` on game launch, then restoring `60 Hz` when the game exits.

### Device verification on Retroid Pocket Nova (`1280x960`)

| Check | Screenshot | Result |
|---|---|---|
| Status bar icons, scaling & spacing | [ui-v2-statusbar-final.png](ui-audit-shots/ui-v2-statusbar-final.png) | White `battery-4`, `wifi`, `bluetooth` icons match date digit height without `%` text; airy consistent spacing. |
| Game Options (`Y`) with `Hide` & theme tint | [ui-v2-game-options-themed.png](ui-audit-shots/ui-v2-game-options-themed.png) | `Start`, `Move`, `Hide`, `Information` rendered inside theme-matched glass sheet; tested hide + restore flow cleanly. |
| System Update popup theming | [ui-v2-sysupdate-themed.png](ui-audit-shots/ui-v2-sysupdate-themed.png) | Confirmation sheet & button highlight match active crimson theme with zero blue/grey mismatch. |
| 60 fps launcher cap (`Show FPS`) | [ui-v2-fps-cap-60.png](ui-audit-shots/ui-v2-fps-cap-60.png) | `FPS: 59.49` even with user `max-fps: 120`; Show FPS restored to Off afterward. |
| Rearrange Status Bar | [ui-v2-statusbar-reordered.png](ui-audit-shots/ui-v2-statusbar-reordered.png) | Moving `Battery` right updates live bar immediately (`Wi-Fi`, `Battery`, `Bluetooth`, `Clock`); reset cleanly to default afterward. |
| In-Game Refresh Rate selector | [ui-v2-refresh-choices.png](ui-audit-shots/ui-v2-refresh-choices.png) | Exposes `30 Hz`, `60 Hz`, `90 Hz`, `120 Hz` (VRR omitted because `DSI-1` reports `adaptive_sync: false`). |
| Launch overlay theming & logo-only card | [ui-v2-launch-themed-nodupe.png](ui-audit-shots/ui-v2-launch-themed-nodupe.png) | Backdrop matches active purple theme and displays cover art card without duplicate title text; Sway switched to `120000` mHz on launch and back to `59999` mHz on quit. |
| Pause overlay theming | [ui-v2-pause-themed.png](ui-audit-shots/ui-v2-pause-themed.png) | Pause sheet, `Resume` highlight, and `Volume`/`Brightness` sliders match active purple theme. |
| Hidden Games actions (`Play Game` / `Restore`) | [ui-v2-hidden-games-actions.png](ui-audit-shots/ui-v2-hidden-games-actions.png) | Hidden Games lists hidden titles with `Play Game`, `Restore to Library`, and `Cancel`. |
| Sort Game Library (`Date Added`) | [ui-v2-sort-date-added.png](ui-audit-shots/ui-v2-sort-date-added.png) | Re-sorts Switch category by manifest mtime (`Need for Speed Hot Pursuit`, `Super Mario Bros. Wonder`, `APE OUT`); restored to `Alphabetically` afterward. |

