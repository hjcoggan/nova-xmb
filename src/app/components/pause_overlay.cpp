/* Nova pause overlay for OpenXMB (ROADMAP B3-B5). */

module;
#include "openxmb/xmb/glass.hpp"
#include "openxmb/xmb/game_id.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

module openxmb.app;

import :pause_overlay;
import :main;
import :xmb_presentation;
import openxmb.config;
import glm;

namespace app {

namespace {

namespace ui = openxmb::xmb::ui;

float smooth01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

std::filesystem::path find_backlight_dir() {
    const std::filesystem::path preferred("/sys/class/backlight/ae94000.dsi.0");
    std::error_code ec;
    if (std::filesystem::is_directory(preferred, ec)) {
        return preferred;
    }
    const std::filesystem::path root("/sys/class/backlight");
    if (!std::filesystem::is_directory(root, ec)) {
        return {};
    }
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (entry.is_directory(ec) || entry.is_symlink(ec)) {
            return entry.path();
        }
    }
    return {};
}

int read_int_file(const std::filesystem::path& path, int fallback) {
    std::ifstream in(path);
    int val = fallback;
    if (in >> val) {
        return val;
    }
    return fallback;
}

int read_volume_pct() {
#ifdef _WIN32
    return 50;
#else
    FILE* pipe = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
    if (!pipe) {
        return 50;
    }
    char buf[128]{};
    const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, pipe);
    pclose(pipe);
    if (n == 0) {
        return 50;
    }
    float vol = 0.5f;
    if (std::sscanf(buf, "Volume: %f", &vol) == 1) {
        return std::clamp(static_cast<int>(std::lround(vol * 100.0f)), 0, 100);
    }
    return 50;
#endif
}

void spawn_detached(const char* prog, const char* arg1, const char* arg2,
                    const char* arg3 = nullptr) {
#ifdef _WIN32
    (void)prog;
    (void)arg1;
    (void)arg2;
    (void)arg3;
#else
    const pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            const int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > STDERR_FILENO) {
                    close(devnull);
                }
            }
            const char* const argv[] = {prog, arg1, arg2, arg3, nullptr};
            execvp(prog, const_cast<char* const*>(argv));
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
#endif
}

void write_volume_pct(int pct) {
    pct = std::clamp(pct, 0, 100);
    char arg[16]{};
    std::snprintf(arg, sizeof(arg), "%d%%", pct);
    spawn_detached("wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", arg);
}

int read_brightness_pct() {
    const auto dir = find_backlight_dir();
    if (dir.empty()) {
        return 50;
    }
    const int max_b = std::max(1, read_int_file(dir / "max_brightness", 100));
    const int cur_b = std::clamp(read_int_file(dir / "brightness", max_b / 2), 0, max_b);
    return std::clamp(static_cast<int>(std::lround(100.0 * cur_b / max_b)), 5, 100);
}

std::filesystem::path brightness_state_path() {
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "nova/brightness";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local/state/nova/brightness";
    }
    return {};
}

struct game_manifest_info {
    std::string title;
    std::vector<std::string> guide_lines;
};

game_manifest_info load_game_manifest(const std::string& game_id) {
    if (!openxmb::xmb::valid_game_id(game_id)) {
        return {};
    }
    const char* home = std::getenv("HOME");
    if (!home || !*home) {
        return {};
    }
    const auto manifest_path = std::filesystem::path(home) / "Games" / game_id / "game.json";
    std::ifstream in(manifest_path);
    if (!in) {
        return {};
    }
    const auto parsed = nlohmann::json::parse(in, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return {};
    }

    game_manifest_info info;
    if (parsed.contains("title") && parsed["title"].is_string()) {
        info.title = parsed["title"].get<std::string>();
    }
    if (!parsed.contains("buttonGuide")) {
        return info;
    }
    const auto& guide = parsed["buttonGuide"];
    std::vector<std::string> items;
    if (guide.is_string()) {
        std::istringstream iss(guide.get<std::string>());
        std::string line;
        while (std::getline(iss, line) && items.size() < 6) {
            if (!line.empty()) items.push_back(line);
        }
    } else if (guide.is_array()) {
        for (const auto& entry : guide) {
            if (entry.is_string() && items.size() < 6) {
                items.push_back(entry.get<std::string>());
            }
        }
    } else if (guide.is_object()) {
        for (auto it = guide.begin(); it != guide.end() && items.size() < 6; ++it) {
            if (it.value().is_string()) {
                items.push_back(it.key() + ": " + it.value().get<std::string>());
            }
        }
    }
    if (items.size() <= 1) {
        info.guide_lines = std::move(items);
        return info;
    }
    std::vector<std::string> lines;
    std::string current;
    for (const auto& item : items) {
        if (current.empty()) {
            current = item;
        } else if (current.size() + 5 + item.size() <= 54) {
            current += "  |  " + item;
        } else {
            lines.push_back(std::move(current));
            current = item;
        }
    }
    if (!current.empty()) {
        lines.push_back(std::move(current));
    }
    if (lines.size() > 2) {
        lines.resize(2);
    }
    info.guide_lines = std::move(lines);
    return info;
}

