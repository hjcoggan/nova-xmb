/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025-2026 Syndromatic Ltd. All rights reserved
 * Designed by Kavish Krishnakumar in Manchester.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

module;
#include "openxmb/xmb/glass.hpp"
#include "openxmb/xmb/scrolling_text.hpp"
#include "../../localization/ui_literal_text.hpp"

#include "openxmb/xmb/catalog.hpp"
#include "desktop_settings_catalog.hpp"
#include "../../localization/shell_locale.hpp"
#include "media_motion.hpp"
#include "openxmb/xmb/background.hpp"
#include "openxmb/desktop/library.hpp"
#include "openxmb/themes/library.hpp"
#include "openxmb/xmb/settings_actions.hpp"
#include "openxmb/xmb/settings_controller.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

module openxmb.app;

import sdl2;
import spdlog;
import i18n;
import glm;
import vulkan_hpp;
import vma;
import auroreengine;
import openxmb.xmb.root_scene;
import openxmb.xmb.settings_scene_renderer;

import openxmb.config;
import :menu_base;
import :menu_utils;
import :choice_overlay;
import :xmb_presentation;
import :applications_menu;
import :settings_menu;
import :users_menu;
import :files_menu;
import :desktop_settings;
import :message_overlay;
import :text_entry;

using namespace mfk::i18n::literals;

namespace app {

namespace {

using namespace openxmb::xmb;

inline constexpr double logical_width = 1920.0;
inline constexpr double logical_height = 1080.0;
inline constexpr int settings_category_index = 1;
inline constexpr float submenu_backdrop_blur_px = 9.0F;
inline constexpr float submenu_backdrop_dim_alpha = 0.0F;
inline constexpr double settings_parent_col_x = 316.0;
inline constexpr double settings_sibling_col_x = 455.0;
inline constexpr double settings_parent_spacing = 80.0;
inline constexpr double xmb_web_icon_glass_alpha = 1.0;
inline constexpr double xmb_web_flat_active_icon_alpha = 0.54;
inline constexpr std::string_view initial_category_env = "OPENXMB_INITIAL_CATEGORY";
inline constexpr std::string_view initial_settings_menu_env = "OPENXMB_INITIAL_SETTINGS_MENU";
inline constexpr std::string_view initial_settings_selection_env =
    "OPENXMB_INITIAL_SETTINGS_SELECTION";
inline constexpr std::string_view initial_settings_open_choice_env =
    "OPENXMB_OPEN_INITIAL_SETTINGS_CHOICE";

struct contained_layout {
    double scale{};
    double offset_x{};
    double offset_y{};
    double framebuffer_width{};
    double framebuffer_height{};

    [[nodiscard]] float x(double logical_x) const noexcept {
        return static_cast<float>((offset_x + logical_x * scale) / framebuffer_width);
    }

    [[nodiscard]] float y(double logical_y) const noexcept {
        return static_cast<float>((offset_y + logical_y * scale) / framebuffer_height);
    }

    [[nodiscard]] float width(double logical_width) const noexcept {
        // ImageRenderer uses framebuffer-height units on both axes and applies
        // aspect correction internally. Dividing by width shrinks 16:9 icons.
        return static_cast<float>(logical_width * scale / framebuffer_height);
    }

