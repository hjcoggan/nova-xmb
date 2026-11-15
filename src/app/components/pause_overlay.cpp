/* Nova pause overlay for OpenXMB (ROADMAP B3-B5). */

module;

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
#include <unistd.h>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>

module openxmb.app;

import :pause_overlay;
import :main;
import :xmb_presentation;
import openxmb.config;
import glm;

namespace app {

namespace {

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
}

void spawn_detached(const char* prog, const char* arg1, const char* arg2,
                    const char* arg3 = nullptr) {
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
    if (game_id.empty() || game_id.find('/') != std::string::npos || game_id.find("..") != std::string::npos) {
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

void pause_overlay::render(auroreengine::gui_renderer& renderer, shell* xmb) {
    const float age = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - start_time).count();
    const float settle = smooth01(age / 0.22f);
    renderer.push_color(glm::vec4(1.0f, 1.0f, 1.0f, settle));

    overlay_canvas c(renderer);
    const float left = c.visible_left();
    const float top = c.visible_top();
    const float span_w = c.visible_right() - left;
    const float span_h = c.visible_bottom() - top;
    c.rect(left, top, span_w, span_h, {0.0f, 0.0f, 0.0f, 0.75f});
    c.rect(left, 199, span_w, 680, {0.0f, 0.0f, 0.0f, 0.65f});
    c.rect(left, 199, span_w, 1, {1.0f, 1.0f, 1.0f, 0.75f});
    c.rect(left, 879, span_w, 1, {1.0f, 1.0f, 1.0f, 0.75f});

    const std::string& display_name = current_game_title.empty()
        ? current_game_id
        : current_game_title;
    const std::string title = display_name.empty()
        ? "Game Paused"
        : ("Game Paused - " + display_name);
    c.text(title, 960, 275, 34, {1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, true);

    const std::array<std::string, 4> labels = {
        "Resume",
        "Quit Game",
        "Volume: < " + std::to_string(volume_pct) + "% >",
        "Brightness: < " + std::to_string(brightness_pct) + "% >",
    };

    for (int i = 0; i < 4; ++i) {
        const float y = 370.0f + static_cast<float>(i) * 76.0f;
        const float glow = (i == selected) ? xmb_canvas::pulse() : 0.0f;
        const glm::vec4 col = (i == selected)
            ? glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)
            : glm::vec4(0.85f, 0.85f, 0.85f, 0.90f);
        c.text(labels[static_cast<std::size_t>(i)], 960, y, 28, col, glow, true);
    }

    for (std::size_t g = 0; g < guide_lines.size(); ++g) {
        const float gy = 696.0f + static_cast<float>(g) * 34.0f;
        c.text(guide_lines[g], 960, gy, 21, {0.82f, 0.86f, 0.92f, 0.88f}, 0.0f, true);
    }

    const auto previous_scale = renderer.text_scale;
    renderer.text_scale = 1.0f;
    xmb->render_controller_buttons(renderer, 0.5f, 0.84f, std::array{
        std::pair{action::ok, "Select"},
        std::pair{action::cancel, "Resume"},
    });
    renderer.text_scale = previous_scale;
    renderer.pop_color();
}

result pause_overlay::on_action(action a) {
    switch (a) {
        case action::up:
            selected = (selected + 3) % 4;
            return result::success;
        case action::down:
            selected = (selected + 1) % 4;
            return result::success;
        case action::left:
            if (selected == 2) {
                volume_pct = std::max(0, volume_pct - 5);
                write_volume_pct(volume_pct);
            } else if (selected == 3) {
                brightness_pct = std::max(5, brightness_pct - 5);
                write_brightness_pct(brightness_pct);
            }
            return result::success;
        case action::right:
            if (selected == 2) {
                volume_pct = std::min(100, volume_pct + 5);
                write_volume_pct(volume_pct);
            } else if (selected == 3) {
                brightness_pct = std::min(100, brightness_pct + 5);
                write_brightness_pct(brightness_pct);
            }
            return result::success;
        case action::ok:
            if (selected == 0) {
                run_nova_home_ctl("resume", current_game_id);
                return result::close;
            }
            if (selected == 1) {
                run_nova_home_ctl("quit", current_game_id);
                return result::close;
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