void write_brightness_pct(int pct) {
    pct = std::clamp(pct, 5, 100);
    if (const auto state = brightness_state_path(); !state.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(state.parent_path(), ec);
        std::ofstream out(state);
        if (out) {
            out << pct << '\n';
        }
    }
    const auto dir = find_backlight_dir();
    if (dir.empty()) {
        return;
    }
    const int max_b = std::max(1, read_int_file(dir / "max_brightness", 100));
    const int raw = std::clamp(static_cast<int>(std::lround(max_b * (pct / 100.0))), 1, max_b);
    std::ofstream out(dir / "brightness");
    if (out) {
        out << raw << '\n';
    }
}

void run_nova_home_ctl(const char* verb, const std::string& game_id) {
    if (game_id.empty()) {
        spawn_detached("nova-home-ctl", verb, nullptr);
    } else {
        spawn_detached("nova-home-ctl", verb, game_id.c_str());
    }
}

} // namespace

pause_overlay::pause_overlay(std::string game_id)
    : current_game_id(std::move(game_id)) {
    auto manifest = load_game_manifest(current_game_id);
    current_game_title = std::move(manifest.title);
    guide_lines = std::move(manifest.guide_lines);
    refresh_levels();
}

void pause_overlay::refresh_levels() {
    volume_pct = read_volume_pct();
    brightness_pct = read_brightness_pct();
}

// Rows, top to bottom. Quit is last, away from Resume.
namespace {
constexpr int row_resume = 0;
#ifdef _WIN32
constexpr int row_volume = -1;
constexpr int row_brightness = -2;
constexpr int row_quit = 1;
constexpr int row_count = 2;
#else
constexpr int row_volume = 1;
constexpr int row_brightness = 2;
constexpr int row_quit = 3;
constexpr int row_count = 4;
#endif
}

void pause_overlay::begin_closing() {
    if (!closing_since) {
        closing_since = std::chrono::steady_clock::now();
    }
}

