# Provenance

Rule: before copying any file from another project, check its licence, record the source
commit or release in the table below, and keep the upstream licence header in the file.

Status: adapted = rewritten from an idea or structure; copied = file(s) taken verbatim;
used = consumed as a build dependency or reference.

| What | From | Licence | Status | Notes / source commit |
|---|---|---|---|---|
| Base XMB shell, C++23 module structure, settings catalogue, wave/particle shaders, config | [phenom64/OpenXMB](https://github.com/phenom64/OpenXMB) | GPL-3.0-only | forked | v0.3.0-beta.3 (`c4bccc8ee6069e1f1f5a3f0a6351e2600bb61eaf`) |
| Original shell architecture | [JnCrMx/xmbshell](https://github.com/JnCrMx/xmbshell) | GPL-3.0-only | inherited via OpenXMB | upstream notices preserved in file headers |
| Rendering engine (`AuroreEngine`) and Liquid Glass panel shader patch (`nova/aurore-engine.patch`) | [phenom64/AuroreEngine](https://github.com/phenom64/AuroreEngine) (derived from [JnCrMx/dreamrender](https://github.com/JnCrMx/dreamrender)) | MPL-2.0 (`LICENSES/MPL-2.0.txt`) | used + patched (`nova/aurore-engine.patch`) | `52c724e316c4154637da317a400a7c15928eaef4` |
| XMB ribbon shaders (`shaders/wave.*`, `shaders/original.*`) | [libretro/RetroArch](https://github.com/libretro/RetroArch) | GPL-3.0-or-later / `LICENSE.RetroArch` | inherited via OpenXMB | see `LICENSE.RetroArch` |
| Monthly gradients, particle cloud constants, icon material references | [TheGammaSqueeze/xmb-web](https://github.com/TheGammaSqueeze/xmb-web) | MIT | reference / adapted constants | documented in `tools/xmb/import_xmb_web.py` and `assets/xmb/manifests/xmb-web-source.json` |
| Child process spawning and rendering idle while a launched game runs (`include/openxmb/xmb/child_process.hpp`) | [MaxiBono/MaXMB](https://github.com/MaxiBono/MaXMB) (`app_launch.cpp`) | MIT | adapted | process-group spawn + idle pattern adapted for `nova-launch` |
| Crossbar, settings, power, and status icons (`assets/shell/icons/`) | [Tabler Icons](https://tabler.io/icons) | MIT | copied + aspect-corrected | console crossbar, Settings, Power category icons, and status bar indicators (`plane-tilt`, `bluetooth`, `bluetooth-connected`, `battery-*`, `wifi*`) |
| Play typeface (`interfaceFX/TypefaceServer/Play-Regular.ttf`) | Play font (Open Font License) | OFL-1.1 | inherited via OpenXMB | |
| Build dependencies (`fmt`, `fast_float`, `i18n-cpp`, `argparse`, `nlohmann/json`, `Vulkan-Hpp`, `VulkanMemoryAllocator-Hpp`, `spdlog`, SDL2, FreeType) | respective upstream projects | MIT / BSL-1.0 / Apache-2.0 / Zlib / FTL | used | `LICENSES/` preserves bundled notices (`fmt-MIT.txt`, `fast_float-MIT.txt`) |