    [[nodiscard]] float height(double logical_height) const noexcept {
        return static_cast<float>(logical_height * scale / framebuffer_height);
    }
};

[[nodiscard]] contained_layout make_contained_layout(const auroreengine::gui_renderer& renderer) noexcept {
    const auto width = static_cast<double>(renderer.frame_size.width);
    const auto height = static_cast<double>(renderer.frame_size.height);
    const auto base_scale = height / logical_height;
    const auto ui = config::CONFIG.ui_scale();
    const auto scale = base_scale * ui;
    const auto base_ox = (width - logical_width * base_scale) * 0.5;
    const auto anchor_x = (width / std::max(1.0, height) < logical_width / logical_height)
        ? (width * 0.28)
        : (base_ox + kActiveCategoryX * base_scale);
    constexpr double kZoomedCategoryCenterYFrac = 0.27;
    const auto target_y = (ui > 1.0) ? (height * kZoomedCategoryCenterYFrac) : (kCategoryBaselineY * base_scale);
    return {
        .scale = scale,
        .offset_x = anchor_x - kActiveCategoryX * scale,
        .offset_y = target_y - (kCategoryBaselineY - (ui > 1.0 ? 10.0 : 0.0)) * scale,
        .framebuffer_width = width,
        .framebuffer_height = height,
    };
}

[[nodiscard]] double ease_out_cubic(double value) noexcept {
    const auto t = std::clamp(value, 0.0, 1.0);
    const auto inverse = 1.0 - t;
    return 1.0 - inverse * inverse * inverse;
}

[[nodiscard]] double progress(std::chrono::steady_clock::time_point now,
                              std::chrono::steady_clock::time_point started,
                              std::chrono::milliseconds duration) noexcept {
    if(started.time_since_epoch().count() == 0) {
        return 1.0;
    }
    const auto elapsed_seconds = std::chrono::duration<double>(now - started).count();
    const auto duration_seconds = std::chrono::duration<double>(duration).count();
    return std::clamp(elapsed_seconds / duration_seconds, 0.0, 1.0);
}

[[nodiscard]] std::filesystem::path preferred_icon(
    const std::filesystem::path& asset_directory,
    std::string_view compatibility_name,
    std::string_view clean_name
) {
    const auto bundled = asset_directory / "icons" / compatibility_name;
    std::error_code bundled_error;
    if(std::filesystem::is_regular_file(bundled, bundled_error) && !bundled_error) {
        return bundled;
    }
    const auto compatibility = asset_directory / "compat/xmb-ui-compat/icons" / compatibility_name;
    std::error_code error;
    if(std::filesystem::is_regular_file(compatibility, error) && !error) {
        return compatibility;
    }

    const auto clean = asset_directory / "icons" / clean_name;
    spdlog::debug("Compatibility icon {} is unavailable; using clean icon {}",
        compatibility.string(), clean.string());
    return clean;
}

[[nodiscard]] bool file_exists(const std::filesystem::path& path) noexcept {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

[[nodiscard]] std::optional<std::filesystem::path> resolve_compat_icon_ref(
    const std::filesystem::path& asset_directory,
    std::string_view icon_ref
) {
    constexpr std::string_view xmb_icon_prefix = "compat.icon.xmb.icon.";
    constexpr std::string_view compat_icon_prefix = "compat.icon.";
    const auto icon_directory = asset_directory / "compat/xmb-ui-compat/icons";

    constexpr std::array<std::pair<std::string_view,std::string_view>,17> settings_icons{{
        {"008","icon_update"},{"005","icon_game_setting"},{"009","icon_bdvd_setting"},
        {"003","icon_music_setting"},{"042","icon_chat_setting"},{"012","icon_system_setting"},
        {"023","icon_theme_setting"},{"014","icon_datetime_setting"},{"056","icon_powersave_setting"},
        {"015","icon_accessory"},{"010","icon_printer_setting"},{"016","icon_display_setting"},
        {"017","icon_sound_setting"},{"018","icon_security_setting"},{"020","icon_remoteplay_setting"},
        {"006","icon_network_setting"},{"022","icon_setting_item"}
    }};
    if(icon_ref.starts_with(xmb_icon_prefix)) {
        for(const auto& [suffix,name]:settings_icons) if(icon_ref.substr(xmb_icon_prefix.size())==suffix) {
            const auto path=asset_directory/"icons/theme"/(std::string(name)+".png");
            if(file_exists(path)) return path;
        }
    }

    if(icon_ref.starts_with(xmb_icon_prefix)) {
        const auto suffix = icon_ref.substr(xmb_icon_prefix.size());
        const auto filename = suffix == "psn"
            ? std::string{"xmb_icon_psn.png"}
            : std::string{"xmb_icon_"} + std::string{suffix} + ".png";
        const auto bundled = asset_directory / "icons" / filename;
        if(file_exists(bundled)) return bundled;
        const auto path = icon_directory / filename;
        if(file_exists(path)) {
            return path;
        }
    }

    if(icon_ref.starts_with(compat_icon_prefix)) {
        auto name = std::string{icon_ref.substr(compat_icon_prefix.size())};
        std::ranges::replace(name, '.', '_');
        const auto underscored = icon_directory / (name + ".png");
        if(file_exists(underscored)) {
            return underscored;
        }
        std::ranges::replace(name, '_', '-');
        const auto dashed = icon_directory / (name + ".png");
        if(file_exists(dashed)) {
            return dashed;
        }
    }

    return std::nullopt;
}

struct ps3_colour_entry {
    std::string_view value;
    glm::vec3 tint;
    glm::vec3 swatch;
};

[[nodiscard]] const std::array<ps3_colour_entry, 21>& ps3_colour_palette() {
    static const std::array<ps3_colour_entry, 21> colours{{
        {"original", {0.820F, 0.620F, 0.900F}, {0.720F, 0.750F, 0.790F}},
        {"yellow", {1.000F, 0.880F, 0.200F}, {0.800F, 0.720F, 0.110F}},
        {"green", {0.650F, 0.870F, 0.300F}, {0.450F, 0.660F, 0.160F}},
        {"pink", {1.000F, 0.640F, 0.720F}, {0.870F, 0.430F, 0.510F}},
        {"dark.green", {0.250F, 0.700F, 0.250F}, {0.110F, 0.480F, 0.110F}},
        {"light.purple", {0.820F, 0.620F, 0.900F}, {0.570F, 0.440F, 0.740F}},
        {"teal", {0.300F, 0.880F, 0.850F}, {0.140F, 0.720F, 0.670F}},
        {"dark.blue", {0.100F, 0.300F, 0.800F}, {0.030F, 0.220F, 0.650F}},
        {"magenta", {0.700F, 0.300F, 0.800F}, {0.550F, 0.210F, 0.610F}},
        {"orange", {1.000F, 0.700F, 0.150F}, {0.760F, 0.560F, 0.070F}},
        {"brown", {0.620F, 0.430F, 0.180F}, {0.620F, 0.430F, 0.180F}},
        {"red", {0.900F, 0.220F, 0.220F}, {0.900F, 0.220F, 0.220F}},
        {"black", {0.060F, 0.060F, 0.075F}, {0.040F, 0.040F, 0.047F}},
        {"white", {0.950F, 0.950F, 0.980F}, {0.950F, 0.950F, 0.960F}},
        {"gray", {0.550F, 0.570F, 0.620F}, {0.550F, 0.560F, 0.620F}},
        {"blue", {0.200F, 0.450F, 0.950F}, {0.210F, 0.450F, 0.950F}},
        {"cyan", {0.200F, 0.850F, 0.950F}, {0.200F, 0.850F, 0.950F}},
        {"lime", {0.550F, 0.950F, 0.200F}, {0.550F, 0.950F, 0.200F}},
        {"gold", {1.000F, 0.780F, 0.250F}, {1.000F, 0.780F, 0.250F}},
        {"violet", {0.550F, 0.350F, 0.950F}, {0.550F, 0.350F, 0.950F}},
        {"crimson", {0.800F, 0.100F, 0.300F}, {0.800F, 0.100F, 0.300F}},
    }};
    return colours;
}

[[nodiscard]] std::optional<ps3_colour_entry> ps3_colour_for_value(
    std::string_view value) {
    for(const auto& colour : ps3_colour_palette()) {
        if(colour.value == value) {
            return colour;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool near_colour(glm::vec3 lhs, glm::vec3 rhs) noexcept {
    constexpr float tolerance = 0.006F;
    return std::abs(lhs.r - rhs.r) <= tolerance &&
           std::abs(lhs.g - rhs.g) <= tolerance &&
           std::abs(lhs.b - rhs.b) <= tolerance;
}

[[nodiscard]] std::string_view current_ps3_colour_value() noexcept {
    if(config::CONFIG.themeOriginalColour) {
        return "original";
    }

    for(const auto& colour : ps3_colour_palette()) {
        if(colour.value == "original") {
            continue;
        }
        if(near_colour(config::CONFIG.themeCustomColour, colour.tint)) {
            return colour.value;
        }
    }
    return "custom";
}

[[nodiscard]] std::string_view current_ps3_background_value() noexcept {
    using background_type = config::config::background_type;
    switch(config::CONFIG.backgroundType) {
        case background_type::original:
            return "original";
        case background_type::wave:
            return "classic";
        case background_type::video:
        case background_type::image:
            return "wallpaper";
        case background_type::color:
            return "brightness";
    }
    return "original";
}

[[nodiscard]] std::string_view current_ps3_theme_value() noexcept {
    return config::CONFIG.backgroundType == config::config::background_type::wave
        ? "classic"
        : "original";
}

[[nodiscard]] std::string_view current_ps3_font_value() noexcept {
    // The native font picker is not yet backed by packaged xmb-web font
    // variants. Keep the panel enterable and truthful instead of guessing.
    return "original";
}

[[nodiscard]] std::string_view current_ps3_day_night_value() noexcept {
    using day_night_mode = config::config::day_night_mode;
    switch(config::CONFIG.themeDayNightMode) {
        case day_night_mode::auto_time_of_day:
            return "auto.time.of.day";
        case day_night_mode::day:
            return "day";
        case day_night_mode::morning:
            return "morning";
        case day_night_mode::dusk:
            return "dusk";
        case day_night_mode::evening:
            return "evening";
        case day_night_mode::night:
            return "night";
    }
    return "auto.time.of.day";
}

[[nodiscard]] std::string_view current_dialog_value(std::string_view dialog_id) noexcept {
    if(dialog_id == "dialog.colour") {
        return current_ps3_colour_value();
    }
    if(dialog_id == "dialog.background") {
        return current_ps3_background_value();
    }
    if(dialog_id == "dialog.theme") {
        return current_ps3_theme_value();
    }
    if(dialog_id == "dialog.font") {
        return current_ps3_font_value();
    }
    if(dialog_id == "dialog.day.night") {
        return current_ps3_day_night_value();
    }
    return "";
}

[[nodiscard]] std::string_view locale_for_catalog_value(std::string_view value) noexcept {
    constexpr std::array<std::pair<std::string_view,std::string_view>,20> locales{{
        {"japanese","ja"},{"english.united.states","en_US"},
        {"english.united.kingdom","en_GB"},{"french","fr"},
        {"spanish","es"},{"german","de"},{"italian","it"},{"dutch","nl"},
        {"portuguese.portugal","pt_PT"},{"portuguese.brazil","pt_BR"},
        {"russian","ru"},{"korean","ko"},{"chinese.traditional","zh_TW"},
        {"chinese.simplified","zh_CN"},{"finnish","fi"},{"swedish","sv"},
        {"danish","da"},{"norwegian","no"},{"polish","pl"},{"turkish","tr"}
    }};
    for(const auto& [catalog,locale]:locales) if(value==catalog) return locale;
    return {};
}

[[nodiscard]] bool settings_dialog_top_action(
    std::string_view dialog_id, std::string_view choice_value) noexcept {
    return (dialog_id == "dialog.theme" && choice_value == "install") ||
        (dialog_id == "dialog.background" && choice_value == "brightness");
}

[[nodiscard]] std::string localized_dialog_choice_label(
    const openxmb::xmb::Catalog& catalog,
    std::string_view dialog_id,
    std::string_view choice_value) {
    const auto dialog_it = catalog.dialogs.find(dialog_id);
    if(dialog_it == catalog.dialogs.end()) {
        return {};
    }
    for(const auto& choice : dialog_it->second.choices) {
        if(settings_dialog_top_action(dialog_id, choice.value) ||
           std::string_view{choice.value} != choice_value) {
            continue;
        }
        const auto localized = catalog.text(choice.label_key);
        return localized.empty() ? choice.value : std::string{localized};
    }
    return {};
}

[[nodiscard]] bool config_override_read_only() noexcept {
    const auto* raw = std::getenv("OPENXMB_CONFIG_READ_ONLY");
    if(raw == nullptr) {
        return false;
    }
    const std::string_view value{raw};
    return value == "1" || value == "true" || value == "TRUE" ||
           value == "yes" || value == "YES" || value == "on" || value == "ON";
}

[[nodiscard]] bool apply_settings_dialog_choice_value(
    std::string_view dialog_id,
    std::string_view choice_value,
    bool log_unsupported = true
) {
    if(dialog_id == "dialog.colour") {
        if(choice_value == "original") {
            config::CONFIG.setThemeOriginalColour(true);
            return true;
        }
        if(const auto colour = ps3_colour_for_value(choice_value)) {
            config::CONFIG.setThemeOriginalColour(false);
            config::CONFIG.setThemeCustomColour(colour->tint);
            return true;
        }
        return false;
    }

    if(dialog_id == "dialog.background") {
        using background_type = config::config::background_type;
        if(choice_value == "original") {
            config::CONFIG.setBackgroundType(background_type::original);
            return true;
        }
        if(choice_value == "classic") {
            config::CONFIG.setBackgroundType(background_type::wave);
            return true;
        }
        if(choice_value == "wallpaper") {
            if(!config::CONFIG.backgroundImage.empty()) {
                config::CONFIG.setBackgroundType(background_type::image);
                return true;
            }
            if(log_unsupported) {
                spdlog::info("Background Wallpaper is available only after a wallpaper path is configured");
            }
            return false;
        }
        if(choice_value == "brightness") {
            config::CONFIG.setBackgroundType(background_type::color);
            if(log_unsupported) {
                spdlog::info("Background Brightness opens a simplified native static-colour fallback until the full xmb-web brightness submenu is ported");
            }
            return true;
        }
        return false;
    }

    if(dialog_id == "dialog.theme") {
        using background_type = config::config::background_type;
        if(choice_value == "original") {
            config::CONFIG.setBackgroundType(background_type::original);
            return true;
        }
        if(choice_value == "classic") {
            config::CONFIG.setBackgroundType(background_type::wave);
            return true;
        }
        if(choice_value == "install" && log_unsupported) {
            spdlog::info("Theme Install is catalog-backed but not live-wired yet");
        }
        return false;
    }

    if(dialog_id == "dialog.font") {
        if(choice_value == "original") {
            config::CONFIG.setFontPath("default");
            return true;
        }
        if(log_unsupported) {
            spdlog::info("Font variant '{}' is catalog-backed but not packaged yet",
                choice_value);
        }
        return false;
    }

    if(dialog_id == "dialog.day.night") {
        config::CONFIG.setThemeDayNightMode(choice_value);
        return true;
    }

    return false;
}

[[nodiscard]] double seconds_from_time_point(
    std::chrono::steady_clock::time_point now) noexcept {
    return std::chrono::duration<double>(now.time_since_epoch()).count();
}

[[nodiscard]] std::optional<int> initial_category_index_from_env() noexcept {
    const auto* raw = std::getenv(initial_category_env.data());
    if(raw == nullptr) {
        if(std::getenv(initial_settings_menu_env.data()) != nullptr) {
            return settings_category_index;
        }
        return std::nullopt;
    }

    const std::string_view value{raw};
    if(value.empty()) {
        return std::nullopt;
    }

    constexpr std::array aliases{
        std::pair{std::string_view{"0"}, 0},
        std::pair{std::string_view{"users"}, 0},
        std::pair{std::string_view{"category.users"}, 0},
        std::pair{std::string_view{"1"}, 1},
        std::pair{std::string_view{"settings"}, 1},
        std::pair{std::string_view{"category.settings"}, 1},
        std::pair{std::string_view{"2"}, 2},
        std::pair{std::string_view{"photo"}, 2},
        std::pair{std::string_view{"category.photo"}, 2},
        std::pair{std::string_view{"3"}, 3},
        std::pair{std::string_view{"music"}, 3},
        std::pair{std::string_view{"category.music"}, 3},
        std::pair{std::string_view{"4"}, 4},
        std::pair{std::string_view{"video"}, 4},
        std::pair{std::string_view{"category.video"}, 4},
        std::pair{std::string_view{"5"},5},std::pair{std::string_view{"tv"},5},
        std::pair{std::string_view{"6"},6},std::pair{std::string_view{"game"},6},std::pair{std::string_view{"category.game"},6},
        std::pair{std::string_view{"7"},7},std::pair{std::string_view{"network"},7},std::pair{std::string_view{"category.network"},7},
        std::pair{std::string_view{"8"},8},std::pair{std::string_view{"psn"},8},std::pair{std::string_view{"online"},8},
        std::pair{std::string_view{"playstation.network"},8},std::pair{std::string_view{"category.online"},8},
        std::pair{std::string_view{"9"},9},std::pair{std::string_view{"friends"},9},std::pair{std::string_view{"category.friends"},9},
    };

    for(const auto& [alias, index] : aliases) {
        if(value == alias) {
            return index;
        }
    }

    return -1;
}

[[nodiscard]] std::optional<std::string_view> env_string(std::string_view name) noexcept {
    const auto* raw = std::getenv(name.data());
    if(raw == nullptr || std::string_view{raw}.empty()) {
        return std::nullopt;
    }
    return std::string_view{raw};
}

[[nodiscard]] std::optional<std::size_t> env_size(std::string_view name) noexcept {
    const auto raw = env_string(name);
    if(!raw) {
        return std::nullopt;
    }
    std::size_t value{};
    const auto* begin = raw->data();
    const auto* end = begin + raw->size();
    const auto parsed = std::from_chars(begin, end, value);
    if(parsed.ec != std::errc{} || parsed.ptr != end) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] bool env_truthy(std::string_view name) noexcept {
    const auto raw = env_string(name);
    if(!raw) {
        return false;
    }
    return *raw == "1" || *raw == "true" || *raw == "yes" || *raw == "on";
}

[[nodiscard]] bool find_settings_menu_path(
    const openxmb::xmb::Catalog& catalog,
    std::string_view current_menu_id,
    std::string_view target_menu_id,
    std::vector<std::size_t>& path
) {
    if(current_menu_id == target_menu_id) {
        return true;
    }

    const auto* menu = catalog.find_node(current_menu_id);
    if(!menu) {
        return false;
    }

    for(std::size_t index = 0; index < menu->children.size(); ++index) {
        const auto* child = catalog.find_node(menu->children[index]);
        if(!child || child->children.empty()) {
            continue;
        }
        path.push_back(index);
        if(find_settings_menu_path(catalog, child->id, target_menu_id, path)) {
            return true;
        }
        path.pop_back();
    }

    return false;
}

struct fitted_icon_box {
    double x{};
    double y{};
    double width{};
    double height{};
};

/// Fits `texture` uniformly inside a logical slot of `slot_width` x `slot_height`
/// centred at (`center_x`, `center_y`) without cropping or non-uniform scaling.
[[nodiscard]] fitted_icon_box fit_texture_in_slot(
    const auroreengine::texture& texture,
    double center_x,
    double center_y,
    double slot_width,
    double slot_height
) noexcept {
    double draw_width = slot_width;
    double draw_height = slot_height;
    if(texture.width > 0 && texture.height > 0 && slot_width > 0.0 && slot_height > 0.0) {
        const double tex_w = static_cast<double>(texture.width);
        const double tex_h = static_cast<double>(texture.height);
        const double fit = std::min(slot_width / tex_w, slot_height / tex_h);
        draw_width = tex_w * fit;
        draw_height = tex_h * fit;
    }
    return {
        .x = center_x - draw_width * 0.5,
        .y = center_y - draw_height * 0.5,
        .width = draw_width,
        .height = draw_height,
    };
}

void draw_icon(
    auroreengine::gui_renderer& renderer,
    const auroreengine::texture& texture,
    const auroreengine::texture* glass_texture,
    const contained_layout& layout,
    double center_x,
    double center_y,
    double extent,
    double alpha,
    bool custom_icon = false
) {
    if(alpha <= 0.001 || extent <= 0.0) {
        return;
    }

    const bool use_glass =
        !custom_icon && config::CONFIG.iconGlassRefraction &&
        glass_texture != nullptr && glass_texture->loaded;
    const auto& target = use_glass ? *glass_texture : texture;
    if(!target.loaded) {
        return;
    }

    if(custom_icon) {
        // Every cover (a game's transparent logo) is fitted inside a box of
        // one size, whatever its own shape: never stretched or cropped.
        using lens = openxmb::xmb::SelectionLens;
        const double card_w = extent * lens::card_width, card_h = extent * lens::card_height;
        const auto fitted = fit_texture_in_slot(target, center_x, center_y, card_w, card_h);
        renderer.draw_image(target, layout.x(fitted.x), layout.y(fitted.y),
            layout.width(fitted.width), layout.height(fitted.height),
            glm::vec4(1.0F, 1.0F, 1.0F, static_cast<float>(alpha)));
        return;
    }
    const auto fitted = fit_texture_in_slot(target, center_x, center_y, extent, extent);
    const auto x = layout.x(fitted.x);
    const auto y = layout.y(fitted.y);
    const auto width = layout.width(fitted.width);
    const auto height = layout.height(fitted.height);
    const auto tint =
        glm::vec4(utils::xmb_icon_colour(openxmb::xmb::wall_clock_now()),
            static_cast<float>(alpha * xmb_web_icon_glass_alpha));
    if(use_glass) {
        renderer.draw_image_glass(target, x, y, width, height, tint);
    } else {
        const auto flat_alpha =
            extent >= kActiveItemIconExtent - 0.5
                ? alpha * xmb_web_flat_active_icon_alpha
                : alpha;
        const auto flat_tint =
            glm::vec4(utils::xmb_icon_colour(openxmb::xmb::wall_clock_now()), static_cast<float>(flat_alpha));
        renderer.draw_image(target, x, y, width, height, flat_tint);
    }
}

void draw_normalized_icon(
    auroreengine::gui_renderer& renderer,
    const auroreengine::texture& texture,
    const auroreengine::texture* glass_texture,
    float x,
    float y,
    double width,
    double height,
    glm::vec4 tint = glm::vec4{1.0F}
) {
    const bool use_glass =
        config::CONFIG.iconGlassRefraction && glass_texture != nullptr && glass_texture->loaded;
    const auto& target = use_glass ? *glass_texture : texture;
    if(!target.loaded) {
        return;
    }

    const auto fitted = fit_texture_in_slot(target, width * 0.5, height * 0.5, width, height);
    const float draw_x = x + static_cast<float>(fitted.x / std::max(1.0, renderer.aspect_ratio));
    const float draw_y = y + static_cast<float>(fitted.y);
    tint *= glm::vec4(utils::xmb_icon_colour(openxmb::xmb::wall_clock_now()), 1.0F);
    if(use_glass) {
        renderer.draw_image_glass(target, draw_x, draw_y,
            static_cast<float>(fitted.width), static_cast<float>(fitted.height), tint);
    } else {
        renderer.draw_image(target, draw_x, draw_y,
            static_cast<float>(fitted.width), static_cast<float>(fitted.height), tint);
    }
}

[[nodiscard]] double item_slot_y(int index, int selected_index) noexcept {
    if(index == selected_index) {
        return SettingsSceneMetrics::focus_y;
    }
    if(index < selected_index) {
        constexpr double item_above_base_y = 120.0;
        return item_above_base_y -
            static_cast<double>(selected_index - index - 1) * SettingsSceneMetrics::item_pitch;
    }
    return SettingsSceneMetrics::focus_y + SettingsSceneMetrics::selected_to_next_padding +
        static_cast<double>(index - selected_index) * SettingsSceneMetrics::item_pitch;
}

} // namespace

main_menu::main_menu(app::shell* xmb) : xmb(xmb) {

}

void main_menu::preload(vk::Device device, vma::Allocator allocator, auroreengine::resource_loader& loader) {
    using ::menu::make_simple;
    using ::menu::make_simple_of;

    finish_media_transition();
    const bool reloading=!menus.empty();
    // A reload rebuilds every column; come back to the same one and the same row.
    std::string previous_column;
    unsigned int previous_row=0;
    if(reloading&&selected>=0&&static_cast<std::size_t>(selected)<menus.size()) {
        previous_column=std::string(menus[static_cast<std::size_t>(selected)]->get_name());
        previous_row=menus[static_cast<std::size_t>(selected)]->get_selected_submenu();
    }
    const auto previous_route=settings_controller?settings_controller->route().id:std::string{};
    const auto previous_selection=settings_controller?settings_controller->route().selection:0U;
    current_submenu=nullptr;submenu_stack.clear();in_submenu=false;media_depth_from=media_depth_to=0;
    settings_controller.reset();settings_catalog.reset();
    settings_previous_route.reset();
    settings_render_values.clear();settings_render_icons.clear();
    settings_icon_textures.clear();settings_value_labels.clear();
    menus.clear();

    const auto& asset_directory = config::CONFIG.asset_directory;
    menus.reserve(16);
    const auto panel_entry = [&](std::string label, std::string section, std::string icon = "icon_category_settings.png") {
        return make_simple<menu::action_menu_entry>(std::move(label), asset_directory/"icons"/icon, loader,
            [xmb = this->xmb, section = std::move(section)] { xmb->open_panel(section); return result::success; });
    };
    const auto panel_menu = [&](std::string label, std::filesystem::path icon,
                                std::vector<std::unique_ptr<menu::menu_entry>> rows) {
        return make_simple<menu::simple_menu>(std::move(label), std::move(icon), loader, std::move(rows));
    };
    // shell.hide-empty-categories: drop media categories whose folder is
    // missing or empty so the crossbar shows no dead-end tiles.
    const auto folder_has_content = [&](const std::filesystem::path& folder) {
        if(!config::CONFIG.hide_empty_categories()) {
            return true;
        }
        std::error_code error;
        if(!std::filesystem::is_directory(folder, error)) {
            return false;
        }
        for(std::filesystem::directory_iterator it(folder,
                std::filesystem::directory_options::skip_permission_denied, error), end;
            it != end; it.increment(error)) {
            if(error) {
                break;
            }
            if(!it->path().filename().string().starts_with('.')) {
                return true;
            }
        }
        return false;
    };
    if(!config::CONFIG.hidden_category("users")) {
        std::vector<std::unique_ptr<menu::menu_entry>> rows;
        rows.push_back(panel_entry("Local Profiles", "users", "icon_category_users.png"));
        rows.push_back(panel_entry("System Tools", "system", "icon_action_poweroff.png"));
        rows.push_back(panel_entry("Custom Sections", "custom", "icon_files_folder.png"));
        rows.push_back(make_simple<menu::users_menu>("Operating System Users", asset_directory/"icons/icon_category_users.png", loader, xmb, loader));
        menus.push_back(panel_menu("Users", preferred_icon(asset_directory,"xmb_icon_000.png","icon_category_users.png"),std::move(rows)));
    }
    if(config::CONFIG.console_mode()&&!config::CONFIG.hidden_category("power")) {
        std::vector<std::unique_ptr<menu::menu_entry>> power_rows;
        const auto power_action_entry = [&](std::string label, const char* icon, const char* cmd) {
            return make_simple<menu::action_menu_entry>(std::move(label), asset_directory/"icons"/icon, loader,
                [cmd] {
                    const int rc = std::system(cmd);
                    return rc == 0 ? result::success : result::unsupported;
                });
        };
        power_rows.push_back(power_action_entry("Sleep", "icon_action_suspend.png", "systemctl suspend"));
        power_rows.push_back(power_action_entry("Restart", "icon_action_reboot.png", "systemctl reboot"));
        power_rows.push_back(power_action_entry("Power Off", "icon_action_poweroff.png", "systemctl poweroff"));
        menus.push_back(panel_menu("Power", asset_directory/"icons/icon_category_power.png", std::move(power_rows)));
    }
    if(!config::CONFIG.hidden_category("settings"))
    menus.push_back(make_simple<menu::settings_menu>("Settings"_(),
        config::CONFIG.console_mode()
            ? asset_directory/"icons/icon_category_settings.png"
            : preferred_icon(asset_directory, "xmb_icon_001.png", "icon_category_settings.png"),
        loader, xmb, loader));
    if(!config::CONFIG.hidden_category("photo")&&folder_has_content(config::CONFIG.pictures_path()))
    menus.push_back(make_simple<menu::files_menu>("Photo"_(),
        preferred_icon(asset_directory, "xmb_icon_002.png", "icon_category_photo.png"), loader, xmb,
        config::CONFIG.pictures_path(), loader, menu::files_menu_profile::photo));
    if(!config::CONFIG.hidden_category("music")&&folder_has_content(config::CONFIG.music_path()))
    menus.push_back(make_simple<menu::files_menu>("Music"_(),
        preferred_icon(asset_directory, "xmb_icon_003.png", "icon_category_music.png"), loader, xmb,
        config::CONFIG.music_path(), loader, menu::files_menu_profile::music));
    if(!config::CONFIG.hidden_category("video")&&folder_has_content(config::CONFIG.videos_path()))
    menus.push_back(make_simple<menu::files_menu>("Video"_(),
        preferred_icon(asset_directory, "xmb_icon_004.png", "icon_category_video.png"), loader, xmb,
        config::CONFIG.videos_path(), loader, menu::files_menu_profile::video));
    if(!config::CONFIG.hidden_category("tv")) {
        std::vector<std::unique_ptr<menu::menu_entry>> rows;
        rows.push_back(make_simple<menu::files_menu>("Video Library",asset_directory/"icons/theme/icon_video.png",loader,xmb,config::CONFIG.videos_path(),loader,menu::files_menu_profile::video));
        if constexpr(OPENXMB_ENABLE_BROWSER != 0)
            rows.push_back(panel_entry("Internet Browser","browser","theme/icon_browser.png"));
        menus.push_back(panel_menu("TV / Video Services",asset_directory/"icons/theme/icon_tv.png",std::move(rows)));
    }
    {
        // The library: Games, Android and Applications. Each item is in the
        // column the player put it in, else the one that suits it; a column
        // with nothing in it is not shown.
        std::array<std::vector<menu::app_info>,menu::library_columns.size()> columns;
        for(auto& item : menu::scan_library()) {
            const auto column=menu::library_column_of(item);
            for(std::size_t i=0;i<menu::library_columns.size();++i) {
                if(menu::library_columns[i].id==column) {
                    columns[i].push_back(std::move(item));
                    break;
                }
            }
        }
        for(std::size_t i=0;i<menu::library_columns.size();++i) {
            const auto& column=menu::library_columns[i];
            if(columns[i].empty()||config::CONFIG.hidden_category(column.id)) {
                continue;
            }
            menus.push_back(make_simple<menu::library_menu>(std::string(column.title),
                asset_directory/"icons"/column.icon,loader,std::string(column.id),xmb,loader,std::move(columns[i])));
        }
    }
    if(!config::CONFIG.hidden_category("network")) {
        std::vector<std::unique_ptr<menu::menu_entry>> rows;
        if constexpr(OPENXMB_ENABLE_BROWSER != 0)
            rows.push_back(panel_entry("Internet Browser", "browser", "icon_category_network.png"));
        rows.push_back(panel_entry("System Tools", "system"));
        rows.push_back(panel_entry("OpenXMB Settings", "settings"));
        menus.push_back(panel_menu("Network",preferred_icon(asset_directory,"xmb_icon_006.png","icon_category_network.png"),std::move(rows)));
    }
    if(!config::CONFIG.hidden_category("playstation-network")) {
        std::vector<std::unique_ptr<menu::menu_entry>> rows;
        rows.push_back(panel_entry("Local Account", "account", "xmb_icon_psn.png"));
        rows.push_back(panel_entry("Trophy Collection", "trophies", "theme/icon_trophy.png"));
        menus.push_back(panel_menu("PlayStation Network",preferred_icon(asset_directory,"xmb_icon_psn.png","icon_category_network.png"),std::move(rows)));
    }
    if(!config::CONFIG.hidden_category("friends")) {
        std::vector<std::unique_ptr<menu::menu_entry>> rows;
        rows.push_back(panel_entry("Local Friends", "friends", "icon_category_friends.png"));
        rows.push_back(panel_entry("Local Account", "account", "xmb_icon_psn.png"));
        menus.push_back(panel_menu("Friends",preferred_icon(asset_directory,"xmb_icon_007.png","icon_category_friends.png"),std::move(rows)));
    }
    try {
        openxmb::desktop::Library library;
        for(const auto& section : library.custom_sections())
            if(folder_has_content(section.folder))
            menus.push_back(make_simple<menu::files_menu>(section.title,
                section.icon.empty() ? asset_directory/"icons/icon_files_folder.png" : section.icon,
                loader,xmb,section.folder,loader,menu::files_menu_profile::generic));
    } catch(const std::exception& error) { spdlog::warn("Could not load custom sections: {}",error.what()); }

    if(const auto initial_category = initial_category_index_from_env()) {
        if(*initial_category >= 0 &&
           static_cast<std::size_t>(*initial_category) < menus.size()) {
            selected = *initial_category;
            last_selected = selected;
            last_selected_menu_item = menus[static_cast<std::size_t>(selected)]
                ->get_selected_submenu();
        } else {
            spdlog::warn("Ignoring unsupported {} value", initial_category_env);
        }
    } else if(reloading) {
        selected=std::clamp(selected,0,static_cast<int>(menus.size())-1);
        for(std::size_t i = 0; i < menus.size(); ++i) {
            if(menus[i]->get_name() == previous_column) {
                selected = static_cast<int>(i);
                if(const auto rows=menus[i]->get_submenus_count(); rows>0)
                    menus[i]->select_submenu(std::min(previous_row,rows-1));
                break;
            }
        }
        last_selected = selected;
        last_selected_menu_item = menus[static_cast<std::size_t>(selected)]->get_selected_submenu();
    } else if(config::CONFIG.console_mode()) {
        // A console opens on its games.
        for(std::size_t i = 0; i < menus.size(); ++i) {
            if(const auto* column=dynamic_cast<menu::library_menu*>(menus[i].get())) {
                selected = static_cast<int>(i);
                last_selected = selected;
                last_selected_menu_item = menus[i]->get_selected_submenu();
                break;
            }
        }
    }

    menus[selected]->on_open();

    const auto catalog_path = asset_directory / "catalog/en.json";
    if(auto loaded = openxmb::xmb::load_catalog_file(catalog_path); loaded) {
        settings_catalog.emplace(std::move(*loaded.value));
        // In console mode populate() builds the short handheld layout.
        desktop_settings::populate(*settings_catalog, openxmb::localization::shell_locale::translate,
            config::CONFIG.console_mode());
        if(auto created = openxmb::xmb::SettingsCatalogController::create(*settings_catalog); created) {
            settings_controller.emplace(std::move(*created.controller));
            std::unordered_set<std::string> loaded_icon_refs;
            for(const auto& [node_id, node] : settings_catalog->nodes) {
                (void)node_id;
                if(node.icon_ref.empty() || !loaded_icon_refs.insert(node.icon_ref).second) {
                    continue;
                }
                auto texture = std::make_unique<auroreengine::texture>(device, allocator);
                const auto path = config::CONFIG.resolve_icon(resolve_compat_icon_ref(asset_directory, node.icon_ref)
                    .value_or(asset_directory / "icons/icon_category_settings.png"));
                try {
                    loader.loadTexture(texture.get(), path);
                    std::unique_ptr<auroreengine::texture> glass_texture;
                    if(const auto normal_map = menu::xmb_web_normal_map_for_icon(path)) {
                        glass_texture = std::make_unique<auroreengine::texture>(device,allocator,vk::ImageUsageFlagBits::eSampled,vk::Format::eR8G8B8A8Unorm);
                        loader.loadTexture(glass_texture.get(), *normal_map);
                    }
                    settings_icon_textures.push_back({
                        .semantic_id = node.icon_ref,
                        .texture = std::move(texture),
                        .glass_texture = std::move(glass_texture),
                    });
                } catch(const std::exception& error) {
                    spdlog::debug("Failed to queue Settings icon {} from {}: {}",
                        node.icon_ref, path.string(), error.what());
                }
            }
            spdlog::info("Loaded catalog-backed Settings controller with {} icon binding(s)",
                settings_icon_textures.size());
            settings_render_values.reserve(settings_catalog->nodes.size());
            settings_render_icons.reserve(settings_icon_textures.size());
            for(const auto& entry:settings_icon_textures)
                settings_render_icons.push_back({entry.semantic_id,entry.texture.get(),entry.glass_texture.get()});
            seed_settings_dialog_values();
            if(!reloading) apply_initial_settings_route();
            else if(!previous_route.empty()) {
                std::vector<std::size_t> path;
                if(find_settings_menu_path(*settings_catalog,"category.settings",previous_route,path)) {
                    const auto now=seconds_from_time_point(std::chrono::steady_clock::now());
                    for(const auto index:path) {
                        if(!settings_controller->set_selection(index,now)||!settings_controller->activate(now)) break;
                    }
                    static_cast<void>(settings_controller->set_selection(previous_selection,now));
                }
            }
        } else {
            spdlog::warn("Settings catalog loaded from {}, but controller creation failed",
                catalog_path.string());
        }
    } else if(loaded.error) {
        spdlog::warn("Could not load Settings catalog {}: {}",
            catalog_path.string(), loaded.error->message);
    } else {
        spdlog::warn("Could not load Settings catalog {}", catalog_path.string());
    }
}

void main_menu::begin_move() {
    if(in_submenu||selected<0||static_cast<std::size_t>(selected)>=menus.size()) return;
    if(dynamic_cast<menu::library_menu*>(menus[static_cast<std::size_t>(selected)].get())==nullptr) return;
    move_mode=true;
    move_started=std::chrono::steady_clock::now();
}

// While an item is being moved: up and down reorder it within its column,
// left and right carry it to the neighbouring library column, and confirm or
// back puts it down.
result main_menu::move_action(action action) {
    auto* column=dynamic_cast<menu::library_menu*>(menus[static_cast<std::size_t>(selected)].get());
    if(column==nullptr) {
        move_mode=false;
        return result::unsupported;
    }
    switch(action) {
        case action::up:
        case action::down: {
            const int delta=action==action::up?-1:1;
            const auto before=static_cast<int>(column->get_selected_submenu());
            if(!column->move_selected(delta)) return result::unsupported | result::error_rumble;
            last_selected_menu_item=before;
            last_selected_menu_item_transition=std::chrono::steady_clock::now();
            column->store_order();
            return result::success | result::ok_sound;
        }
        case action::left:
        case action::right: {
            std::size_t from=0;
            for(std::size_t i=0;i<menu::library_columns.size();++i)
                if(menu::library_columns[i].id==column->column()) from=i;
            const int step=action==action::left?-1:1;
            int to=static_cast<int>(from)+step;
            while(to>=0&&to<static_cast<int>(menu::library_columns.size())&&
                  config::CONFIG.hidden_category(menu::library_columns[static_cast<std::size_t>(to)].id)) {
                to+=step;
            }
            if(to<0||to>=static_cast<int>(menu::library_columns.size()))
                return result::unsupported | result::error_rumble;
            const std::string target(menu::library_columns[static_cast<std::size_t>(to)].id);
            const auto id=column->selected_id();
            if(id.empty()) return result::unsupported | result::error_rumble;
            // Record every column's order as it stands, then put the item at
            // the end of its new column.
            for(const auto& entry:menus)
                if(const auto* other=dynamic_cast<menu::library_menu*>(entry.get())) other->store_order();
            for(auto& [name,order]:config::CONFIG.libraryOrder) std::erase(order,id);
            config::CONFIG.libraryOrder[target].push_back(id);
            config::CONFIG.libraryPlacement[id]=target;
            config::CONFIG.save_config();
            xmb->reload_menu();
            bool landed=false;
            for(std::size_t i=0;i<menus.size();++i) {
                auto* candidate=dynamic_cast<menu::library_menu*>(menus[i].get());
                if(candidate==nullptr||candidate->column()!=target) continue;
                const auto row=candidate->index_of(id);
                if(static_cast<int>(i)!=selected) {
                    menus[static_cast<std::size_t>(selected)]->on_close();
                    last_selected=selected;
                    last_selected_transition=std::chrono::steady_clock::now();
                    selected=static_cast<int>(i);
                    menus[i]->on_open();
                }
                if(row>=0) candidate->select_submenu(static_cast<unsigned int>(row));
                last_selected_menu_item=static_cast<int>(candidate->get_selected_submenu());
                landed=true;
                break;
            }
            if(!landed) move_mode=false;
            return result::success | result::ok_sound;
        }
        case action::ok:
        case action::cancel:
            move_mode=false;
            move_started=std::chrono::steady_clock::now();
            column->store_order();
            config::CONFIG.save_config();
            return result::success | result::confirm_sound;
        default:
            return result::success;
    }
}

result main_menu::on_action(action action) {
    if(move_mode) return move_action(action);
    switch(action) {
        case action::left:
            return select_relative(direction::left) ? result::success | result::ok_sound : result::unsupported | result::error_rumble;
        case action::right:
            return select_relative(direction::right) ? result::success | result::ok_sound : result::unsupported | result::error_rumble;
        case action::up:
            return select_relative(direction::up) ? result::success | result::ok_sound : result::unsupported | result::error_rumble;
        case action::down:
            return select_relative(direction::down) ? result::success | result::ok_sound : result::unsupported | result::error_rumble;
        case action::ok:
            return activate_current(action)
                ? result::success | result::confirm_sound
                : result::unsupported | result::error_rumble;
        case action::options:
            if(settings_category_active()) return result::success;
            [[fallthrough]];
        case action::extra:
            return activate_current(action)
                ? result::success | result::ok_sound
                : result::unsupported | result::error_rumble;
        case action::cancel:
            return back() ? (result::success | result::back_sound) : result::unsupported | result::error_rumble;
        default:
            return result::unsupported;
    }
    return result::unsupported;
}

bool main_menu::select_relative(direction dir) {
    if(!in_submenu) {
        if(settings_category_active() &&
           (dir == direction::up || dir == direction::down)) {
            return select_settings_relative(dir);
        }
        if(dir == direction::left) {
            if(selected > 0) {
                select(selected-1);
                return true;
            }
        } else if(dir == direction::right) {
            if(selected < menus.size()-1) {
                select(selected+1);
                return true;
            }
        } else if(dir == direction::up) {
            auto& menu = menus[selected];
            if(menu->get_selected_submenu() > 0) {
                select_menu_item(menu->get_selected_submenu()-1);
                return true;
            }
        } else if(dir == direction::down) {
            auto& menu = menus[selected];
            if(menu->get_selected_submenu() < menu->get_submenus_count()-1) {
                select_menu_item(menu->get_selected_submenu()+1);
                return true;
            }
        }
    } else {
        auto& menu = current_submenu;
        if(dir == direction::up) {
            if(menu->get_selected_submenu() > 0) {
                select_submenu_item(menu->get_selected_submenu()-1);
                return true;
            }
        } else if(dir == direction::down) {
            if(menu->get_selected_submenu() < menu->get_submenus_count()-1) {
                select_submenu_item(menu->get_selected_submenu()+1);
                return true;
            }
        }
    }
    return false;
}
bool main_menu::activate_current(action action) {
    if(settings_category_active()) {
        return activate_settings(action);
    }
    finish_media_transition();
    auto& menu = *menus[selected];
    auto res = current_submenu ? current_submenu->activate(action) : menu.activate(action);
    if(res == result::submenu) {
        auto& entry = current_submenu ? current_submenu->get_submenu(current_submenu->get_selected_submenu())
            : menu.get_submenu(menu.get_selected_submenu());
        if(auto submenu = dynamic_cast<menu::menu*>(&entry)) {
            const auto now=std::chrono::steady_clock::now();
            media_depth_from=in_submenu?static_cast<unsigned>(submenu_stack.size()+1):0;
            media_depth_to=media_depth_from+1;media_returning=false;
            if(current_submenu) {
                outgoing_media=capture_media_scene(*current_submenu,now,false);
                submenu_stack.push_back(current_submenu);
            }
            current_submenu=submenu;in_submenu=true;last_submenu_transition=now;
            current_submenu->on_open();
            last_selected_submenu_item=current_submenu->get_selected_submenu();
            last_selected_submenu_item_transition=now;
            return true;
        }
        return false;
    }
    return res == result::success;
}
bool main_menu::back() {
    if(settings_category_active() && back_settings()) {
        return true;
    }
    if(in_submenu) {
        finish_media_transition();
        const auto now=std::chrono::steady_clock::now();
        outgoing_media=capture_media_scene(*current_submenu,now,false);
        close_after_transition=current_submenu;
        media_depth_from=static_cast<unsigned>(submenu_stack.size()+1);
        media_depth_to=media_depth_from-1;media_returning=true;
        if(submenu_stack.empty()) {current_submenu=nullptr;in_submenu=false;}
        else {current_submenu=submenu_stack.back();submenu_stack.pop_back();last_selected_submenu_item=current_submenu->get_selected_submenu();}
        last_submenu_transition=now;last_selected_submenu_item_transition=now;
        return true;
    }
    return false;
}

bool main_menu::settings_category_active() const {
    return selected == settings_category_index && settings_catalog &&
        settings_controller && !in_submenu;
}

bool main_menu::settings_chrome_collapsed() const {
    return settings_category_active() &&
        settings_controller->route().id != "category.settings";
}

float main_menu::background_blur_px() const {
    if(in_submenu || settings_chrome_collapsed()) {
        return submenu_backdrop_blur_px;
    }
    return 0.0F;
}

float main_menu::background_dim_alpha() const {
    if(in_submenu || settings_chrome_collapsed()) {
        return submenu_backdrop_dim_alpha;
    }
    return 0.0F;
}

void main_menu::seed_settings_dialog_values() {
    if(!settings_catalog) {
        return;
    }
    for(const auto& [node_id, node] : settings_catalog->nodes) {
        (void)node_id;
        if(node.action_id.starts_with("desktop.")) {
            settings_value_labels[node.id]=desktop_settings::display_value(node.action_id.substr(8),xmb);continue;
        }
        if(node.action_id=="simulate.system.language") {
            for(const auto& choice:node.choices)
                if(locale_for_catalog_value(choice.value)==config::CONFIG.language||
                   (choice.value=="english.united.states"&&config::CONFIG.language=="en"))
                    settings_value_labels[node.id]=settings_catalog->text(choice.label_key);
        }
        if(node.action_id.rfind("dialog.", 0) != 0) {
            continue;
        }
        const auto current_value = current_dialog_value(node.action_id);
        if(current_value.empty()) {
            continue;
        }
        auto label = localized_dialog_choice_label(
            *settings_catalog, node.action_id, current_value);
        if(node.action_id=="dialog.colour"&&current_value=="custom")
            label=nlohmann::json::parse(config::CONFIG.shell_snapshot()).value("theme-custom-colour",std::string{"Custom"});
        if(!label.empty()) {
            settings_value_labels[node.id] = std::move(label);
        }
        if(node.action_id=="dialog.theme") {
            if(const auto active=xmb->theme_library()->active();active&&!active->reverted&&!active->id.empty())
                settings_value_labels[node.id]=active->name;
        }
    }
}

bool main_menu::select_settings_relative(direction dir) {
    if(!settings_controller) {
        return false;
    }
    const auto delta = dir == direction::up ? -1 : 1;
    const auto step = settings_controller->move_selection(
        delta, seconds_from_time_point(std::chrono::steady_clock::now()));
    return step && step.changed;
}

bool main_menu::activate_settings(action action) {
    if(action == action::options) return true;
    if(action != action::ok || !settings_catalog || !settings_controller) {
        return false;
    }

    const auto& route = settings_controller->route();
    if(route.layer != openxmb::xmb::NavigationLayer::nested_menu) {
        return false;
    }
    const auto* menu_node = settings_catalog->find_node(route.id);
    if(!menu_node || route.selection >= menu_node->children.size()) {
        return false;
    }

    const auto* selected_node=settings_catalog->find_node(menu_node->children[route.selection]);
    if(selected_node && selected_node->action_id.starts_with("desktop.")) {
        desktop_settings::activate(xmb,*selected_node);return true;
    }
    const auto resolved = openxmb::xmb::resolve_settings_action(
        *settings_catalog, menu_node->children[route.selection]);
    if(!resolved || !resolved.plan) {
        return false;
    }

    switch(resolved.plan->kind) {
        case openxmb::xmb::SettingsActionKind::navigate_menu: {
            const auto previous=settings_controller->route();
            const auto now=std::chrono::steady_clock::now();
            const auto step = settings_controller->activate(
                seconds_from_time_point(now));
            if(step&&step.changed) {
                settings_previous_route=previous;
                settings_route_changed=now;settings_returning=false;
            }
            return step && step.changed;
        }
        case openxmb::xmb::SettingsActionKind::open_dialog:
            if(open_settings_dialog_choice_overlay(*resolved.plan)) {
                return true;
            }
            xmb->emplace_overlay<message_overlay>("Unavailable", "This setting is not supported by this build.");
            return true;
        case openxmb::xmb::SettingsActionKind::simulated_setting:
            if(resolved.plan->may_update_settings_state) {
                if(const auto* node = settings_catalog->find_node(resolved.plan->node_id);
                   node && open_settings_choice_overlay(*node)) {
                    return true;
                }
            }
            xmb->emplace_overlay<message_overlay>("Unavailable", "This setting is not supported by this build.");
            return true;
        default:
            xmb->emplace_overlay<message_overlay>("Unavailable", "This setting is not supported by this build.");
            return true;
    }
}

bool main_menu::back_settings() {
    if(!settings_controller || settings_controller->navigation().stack.size() <= 2) {
        return false;
    }
    const auto previous=settings_controller->route();
    const auto now=std::chrono::steady_clock::now();
    const auto step = settings_controller->back(seconds_from_time_point(now));
    if(step&&step.changed) {
        settings_previous_route=previous;
        settings_route_changed=now;settings_returning=true;
    }
    return step && step.changed;
}

bool main_menu::open_settings_choice_overlay(const openxmb::xmb::CatalogNode& node) {
    if(!settings_catalog || node.choices.empty()) {
        return false;
    }

    std::vector<std::string> labels;
    labels.reserve(node.choices.size());
    for(const auto& choice : node.choices) {
        const auto localized = settings_catalog->text(choice.label_key);
        labels.push_back(localized.empty() ? choice.value : std::string{localized});
    }

    auto selection = static_cast<unsigned int>(
        std::min(node.default_selection, node.choices.size() - 1));
    if(const auto saved = settings_value_labels.find(node.id);
       saved != settings_value_labels.end()) {
        for(std::size_t index = 0; index < labels.size(); ++index) {
            if(labels[index] == saved->second) {
                selection = static_cast<unsigned int>(index);
                break;
            }
        }
    }

    settings_choice_panel_open = true;
    auto* overlay = xmb->emplace_overlay<app::choice_overlay>(
        labels,
        selection,
        [this, node_id = node.id, labels, choices=node.choices,
         system_language=node.action_id=="simulate.system.language"](unsigned int index) {
            settings_choice_panel_open = false;
            if(index < labels.size()) {
                settings_value_labels[node_id] = labels[index];
                config::CONFIG.preferences[node_id] = labels[index];
                if(system_language&&index<choices.size()) {
                    const auto locale=std::string(locale_for_catalog_value(choices[index].value));
                    // The language callback rebuilds the menu. Do not access
                    // this menu again after applying the real locale setting.
                    if(!locale.empty()) config::CONFIG.setLanguage(locale);
                }
                if(!config_override_read_only()) config::CONFIG.save_config();
            }
        },
        [this]() {
            settings_choice_panel_open = false;
        });
    return true;
}

bool main_menu::open_settings_dialog_choice_overlay(
    const openxmb::xmb::SettingsActionPlan& plan) {
    if(!settings_catalog) {
        return false;
    }
    if(plan.target_id=="dialog.font") {
        settings_choice_panel_open=true;auto* host=xmb;
        host->emplace_overlay<choice_overlay>(::app::detail::localize_ui_choices(std::vector<std::string>{"Original","Choose Font File..."}),0,
            [this,host](unsigned i){settings_choice_panel_open=false;if(!i){host->set_font_file("default");config::CONFIG.save_config();}
            else host->emplace_overlay<text_entry_overlay>("Font file",config::CONFIG.fontPath.string(),[host](std::string path){if(std::filesystem::is_regular_file(std::filesystem::u8path(path))){try{host->set_font_file(path);config::CONFIG.save_config();}catch(const std::exception& error){host->emplace_overlay<message_overlay>("Font",error.what());}}else host->emplace_overlay<message_overlay>("Font","Font file not found.");});},[this]{settings_choice_panel_open=false;});return true;
    }
    if(plan.target_id=="dialog.background") {
        settings_choice_panel_open=true;auto* host=xmb;
        host->emplace_overlay<choice_overlay>(::app::detail::localize_ui_choices(std::vector<std::string>{"Original","Classic","Wallpaper","Brightness"}),0,
            [this,host](unsigned i){settings_choice_panel_open=false;
                if(i<2){config::CONFIG.setBackgroundType(i?config::config::background_type::wave:config::config::background_type::original);config::CONFIG.save_config();return;}
                openxmb::xmb::CatalogNode node;node.id="category.settings.desktop.background";node.action_id=i==2?"desktop.theme.wallpaper":"desktop.display.brightness";
                if(i==3)for(const auto* value:{"0.6","0.8","1.0","1.2"})node.choices.push_back({value,value,value});
                desktop_settings::activate(host,node);},[this]{settings_choice_panel_open=false;});return true;
    }
    if(plan.target_id=="dialog.theme") {
        const auto library=xmb->theme_library();
        const auto installed=library->discover();
        const auto active=library->active();
        std::vector<std::string> labels{"Original","Classic"};
        unsigned int selection=current_ps3_theme_value()=="classic"?1U:0U;
        for(const auto& theme:installed) {
            if(active&&active->id==theme.id) selection=static_cast<unsigned int>(labels.size());
            labels.push_back(theme.name);
        }
        labels.push_back("Install / manage themes...");
        settings_choice_panel_open=true;
        auto* shell=xmb;
        shell->emplace_overlay<app::choice_overlay>(labels,selection,
            [this,shell,library,installed,labels,node_id=plan.node_id](unsigned int index) {
                settings_choice_panel_open=false;
                if(index>=labels.size()) return;
                if(index==labels.size()-1) {shell->open_panel("themes");return;}
                settings_value_labels[node_id]=labels[index];
                // Applying a pack reloads icon resources and reconstructs the
                // menu; retain no references to this instance afterwards.
                if(index>=2) {shell->apply_theme(library->apply(installed[index-2].id));return;}
                shell->apply_theme(library->revert());
                if(index==1) {
                    config::CONFIG.setBackgroundType(config::config::background_type::wave);
                    if(!config_override_read_only()) config::CONFIG.save_config();
                }
            },[this] {settings_choice_panel_open=false;});
        return true;
    }
    if(plan.target_id.rfind("dialog.", 0) != 0) {
        return false;
    }

    const auto dialog_it = settings_catalog->dialogs.find(plan.target_id);
    if(dialog_it == settings_catalog->dialogs.end() ||
       dialog_it->second.choices.empty()) {
        return false;
    }

    const auto& dialog = dialog_it->second;
    std::vector<openxmb::xmb::CatalogChoice> selectable_choices;
    selectable_choices.reserve(dialog.choices.size());
    std::string top_action_label;
    for(const auto& choice : dialog.choices) {
        const bool top_action =
            settings_dialog_top_action(plan.target_id, choice.value);
        if(top_action) {
            const auto localized = settings_catalog->text(choice.label_key);
            top_action_label =
                localized.empty() ? choice.value : std::string{localized};
            continue;
        }
        selectable_choices.push_back(choice);
    }
    if(selectable_choices.empty()) {
        return false;
    }

    std::vector<std::string> labels;
    labels.reserve(selectable_choices.size());
    for(const auto& choice : selectable_choices) {
        const auto localized = settings_catalog->text(choice.label_key);
        labels.push_back(localized.empty() ? choice.value : std::string{localized});
    }

    const auto current_value = current_dialog_value(plan.target_id);
    unsigned int selection = 0;
    if(!current_value.empty()) {
        for(std::size_t index = 0; index < selectable_choices.size(); ++index) {
            if(selectable_choices[index].value == current_value) {
                selection = static_cast<unsigned int>(index);
                break;
            }
        }
    }
    const auto original_value_label=settings_value_labels.contains(plan.node_id)?settings_value_labels.at(plan.node_id):std::string{};
    if(selection < labels.size()) {
        settings_value_labels[plan.node_id] = labels[selection];
    }

    std::vector<glm::vec3> swatches;
    if(plan.target_id == "dialog.colour") {
        swatches.reserve(selectable_choices.size());
        for(const auto& choice : selectable_choices) {
            const auto colour = ps3_colour_for_value(choice.value)
                .value_or(ps3_colour_entry{choice.value, {0.72F, 0.75F, 0.79F}, {0.72F, 0.75F, 0.79F}});
            swatches.push_back(colour.swatch);
        }
    }

    const auto dialog_id = plan.target_id;
    const auto node_id = plan.node_id;
    const auto original_selection = selection;
    const auto original_colour=config::CONFIG.themeCustomColour;
    const auto original_colour_mode=config::CONFIG.themeOriginalColour;
    const auto original_background=config::CONFIG.backgroundType;
    const auto original_font=config::CONFIG.fontPath;
    const auto original_label=original_value_label;
    settings_choice_panel_open = true;
    auto* overlay = xmb->emplace_overlay<app::choice_overlay>(
        labels,
        selection,
        [this,
         dialog_id,
         node_id,
         labels,
         choices = selectable_choices](unsigned int index) {
            settings_choice_panel_open = false;
            if(index >= choices.size() || index >= labels.size()) {
                return;
            }

            const auto& choice = choices[index];
            const bool applied = apply_settings_dialog_choice_value(
                dialog_id, choice.value, true);

            if(applied) {
                settings_value_labels[node_id] = labels[index];
            }

            if(applied) {
                if(config_override_read_only()) {
                    spdlog::info("Skipping config save because OPENXMB_CONFIG_READ_ONLY is set");
                } else {
                    config::CONFIG.save_config();
                }
            }
        },
        [this, dialog_id, node_id, labels, choices = selectable_choices,
         original_selection,original_colour,original_colour_mode,original_background,original_font,original_label]() {
            settings_choice_panel_open = false;
            if(dialog_id=="dialog.colour") {
                config::CONFIG.setThemeCustomColour(original_colour);
                config::CONFIG.setThemeOriginalColour(original_colour_mode);
            } else if(dialog_id=="dialog.background") {
                config::CONFIG.setBackgroundType(original_background);
            } else if(dialog_id=="dialog.font") {
                config::CONFIG.setFontPath(original_font.string());
            } else if(original_selection < choices.size()) {
                (void)apply_settings_dialog_choice_value(
                    dialog_id, choices[original_selection].value, false);
            }
            settings_value_labels[node_id] = original_label;
        },
        [dialog_id, choices = selectable_choices](unsigned int index) {
            if(index < choices.size()) {
                (void)apply_settings_dialog_choice_value(
                    dialog_id, choices[index].value, false);
            }
        });

    if(!swatches.empty()) {
        overlay->set_colour_swatches(swatches);
    }
    if(!top_action_label.empty()) {
        const float top_action_y = plan.target_id == "dialog.theme"
            ? 270.0F / 1080.0F
            : 390.0F / 1080.0F;
        overlay->set_top_action(top_action_label, top_action_y);
    }
    return true;
}

void main_menu::apply_initial_settings_route() {
    if(!settings_catalog || !settings_controller) {
        return;
    }

    const auto target_menu = env_string(initial_settings_menu_env);
    if(!target_menu) {
        return;
    }
    if(!openxmb::xmb::is_settings_catalog_id(*target_menu)) {
        spdlog::warn("Ignoring non-Settings {} value", initial_settings_menu_env);
        return;
    }

    selected = settings_category_index;
    last_selected = selected;
    last_selected_menu_item = menus[static_cast<std::size_t>(selected)]
        ->get_selected_submenu();

    std::vector<std::size_t> path;
    if(!find_settings_menu_path(
           *settings_catalog, "category.settings", *target_menu, path)) {
        spdlog::warn("Ignoring unknown {} value '{}'",
            initial_settings_menu_env, *target_menu);
        return;
    }

    const auto now = seconds_from_time_point(std::chrono::steady_clock::now());
    for(const auto selection : path) {
        const auto selected_child = settings_controller->set_selection(selection, now);
        if(!selected_child) {
            spdlog::warn("Could not select Settings route segment {} for '{}'",
                selection, *target_menu);
            return;
        }
        const auto entered = settings_controller->activate(now);
        if(!entered || !entered.changed) {
            spdlog::warn("Could not enter Settings route segment {} for '{}'",
                selection, *target_menu);
            return;
        }
    }

    if(const auto selection = env_size(initial_settings_selection_env)) {
        if(const auto selected_child = settings_controller->set_selection(*selection, now);
           !selected_child) {
            spdlog::warn("Ignoring unsupported {} value",
                initial_settings_selection_env);
        }
    }

    if(env_truthy(initial_settings_open_choice_env) &&
       !activate_settings(action::ok)) {
        spdlog::warn("Could not open initial Settings choice panel");
    }
}

void main_menu::render_settings_parent_layer(auroreengine::gui_renderer& renderer) {
    if(!settings_catalog || !settings_controller) {
        return;
    }
    const auto& stack = settings_controller->navigation().stack;
    if(stack.size() <= 2) {
        return;
    }

    const auto& parent_route = stack[stack.size() - 2];
    const auto* parent_menu = settings_catalog->find_node(parent_route.id);
    if(!parent_menu || parent_menu->children.empty() ||
       parent_route.selection >= parent_menu->children.size()) {
        return;
    }

    const auto layout = make_contained_layout(renderer);
    const auto find_icon = [this](std::string_view icon_ref)
        -> const settings_icon_texture* {
        for(const auto& icon : settings_icon_textures) {
            if(icon.semantic_id == icon_ref) {
                return &icon;
            }
        }
        return nullptr;
    };

    const auto draw_parent_icon = [&](const openxmb::xmb::CatalogNode& node,
                                      double center_x,
                                      double center_y,
                                      double alpha) {
        const auto* icon = find_icon(node.icon_ref);
        if(icon == nullptr || !icon->texture || !icon->texture->loaded) {
            return;
        }
        draw_icon(renderer, *icon->texture, icon->glass_texture.get(), layout,
            center_x, center_y, openxmb::xmb::SettingsSceneMetrics::icon_extent,
            alpha);
    };

    for(std::size_t index = 0; index < parent_menu->children.size(); ++index) {
        const auto* node = settings_catalog->find_node(parent_menu->children[index]);
        if(node == nullptr) {
            continue;
        }

        const auto selected_parent = index == parent_route.selection;
        const auto signed_delta = static_cast<int>(index) -
            static_cast<int>(parent_route.selection);
        const auto center_y = openxmb::xmb::SettingsSceneMetrics::focus_y +
            static_cast<double>(signed_delta) * settings_parent_spacing;
        if(center_y < openxmb::xmb::SettingsSceneMetrics::clip_top - 40.0 ||
           center_y > openxmb::xmb::SettingsSceneMetrics::clip_bottom + 40.0) {
            continue;
        }

        const auto distance = std::abs(signed_delta);
        auto alpha = selected_parent
            ? 0.92
            : std::max(0.12, 0.42 - static_cast<double>(distance - 1) * 0.05);
        if(center_y < 40.0) {
            alpha *= std::max(0.0, center_y / 40.0);
        }
        if(center_y > openxmb::xmb::SettingsSceneMetrics::logical_height - 20.0) {
            alpha *= std::max(0.0,
                (openxmb::xmb::SettingsSceneMetrics::logical_height - center_y) / 20.0);
        }
        draw_parent_icon(*node,
            selected_parent ? settings_parent_col_x : settings_sibling_col_x,
            center_y, alpha);
    }

    const auto arrow_alpha = 0.95F;
    const auto arrow_top = glm::vec4(206.0F / 255.0F, 206.0F / 255.0F,
        208.0F / 255.0F, arrow_alpha);
    const auto arrow_bottom = glm::vec4(120.0F / 255.0F, 120.0F / 255.0F,
        124.0F / 255.0F, arrow_alpha);
    const std::array<auroreengine::simple_renderer::vertex_data, 3> chevron{{
        {{layout.x(430.0), layout.y(openxmb::xmb::SettingsSceneMetrics::focus_y)},
            arrow_top, {0.0F, 0.5F}},
        {{layout.x(462.0), layout.y(openxmb::xmb::SettingsSceneMetrics::focus_y - 15.0)},
            arrow_top, {1.0F, 0.0F}},
        {{layout.x(462.0), layout.y(openxmb::xmb::SettingsSceneMetrics::focus_y + 15.0)},
            arrow_bottom, {1.0F, 1.0F}},
    }};
    renderer.draw_generic(chevron);
}

void main_menu::select(int index) {
    if(index == selected) {
        return;
    }
    finish_media_transition();
    if(index < 0 || index >= menus.size()) {
        return;
    }

    last_selected = selected;
    last_selected_transition = std::chrono::steady_clock::now();
    selected = index;

    menus[last_selected]->on_close();
    menus[selected]->on_open();

    last_selected_menu_item = menus[selected]->get_selected_submenu();
}
void main_menu::select_menu_item(int index) {
    auto& menu = menus[selected];
    if(index == menu->get_selected_submenu()) {
        return;
    }
    if(index < 0 || index >= menu->get_submenus_count()) {
        return;
    }

    last_selected_menu_item = menu->get_selected_submenu();
    last_selected_menu_item_transition = std::chrono::steady_clock::now();
    menu->select_submenu(index);
}
void main_menu::select_submenu_item(int index) {
    auto& menu = current_submenu;
    if(index == menu->get_selected_submenu()) {
        return;
    }
    if(index < 0 || index >= menu->get_submenus_count()) {
        return;
    }

    last_selected_submenu_item = menu->get_selected_submenu();
    last_selected_submenu_item_transition = std::chrono::steady_clock::now();
    menu->select_submenu(index);
}

void main_menu::render(auroreengine::gui_renderer& renderer) {
    auto now = std::chrono::steady_clock::now();

    const auto submenu_progress = ease_out_cubic(
        progress(now, last_submenu_transition, transition_submenu_activate_duration));
    const auto partial = last_submenu_transition.time_since_epoch().count() == 0
        ? (in_submenu ? 1.0 : 0.0)
        : (in_submenu ? submenu_progress : 1.0 - submenu_progress);
    bool in_submenu_now = in_submenu || partial > 0.0;

    if(!settings_category_active()) {
        const auroreengine::texture* bg = nullptr;
        if(in_submenu && current_submenu && current_submenu->get_submenus_count() > 0) {
            const auto idx = std::min(current_submenu->get_selected_submenu(), current_submenu->get_submenus_count() - 1);
            bg = current_submenu->get_submenu(idx).get_background();
        } else if(selected >= 0 && static_cast<std::size_t>(selected) < menus.size() &&
                  menus[static_cast<std::size_t>(selected)]->get_submenus_count() > 0) {
            auto& cat_menu = *menus[static_cast<std::size_t>(selected)];
            const auto idx = std::min(cat_menu.get_selected_submenu(), cat_menu.get_submenus_count() - 1);
            bg = cat_menu.get_submenu(idx).get_background();
        }
        if(bg != nullptr && bg->loaded && bg->imageView) {
            renderer.draw_image(bg->imageView.get(), 0.0f, 0.0f,
                static_cast<float>(renderer.aspect_ratio), 1.0f,
                {1.0f, 1.0f, 1.0f, 0.85f});
        }
    }

    if(settings_category_active()&&settings_catalog)for(const auto& [id,node]:settings_catalog->nodes)if(node.action_id.starts_with("desktop."))settings_value_labels[id]=desktop_settings::display_value(node.action_id.substr(8),xmb);
    render_crossbar(renderer, now);
    if(settings_category_active()) {
        render_settings_scene(renderer, now);
    }

    std::vector<std::pair<action, std::string>> buttons{};
    buttons.reserve(5);
    menus[selected]->get_button_actions(buttons);
    if((in_submenu_now&&current_submenu)||outgoing_media)render_submenu(renderer,now);
    if(in_submenu_now&&current_submenu)current_submenu->get_button_actions(buttons);
    // One hint bar on every screen that takes input.
    if(move_mode) {
        render_move_hint(renderer, now);
        const std::vector<std::pair<action, std::string>> done{{action::ok, "Done"_()}};
        xmb->render_hint_bar(renderer, done);
    } else if(in_submenu_now) {
        xmb->render_hint_bar(renderer, buttons);
    } else if(!settings_category_active() &&
              dynamic_cast<menu::library_menu*>(menus[static_cast<std::size_t>(selected)].get()) != nullptr) {
        const std::vector<std::pair<action, std::string>> library_buttons{
            {action::ok, "Start"_()}, {action::options, "Options"_()}};
        xmb->render_hint_bar(renderer, library_buttons);
    } else if(settings_category_active() && settings_controller &&
              settings_controller->route().id != "category.settings") {
        // Inside a Settings group: Back leads out of it.
        const std::vector<std::pair<action, std::string>> settings_buttons{
            {action::ok, "Select"_()}, {action::cancel, "Back"_()}};
        xmb->render_hint_bar(renderer, settings_buttons);
    } else {
        const std::vector<std::pair<action, std::string>> select{{action::ok, "Select"_()}};
        xmb->render_hint_bar(renderer, select);
    }
}

void main_menu::render_settings_scene(auroreengine::gui_renderer& renderer, time_point now) {
    if(!settings_catalog || !settings_controller) {
        return;
    }

    auto& value_overrides=settings_render_values;
    value_overrides.clear();
    for(const auto& [node_id, label] : settings_value_labels) {
        value_overrides.push_back({
            .node_id = node_id,
            .localized_value = label,
        });
    }

    const auto transition_progress=progress(now,settings_route_changed,transition_submenu_activate_duration);
    const bool transitioning=settings_previous_route&&transition_progress<1.0;
    const auto presence=transitioning?openxmb::xmb::SettingsScenePresence::entering:openxmb::xmb::SettingsScenePresence::visible;
    const auto input=openxmb::xmb::make_settings_scene_input(settings_controller->route(),
        settings_controller->previous_selection(),settings_controller->focus_transition_started_seconds(),
        presence,seconds_from_time_point(settings_route_changed));
    auto sampled=openxmb::xmb::sample_settings_scene(*settings_catalog,input,
        seconds_from_time_point(now),value_overrides);
    if(!sampled) {
        return;
    }

    const openxmb::xmb::SettingsSceneRenderOptions options{
        .glass_icons=config::CONFIG.iconGlassRefraction,
        .suppress_value_labels=settings_choice_panel_open||xmb->has_choice_overlay()
    };
    if(transitioning) {
        const auto previous_input=openxmb::xmb::make_settings_scene_input(*settings_previous_route,
            settings_previous_route->selection,0.0,
            openxmb::xmb::SettingsScenePresence::exiting,seconds_from_time_point(settings_route_changed));
        auto previous=openxmb::xmb::sample_settings_scene(*settings_catalog,previous_input,
            seconds_from_time_point(now),value_overrides);
        if(previous) {
            if(!settings_returning) for(auto& row:previous.snapshot.rows) {
                row.icon_center_x-=2.0*previous.snapshot.content_x_shift;
                row.label_x-=2.0*previous.snapshot.content_x_shift;
                row.description_x-=2.0*previous.snapshot.content_x_shift;
            }
            settings_scene_renderer.render(renderer,previous.snapshot,settings_render_icons,options);
        }
        if(settings_returning) for(auto& row:sampled.snapshot.rows) {
            row.icon_center_x-=2.0*sampled.snapshot.content_x_shift;
            row.label_x-=2.0*sampled.snapshot.content_x_shift;
            row.description_x-=2.0*sampled.snapshot.content_x_shift;
        }
        renderer.push_color({1,1,1,static_cast<float>(sampled.snapshot.presence)});
        render_settings_parent_layer(renderer);
        renderer.pop_color();
    } else {
        settings_previous_route.reset();
        render_settings_parent_layer(renderer);
    }
    settings_scene_renderer.render(renderer,sampled.snapshot,settings_render_icons,options);
}

void main_menu::render_crossbar(auroreengine::gui_renderer& renderer, time_point now) {
    const auto layout = make_contained_layout(renderer);
    const xmb_canvas canvas(renderer);

    const auto category_linear = selected == last_selected
        ? 1.0
        : progress(now, last_selected_transition, transition_duration);
    const auto category_eased = ease_out_cubic(category_linear);
    const auto visual_selection = static_cast<double>(last_selected) +
        static_cast<double>(selected - last_selected) * category_eased;

    const auto submenu_linear = progress(
        now, last_submenu_transition, transition_submenu_activate_duration);
    const auto submenu_transition = last_submenu_transition.time_since_epoch().count() == 0
        ? (in_submenu ? 1.0 : 0.0)
        : media_motion::category(media_depth_from,media_depth_to,config::CONFIG.enabled("power.reduced.motion",false)?1.0:submenu_linear);
    auto chrome_transition = std::max(
        submenu_transition, settings_chrome_collapsed() ? 1.0 : 0.0);
    if(settings_category_active()&&settings_previous_route) {
        const auto p=ease_out_cubic(progress(now,settings_route_changed,transition_submenu_activate_duration));
        const double from=settings_previous_route->id!="category.settings"?1.0:0.0;
        const double to=settings_chrome_collapsed()?1.0:0.0;
        chrome_transition=from+(to-from)*p;
    }

    const auto x_compress = std::clamp(
        (static_cast<double>(renderer.frame_size.width) /
         std::max(1.0, static_cast<double>(renderer.frame_size.height))) /
            (logical_width / logical_height),
        0.65, 1.0);
    const auto category_spacing = kCategorySpacing * x_compress;

    for(std::size_t index = 0; index < menus.size(); ++index) {
        const auto active = static_cast<int>(index) == selected;
        const auto distance = std::abs(static_cast<int>(index) - selected);
        auto center_x = kActiveCategoryX +
            (static_cast<double>(index) - visual_selection) * category_spacing;
        auto center_y = kCategoryBaselineY + (active ? -10.0 : 0.0);
        auto extent = active ? kActiveCategoryIconExtent : kInactiveCategoryIconExtent;
        // On the console the category icons sit a little smaller, so the
        // glass lens of the selected row below has room.
        if(config::CONFIG.console_mode()) {
            extent *= active ? 0.80 : 0.85;
            center_y -= active ? 14.0 : 8.0;
        }
        auto alpha = active ? 0.90 : 0.85;

        if(active) {
            center_x -= 257.0 * chrome_transition;
            extent *= 1.0 + (kInactiveItemScaleTier - 1.0) * chrome_transition;
            alpha *= 1.0 + (0.6 - 1.0) * chrome_transition;
        } else {
            alpha *= 1.0 - chrome_transition;
        }

        // The PSN sphere fills substantially more of its source canvas than
        // the numbered icon family. xmb-web measures a 0.68 correction.
        if(index == 8) {
            extent *= 0.68;
        }

        draw_icon(renderer, menus[index]->get_icon(),
            menus[index]->get_glass_icon(), layout,
            center_x, center_y, extent, alpha);

        if(active && !config::CONFIG.console_mode()) {
            canvas.text(menus[index]->get_name(),center_x,kCategoryLabelY+18,24,
                {1,1,1,static_cast<float>(0.95*(1.0-0.55*chrome_transition))},0,true);
        }
    }

    const auto render_item_rail = [&](const menu::menu& menu,
                                      int selected_item,
                                      int previous_item,
                                      double transition,
                                      double x_shift,
                                      double alpha_multiplier) {
        const auto eased = ease_out_cubic(transition);
        const auto count = static_cast<int>(menu.get_submenus_count());
        // A column with any wide logo keeps the wide row box; a column of
        // square icons uses a square one, so labels sit close to them.
        bool wide_art = false;
        for(int index = 0; index < count && !wide_art; ++index) {
            const auto& candidate = menu.get_submenu(static_cast<unsigned int>(index));
            const auto& texture = candidate.get_icon();
            wide_art = candidate.has_custom_icon() && texture.height > 0 &&
                static_cast<double>(texture.width) > 1.2 * static_cast<double>(texture.height);
        }
        using lens = openxmb::xmb::SelectionLens;
        const double column_label_shift = lens::label_shift_for(SettingsSceneMetrics::focused_icon_extent, wide_art);
        constexpr double kZoomedSelectedItemYFrac = 0.50;
        const auto focus_y_shift = (config::CONFIG.ui_scale() > 1.0)
            ? (((layout.framebuffer_height * kZoomedSelectedItemYFrac - layout.offset_y) / layout.scale) - SettingsSceneMetrics::focus_y)
            : 0.0;
        for(int index = 0; index < count; ++index) {
            const auto old_y = item_slot_y(index, previous_item);
            const auto new_y = item_slot_y(index, selected_item);
            const auto center_y = old_y + (new_y - old_y) * eased + focus_y_shift;
            if(center_y < -80.0 || center_y > logical_height + 80.0) {
                continue;
            }
            const auto focused = index == selected_item;
            const auto extent = focused ? SettingsSceneMetrics::focused_icon_extent : SettingsSceneMetrics::icon_extent;
            if(layout.y(center_y - extent * 0.5) * layout.framebuffer_height < 16.0) {
                continue;
            }
            const auto top_fade = (config::CONFIG.ui_scale() > 1.0)
                ? std::clamp((center_y - (kCategoryBaselineY + 80.0)) / 90.0, 0.0, 1.0)
                : 1.0;
            const auto screen_bottom_y = layout.y(center_y + extent * 0.5) * layout.framebuffer_height;
            const auto bottom_fade = (config::CONFIG.ui_scale() > 1.0)
                ? std::clamp((layout.framebuffer_height * 0.89 - screen_bottom_y) / (layout.framebuffer_height * 0.06), 0.0, 1.0)
                : 1.0;
            const auto edge_fade = std::min(top_fade, bottom_fade);
            if(edge_fade <= 0.001) {
                continue;
            }

            const auto alpha = (focused ? kFocusAlpha : kInactiveItemAlpha) * alpha_multiplier * edge_fade;
            auto& entry = menu.get_submenu(static_cast<unsigned int>(index));
            // A slot, not an unbounded string: the lens used to grow with the
            // title until it ran off the panel. Long labels scroll when
            // focused and ellipsise when not; the lens uses the same slot.
            const double label_left = SettingsSceneMetrics::label_x + column_label_shift + x_shift;
            const double label_right_edge =
                static_cast<double>(layout.framebuffer_width - layout.offset_x) / layout.scale -
                openxmb::xmb::ui::px(openxmb::xmb::ui::space::lg);
            const auto label_room = std::max(120.0, label_right_edge - label_left);
            const auto measure_label = [&](std::string_view text) {
                return static_cast<double>(renderer.text_width_pixels(text,
                    static_cast<float>(SettingsSceneMetrics::active_label_size * layout.scale))) / layout.scale;
            };
            if(focused && config::CONFIG.console_mode()) {
                // The focused row rests on a lens of clear glass; while it is
                // being moved, on a lit capsule. Same shape in every column.
                double text_right = std::min(label_left + measure_label(entry.get_name()), label_right_edge);
                if(!entry.get_description().empty())
                    text_right = std::max(text_right, std::min(label_left + renderer.text_width_pixels(entry.get_description(),
                        static_cast<float>(SettingsSceneMetrics::description_size * layout.scale)) / layout.scale, label_right_edge));
                const auto height = lens::height(extent);
                const bool current_column = &menu == menus[static_cast<std::size_t>(selected)].get();
                const bool grabbed = move_mode && current_column;
                auto left = lens::left(SettingsSceneMetrics::icon_center_x + x_shift, extent, wide_art);
                // The card ends at the slot edge, never at the panel edge.
                auto right = std::min(text_right + lens::right_padding, label_right_edge);
                if(current_column) {
                    // The lens stays lit across moves; only its width eases.
                    const float now_ms = static_cast<float>(std::chrono::duration<double, std::milli>(now.time_since_epoch()).count());
                    lens_focus.move_to({static_cast<float>(left), 0.0F, static_cast<float>(right - left), 0.0F}, now_ms);
                    const auto at = lens_focus.at(now_ms);
                    left = at.x;
                    right = at.x + at.w;
                }
                const auto lens_alpha = static_cast<float>(alpha);
                canvas.glass(static_cast<float>(left), static_cast<float>(center_y - height * 0.5),
                    static_cast<float>(right - left), static_cast<float>(height),
                    grabbed ? openxmb::xmb::glass::variant::focus : openxmb::xmb::glass::variant::clear,
                    static_cast<float>(openxmb::xmb::SelectionLens::radius), lens_alpha);
            }
            // On the console a square icon is fitted to the same box as a
            // logo: its height is the box's height.
            const auto icon_extent = (config::CONFIG.console_mode() && !entry.has_custom_icon())
                ? openxmb::xmb::SelectionLens::box_height(extent) : extent;
            draw_icon(renderer, entry.get_icon(), entry.get_glass_icon(), layout,
                SettingsSceneMetrics::icon_center_x + x_shift, center_y, icon_extent, alpha,
                entry.has_custom_icon());

            const auto& icon_tex = entry.get_icon();
            // Covers are cards wider than an icon; their labels move right.
            const double wide_label_shift = config::CONFIG.console_mode() ? column_label_shift
                : (entry.has_custom_icon() ? openxmb::xmb::SelectionLens::label_shift : 0.0);
            const auto label_size = focused ? SettingsSceneMetrics::active_label_size : SettingsSceneMetrics::inactive_label_size;
            const auto text_value = focused ? 1.0f : 235.0f / 255.0f;
            const float item_pulse = 0.8F + 0.4F * std::sin(static_cast<float>(seconds_from_time_point(now)) * 1.848F);
            const auto label_px = static_cast<float>(layout.offset_x + (SettingsSceneMetrics::label_x + wide_label_shift + x_shift) * layout.scale);
            const auto label_py = static_cast<float>(layout.offset_y + (center_y + (focused ? 3.0 : 11.0)) * layout.scale);
            const std::string_view name(entry.get_name());
            const auto fitting = openxmb::xmb::ui::fitting_count(name,static_cast<float>(label_room),measure_label);
            const float glyph_px = fitting ? static_cast<float>(measure_label(name.substr(0,1))) : 1.0F;
            std::string_view shown=name;
            float shift=0.0F;
            if(name.size()>fitting && fitting>0) {
                if(focused) {
                    // The one slot is shared by every column; only the item
                    // with focus ever animates.
                    static openxmb::xmb::ui::scrolling_text slot;
                    static std::string_view bound;
                    const double now_ms=std::chrono::duration<double,std::milli>(now.time_since_epoch()).count();
                    if(bound.data()!=name.data()||bound.size()!=name.size()||!slot.focused())
                        slot.set_text(name,fitting,true,now_ms);
                    bound=name;
                    const auto run=slot.sample(now_ms,glyph_px);
                    shown=name.substr(run.first,run.count);
                    shift=-run.offset_px;
                } else {
                    static std::string truncated;
                    truncated.clear();
                    truncated.append(name.substr(0,fitting));
                    while(!truncated.empty()&&(static_cast<unsigned char>(truncated.back())&0xC0)==0x80) truncated.pop_back();
                    while(!truncated.empty()&&truncated.back()==' ') truncated.pop_back();
                    if(!truncated.empty()) truncated+="...";
                    shown=truncated;
                }
            }
            // Clip to the slot so the spare glyph of a scrolled run cannot
            // spill past the safe area on the right of the panel.
            renderer.set_clip(label_px / layout.framebuffer_width,
                (label_py - label_size * layout.scale) / layout.framebuffer_height,
                label_room * layout.scale / layout.framebuffer_width,
                label_size * 1.5F * layout.scale / layout.framebuffer_height);
            renderer.draw_text_pixels(shown, label_px + shift * layout.scale, label_py,
                static_cast<float>(label_size * layout.scale),
                {text_value, text_value, text_value, static_cast<float>(alpha)},
                focused ? 0.72F : 0.7F, focused ? item_pulse : 0.0F);
            renderer.reset_clip();
            if(focused&&!entry.get_description().empty())
                renderer.draw_text_pixels(entry.get_description(), label_px,
                    static_cast<float>(layout.offset_y + (center_y + 32.0) * layout.scale),
                    static_cast<float>(SettingsSceneMetrics::description_size * layout.scale),
                    {1, 1, 1, static_cast<float>(alpha * 0.82)}, 0.7F);
        }
    };

    const auto rail_alpha = std::max(0.0, 1.0 - chrome_transition);
    if(settings_category_active()) {
        return;
    }

    if(selected != last_selected && category_linear < 1.0) {
        const auto travel = static_cast<double>(selected - last_selected) * category_spacing;
        const auto old_shift = -travel * category_eased;
        const auto new_shift = travel * (1.0 - category_eased);
        const auto old_alpha = std::max(0.0, 1.0 - category_eased / 0.4) * rail_alpha;
        const auto new_alpha = std::max(0.0, (category_eased - 0.5) / 0.5) * rail_alpha;

        const auto& old_menu = *menus[static_cast<std::size_t>(last_selected)];
        const auto old_selected = static_cast<int>(old_menu.get_selected_submenu());
        render_item_rail(old_menu, old_selected, old_selected, 1.0,
            old_shift, old_alpha);

        const auto& new_menu = *menus[static_cast<std::size_t>(selected)];
        const auto new_selected = static_cast<int>(new_menu.get_selected_submenu());
        render_item_rail(new_menu, new_selected, new_selected, 1.0,
            new_shift, new_alpha);
    } else {
        const auto& current = *menus[static_cast<std::size_t>(selected)];
        const auto selected_item = static_cast<int>(current.get_selected_submenu());
        const auto item_linear = selected_item == last_selected_menu_item
            ? 1.0
            : progress(now, last_selected_menu_item_transition, transition_menu_item_duration);
        render_item_rail(current, selected_item, last_selected_menu_item,
            item_linear, 0.0, rail_alpha);
        if(item_linear >= 1.0) {
            last_selected_menu_item = selected_item;
        }
    }
}

void main_menu::render_move_hint(auroreengine::gui_renderer& renderer, time_point now) {
    namespace glass=openxmb::xmb::glass;
    const auto* column=dynamic_cast<menu::library_menu*>(menus[static_cast<std::size_t>(selected)].get());
    if(column==nullptr) return;
    const overlay_canvas canvas(renderer);
    const auto pose=glass::panel_in(static_cast<float>(std::chrono::duration<double,std::milli>(now-move_started).count()));
    const float center=(canvas.visible_left()+canvas.visible_right())*0.5F;
    const float width=std::min(896.0F,canvas.visible_right()-canvas.visible_left()-72.0F);
    constexpr float height=120.0F,hint_reserve=112.0F;
    // A card just above the hint bar, which carries "Done".
    const float top=canvas.visible_bottom()-hint_reserve-height;
    canvas.glass(center-width*pose.scale*0.5F,top+height*(1.0F-pose.scale)*0.5F,width*pose.scale,height*pose.scale,
        glass::variant::regular,glass::card_radius,pose.alpha);
    canvas.text(std::string("Moving ")+column->selected_title(),center,top+48.0F,30,{1,1,1,glass::text_primary*pose.alpha},0,true);
    // Arrowheads and words on one centred line.
    using direction=xmb_canvas::arrow_direction;
    const auto measure=[&](const std::string& text) {
        return renderer.text_width_pixels(text,22.0F*canvas.scale*static_cast<float>(config::CONFIG.textScale))/canvas.scale;
    };
    const std::string reorder="Reorder",change="Change column";
    constexpr float arrows=40.0F,gap=48.0F;
    const float total=arrows+measure(reorder)+gap+arrows+measure(change);
    float cursor=center-total*0.5F;
    const float line=top+88.0F,middle=line-8.0F;
    const glm::vec4 ink{1,1,1,glass::text_secondary*pose.alpha};
    canvas.arrow(cursor+8.0F,middle-7.0F,12.0F,direction::up,ink);
    canvas.arrow(cursor+8.0F,middle+7.0F,12.0F,direction::down,ink);
    cursor+=arrows;
    canvas.text(reorder,cursor,line,22,ink);
    cursor+=measure(reorder)+gap;
    canvas.arrow(cursor+4.0F,middle,12.0F,direction::left,ink);
    canvas.arrow(cursor+20.0F,middle,12.0F,direction::right,ink);
    cursor+=arrows;
    canvas.text(change,cursor,line,22,ink);
}

void main_menu::finish_media_transition() {
    outgoing_media.reset();
    if(auto* closing=std::exchange(close_after_transition,nullptr)) {
        xmb->get_window()->waitIdle();closing->on_close();
    }
}
void main_menu::show_settings() {
    finish_media_transition();
    while(in_submenu){back();finish_media_transition();}
    select(1);
}
void main_menu::show_power() {
    finish_media_transition();
    while(in_submenu){back();finish_media_transition();}
    // Power is the first console column; keep the highlighted action on Sleep.
    select(0);
    if(!menus.empty()&&menus[0]->get_submenus_count()>0) menus[0]->select_submenu(0);
}
std::unique_ptr<main_menu::media_scene_state> main_menu::capture_media_scene(menu::menu& child,time_point now,bool animate_focus) {
    auto state=std::make_unique<media_scene_state>();
    auto& presentation=state->catalog;
    const auto count=child.get_submenus_count();
    const auto selected_item=count?std::min<std::size_t>(child.get_selected_submenu(),count-1):0;
    auto previous=count&&animate_focus?std::min<std::size_t>(last_selected_submenu_item,count-1):selected_item;
    if(std::abs(static_cast<long long>(previous)-static_cast<long long>(selected_item))>10)previous=selected_item;
    const auto first=std::min(selected_item,previous)>4?std::min(selected_item,previous)-4:0;
    const auto last=count?std::min<std::size_t>(count-1,std::max(selected_item,previous)+12):0;
    openxmb::xmb::CatalogNode root;root.id="category.settings.media";root.kind=openxmb::xmb::NodeKind::menu;
    if(count)for(std::size_t i=first;i<=last;++i) {
        auto& entry=child.get_submenu(static_cast<unsigned>(i));
        openxmb::xmb::CatalogNode node;node.id="category.settings.media."+std::to_string(i);
        node.label_key=node.id+".label";node.description_key=node.id+".description";node.icon_ref=node.id+".icon";
        node.kind=dynamic_cast<const menu::menu*>(&entry)?openxmb::xmb::NodeKind::menu:openxmb::xmb::NodeKind::media;
        presentation.strings[node.label_key]=entry.get_name();presentation.strings[node.description_key]=entry.get_description();
        root.children.push_back(node.id);const auto id=node.id;presentation.nodes.emplace(id,std::move(node));
        const auto& stored=presentation.nodes.at(id);state->icons.push_back({stored.icon_ref,&entry.get_icon(),entry.get_glass_icon(),entry.has_custom_icon()});
    }
    presentation.nodes.emplace("category.settings.media",std::move(root));
    const openxmb::xmb::NavigationRoute route{openxmb::xmb::NavigationLayer::nested_menu,"category.settings.media",selected_item-first};
    const auto input=openxmb::xmb::make_settings_scene_input(route,previous-first,seconds_from_time_point(last_selected_submenu_item_transition),openxmb::xmb::SettingsScenePresence::visible);
    if(const auto sampled=openxmb::xmb::sample_settings_scene(presentation,input,seconds_from_time_point(now));sampled)state->snapshot=sampled.snapshot;
    else state->empty=!count;
    const auto* parent=submenu_stack.empty()?menus[selected].get():submenu_stack.back();
    for(unsigned i=0;i<parent->get_submenus_count();++i) {
        const auto delta=static_cast<int>(i)-static_cast<int>(parent->get_selected_submenu());
        const auto y=openxmb::xmb::SettingsSceneMetrics::focus_y+delta*settings_parent_spacing;
        if(y<-40||y>1120)continue;
        const auto& entry=parent->get_submenu(i);const auto alpha=delta==0?0.92:std::max(0.12,0.42-(std::abs(delta)-1)*0.05);
        state->parents.push_back({&entry.get_icon(),entry.get_glass_icon(),delta==0?settings_parent_col_x:settings_sibling_col_x,y,alpha,entry.has_custom_icon()});
    }
    return state;
}
void main_menu::draw_media_scene(auroreengine::gui_renderer& renderer,const media_scene_state& state,double shift,double alpha,double parent_shift) {
    const auto layout=make_contained_layout(renderer);
    const auto focus_y_shift=(config::CONFIG.ui_scale()>1.0)?(((layout.framebuffer_height*0.50-layout.offset_y)/layout.scale)-515.0):0.0;
    for(const auto& entry:state.parents) {
        const auto py=entry.y+focus_y_shift;
        const auto sy=layout.y(py+openxmb::xmb::SettingsSceneMetrics::icon_extent*0.5)*layout.framebuffer_height;
        if(sy>layout.framebuffer_height*0.89||layout.y(py-openxmb::xmb::SettingsSceneMetrics::icon_extent*0.5)*layout.framebuffer_height<16.0) continue;
        draw_icon(renderer,*entry.icon,entry.glass,layout,entry.x+parent_shift,py,openxmb::xmb::SettingsSceneMetrics::icon_extent,entry.alpha*alpha,entry.custom);
    }
    auto snapshot=state.snapshot;
    for(std::size_t i=0;i<snapshot.row_count;++i) {
        auto& row=snapshot.rows[i];row.icon_center_x+=shift;row.label_x+=shift;row.description_x+=shift;
        row.alpha*=alpha;row.value_alpha*=alpha;row.description_alpha*=alpha;
    }
    settings_scene_renderer.render(renderer,snapshot,state.icons,{.glass_icons=config::CONFIG.iconGlassRefraction});
    if(state.empty)xmb_canvas(renderer).text("This folder is empty.",683+static_cast<float>(shift),515+static_cast<float>(focus_y_shift),34,{1,1,1,static_cast<float>(alpha)});
}
void main_menu::render_submenu(auroreengine::gui_renderer& renderer,time_point now) {
    const auto raw=config::CONFIG.enabled("power.reduced.motion",false)?1.0:progress(now,last_submenu_transition,transition_submenu_activate_duration);
    const auto layers=media_motion::layers(media_returning,raw);
    if(outgoing_media&&raw<1)draw_media_scene(renderer,*outgoing_media,layers.outgoing_x,layers.outgoing_alpha,layers.outgoing_x);
    else finish_media_transition();
    if(in_submenu&&current_submenu) {
        auto incoming=capture_media_scene(*current_submenu,now,true);
        draw_media_scene(renderer,*incoming,layers.incoming_x,layers.incoming_alpha,0);
        if(progress(now,last_selected_submenu_item_transition,transition_submenu_item_duration)>=1)last_selected_submenu_item=current_submenu->get_selected_submenu();
    }
    const auto layout=make_contained_layout(renderer);
    const auto alpha=static_cast<float>(in_submenu?1.0:layers.outgoing_alpha);
    const auto focus_y_shift=(config::CONFIG.ui_scale()>1.0)?(((layout.framebuffer_height*0.50-layout.offset_y)/layout.scale)-515.0):0.0;
    const std::array<auroreengine::simple_renderer::vertex_data,3> chevron{{
        {{layout.x(430),layout.y(515+focus_y_shift)},{0.81F,0.81F,0.82F,alpha},{0,0.5F}},
        {{layout.x(462),layout.y(500+focus_y_shift)},{0.81F,0.81F,0.82F,alpha},{1,0}},
        {{layout.x(462),layout.y(530+focus_y_shift)},{0.47F,0.47F,0.49F,alpha},{1,1}}}};
    renderer.draw_generic(chevron);
}

}
