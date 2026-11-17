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

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>

module openxmb.app;

import spdlog;
import i18n;
import auroreengine;
import openxmb.config;

import :applications_menu;
import :choice_overlay;
import :message_overlay;
import :menu_utils;

namespace menu {

using namespace mfk::i18n::literals;

namespace {

bool desktop_bool(std::string value)
{
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value == "true" || value == "1";
}

std::vector<std::string> split_desktop_exec(const std::string& command_line)
{
    std::vector<std::string> tokens;
    std::string token;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;

    auto flush = [&]() {
        if(!token.empty()) {
            tokens.push_back(std::move(token));
            token.clear();
        }
    };

    for(char c : command_line) {
        if(escaped) {
            token.push_back(c);
            escaped = false;
            continue;
        }

        if(c == '\\') {
            escaped = true;
            continue;
        }

        if(c == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            continue;
        }

        if(c == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            continue;
        }

        if(std::isspace(static_cast<unsigned char>(c)) && !in_single_quote && !in_double_quote) {
            flush();
            continue;
        }

        token.push_back(c);
    }

    if(escaped) {
        token.push_back('\\');
    }
    flush();
    return tokens;
}

std::optional<std::vector<std::string>> expand_desktop_exec_token(const std::string& token, const app_info& app)
{
    if(token == "%i") {
        if(app.icon.empty()) {
            return std::vector<std::string>{};
        }
        return std::vector<std::string>{"--icon", app.icon};
    }

    std::string expanded;
    expanded.reserve(token.size());
    for(std::size_t i = 0; i < token.size(); ++i) {
        if(token[i] != '%') {
            expanded.push_back(token[i]);
            continue;
        }

        if(i + 1 >= token.size()) {
            return std::nullopt;
        }

        char code = token[++i];
        switch(code) {
            case '%':
                expanded.push_back('%');
                break;
            case 'c':
                expanded += app.name;
                break;
            case 'k':
                expanded += app.desktop_file.string();
                break;
            case 'i':
                expanded += app.icon;
                break;
            case 'f':
            case 'F':
            case 'u':
            case 'U':
            case 'd':
            case 'D':
            case 'n':
            case 'N':
            case 'v':
            case 'm':
                return std::nullopt;
            default:
                return std::nullopt;
        }
    }

    if(expanded.empty()) {
        return std::vector<std::string>{};
    }
    return std::vector<std::string>{std::move(expanded)};
}

std::vector<std::string> build_launch_args(const app_info& app)
{
    std::vector<std::string> args;
    for(const auto& token : split_desktop_exec(app.exec)) {
        auto expanded = expand_desktop_exec_token(token, app);
        if(!expanded) {
            continue;
        }
        args.insert(args.end(), expanded->begin(), expanded->end());
    }

    if(app.terminal) {
        return terminal_command(std::move(args));
    }
    return args;
}

bool valid_game_manifest_id(std::string_view id)
{
    if(id.empty() || id == "." || id == "..") {
        return false;
    }
    if(!std::isalnum(static_cast<unsigned char>(id.front()))) {
        return false;
    }
    for(unsigned char ch : id) {
        if(!(std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-')) {
            return false;
        }
    }
    return true;
}

std::filesystem::path expand_manifest_path(const std::filesystem::path& base, std::string_view raw)
{
    if(raw.empty()) {
        return {};
    }
    std::string value(raw);
    if(value == "~" || value.starts_with("~/")) {
        if(const char* home = std::getenv("HOME"); home && *home) {
            value = std::string(home) + value.substr(1);
        }
    }
    std::filesystem::path candidate(value);
    if(!candidate.is_absolute()) {
        candidate = base / candidate;
    }
    return candidate;
}

std::string resolve_manifest_asset(
    const std::filesystem::path& base,
    std::string_view explicit_rel,
    std::initializer_list<const char*> defaults
) {
    std::error_code ec;
    if(!explicit_rel.empty()) {
        const auto candidate = expand_manifest_path(base, explicit_rel);
        if(std::filesystem::is_regular_file(candidate, ec)) {
            return candidate.string();
        }
    }
    for(const char* name : defaults) {
        const auto candidate = base / name;
        if(std::filesystem::is_regular_file(candidate, ec)) {
            return candidate.string();
        }
    }
    return {};
}

bool manifest_has_valid_command(const nlohmann::json& json)
{
    auto it = json.find("command");
    if(it == json.end()) {
        return false;
    }
    if(it->is_string()) {
        const auto s = it->get<std::string>();
        return std::ranges::any_of(s, [](unsigned char c) { return !std::isspace(c); });
    }
    if(it->is_array() && !it->empty() && (*it)[0].is_string()) {
        const auto s = (*it)[0].get<std::string>();
        return std::ranges::any_of(s, [](unsigned char c) { return !std::isspace(c); });
    }
    return false;
}

std::optional<app_info> parse_game_manifest(const std::filesystem::path& manifest_path)
{
    std::error_code ec;
    if(!std::filesystem::is_regular_file(manifest_path, ec)) {
        return std::nullopt;
    }
    std::ifstream in(manifest_path);
    if(!in) {
        return std::nullopt;
    }
    const auto json = nlohmann::json::parse(in, nullptr, false);
    if(!json.is_object() || !manifest_has_valid_command(json)) {
        return std::nullopt;
    }

    const auto game_dir = manifest_path.parent_path();
    const std::string game_id = json.value("id", game_dir.filename().string());
    if(!valid_game_manifest_id(game_id)) {
        return std::nullopt;
    }

    app_info app;
    app.id = game_id;
    app.name = json.value("title", app.id);
    if(app.name.empty()) {
        app.name = app.id;
    }
    app.runtime = json.value("runtime", std::string("native"));
    if(json.contains("description") && json["description"].is_string()) {
        app.comment = json["description"].get<std::string>();
    }
    app.exec = "nova-launch " + app.id;
    app.icon = resolve_manifest_asset(
        game_dir,
        json.value("icon", std::string{}),
        {"icon.png", "ICON0.PNG", "icon.jpg"}
    );
    app.background = resolve_manifest_asset(
        game_dir,
        json.value("background", std::string{}),
        {"background.png", "PIC1.PNG", "background.jpg"}
    );
    app.categories = "Game;";
    app.desktop_file = manifest_path;
    app.terminal = false;
    app.hidden = false;
    app.no_display = false;
    return app;
}

} // namespace

std::vector<app_info> scan_game_manifests()
{
    std::vector<app_info> manifests;
    std::unordered_set<std::string> seen_ids;
    std::vector<std::filesystem::path> roots;

    if(const char* env_dir = std::getenv("NOVA_GAMES_DIR"); env_dir && *env_dir) {
        roots.emplace_back(env_dir);
    }
    if(const char* home = std::getenv("HOME"); home && *home) {
        roots.push_back(std::filesystem::path(home) / "Games");
    }

    std::error_code ec;
    for(const auto& root : roots) {
        if(!std::filesystem::exists(root, ec)) {
            continue;
        }
        for(std::filesystem::directory_iterator it(
                root,
                std::filesystem::directory_options::skip_permission_denied,
                ec), end;
            it != end;
            it.increment(ec)) {
            if(ec || !it->is_directory(ec)) {
                continue;
            }
            const auto manifest_path = it->path() / "game.json";
            if(auto parsed = parse_game_manifest(manifest_path)) {
                if(seen_ids.insert(parsed->id).second) {
                    manifests.push_back(std::move(*parsed));
                }
            }
        }
    }

    std::ranges::sort(manifests, [](const app_info& a, const app_info& b) {
        return a.name < b.name;
    });
    return manifests;
}

// Parse desktop file to extract application info
app_info::app_info(const std::filesystem::path& desktop_file) {
    this->desktop_file = desktop_file;
    id = desktop_file.stem().string();
    name = id;
    comment = "";
    exec = "";
    icon = "";
    categories = "";
    terminal = false;
    hidden = false;
    no_display = false;
    
    if (!std::filesystem::exists(desktop_file)) {
        return;
    }
    
    try {
        std::ifstream file(desktop_file);
        std::string line;
        bool in_desktop_entry = false;
        bool unsupported_type = false;
        
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            
            if (line == "[Desktop Entry]") {
                in_desktop_entry = true;
                continue;
            }
            // Any other group ([Desktop Action ...]) ends the entry. Reading on
            // gave every Waydroid app the name and command of its "App Settings"
            // action.
            if (line.front() == '[') {
                in_desktop_entry = false;
                continue;
            }
            
            if (!in_desktop_entry) {
                continue;
            }
            
            size_t pos = line.find('=');
            if (pos == std::string::npos) {
                continue;
            }
            
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            
            if (key == "Type" && value != "Application") {
                unsupported_type = true;
            } else if (key == "Name") {
                name = value;
            } else if (key == "Comment") {
                comment = value;
            } else if (key == "Exec") {
                exec = value;
            } else if (key == "Icon") {
                icon = value;
            } else if (key == "Categories") {
                categories = value;
            } else if (key == "Terminal") {
                terminal = desktop_bool(value);
            } else if (key == "Hidden") {
                hidden = desktop_bool(value);
            } else if (key == "NoDisplay") {
                no_display = desktop_bool(value);
            }
        }
        hidden = hidden || unsupported_type;
    } catch (const std::exception& e) {
        spdlog::warn("Failed to parse desktop file {}: {}", desktop_file.string(), e.what());
    }
}

applications_menu::applications_menu(std::string name, auroreengine::texture&& icon, app::shell* xmb, auroreengine::resource_loader& loader, AppFilter filter)
    : simple_menu(std::move(name), std::move(icon)), xmb(xmb), loader(loader), filter(filter)
{
    auto scanned_apps = scan_applications();
    for (const auto& app : scanned_apps) {
        if(!filter(app))
            continue;
        bool is_hidden = app.no_display || config::CONFIG.excludedApplications.contains(app.id);
        if(!show_hidden && is_hidden)
            continue;

        spdlog::trace("Found application: {} ({})", app.name, app.id);
        auto entry = create_action_menu_entry(app, is_hidden);
        apps.push_back(app);
        entries.push_back(std::move(entry));
    }
}

std::unique_ptr<action_menu_entry> applications_menu::create_action_menu_entry(const app_info& app, bool hidden) {
    std::string icon_path;
    bool custom_icon = false;
    std::error_code ec;
    if(!app.icon.empty() && std::filesystem::is_regular_file(app.icon, ec)) {
        icon_path = app.icon;
        custom_icon = true;
    } else if(auto r = utils::resolve_icon_from_json(app.icon)) {
        icon_path = r->string();
        custom_icon = true;
    } else {
        const auto fallback_name = app.categories.find("Game") != std::string::npos
            ? "icon_category_game.png"
            : "icon_category_application.png";
        const auto fallback_path = config::CONFIG.asset_directory / "icons" / fallback_name;
        if(std::filesystem::is_regular_file(fallback_path, ec)) {
            icon_path = fallback_path.string();
        } else {
            spdlog::warn("Could not resolve icon for application: {}", app.name);
        }
    }

    auroreengine::texture icon_texture(loader.getDevice(), loader.getAllocator());
    std::string name = app.name;
    if(hidden) {
        name += " (hidden)"_();
    }
    auto entry = std::make_unique<action_menu_entry>(name, std::move(icon_texture),
        std::function<result()>{}, [this, app](action a) { return activate_app(app, a); },
        app.comment);
    entry->set_custom_icon(custom_icon);
    if(!icon_path.empty()) {
        loader.loadTexture(&entry->get_icon(), icon_path);
    }
    if(!app.background.empty() && std::filesystem::is_regular_file(app.background, ec)) {
        auroreengine::texture bg_texture(loader.getDevice(), loader.getAllocator());
        auto& bg_slot = entry->emplace_background(std::move(bg_texture));
        loader.loadTexture(&bg_slot, app.background);
    }
    return entry;
}

namespace {

bool is_game_manifest(const app_info& app)
{
    return app.exec.starts_with("nova-launch ");
}

std::string runtime_label(const app_info& app)
{
    if(!is_game_manifest(app)) return "Application";
    const auto& runtime = app.runtime;
    if(runtime == "proton") return "Windows (Proton)";
    if(runtime == "switch") return "Switch (nx2penguin)";
    if(runtime == "fex") return "x86 Linux (FEX)";
    if(runtime == "android" || runtime == "waydroid") return "Android";
    if(runtime == "emulation" || runtime == "retroarch") return "Emulator";
    return "Native";
}

// Desktop entries a console has no use for: terminal programs, entries meant
// for a desktop's own menus, and Waydroid's launchers (Android apps are
// started through their game.json, so Home, pause and quit work).
bool console_desktop_entry(const app_info& app)
{
    if(app.name.empty() || app.exec.empty() || app.hidden || app.no_display || app.terminal) {
        return false;
    }
    if(app.id.starts_with("waydroid.") || app.id == "Waydroid" ||
       app.id == "org.freedesktop.Xwayland") {
        return false;
    }
    return true;
}

bool start_library_item(const app_info& app, app::shell* xmb)
{
    auto args = build_launch_args(app);
    if(args.empty() || !launch_detached(args)) {
        spdlog::warn("Failed to start {} ({})", app.name, app.exec);
        return false;
    }
    if(xmb != nullptr && is_game_manifest(app)) {
        xmb->begin_launch(app.name, app.icon);
    }
    return true;
}

} // namespace

std::unique_ptr<action_menu_entry> create_game_menu_entry(
    const app_info& app,
    app::shell* xmb,
    auroreengine::resource_loader& loader
) {
    std::string icon_path;
    bool custom_icon = false;
    std::error_code ec;
    if(!app.icon.empty() && std::filesystem::is_regular_file(app.icon, ec)) {
        icon_path = app.icon;
        custom_icon = true;
    } else if(auto resolved = is_game_manifest(app)
            ? std::optional<std::filesystem::path>{}
            : utils::resolve_icon_from_json(app.icon)) {
        icon_path = resolved->string();
        custom_icon = true;
    } else {
        const auto fallback_path = config::CONFIG.asset_directory / "icons" /
            (is_game_manifest(app) ? "icon_category_game.png" : "icon_category_application.png");
        if(std::filesystem::is_regular_file(fallback_path, ec)) {
            icon_path = fallback_path.string();
        }
    }

    auroreengine::texture icon_texture(loader.getDevice(), loader.getAllocator());
    auto entry = std::make_unique<action_menu_entry>(
        app.name,
        std::move(icon_texture),
        std::function<result()>{},
        [xmb, app](action a) -> result {
            if(a == action::ok) {
                return start_library_item(app, xmb) ? result::success : result::failure;
            }
            if(a == action::options && xmb != nullptr) {
                xmb->emplace_overlay<app::choice_overlay>(
                    std::vector{"Start"_(), "Move"_(), "Information"_()},
                    0,
                    [xmb, app](unsigned int index) {
                        if(index == 0) {
                            start_library_item(app, xmb);
                        } else if(index == 1) {
                            xmb->begin_library_move();
                        } else if(index == 2) {
                            xmb->emplace_overlay<app::message_overlay>(
                                app.name,
                                std::string("Runs as: ") + runtime_label(app) + "\n" +
                                std::string("Name in the library: ") + app.id + "\n" +
                                std::string("Files: ") + app.desktop_file.parent_path().string()
                            );
                        }
                    }
                );
                return result::success;
            }
            return result::unsupported;
        },
        app.comment
    );
    entry->set_custom_icon(custom_icon);
    if(!icon_path.empty()) {
        loader.loadTexture(&entry->get_icon(), icon_path);
    }
    if(!app.background.empty() && std::filesystem::is_regular_file(app.background, ec)) {
        auroreengine::texture bg_texture(loader.getDevice(), loader.getAllocator());
        auto& bg_slot = entry->emplace_background(std::move(bg_texture));
        loader.loadTexture(&bg_slot, app.background);
    }
    return entry;
}

std::vector<app_info> scan_library()
{
    std::vector<app_info> items;
    std::unordered_set<std::string> seen_ids;
    for(auto& game : scan_game_manifests()) {
        if(!config::CONFIG.excludedApplications.contains(game.id) && seen_ids.insert(game.id).second) {
            items.push_back(std::move(game));
        }
    }

    const char* home = std::getenv("HOME");
    const std::vector<std::filesystem::path> search_paths = {
        std::filesystem::path(home ? home : "") / ".local/share/applications",
        "/run/current-system/sw/share/applications",
        "/usr/share/applications",
        "/usr/local/share/applications",
    };
    std::error_code ec;
    for(const auto& search_path : search_paths) {
        for(std::filesystem::directory_iterator it(search_path,
                std::filesystem::directory_options::skip_permission_denied, ec), end;
            !ec && it != end; it.increment(ec)) {
            if(it->path().extension() != ".desktop") {
                continue;
            }
            app_info app(it->path());
            if(console_desktop_entry(app) &&
               !config::CONFIG.excludedApplications.contains(app.id) &&
               seen_ids.insert(app.id).second) {
                items.push_back(std::move(app));
            }
        }
        ec.clear();
    }
    return items;
}

std::string library_column_of(const app_info& app)
{
    if(const auto it = config::CONFIG.libraryPlacement.find(app.id);
       it != config::CONFIG.libraryPlacement.end()) {
        for(const auto& column : library_columns) {
            if(column.id == it->second) return it->second;
        }
    }
    if(!is_game_manifest(app)) return "applications";
    if(app.runtime == "switch") return "switch";
    if(app.runtime == "proton") return "windows";
    if(app.runtime == "android" || app.runtime == "waydroid") return "android";
    return "linux";
}

library_menu::library_menu(std::string name, auroreengine::texture&& icon, std::string column,
    app::shell* xmb, auroreengine::resource_loader& loader, std::vector<app_info> library_items)
    : simple_menu(std::move(name), std::move(icon)), column_id(std::move(column)), items(std::move(library_items))
{
    // The player's order first; anything new goes after it, by title.
    const auto order_it = config::CONFIG.libraryOrder.find(column_id);
    const auto rank = [&](const app_info& app) {
        if(order_it != config::CONFIG.libraryOrder.end()) {
            const auto& order = order_it->second;
            if(const auto found = std::ranges::find(order, app.id); found != order.end()) {
                return static_cast<std::size_t>(std::distance(order.begin(), found));
            }
        }
        return static_cast<std::size_t>(-1);
    };
    std::ranges::stable_sort(items, [&](const app_info& a, const app_info& b) {
        const auto ra = rank(a), rb = rank(b);
        return ra != rb ? ra < rb : a.name < b.name;
    });
    for(const auto& app : items) {
        entries.push_back(create_game_menu_entry(app, xmb, loader));
    }
}

std::string library_menu::selected_id() const
{
    return selected_submenu < items.size() ? items[selected_submenu].id : std::string{};
}

std::string library_menu::selected_title() const
{
    return selected_submenu < items.size() ? items[selected_submenu].name : std::string{};
}

int library_menu::index_of(std::string_view id) const
{
    for(std::size_t i = 0; i < items.size(); ++i) {
        if(items[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

bool library_menu::move_selected(int delta)
{
    const auto from = static_cast<int>(selected_submenu);
    const auto to = from + delta;
    if(delta == 0 || to < 0 || to >= static_cast<int>(items.size())) {
        return false;
    }
    std::swap(items[from], items[to]);
    std::swap(entries[from], entries[to]);
    selected_submenu = static_cast<unsigned int>(to);
    return true;
}

void library_menu::store_order() const
{
    auto& order = config::CONFIG.libraryOrder[column_id];
    order.clear();
    for(const auto& app : items) order.push_back(app.id);
}

std::vector<app_info> applications_menu::scan_applications() {
    std::vector<app_info> app_list;
    std::unordered_set<std::string> seen_ids;

    for(auto& game_app : scan_game_manifests()) {
        if(seen_ids.insert(game_app.id).second) {
            app_list.push_back(std::move(game_app));
        }
    }
    
    // Common desktop file locations
    std::vector<std::filesystem::path> search_paths = {
        std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "") / ".local/share/applications",
        "/usr/share/applications",
        "/usr/local/share/applications",
        "/opt/applications"
    };
    
    for (const auto& search_path : search_paths) {
        if (!std::filesystem::exists(search_path)) {
            continue;
        }
        
        try {
            for (const auto& entry : std::filesystem::directory_iterator(search_path)) {
                if (entry.path().extension() == ".desktop") {
                    app_info app(entry.path());
                    if (!app.name.empty() && !app.exec.empty() && !app.hidden && !seen_ids.contains(app.id)) {
                        seen_ids.insert(app.id);
                        app_list.push_back(app);
                    }
                }
            }
        } catch (const std::exception& e) {
            spdlog::debug("Error scanning directory {}: {}", search_path.string(), e.what());
        }
    }

    std::ranges::sort(app_list, [](const app_info& a, const app_info& b) {
        return a.name < b.name;
    });
    
    return app_list;
}

void applications_menu::reload() {
    std::string selected_app_id;
    if(selected_submenu < apps.size()) {
        selected_app_id = apps[selected_submenu].id;
    }

    auto scanned_apps = scan_applications();
    apps.clear();
    entries.clear();
    
    for (const auto& app : scanned_apps) {
        if(!filter(app))
            continue;
        bool is_hidden = app.no_display || config::CONFIG.excludedApplications.contains(app.id);
        if(!show_hidden && is_hidden)
            continue;

        spdlog::trace("Found application: {} ({})", app.name, app.id);
        auto entry = create_action_menu_entry(app, is_hidden);
        apps.push_back(app);
        entries.push_back(std::move(entry));
    }

    selected_submenu = 0;
    if(!selected_app_id.empty()) {
        if(auto it = std::ranges::find_if(apps, [&selected_app_id](const app_info& app) {
            return app.id == selected_app_id;
        }); it != apps.end()) {
            selected_submenu = static_cast<unsigned int>(std::distance(apps.begin(), it));
        }
    }
}

result applications_menu::activate_app(const app_info& app, action action) {
    if(action == action::ok) {
        auto args = build_launch_args(app);
        if(args.empty()) {
            spdlog::warn("Could not build launch command for application: {}", app.name);
            return result::failure;
        }

        if(!launch_detached(args)) {
            spdlog::warn("Failed to launch application: {} ({})", app.name, app.exec);
            return result::failure;
        }

        return result::success;
    } else if(action == action::options) {
        bool hidden = config::CONFIG.excludedApplications.contains(app.id);
        xmb->emplace_overlay<app::choice_overlay>(std::vector{
            "Launch Application"_(), "View information"_(), hidden ? "Show in XMB"_() : "Hide from XMB"_()
        }, 0, [this, app, hidden](unsigned int index){
            switch(index) {
                case 0:
                    return activate_app(app, action::ok);
                case 1: {
                    auto args = build_launch_args(app);
                    std::string command_line;
                    for(const auto& arg : args) {
                        if(!command_line.empty()) {
                            command_line += " ";
                        }
                        command_line += arg;
                    }
                    xmb->emplace_overlay<app::message_overlay>(
                        "Application Information"_(),
                        std::string("Name: ") + app.name + "\n" +
                        std::string("ID: ") + app.id + "\n" +
                        std::string("Exec: ") + app.exec + "\n" +
                        std::string("Command: ") + command_line + "\n" +
                        std::string("Categories: ") + app.categories + "\n" +
                        std::string("Terminal: ") + (app.terminal ? "Yes" : "No") + "\n" +
                        std::string("Desktop file: ") + app.desktop_file.string()
                    );
                    return result::close;
                }
                case 2:
                    if(hidden) {
                        config::CONFIG.excludeApplication(app.id, false);
                        config::CONFIG.save_config();
                        reload();
                    } else {
                        xmb->emplace_overlay<app::message_overlay>(
                            "Hide Application"_(),
                            "Are you sure you want to hide this application from OpenXMB?"_(),
                            std::vector<std::string>{"Yes"_(), "No"_()},
                            [this, app](unsigned int choice) {
                                if(choice == 0) {
                                    config::CONFIG.excludeApplication(app.id);
                                    config::CONFIG.save_config();
                                    reload();
                                }
                            },
                            true
                        );
                    }
                    return result::close;
                default:
                    return result::unsupported;
            }
        });
        return result::success;
    }
    return result::unsupported;
}

result applications_menu::activate(action action) {
    if(action == action::extra) {
        show_hidden = !show_hidden;
        reload();
        return result::success;
    }
    return simple_menu::activate(action);
}

void applications_menu::get_button_actions(std::vector<std::pair<action, std::string>>& v) {
    if(!v.empty()) {
        return;
    }
    v.emplace_back(action::none, "");
    v.emplace_back(action::none, "");
    v.emplace_back(action::options, "Options"_());
    v.emplace_back(action::extra, show_hidden ? "Hide excluded apps"_() : "Show excluded apps"_());
}

}
