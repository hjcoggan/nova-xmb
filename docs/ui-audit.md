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

## Phase 3 — removal candidates (approval required before deletion)

Each line is a setting, entry or screen that does not serve a handheld game
launcher with sleep, pause, resume and quit, or that duplicates another one.
Nothing here is deleted yet.

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
- Continuous (squircle) corners: the engine shader uses a circular
  `roundedBox`; changing it means an AuroreEngine patch, so the concentric
  circular scale is used instead.