void pause_overlay::render(auroreengine::gui_renderer& renderer, shell* xmb) {
    namespace glass = openxmb::xmb::glass;
    const auto now = std::chrono::steady_clock::now();
    const float age_ms = std::chrono::duration<float, std::milli>(now - start_time).count();
    const float now_ms = std::chrono::duration<float, std::milli>(now.time_since_epoch()).count();
    const auto pose = glass::panel_in(age_ms);
    renderer.push_color(glm::vec4(1.0f, 1.0f, 1.0f, pose.alpha));

    overlay_canvas c(renderer);
    const float left = c.visible_left();
    const float top = c.visible_top();
    const float span_w = c.visible_right() - left;
    const float span_h = c.visible_bottom() - top;
    // The frozen game stays in view behind the glass, a little dimmed so the
    // card reads as the top layer. While the game is being stopped the veil
    // closes completely, so its window can vanish behind it unseen.
    const float closing_seconds = std::chrono::duration<float>(closing_for()).count();
    const float shut = closing() ? smooth01(closing_seconds / 0.25f) : 0.0f;
    c.rect(left - 2, top - 2, span_w + 4, span_h + 4, {0.0f, 0.0f, 0.0f, 0.30f + 0.70f * shut});

    const glm::vec4 primary{1.0f, 1.0f, 1.0f, glass::text_primary};
    const glm::vec4 secondary{1.0f, 1.0f, 1.0f, glass::text_secondary};
    const std::string& display_name = current_game_title.empty()
        ? current_game_id
        : current_game_title;
    if (closing()) {
        const std::string name = display_name.empty() ? std::string("Game") : display_name;
        constexpr float w = 560.0f, h = 208.0f;
        c.glass(960.0f - w * 0.5f, 540.0f - h * 0.5f - 24.0f, w, h, glass::variant::regular, glass::card_radius, shut);
        renderer.push_color({1.0f, 1.0f, 1.0f, shut});
        c.text(std::string("Closing"), 960, 540.0f - 46.0f, 24, secondary, 0.0f, true);
        c.text(name, 960, 540.0f + 6.0f, 38, primary, 0.0f, true);
        for (int i = 0; i < 3; ++i) {
            const float lit = 0.5f + 0.5f * std::sin((closing_seconds - static_cast<float>(i) * 0.33f) * 3.9f);
            const float size = 11.0f + 4.0f * lit;
            c.rect(960.0f + (static_cast<float>(i) - 1.0f) * 32.0f - size * 0.5f, 540.0f + 44.0f - size * 0.5f,
                size, size, {1.0f, 1.0f, 1.0f, 0.28f + 0.72f * lit}, size * 0.5f);
        }
        renderer.pop_color();
        renderer.pop_color();
        return;
    }

    // Layout on the one 4-point grid: padding 32, rows 72 high with 8 between.
    constexpr float card_w = 656.0f, row_h = 72.0f, header_h = 136.0f;
    const float row_gap = ui::px(ui::space::xs);
    const float guide_h = guide_lines.empty() ? 0.0f : 24.0f + 32.0f * static_cast<float>(guide_lines.size());
    const float card_h = header_h + row_count * (row_h + row_gap) - row_gap + guide_h + glass::card_padding;
    const float card_x = 960.0f - card_w * 0.5f;
    // Sit above the hint bar.
    const float card_y = 540.0f - card_h * 0.5f - 40.0f;
    {
        const float sw = card_w * pose.scale, sh = card_h * pose.scale;
        c.glass(960.0f - sw * 0.5f, card_y + card_h * 0.5f - sh * 0.5f, sw, sh);
    }

    c.text(std::string("Paused"), 960, card_y + 56.0f, 24, secondary, 0.0f, true);
    c.text(display_name.empty() ? std::string("Game") : display_name, 960, card_y + 104.0f, 38, primary, 0.0f, true);

    const float rows_y = card_y + header_h;
    const float row_x = card_x + glass::card_padding * 0.75f, row_w = card_w - glass::card_padding * 1.5f;
    const auto text_width = [&](const std::string& text, float size) {
        return renderer.text_width_pixels(text, size * c.scale * config::CONFIG.textScale) / c.scale;
    };
    const glm::vec4 accent{glass::srgb_to_linear(glass::accent_srgb[0]), glass::srgb_to_linear(glass::accent_srgb[1]),
                           glass::srgb_to_linear(glass::accent_srgb[2]), 1.0f};
    const auto slider = [&](float y, int percent, bool focused, float label_w) {
        constexpr float track_w = 200.0f, track_h = 8.0f;
        // Start the track after the widest label plus one spacing step, so
        // the focused left arrow never lands on "Brightness".
        const float track_x = std::max(row_x + 256.0f,
            row_x + 32.0f + label_w + ui::px(ui::space::lg));
        const float track_y = y + row_h * 0.5f - track_h * 0.5f;
        const float fill = track_w * static_cast<float>(std::clamp(percent, 0, 100)) / 100.0f;
        c.rect(track_x, track_y, track_w, track_h, {1.0f, 1.0f, 1.0f, 0.25f}, track_h * 0.5f);
        if (fill > 1.0f) c.rect(track_x, track_y, fill, track_h, accent, track_h * 0.5f);
        if (focused) {
            using direction = xmb_canvas::arrow_direction;
            c.arrow(track_x - 24.0f, y + row_h * 0.5f, 14.0f, direction::left, primary);
            c.arrow(track_x + track_w + 24.0f, y + row_h * 0.5f, 14.0f, direction::right, primary);
        }
        const float knob = focused ? 24.0f : 16.0f;
        c.rect(track_x + fill - knob * 0.5f, y + row_h * 0.5f - knob * 0.5f, knob, knob, primary, knob * 0.5f);
        const auto value = std::to_string(percent) + "%";
        c.text(value, row_x + row_w - 32.0f - text_width(value, 26), y + row_h * 0.5f + 10.0f, 26, focused ? primary : secondary);
    };

    // The focus capsule slides between rows.
    focus.move_to({row_x, rows_y + static_cast<float>(selected) * (row_h + row_gap), row_w, row_h}, now_ms);
    const auto lit = focus.at(now_ms);
    c.glass(lit.x, lit.y, lit.w, lit.h, glass::variant::focus, lit.h * 0.5f);

#ifdef _WIN32
    const std::array<std::string, row_count> labels = {"Resume", "Quit Game"};
#else
    const std::array<std::string, row_count> labels = {"Resume", "Volume", "Brightness", "Quit Game"};
#endif
    for (int i = 0; i < row_count; ++i) {
        const float y = rows_y + static_cast<float>(i) * (row_h + row_gap);
        const bool focused = i == selected;
        const float baseline = y + row_h * 0.5f + 11.0f;
        if (i == row_volume || i == row_brightness) {
            c.text(labels[static_cast<std::size_t>(i)], row_x + 32.0f, baseline, 28, primary);
            slider(y, i == row_volume ? volume_pct : brightness_pct, focused,
                   text_width(labels[static_cast<std::size_t>(i)], 28));
        } else {
            c.text(labels[static_cast<std::size_t>(i)], 960, baseline, 30, primary, 0.0f, true);
        }
    }

    for (std::size_t g = 0; g < guide_lines.size(); ++g) {
        const float gy = rows_y + row_count * (row_h + row_gap) + 24.0f + static_cast<float>(g) * 32.0f;
        c.text(guide_lines[g], 960, gy, 21, secondary, 0.0f, true);
    }

    const auto previous_scale = renderer.text_scale;
    renderer.text_scale = 1.0f;
    // Confirm does nothing on a slider, so it is not offered there.
    const bool on_slider = selected == row_volume || selected == row_brightness;
    xmb->render_hint_bar(renderer, std::array{
        std::pair{on_slider ? action::none : action::ok, "Select"},
        std::pair{action::cancel, "Resume"},
    });
    renderer.text_scale = previous_scale;
    renderer.pop_color();
}

result pause_overlay::on_action(action a) {
    if (closing()) {
        return result::success;
    }
    switch (a) {
        case action::up:
            selected = (selected + row_count - 1) % row_count;
            return result::success | result::ok_sound;
        case action::down:
            selected = (selected + 1) % row_count;
            return result::success | result::ok_sound;
        case action::left:
            if (selected == row_volume) {
                volume_pct = std::max(0, volume_pct - 5);
                write_volume_pct(volume_pct);
            } else if (selected == row_brightness) {
                brightness_pct = std::max(5, brightness_pct - 5);
                write_brightness_pct(brightness_pct);
            }
            return result::success;
        case action::right:
            if (selected == row_volume) {
                volume_pct = std::min(100, volume_pct + 5);
                write_volume_pct(volume_pct);
            } else if (selected == row_brightness) {
                brightness_pct = std::min(100, brightness_pct + 5);
                write_brightness_pct(brightness_pct);
            }
            return result::success;
        case action::ok:
            if (selected == row_resume) {
                run_nova_home_ctl("resume", current_game_id);
                return result::close;
            }
            if (selected == row_quit) {
                run_nova_home_ctl("quit", current_game_id);
                begin_closing();
                return result::success | result::confirm_sound;
            }
            return result::success;
        case action::cancel:
            run_nova_home_ctl("resume", current_game_id);
            return result::close;
        default:
            return result::unsupported;
    }
}

} // namespace app
