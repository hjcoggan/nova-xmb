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
#include "openxmb/xmb/pause_backdrop.hpp"
#include "openxmb/xmb/ui_tokens.hpp"
#include "openxmb/xmb/child_process.hpp"
#include "openxmb/xmb/ipc_message.hpp"
#include <future>

#include <array>
#include <algorithm>
#include <charconv>
#include <fast_float/fast_float.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <ranges>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#include <version>

// gettext
#include <libintl.h>
#include "../localization/shell_locale.hpp"
#include <ctime>
#include <SDL2/SDL.h>
#undef main
#ifdef _WIN32
#include <windows.h>
#endif

#include <glm/vec2.hpp>

#include "openxmb/xmb/boot_timeline.hpp"
#include "openxmb/xmb/background.hpp"
#include "input_bindings.hpp"
#include "components/media_motion.hpp"
#include "../localization/font_selection.hpp"
#include "openxmb/desktop/library.hpp"
#include "openxmb/themes/library.hpp"
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

module openxmb.app;

import i18n;
import spdlog;
import auroreengine;
import glm;
import vulkan_hpp;
import vma;
import sdl2;
import openxmb.config;
import openxmb.constants;
import openxmb.render;
import openxmb.debug;
import openxmb.utils;
import openxmb.xmb.captured_wave_renderer;
import openxmb.xmb.monthly_background_renderer;
import :startup_overlay;
import :choice_overlay;
import :message_overlay;
import :pause_overlay;
import :theme_manager;
import :menu_utils;
import :library_manager;
import :desktop_settings;

using namespace mfk::i18n::literals;

namespace app
{
    struct shell::theme_state {
        std::shared_ptr<openxmb::themes::ThemeLibrary> library;
        std::optional<openxmb::themes::AppliedTheme> active;
        std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
        openxmb::themes::AnimationFrame frame;
        double animation_seconds=0;
        std::chrono::steady_clock::time_point sampled=std::chrono::steady_clock::now();
        std::map<std::filesystem::path,std::unique_ptr<texture>> textures;
    };

    namespace {
        inline constexpr float xmb_web_native_steady_wave_fill_alpha = 0.078F;

        bool truthy_environment_flag(const char* name) noexcept
        {
            const char* value = std::getenv(name);
            if(value == nullptr || value[0] == '\0') {
                return false;
            }
            const std::string_view text(value);
            return text != "0" && text != "false" && text != "FALSE" &&
                   text != "off" && text != "OFF" && text != "no" &&
                   text != "NO";
        }

        bool force_headless_startup_overlay() noexcept
        {
            return truthy_environment_flag("OPENXMB_HEADLESS_STARTUP");
        }

        events::logical_controller_button to_logical_button(sdl::GameControllerButton button)
        {
            return static_cast<events::logical_controller_button>(std::to_underlying(button));
        }

        events::logical_mouse_button to_logical_mouse_button(int button)
        {
            switch(button) {
                case SDL_BUTTON_LEFT:
                    return events::logical_mouse_button::left;
                case SDL_BUTTON_MIDDLE:
                    return events::logical_mouse_button::middle;
                case SDL_BUTTON_RIGHT:
                    return events::logical_mouse_button::right;
                case SDL_BUTTON_X1:
                    return events::logical_mouse_button::x1;
                case SDL_BUTTON_X2:
                    return events::logical_mouse_button::x2;
                default:
                    return events::logical_mouse_button::left;
            }
        }

        bool is_retiring_pause_overlay(const std::unique_ptr<component>& old_overlay)
        {
            return old_overlay &&
                   (dynamic_cast<app::pause_overlay*>(old_overlay.get()) != nullptr) &&
                   menu::has_active_child();
        }

        std::filesystem::path existing_asset_or_fallback(std::filesystem::path path, std::filesystem::path fallback)
        {
            std::error_code ec;
            if(std::filesystem::exists(path, ec) && !ec) {
                return path;
            }
            ec.clear();
            if(std::filesystem::exists(fallback, ec) && !ec) {
                return fallback;
            }
            return path;
        }

        [[nodiscard]] std::optional<double> fixed_seconds_from_environment(
            const char* name)
        {
            const char* value = std::getenv(name);
            if(value == nullptr || *value == '\0') {
                return std::nullopt;
            }

            const std::string_view text{value};
            double seconds{};
            const auto [end, error] = fast_float::from_chars(
                text.data(), text.data() + text.size(), seconds,
                fast_float::chars_format::general);
            if(error != std::errc{} || end != text.data() + text.size() ||
               !std::isfinite(seconds) || seconds < 0.0) {
                spdlog::warn(
                    "Ignoring invalid {} value '{}'; expected a finite non-negative number",
                    name, text);
                return std::nullopt;
            }
            return seconds;
        }

        [[nodiscard]] std::optional<double> fixed_wave_seconds_from_environment()
        {
            return fixed_seconds_from_environment("OPENXMB_FIXED_WAVE_SECONDS");
        }

        [[nodiscard]] std::optional<double> fixed_boot_seconds_from_environment()
        {
            return fixed_seconds_from_environment("OPENXMB_FIXED_BOOT_SECONDS");
        }
    }

    struct BlurConstants {
        int axis = 0;
        int size = 20;
    };

    void component::render_controller_buttons(app::shell* xmb, auroreengine::gui_renderer& renderer, float x, float y, std::span<const std::pair<::action, std::string_view>> buttons) const
    {
        const auto saved_scale = renderer.text_scale;
        renderer.text_scale /= std::max(1.0F, typography_scale());
        xmb->render_controller_buttons(renderer, x, y, buttons);
        renderer.text_scale = saved_scale;
    }

    namespace {
        volatile std::sig_atomic_t nova_pause_sigusr1_pending = 0;

        void nova_handle_sigusr1(int) {
            nova_pause_sigusr1_pending = 1;
        }
    }

    shell::shell(window* window) : phase(window),theme(std::make_unique<theme_state>())
    {
        struct sigaction sa{};
        sa.sa_handler = nova_handle_sigusr1;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGUSR1, &sa, nullptr);

        const char* env_sock = std::getenv("NOVA_PAUSE_SOCKET");
        pause_sock_path = (env_sock && *env_sock)
            ? std::string(env_sock)
            : ("/run/user/" + std::to_string(static_cast<unsigned long>(getuid())) + "/nova-pause.sock");
        if (pause_sock_path.size() < sizeof(sockaddr_un::sun_path)) {
            int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (fd >= 0) {
                unlink(pause_sock_path.c_str());
                sockaddr_un addr{};
                addr.sun_family = AF_UNIX;
                std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", pause_sock_path.c_str());
                if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
                    listen(fd, 8) == 0) {
                    pause_sock_fd = fd;
                    if (win) {
                        win->config.wakeFd = pause_sock_fd;
                    }
                } else {
                    close(fd);
                    pause_sock_path.clear();
                }
            }
        }
    }

    shell::~shell()
    {
        drop_pause_still();
        if (pause_sock_fd >= 0) {
            if (win && win->config.wakeFd == pause_sock_fd) {
                win->config.wakeFd = -1;
            }
            close(pause_sock_fd);
            pause_sock_fd = -1;
        }
        if (!pause_sock_path.empty()) {
            unlink(pause_sock_path.c_str());
            pause_sock_path.clear();
        }
    }

    void shell::begin_launch(const std::string& title, const std::string& artwork)
    {
        if(has_launch_overlay()) {
            return;
        }
        launch_menu_had_focus = false;
        launch_started = std::chrono::steady_clock::now();
        emplace_overlay<app::launch_overlay>(title, artwork, *loader);
    }

    void shell::end_launch()
    {
        for(unsigned int i = 0; i < overlays.size(); ++i) {
            if(dynamic_cast<app::launch_overlay*>(overlays[i].get()) != nullptr) {
                remove_overlay(i);
                return;
            }
        }
    }

    // Capture the frozen game for the pause panel's glass. Until the panel
    // opens, the menu's surface is transparent and empty, so the capture
    // holds only the game.
    void shell::start_pause_capture()
    {
        drop_pause_still();
        pause_backdrop.start();
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        const std::string path = std::string(runtime && *runtime ? runtime : "/tmp") + "/nova-pause-still.png";
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        if (!pause_capture.spawn({"grim", "-t", "png", "-l", "0", path})) {
            spdlog::warn("Pause: could not run grim; the panel uses its fallback glass");
            pause_backdrop.finish(false);
        }
    }

    void shell::poll_pause_capture()
    {
        openxmb::xmb::child_process::reap_orphans();
        if (pause_capture.running()) {
            if (const auto exited_ok = pause_capture.poll()) {
                if (*exited_ok && pause_backdrop.state == openxmb::xmb::pause_backdrop_state::capturing) {
                    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
                    const std::string path = std::string(runtime && *runtime ? runtime : "/tmp") + "/nova-pause-still.png";
                    pause_still = std::make_unique<texture>(device, allocator);
                    pause_still_loading = loader->loadTexture(pause_still.get(), path);
                    pause_still_loading_started = true;
                } else {
                    pause_backdrop.finish(false);
                }
            }
        }
        if (pause_still_loading_started && pause_still_loading.valid() &&
            pause_still_loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            pause_still_loading_started = false;
            try {
                pause_still_loading.get();
                pause_backdrop.finish(pause_still && pause_still->loaded);
            } catch (const std::exception& error) {
                spdlog::warn("Pause: the captured still did not load: {}", error.what());
                pause_backdrop.finish(false);
            }
        }
    }

    void shell::drop_pause_still()
    {
        pause_backdrop.cancel();
        // Never wait for the capture here: it is let go and reaped later.
        pause_capture.terminate();
        if (pause_still_loading.valid()) pause_still_loading.wait();
        pause_still_loading_started = false;
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        const std::string path = std::string(runtime && *runtime ? runtime : "/tmp") + "/nova-pause-still.png";
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        if (pause_still) {
            // The frame that last sampled it may still be in flight.
            device.waitIdle();
            pause_still.reset();
        }
    }

    void shell::poll_pause_ipc()
    {
        auto open_pause = [this](const std::string& game_id) {
            end_launch();
            for (unsigned int i = 0; i < overlays.size(); ++i) {
                if (dynamic_cast<app::pause_overlay*>(overlays[i].get()) != nullptr) {
                    return;
                }
            }
            if (pending_pause_game) return;
            pending_pause_game = game_id;
            pause_requested_at = std::chrono::steady_clock::now();
            start_pause_capture();
        };
        auto close_pause = [this]() {
            if (pending_pause_game) {
                // RESUME or QUIT before the panel opened: nothing to show.
                pending_pause_game.reset();
                drop_pause_still();
            }
            for (unsigned int i = 0; i < overlays.size(); ++i) {
                if (dynamic_cast<app::pause_overlay*>(overlays[i].get()) != nullptr) {
                    remove_overlay(i);
                    break;
                }
            }
        };

        if (pause_sock_fd >= 0) {
            for (;;) {
                int client = accept4(pause_sock_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (client < 0) {
                    break;
                }
                // A connect wakes the menu before the peer has written:
                // wait briefly for the line rather than dropping it.
                const auto line = openxmb::xmb::read_ipc_message(client, 200);
                close(client);
                if (!line) {
                    continue;
                }
                const std::string& msg = *line;
                if (msg == "PAUSE") {
                    open_pause("");
                } else if (msg.rfind("PAUSE ", 0) == 0) {
                    open_pause(msg.substr(6));
                } else if (msg == "QUIT") {
                    // Keep the panel up, saying so, until the game is gone.
                    bool marked = false;
                    for (auto& overlay : overlays) {
                        if (auto* pause = dynamic_cast<app::pause_overlay*>(overlay.get())) {
                            pause->begin_closing();
                            marked = true;
                        }
                    }
                    if (!marked) close_pause();
                } else if (msg == "RESUME") {
                    close_pause();
                } else if (msg == "POWER_MENU") {
                    // One power UI: the Power column. The socket verb just
                    // focuses it and highlights Sleep; nothing is actioned
                    // until A is pressed on the device.
                    menu.show_power();
                }
            }
        }

        poll_pause_capture();
        if (pending_pause_game) {
            const auto waited = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - pause_requested_at).count();
            if (pause_backdrop.panel_may_show(waited)) {
                const auto game_id = *pending_pause_game;
                pending_pause_game.reset();
                emplace_overlay<app::pause_overlay>(game_id);
            }
        }

        // A panel that is closing its game goes when the game's process has.
        for (unsigned int i = 0; i < overlays.size(); ++i) {
            const auto* pause = dynamic_cast<app::pause_overlay*>(overlays[i].get());
            if (pause != nullptr && pause->closing() &&
                (!menu::has_active_child() || pause->closing_for() > std::chrono::seconds(12))) {
                remove_overlay(i);
                break;
            }
        }

        if (nova_pause_sigusr1_pending) {
            nova_pause_sigusr1_pending = 0;
            if (has_pause_overlay()) {
                close_pause();
            } else {
                open_pause("");
            }
        }
    }

    void shell::preload()
    {
        phase::preload();
        desktop_settings::restore_saved_brightness();

        font_render = std::make_unique<font_renderer>(openxmb::localization::select_font(config::CONFIG.fontPath,config::CONFIG.asset_directory,config::CONFIG.language).string(), 32, device, allocator, win->swapchainExtent, win->gpuFeatures);
        image_render = std::make_unique<image_renderer>(device, win->swapchainExtent, win->gpuFeatures);
        image_render->setAllocator(allocator);
        simple_render = std::make_unique<simple_renderer>(device, allocator, win->swapchainExtent, win->gpuFeatures);
        wave_render = std::make_unique<render::wave_renderer>(device, allocator, win->swapchainExtent);
        procedural_wave_render = std::make_unique<render::procedural_wave_renderer>(device, allocator, win->swapchainExtent);
        monthly_background_render =
            std::make_unique<openxmb::xmb::MonthlyBackgroundRenderer>(device, win->swapchainExtent);

        create_render_passes();
        renderPassSamples = win->config.sampleCount;
        {
            std::array<vk::DescriptorSetLayoutBinding, 2> bindings = {
                vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute),
                vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute),
            };
            vk::DescriptorSetLayoutCreateInfo info({}, bindings);
            blurDescriptorSetLayout = device.createDescriptorSetLayoutUnique(info);
        }
        {
            vk::PushConstantRange range{vk::ShaderStageFlagBits::eCompute, 0, sizeof(BlurConstants)};
            vk::PipelineLayoutCreateInfo info({}, blurDescriptorSetLayout.get(), range);
            blurPipelineLayout = device.createPipelineLayoutUnique(info);
        }
        {
            vk::UniqueShaderModule compShader = render::shaders::blur::comp(device);
            vk::PipelineShaderStageCreateInfo shader({}, vk::ShaderStageFlagBits::eCompute, compShader.get(), "main");
            vk::ComputePipelineCreateInfo info({}, shader, blurPipelineLayout.get());
            blurPipeline = device.createComputePipelineUnique({}, info).value;
            debugName(device, blurPipeline.get(), "Blur Pipeline");
        }
        {
            vk::UniqueShaderModule compShader = render::shaders::downsample::comp(device);
            vk::PipelineShaderStageCreateInfo shader({}, vk::ShaderStageFlagBits::eCompute, compShader.get(), "main");
            vk::ComputePipelineCreateInfo info({}, shader, blurPipelineLayout.get());
            downsamplePipeline = device.createComputePipelineUnique({}, info).value;
            debugName(device, downsamplePipeline.get(), "Downsample Pipeline");
        }
        {
            vk::UniqueShaderModule compShader = render::shaders::upsample::comp(device);
            vk::PipelineShaderStageCreateInfo shader({}, vk::ShaderStageFlagBits::eCompute, compShader.get(), "main");
            vk::ComputePipelineCreateInfo info({}, shader, blurPipelineLayout.get());
            upsamplePipeline = device.createComputePipelineUnique({}, info).value;
            debugName(device, upsamplePipeline.get(), "Upsample Pipeline");
        }

        font_render->set_preload_texts(openxmb::localization::font_catalog_texts(config::CONFIG.locale_directory,config::CONFIG.language));
        add_task(font_render->preload(loader, {shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get(), nullptr, 0x20, 0x1ff));
        image_render->preload({backgroundRenderPass.get(), shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
        simple_render->preload({shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
        wave_render->preload({backgroundRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
        try {
            monthly_background_render->preload(
                {backgroundRenderPass.get()}, win->config.sampleCount,
                win->pipelineCache.get());
        } catch(const std::exception& error) {
            monthly_background_failed = true;
            spdlog::error(
                "Monthly-background Vulkan pipeline initialization failed ({}); using the safe clear-colour fallback",
                error.what());
        }
        procedural_wave_render->preload({backgroundRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());

        if(config::CONFIG.backgroundType == config::config::background_type::image) {
            backgroundTexture = std::make_unique<texture>(device, allocator);
            loader->loadTexture(backgroundTexture.get(), config::CONFIG.backgroundImage);
        }
        iconGlassAmbientTexture = std::make_unique<texture>(device, allocator);
        iconGlassEnvironmentTexture = std::make_unique<texture>(device, allocator);
        const auto glass_environment_directory =
            config::CONFIG.asset_directory / "materials";
        loader->loadTexture(iconGlassAmbientTexture.get(),
                            glass_environment_directory / "icon_amb.png");
        loader->loadTexture(iconGlassEnvironmentTexture.get(),
                            glass_environment_directory / "texenv.png");
        config::CONFIG.addCallback("background-type", [this](const std::string&){
            if(config::CONFIG.backgroundType == config::config::background_type::image || config::CONFIG.backgroundType == config::config::background_type::video) {
                reload_background();
            } else {
                backgroundTexture.reset();
            }
        });
        config::CONFIG.addCallback("background-image", [this](const std::string&){
            if(config::CONFIG.backgroundType == config::config::background_type::image || config::CONFIG.backgroundType == config::config::background_type::video) {
                reload_background();
            } else {
                backgroundTexture.reset();
            }
        });
        config::CONFIG.addCallback("controller-type", [this](const std::string&){
            reload_button_icons();
        });
        config::CONFIG.addCallback("language", [this](const std::string&){
            reload_language();
        });
        config::CONFIG.addCallback("vsync", [this](const std::string&){
            spdlog::info("VSync changed to {}", config::CONFIG.preferredPresentMode == vk::PresentModeKHR::eFifoRelaxed ? "on" : "off");
            win->config.preferredPresentMode = config::CONFIG.preferredPresentMode;
            win->recreateSwapchain();
        });

        reload_background();
        if(!background_only) {
            if(auto saved = theme_library()->active()) apply_theme(*saved);
            preload_fixed_components();
        }
    }

    void shell::create_render_passes()
    {
        {
            const auto background_format=win->swapchainFormat.format==vk::Format::eB8G8R8A8Srgb?vk::Format::eB8G8R8A8Unorm:
                win->swapchainFormat.format==vk::Format::eR8G8B8A8Srgb?vk::Format::eR8G8B8A8Unorm:win->swapchainFormat.format;
            std::array<vk::SubpassDependency, 2> deps{
                vk::SubpassDependency(vk::SubpassExternal, 0,
                    vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eLateFragmentTests,
                    vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests,
                    vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                    vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite),
                vk::SubpassDependency(0, vk::SubpassExternal,
                    vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eFragmentShader,
                    vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead)
            };
            const auto depth_attachment = vk::AttachmentDescription({}, vk::Format::eD32Sfloat,
                win->config.sampleCount, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare,
                vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
                vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthStencilAttachmentOptimal);
            if(win->config.sampleCount == vk::SampleCountFlagBits::e1) {
                const std::array attachments{
                    vk::AttachmentDescription({}, background_format,
                        vk::SampleCountFlagBits::e1,
                        vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore,
                        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
                        vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal),
                    depth_attachment
                };
                const vk::AttachmentReference color_ref(0, vk::ImageLayout::eColorAttachmentOptimal);
                const vk::AttachmentReference depth_ref(1, vk::ImageLayout::eDepthStencilAttachmentOptimal);
                const vk::SubpassDescription subpass({}, vk::PipelineBindPoint::eGraphics,
                    {}, color_ref, {}, &depth_ref);
                backgroundRenderPass = device.createRenderPassUnique(
                    vk::RenderPassCreateInfo({}, attachments, subpass, deps));
            } else {
                const std::array attachments{
                    vk::AttachmentDescription({}, background_format, win->config.sampleCount,
                        vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare,
                        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
                        vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal),
                    vk::AttachmentDescription({}, background_format, vk::SampleCountFlagBits::e1,
                        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore,
                        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
                        vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal),
                    depth_attachment
                };
                const vk::AttachmentReference color_ref(0, vk::ImageLayout::eColorAttachmentOptimal);
                const vk::AttachmentReference resolve_ref(1, vk::ImageLayout::eColorAttachmentOptimal);
                const vk::AttachmentReference depth_ref(2, vk::ImageLayout::eDepthStencilAttachmentOptimal);
                const vk::SubpassDescription subpass({}, vk::PipelineBindPoint::eGraphics,
                    {}, color_ref, resolve_ref, &depth_ref);
                backgroundRenderPass = device.createRenderPassUnique(
                    vk::RenderPassCreateInfo({}, attachments, subpass, deps));
            }
            debugName(device, backgroundRenderPass.get(), "Background Render Pass");
        }
        {
            vk::SubpassDependency dep(vk::SubpassExternal, 0,
                vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eColorAttachmentOutput,
                vk::AccessFlagBits::eTransferWrite | vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eColorAttachmentWrite);
            if(win->config.sampleCount == vk::SampleCountFlagBits::e1) {
                const std::array attachments{
                    vk::AttachmentDescription({}, win->swapchainFormat.format,
                        vk::SampleCountFlagBits::e1,
                        vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore,
                        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
                        vk::ImageLayout::eUndefined, win->swapchainFinalLayout)
                };
                const vk::AttachmentReference color_ref(
                    0, vk::ImageLayout::eColorAttachmentOptimal);
                const vk::SubpassDescription subpass(
                    {}, vk::PipelineBindPoint::eGraphics, {}, color_ref);
                shellRenderPass = device.createRenderPassUnique(
                    vk::RenderPassCreateInfo({}, attachments, subpass, dep));
            } else {
                const std::array attachments{
                    vk::AttachmentDescription({}, win->swapchainFormat.format, win->config.sampleCount,
                        vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare,
                        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
                        vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal),
                    vk::AttachmentDescription({}, win->swapchainFormat.format, vk::SampleCountFlagBits::e1,
                        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore,
                        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
                        vk::ImageLayout::eUndefined, win->swapchainFinalLayout)
                };
                const vk::AttachmentReference color_ref(
                    0, vk::ImageLayout::eColorAttachmentOptimal);
                const vk::AttachmentReference resolve_ref(
                    1, vk::ImageLayout::eColorAttachmentOptimal);
                const vk::SubpassDescription subpass(
                    {}, vk::PipelineBindPoint::eGraphics, {}, color_ref,
                    resolve_ref);
                shellRenderPass = device.createRenderPassUnique(
                    vk::RenderPassCreateInfo({}, attachments, subpass, dep));
            }
            debugName(device, shellRenderPass.get(), "Shell Render Pass");
        }
    }

    void shell::preload_fixed_components()
    {
        if(fixed_components_loaded) {
            return;
        }

        menu.preload(device, allocator, *loader);

        if(!win->config.headless) {
            ok_sound = sdl::mix::unique_chunk{sdl::mix::LoadWAV(config::CONFIG.resolve_sound(config::CONFIG.asset_directory/"sounds/ok.wav").string().c_str())};
            if(!ok_sound) {
                spdlog::warn("sdl::mix::LoadWAV: {}", sdl::mix::GetError());
            }
            auto load_sound_multi = [&](sdl::mix::unique_chunk& slot, std::initializer_list<const char*> names){
                for(const char* n : names) {
                    auto p = config::CONFIG.resolve_sound(config::CONFIG.asset_directory/"sounds"/n).string();
                    slot = sdl::mix::unique_chunk{sdl::mix::LoadWAV(p.c_str())};
                    if(slot) { spdlog::debug("Loaded sound {}", p); return; }
                }
                // final fallback to ok.wav so UX isn't silent
                auto fallback = (config::CONFIG.asset_directory/"sounds/ok.wav").string();
                slot = sdl::mix::unique_chunk{sdl::mix::LoadWAV(fallback.c_str())};
                if(!slot) spdlog::debug("Failed to load any sound from list; last error: {}", sdl::mix::GetError());
            };
            load_sound_multi(question_sound, {"NSE.questionMark.wav", "NSE.questionMark.ogg"});
            load_sound_multi(confirm_sound,  {"NSE.ui.Confirm.wav",   "NSE.ui.Confirm.ogg"});
            load_sound_multi(cancel_sound,   {"NSE.ui.Cancel.wav",    "NSE.ui.Cancel.ogg"});
            load_sound_multi(back_sound,     {"NSE.clicker.Cancel.wav","NSE.clicker.Cancel.ogg"});
        }

        reload_button_icons();

        cursorTexture = std::make_unique<texture>(device, allocator);
        loader->loadTexture(cursorTexture.get(), existing_asset_or_fallback(
            config::CONFIG.asset_directory/"icons/icon_cursor.png",
            config::CONFIG.asset_directory/"icons/icon_category_settings.png"));

        // Push startup splash overlay (plays jingle, fades text). Headless
        // runs keep their settled-scene default unless visual verification
        // explicitly opts into the real boot overlay.
        if(!win->config.headless || force_headless_startup_overlay()) {
            shader_time_zero = std::chrono::steady_clock::now();
            startup_timeline.reset();
            emplace_overlay<app::startup_overlay>();
        }
        fixed_components_loaded = true;
    }

    void shell::prepare(std::vector<vk::Image> swapchainImages, std::vector<vk::ImageView> swapchainViews)
    {
        phase::prepare(swapchainImages, swapchainViews);

        const auto imageCount = static_cast<std::uint32_t>(swapchainImages.size());
        this->swapchainImages = swapchainImages;

        framebuffers.clear();
        backgroundFramebuffers.clear();
        blurDescriptorSets.clear();
        blurDescriptorPool.reset();
        blurExtraDescriptorPool.reset();
        backgroundAttachmentViews.clear();backgroundMsaaViews.clear();
        renderImages.clear();
        backgroundResolve.clear();
        backgroundDepth.clear();
        blurFrames.clear();
        glassBackdrops.clear();

        // recreateSwapchain has already idled the device. Retire framebuffers
        // first, then rebuild every pipeline against the matching render passes
        // before allocating attachments for the new sample count.
        if(renderPassSamples != win->config.sampleCount) {
            procedural_wave_render->prepare(0);
            create_render_passes();
            font_render->rebuild_pipelines({shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
            image_render->preload({backgroundRenderPass.get(), shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
            simple_render->preload({shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
            wave_render->preload({backgroundRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
            try {
                monthly_background_render->preload({backgroundRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
                monthly_background_failed = false;
            } catch(const std::exception& error) {
                monthly_background_failed = true;
                spdlog::error("Monthly-background Vulkan pipeline initialization failed ({}); using the safe clear-colour fallback", error.what());
            }
            procedural_wave_render->preload({backgroundRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
            renderPassSamples = win->config.sampleCount;
        }
        procedural_wave_render->prepare(static_cast<int>(swapchainImages.size()));

        if(imageCount == 0) {
            font_render->prepare(0);
            image_render->prepare(0);
            simple_render->prepare(0);
            wave_render->prepare(0);
            return;
        }

        const auto extent = win->swapchainExtent;
        framebuffers.reserve(imageCount);
        backgroundFramebuffers.reserve(imageCount);
        renderImages.reserve(imageCount);
        backgroundResolve.reserve(imageCount);
        backgroundDepth.reserve(imageCount);
        blurFrames.reserve(imageCount);

        for(std::uint32_t i=0; i<imageCount; i++)
        {
            backgroundDepth.push_back(std::make_unique<texture>(device, allocator, extent,
                vk::ImageUsageFlagBits::eDepthStencilAttachment, vk::Format::eD32Sfloat,
                win->config.sampleCount, false, vk::ImageAspectFlagBits::eDepth));
            debugName(device, swapchainImages[i], "Swapchain Image #"+std::to_string(i));
            if(win->config.sampleCount != vk::SampleCountFlagBits::e1) {
                renderImages.push_back(std::make_unique<texture>(device, allocator,
                    extent, vk::ImageUsageFlagBits::eColorAttachment,
                    win->swapchainFormat.format, win->config.sampleCount,
                    false, vk::ImageAspectFlagBits::eColor));
                debugName(device, renderImages.back()->image, "Shell Render Image #"+std::to_string(i));
            }
            {
                // Create per-frame background resolve target (single-sample, sampled + transfer)
                auto tex = std::make_unique<texture>(device, allocator,
                    extent,
                    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc,
                    win->swapchainFormat.format, vk::SampleCountFlagBits::e1, false, vk::ImageAspectFlagBits::eColor);
                debugName(device, tex->image, ("Background Resolve #"+std::to_string(i)).c_str());
                const auto unorm=win->swapchainFormat.format==vk::Format::eB8G8R8A8Srgb?vk::Format::eB8G8R8A8Unorm:
                    win->swapchainFormat.format==vk::Format::eR8G8B8A8Srgb?vk::Format::eR8G8B8A8Unorm:win->swapchainFormat.format;
                backgroundAttachmentViews.push_back(tex->create_view(unorm));
                if(win->config.sampleCount!=vk::SampleCountFlagBits::e1) backgroundMsaaViews.push_back(renderImages[i]->create_view(unorm));
                backgroundResolve.push_back(std::move(tex));
            }
            blurFrames.emplace_back();
            {
                // The glass backdrop: same format as the background, a mip
                // chain filled by blits, sampled by glass panels.
                auto& backdrop = glassBackdrops.emplace_back();
                const auto format = win->swapchainFormat.format;
                const auto features = win->physicalDevice.getFormatProperties(format).optimalTilingFeatures;
                const auto needed = vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst |
                    vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
                if((features & needed) == needed) {
                    vk::ImageCreateInfo info({}, vk::ImageType::e2D, format, vk::Extent3D(extent.width, extent.height, 1),
                        glass_backdrop_levels, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal,
                        vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                        vk::SharingMode::eExclusive, {}, vk::ImageLayout::eUndefined);
                    vma::AllocationCreateInfo allocation({}, vma::MemoryUsage::eGpuOnly);
                    auto [image, memory] = allocator.createImageUnique(info, allocation);
                    backdrop.image = std::move(image);
                    backdrop.allocation = std::move(memory);
                    backdrop.view = device.createImageViewUnique(vk::ImageViewCreateInfo({}, backdrop.image.get(),
                        vk::ImageViewType::e2D, format, {},
                        vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, glass_backdrop_levels, 0, 1)));
                    debugName(device, backdrop.image.get(), ("Glass Backdrop #"+std::to_string(i)).c_str());
                } else if(i == 0) {
                    spdlog::warn("Glass: the background format cannot be blitted; panels use their opaque fallback");
                }
            }
            {
                if(win->config.sampleCount == vk::SampleCountFlagBits::e1) {
                    const std::array attachments{swapchainViews[i]};
                    framebuffers.push_back(device.createFramebufferUnique(
                        vk::FramebufferCreateInfo({}, shellRenderPass.get(), attachments,
                            extent.width, extent.height, 1)));
                } else {
                    const std::array attachments{
                        renderImages[i]->imageView.get(), swapchainViews[i]};
                    framebuffers.push_back(device.createFramebufferUnique(
                        vk::FramebufferCreateInfo({}, shellRenderPass.get(), attachments,
                            extent.width, extent.height, 1)));
                }
                debugName(device, framebuffers.back().get(), "XMB Shell Framebuffer #"+std::to_string(i));
            }
            {
                if(win->config.sampleCount == vk::SampleCountFlagBits::e1) {
                    const std::array attachments{
                        backgroundAttachmentViews[i].get(), backgroundDepth[i]->imageView.get()};
                    backgroundFramebuffers.push_back(device.createFramebufferUnique(
                        vk::FramebufferCreateInfo({}, backgroundRenderPass.get(), attachments,
                            extent.width, extent.height, 1)));
                } else {
                    const std::array attachments{
                        backgroundMsaaViews[i].get(),
                        backgroundAttachmentViews[i].get(), backgroundDepth[i]->imageView.get()};
                    backgroundFramebuffers.push_back(device.createFramebufferUnique(
                        vk::FramebufferCreateInfo({}, backgroundRenderPass.get(), attachments,
                            extent.width, extent.height, 1)));
                }
                debugName(device, backgroundFramebuffers.back().get(), "XMB Shell Background Framebuffer #"+std::to_string(i));
            }
        }

        {
            vk::DescriptorPoolSize size(vk::DescriptorType::eStorageImage, 2*imageCount);
            vk::DescriptorPoolCreateInfo pool_info({}, imageCount, size);
            blurDescriptorPool = device.createDescriptorPoolUnique(pool_info);

            std::vector<vk::DescriptorSetLayout> layouts(imageCount, blurDescriptorSetLayout.get());
            vk::DescriptorSetAllocateInfo alloc_info(blurDescriptorPool.get(), layouts);
            blurDescriptorSets = device.allocateDescriptorSets(alloc_info);
        }
        {
            vk::DescriptorPoolSize size(vk::DescriptorType::eStorageImage, 12*imageCount);
            vk::DescriptorPoolCreateInfo pool_info({}, 6*imageCount, size);
            blurExtraDescriptorPool = device.createDescriptorPoolUnique(pool_info);

            std::vector<vk::DescriptorSetLayout> layouts(6*imageCount, blurDescriptorSetLayout.get());
            vk::DescriptorSetAllocateInfo alloc_info(blurExtraDescriptorPool.get(), layouts);
            auto sets = device.allocateDescriptorSets(alloc_info);

            for(std::uint32_t i=0; i<imageCount; i++) {
                auto& blurFrame = blurFrames[i];
                const auto setBase = 6*i;
                blurFrame.downsampleSet  = sets[setBase + 0]; // full -> half
                blurFrame.halfBlurSet    = sets[setBase + 1]; // half -> half
                blurFrame.upsampleSet    = sets[setBase + 2]; // half -> full
                blurFrame.downsample2Set = sets[setBase + 3]; // half -> quarter
                blurFrame.quarterBlurSet = sets[setBase + 4]; // quarter -> quarter
                blurFrame.upsample2Set   = sets[setBase + 5]; // quarter -> half
            }
        }

        font_render->prepare(swapchainViews.size());
        image_render->prepare(swapchainViews.size());
        simple_render->prepare(swapchainViews.size());
        wave_render->prepare(swapchainViews.size());
    }

    void shell::ensure_blur(int frame) {
        auto& resources = blurFrames.at(frame);
        if(resources.fullSrc) return;
        const auto extent = win->swapchainExtent;
        const vk::Extent2D half{std::max(1u, extent.width / 2), std::max(1u, extent.height / 2)};
        const vk::Extent2D quarter{std::max(1u, half.width / 2), std::max(1u, half.height / 2)};
        const auto usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc |
            vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
        const auto make = [&](vk::Extent2D size) {
            return std::make_unique<texture>(device, allocator, size, usage,
                vk::Format::eR16G16B16A16Sfloat, vk::SampleCountFlagBits::e1, false);
        };
        resources.fullSrc = make(extent); resources.fullDst = make(extent);
        resources.halfSrc = make(half); resources.halfDst = make(half);
        resources.quarterSrc = make(quarter); resources.quarterDst = make(quarter);
        const std::array<vk::DescriptorSet, 7> sets{blurDescriptorSets[frame],
            resources.downsampleSet, resources.halfBlurSet, resources.upsampleSet,
            resources.downsample2Set, resources.quarterBlurSet, resources.upsample2Set};
        const std::array<std::array<texture*, 2>, 7> pairs{{
            {resources.fullSrc.get(), resources.fullDst.get()},
            {resources.fullSrc.get(), resources.halfSrc.get()},
            {resources.halfSrc.get(), resources.halfDst.get()},
            {resources.halfDst.get(), resources.fullDst.get()},
            {resources.halfSrc.get(), resources.quarterSrc.get()},
            {resources.quarterSrc.get(), resources.quarterDst.get()},
            {resources.quarterDst.get(), resources.halfDst.get()}}};
        std::array<std::array<vk::DescriptorImageInfo, 2>, 7> infos;
        std::array<vk::WriteDescriptorSet, 7> writes;
        for(std::size_t i = 0; i < sets.size(); ++i) {
            for(std::size_t j = 0; j < 2; ++j)
                infos[i][j] = vk::DescriptorImageInfo({}, pairs[i][j]->imageView.get(), vk::ImageLayout::eGeneral);
            writes[i] = vk::WriteDescriptorSet(sets[i], 0, 0, 2, vk::DescriptorType::eStorageImage, infos[i].data());
        }
        device.updateDescriptorSets(writes, {});
    }

    // Copy the rendered background into the glass backdrop and build its mip
    // chain with linear blits. backgroundResolve leaves and returns to
    // shader-read layout, so the blur path after this is unaffected.
    void shell::build_glass_backdrop(vk::CommandBuffer commandBuffer, int frame) {
        if(static_cast<std::size_t>(frame) >= glassBackdrops.size() || !glassBackdrops[static_cast<std::size_t>(frame)].image) return;
        const vk::Image target = glassBackdrops[static_cast<std::size_t>(frame)].image.get();
        const vk::Image source = backgroundResolve[static_cast<std::size_t>(frame)]->image;
        const auto range = [](std::uint32_t level, std::uint32_t count) {
            return vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, level, count, 0, 1);
        };
        const auto barrier = [&](vk::Image image, vk::ImageSubresourceRange subresource, vk::AccessFlags from_access, vk::AccessFlags to_access,
                                 vk::ImageLayout from, vk::ImageLayout to, vk::PipelineStageFlags from_stage, vk::PipelineStageFlags to_stage) {
            commandBuffer.pipelineBarrier(from_stage, to_stage, {}, {}, {},
                vk::ImageMemoryBarrier(from_access, to_access, from, to, vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, image, subresource));
        };
        const auto size = [](std::uint32_t extent, std::uint32_t level) {
            return static_cast<std::int32_t>(std::max(1u, extent >> level));
        };
        const auto width = win->swapchainExtent.width, height = win->swapchainExtent.height;
        barrier(source, range(0, 1), vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderRead, vk::AccessFlagBits::eTransferRead,
            vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal,
            vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eTransfer);
        barrier(target, range(0, glass_backdrop_levels), vk::AccessFlagBits::eShaderRead, vk::AccessFlagBits::eTransferWrite,
            vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eTransfer);
        const auto blit = [&](vk::Image from, std::uint32_t from_level, std::uint32_t to_level) {
            const vk::ImageBlit region(
                vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, from_level, 0, 1),
                {vk::Offset3D{0, 0, 0}, vk::Offset3D{size(width, from_level), size(height, from_level), 1}},
                vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, to_level, 0, 1),
                {vk::Offset3D{0, 0, 0}, vk::Offset3D{size(width, to_level), size(height, to_level), 1}});
            commandBuffer.blitImage(from, vk::ImageLayout::eTransferSrcOptimal, target, vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eLinear);
        };
        blit(source, 0, 0);
        for(std::uint32_t level = 1; level < glass_backdrop_levels; ++level) {
            barrier(target, range(level - 1, 1), vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferRead,
                vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
                vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer);
            blit(target, level - 1, level);
        }
        barrier(target, range(0, glass_backdrop_levels - 1), vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderRead,
            vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader);
        barrier(target, range(glass_backdrop_levels - 1, 1), vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
            vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader);
        barrier(source, range(0, 1), vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderRead,
            vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader);
    }

    void shell::reload_language() {
        try {
            const std::string& lang = config::CONFIG.language;
            openxmb::localization::shell_locale::apply(lang, config::CONFIG.locale_directory);
            reload_fonts();
            spdlog::info("Language set to '{}'; reloading menus", lang);

            // Rebuild main menu to refresh translated strings
            reload_menu();
        } catch(const std::exception& e) {
            spdlog::error("reload_language failed: {}", e.what());
        }
    }

    void shell::render(int frame, vk::Semaphore imageAvailable, vk::Semaphore renderFinished, vk::Fence fence)
    {
        tick();

        vk::CommandBuffer commandBuffer = commandBuffers[frame];
        auto& blurFrame = blurFrames[frame];
        auto now = std::chrono::steady_clock::now();

        if(theme->active && !theme->active->animation.empty()) {
            try {
                if(!config::CONFIG.enabled("power.reduced.motion",false))theme->animation_seconds+=std::chrono::duration<double>(now-theme->sampled).count();
                theme->sampled=now;
                const auto seconds=theme->animation_seconds;
                theme->frame = openxmb::themes::ThemeLibrary::sample_animation(*theme->active,
                    fixed_wave_seconds_from_environment().value_or(seconds));
                std::vector<std::filesystem::path> needed;
                if(!theme->frame.background.empty()) needed.push_back(theme->frame.background);
                for(const auto& layer : theme->frame.layers) needed.push_back(layer.image);
                for(const auto& draw : theme->frame.draws) needed.push_back(draw.image);
                for(const auto& path : needed) {
                    const auto ext=path.extension().string();
                    const bool video=ext==".mp4" || ext==".webm" || ext==".mkv" || ext==".mov" || ext==".avi";
                    if(path.empty() || video || theme->textures.contains(path)) continue;
                    std::size_t bytes=0;
                    for(const auto& [key, value] : theme->textures) if(value->loaded) bytes+=static_cast<std::size_t>(value->width)*value->height*4;
                    if(theme->textures.size()>=128 || bytes>128u*1024u*1024u) {
                        win->waitIdle();
                        std::erase_if(theme->textures,[&](const auto& item) { return std::find(needed.begin(),needed.end(),item.first)==needed.end(); });
                        if(theme->textures.size()>=128) break;
                    }
                    auto image=std::make_unique<texture>(device,allocator);
                    loader->loadTexture(image.get(),path);
                    theme->textures.emplace(path,std::move(image));
                }
            } catch(const std::exception& error) { spdlog::warn("Theme animation failed: {}",error.what()); }
        }
        const bool retiring_pause_overlay = is_retiring_pause_overlay(old_overlay);
        const bool overlay_ingame = ingame_mode || has_pause_overlay() || retiring_pause_overlay || game_has_screen;
        // The paused game's still goes once its panel has fully gone.
        if(!pending_pause_game && !has_pause_overlay() && !retiring_pause_overlay &&
           pause_backdrop.state != openxmb::xmb::pause_backdrop_state::idle && pause_backdrop.state != openxmb::xmb::pause_backdrop_state::capturing)
            drop_pause_still();
        // While paused, the menu draws the frozen game's still as its
        // background so the glass and the frame show the same picture.
        const bool still_backdrop = overlay_ingame && pause_still_drawn() && (has_pause_overlay() || retiring_pause_overlay);
        if (win) {
            win->config.transparentSurface = overlay_ingame;
        }
        commandBuffer.begin(vk::CommandBufferBeginInfo());
        for(auto& overlay : std::views::reverse(overlays)) {
            overlay->prerender(commandBuffer, frame, this);
        }
        {
            const auto wall_now = openxmb::xmb::wall_clock_now();
            auto themeColour = utils::xmb_resolve_theme_colour(wall_now);
            glm::vec3 baseThemeColour = themeColour.base_colour;
            const auto wall_time = std::chrono::system_clock::to_time_t(wall_now);
            std::tm local_time{};
#if defined(_WIN32)
            localtime_s(&local_time, &wall_time);
#else
            localtime_r(&wall_time, &local_time);
#endif
            const auto local_hour = static_cast<float>(local_time.tm_hour) +
                static_cast<float>(local_time.tm_min) / 60.0F +
                static_cast<float>(local_time.tm_sec) / 3600.0F;
            const auto day_night_hour =
                utils::xmb_effective_day_night_hour(local_hour);
            const auto original_gradient =
                openxmb::xmb::resolve_background_gradient(
                    local_time.tm_mon, day_night_hour);
            const auto manual_gradient =
                openxmb::xmb::resolve_manual_background_gradient(
                    {themeColour.base_colour.r, themeColour.base_colour.g,
                     themeColour.base_colour.b},
                    day_night_hour);
            auto background_gradient = config::CONFIG.themeOriginalColour
                ? original_gradient
                : manual_gradient;
            // xmb-web's Original background lets the calendar-driven gradient
            // carry the hue.  The captured ribbon and dust/glints stay close to
            // neutral silver/white so they read over every month instead of
            // forcing the older OpenXMB theme colour (which made the scene feel
            // stuck on June/cyan).
            const bool music_scene=std::any_of(overlays.begin(),overlays.end(),[](const auto& overlay){return overlay->music_background();});
            const bool reactive_scene=std::any_of(overlays.begin(),overlays.end(),[](const auto& overlay){return overlay->music_reactive();});
            const bool flow_scene=reactive_scene&&config::CONFIG.preference("music.visualizer","Wave")=="Flow";
            procedural_wave_render->flow_mode=flow_scene;
            procedural_wave_render->visualizer_time=flow_scene?static_cast<float>(config::CONFIG.enabled("power.reduced.motion",false)?36.0:fixed_wave_seconds_from_environment().value_or(-1.0)):-1.0F;
            // Preserve the departing scene's endpoint until its mix reaches
            // zero; switching endpoints on close made the ribbon jump first.
            if(music_scene)procedural_wave_render->music_vertical=reactive_scene?-0.02F:0.26F;
            procedural_wave_render->set_music_signal({},false,0);
            procedural_wave_render->set_music_mode(music_scene);
            const float music_mix=procedural_wave_render->music_mix();background_gradient.music_mix=music_mix;
            if(flow_scene) {background_gradient.top_rgb={0,0,0};background_gradient.bottom_rgb={0,0,0};background_gradient.music_mix=0;}
            glm::vec3 effect_color{background_gradient.bottom_rgb[0],background_gradient.bottom_rgb[1],background_gradient.bottom_rgb[2]};
            effect_color=glm::mix(glm::min(effect_color*1.6F,glm::vec3(1.0F)),glm::vec3(1.0F),0.35F);
            const glm::vec3 original_effect_tint=glm::mix(effect_color,glm::vec3(192.0F/255,108.0F/255,200.0F/255),music_mix);
            const float original_effect_brightness = 1.0F;
            const bool can_render_monthly_background =
                (music_scene||music_mix>0.001F||config::CONFIG.backgroundType ==
                    config::config::background_type::original) &&
                monthly_background_render->pipelines_ready() &&
                !monthly_background_failed;
            // Always tint the background clear colour (for both Original and Classic)
            vk::ClearValue color(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});
            if(!can_render_monthly_background) {
                glm::vec3 c = themeColour.shaded_colour;
                color = vk::ClearColorValue(std::array<float, 4>{ c.r, c.g, c.b, 1.0f });
            }
            if(overlay_ingame) {
                color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
            }
            for(const auto& overlay : std::views::reverse(overlays)) {
                if(!overlay_ingame && overlay->covers_scene()) {
                    const auto c = overlay->backdrop_srgb();
                    color = vk::ClearColorValue(std::array<float, 4>{c[0], c[1], c[2], 1.0f});
                    break;
                }
            }
            std::array<vk::ClearValue, 3> background_clears{color, color, color};
            const auto depth_index = win->config.sampleCount == vk::SampleCountFlagBits::e1 ? 1u : 2u;
            background_clears[depth_index] = vk::ClearDepthStencilValue(1.0F, 0);
            image_render->setOutputEncoded(true);
            commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(backgroundRenderPass.get(), backgroundFramebuffers[frame].get(),
                vk::Rect2D({0, 0}, win->swapchainExtent), depth_index + 1, background_clears.data()), vk::SubpassContents::eInline);
            vk::Viewport viewport(0.0f, 0.0f,
                static_cast<float>(win->swapchainExtent.width),
                static_cast<float>(win->swapchainExtent.height), 0.0f, 1.0f);
            vk::Rect2D scissor({0,0}, win->swapchainExtent);
            commandBuffer.setViewport(0, viewport);
            commandBuffer.setScissor(0, scissor);

            const auto boot = startup_sample();

            // A loading screen that covers everything: skip the wallpaper, so
            // the starting game gets the processor and the GPU.
            const bool scene_covered=std::any_of(overlays.begin(),overlays.end(),[](const auto& overlay){return overlay->covers_scene();});
            if(still_backdrop)
                image_render->renderImageSized(commandBuffer,frame,backgroundRenderPass.get(),*pause_still,0,0,
                    static_cast<int>(win->swapchainExtent.width),static_cast<int>(win->swapchainExtent.height));
            if(!overlay_ingame&&!scene_covered) {
                if(music_scene||music_mix>0.001F||config::CONFIG.backgroundType == config::config::background_type::original) {
                    if(can_render_monthly_background) {
                        try {
                            const std::array boot_background{
                                static_cast<float>(boot.background_exposure_top),
                                static_cast<float>(boot.background_exposure_bottom),
                                static_cast<float>(boot.background_sweep),
                                boot.background_active ? 1.0F : 0.0F,
                            };
                            monthly_background_render->render(
                                commandBuffer, backgroundRenderPass.get(),
                                background_gradient, boot_background);
                        } catch(const std::exception& error) {
                            monthly_background_failed = true;
                            spdlog::error(
                                "Monthly-background rendering failed ({}); using the safe clear-colour fallback",
                                error.what());
                        }
                    }
                    procedural_wave_render->render(
                        commandBuffer, frame, backgroundRenderPass.get(),
                        original_effect_tint, original_effect_brightness,
                        static_cast<float>(config::CONFIG.enabled("power.reduced.motion",false)?36.0:fixed_wave_seconds_from_environment().value_or(boot.wave_boot_time_seconds)));

                }
                else if(config::CONFIG.backgroundType == config::config::background_type::wave) {
                    wave_render->waveColor = baseThemeColour; // PS3 look: wave uses base, brightness on background only
                    wave_render->paused=config::CONFIG.enabled("power.reduced.motion",false);
                    if(config::CONFIG.wavesEnabled)wave_render->render(commandBuffer, frame, backgroundRenderPass.get());
                }
                else if(config::CONFIG.backgroundType == config::config::background_type::image) {
                    if(backgroundTexture) {
                        image_render->renderImageSized(commandBuffer, frame, backgroundRenderPass.get(), *backgroundTexture,
                            0.0f, 0.0f,
                            static_cast<int>(win->swapchainExtent.width),
                            static_cast<int>(win->swapchainExtent.height)
                        );
                    }
                }
            }

            if(theme->active) {
                gui_renderer theme_gui(commandBuffer,frame,backgroundRenderPass.get(),win->swapchainExtent,font_render.get(),image_render.get(),simple_render.get());
                if(const auto it=theme->textures.find(theme->frame.background);it!=theme->textures.end() && it->second->loaded)
                    image_render->renderImageSized(commandBuffer,frame,backgroundRenderPass.get(),*it->second,0,0,win->swapchainExtent.width,win->swapchainExtent.height);
                for(const auto& layer : theme->frame.layers) {
                    const auto it=theme->textures.find(layer.image);
                    if(it!=theme->textures.end() && it->second->loaded)
                        image_render->renderImageSized(commandBuffer,frame,backgroundRenderPass.get(),*it->second,layer.x,layer.y,
                            layer.width*win->swapchainExtent.width,layer.height*win->swapchainExtent.height,glm::vec4(1,1,1,layer.opacity));
                }
                for(const auto& draw : theme->frame.draws) {
                    const auto it=theme->textures.find(draw.image);
                    if(it==theme->textures.end() || !it->second->loaded) continue;
                    std::vector<image_renderer::textured_vertex> vertices;
                    vertices.reserve(draw.vertices.size());
                    for(const auto& v:draw.vertices) vertices.push_back({{v.x,v.y,v.z,v.w},{v.u,v.v},{v.color[0],v.color[1],v.color[2],v.color[3]}});
                    theme_gui.draw_textured_triangles(*it->second,vertices,draw.additive,draw.depth);
                }
            }
            commandBuffer.endRenderPass();
            image_render->setOutputEncoded(false);
        }
        build_glass_backdrop(commandBuffer, frame);
        double blur_background_progress = utils::progress(now, last_blur_background_change, blur_background_transition_duration);
        const auto startup_blur_radius = std::max(0.0f, startup_background_blur_px);
        const bool use_startup_blur_background = startup_blur_radius > 0.05f;
        const bool opaque_scene=std::any_of(overlays.begin(),overlays.end(),[](const auto& overlay){return overlay->is_opaque();});
        const auto menu_blur_radius = background_only ? 0.0F : media_motion::backdrop(menu.background_blur_px(),opaque_scene);
        const auto menu_dim_alpha = background_only ? 0.0F : media_motion::backdrop(menu.background_dim_alpha(),opaque_scene);
        const bool use_menu_blur_background = menu_blur_radius > 0.05F;
        const bool use_blur_background = !overlay_ingame && (blur_background || blur_background_progress < 1.0 ||
            use_startup_blur_background || use_menu_blur_background);
        vk::ImageView compositedBackgroundView = backgroundResolve[frame]->imageView.get();
        if(use_blur_background) {
            ensure_blur(frame);
            compositedBackgroundView = blurFrame.fullDst->imageView.get();
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eFragmentShader,
                vk::PipelineStageFlagBits::eTransfer,
                {}, {}, {},
                {
                    // Make resolved background available for transfer
                    vk::ImageMemoryBarrier(
                        vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderRead,
                        vk::AccessFlagBits::eTransferRead,
                        vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        backgroundResolve[frame]->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    ),
                    // Prepare blur source for copy destination
                    vk::ImageMemoryBarrier(
                        {}, vk::AccessFlagBits::eTransferWrite,
                        vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        blurFrame.fullSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    ),
                }
            );

            // Copy the current swapchain image into our working src image (no scaling needed, copy is cheaper than blit)
            {
                // Blit allows format conversion (e.g., B8G8R8A8 -> R16G16B16A16)
                vk::ImageBlit blit{
                    vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                    { vk::Offset3D{0,0,0}, vk::Offset3D{static_cast<int>(win->swapchainExtent.width), static_cast<int>(win->swapchainExtent.height), 1} },
                    vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                    { vk::Offset3D{0,0,0}, vk::Offset3D{static_cast<int>(blurFrame.fullSrc->width), static_cast<int>(blurFrame.fullSrc->height), 1} }
                };
                commandBuffer.blitImage(backgroundResolve[frame]->image, vk::ImageLayout::eTransferSrcOptimal,
                                        blurFrame.fullSrc->image, vk::ImageLayout::eTransferDstOptimal,
                                        blit, vk::Filter::eLinear);
            }

            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader,
                {}, {}, {},
                {
                    vk::ImageMemoryBarrier(
                        vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                        vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        blurFrame.fullSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    ),
                    vk::ImageMemoryBarrier(
                        {}, vk::AccessFlagBits::eShaderWrite,
                        vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        blurFrame.fullDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    ),
                }
            );

            // Decide path: full-res separable Gaussian for small radius; downsampled pipeline for larger
            BlurConstants constants{};
            const int modalRadius = static_cast<int>(20 * (blur_background ? blur_background_progress : (1.0 - blur_background_progress)));
            const int startupRadius = static_cast<int>(std::round(startup_blur_radius));
            const int menuRadius = static_cast<int>(std::round(
                menu_blur_radius * static_cast<float>(win->swapchainExtent.height) / 1080.0F));
            const int targetRadius = std::max({modalRadius, startupRadius, menuRadius});

            if(targetRadius <= 4) {
                int groupCountX = static_cast<int>(std::ceil(blurFrame.fullSrc->width/16.0));
                int groupCountY = static_cast<int>(std::ceil(blurFrame.fullSrc->height/16.0));

                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, blurPipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {blurDescriptorSets[frame]}, {});
                constants.size = targetRadius;
                // Pass 1: horizontal blur into blurFrame.fullDst
                constants.axis = 0;
                commandBuffer.pushConstants(blurPipelineLayout.get(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(BlurConstants), &constants);
                commandBuffer.dispatch(groupCountX, groupCountY, 1);

                // Prepare to copy blurFrame.fullDst -> blurFrame.fullSrc (ping-pong)
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.fullDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eTransferWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferDstOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.fullSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                    }
                );
                // Copy into src for the second pass to read
                vk::ImageCopy ic{};
                ic.setSrcSubresource({vk::ImageAspectFlagBits::eColor, 0, 0, 1});
                ic.setDstSubresource({vk::ImageAspectFlagBits::eColor, 0, 0, 1});
                ic.setExtent(vk::Extent3D{
                    static_cast<uint32_t>(blurFrame.fullSrc->width),
                    static_cast<uint32_t>(blurFrame.fullSrc->height),
                    1u
                });
                commandBuffer.copyImage(blurFrame.fullDst->image, vk::ImageLayout::eTransferSrcOptimal,
                                        blurFrame.fullSrc->image, vk::ImageLayout::eTransferDstOptimal,
                                        ic);

                // Prepare images for second compute pass (vertical): src=GENERAL (read), dst=GENERAL (write)
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                            vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.fullSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.fullDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                    }
                );

                // Pass 2: vertical blur into blurFrame.fullDst
                constants.axis = 1;
                commandBuffer.pushConstants(blurPipelineLayout.get(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(BlurConstants), &constants);
                commandBuffer.dispatch(groupCountX, groupCountY, 1);

                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eFragmentShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.fullDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                    }
                );
            } else if(targetRadius <= 8) {
                // Downsample to half-res, blur there, upsample back
                int halfX = static_cast<int>(std::ceil(blurFrame.halfSrc->width/16.0));
                int halfY = static_cast<int>(std::ceil(blurFrame.halfSrc->height/16.0));

                // Ensure half images are in GENERAL layout before their first storage use.
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        )
                    }
                );

                // Pass A: downsample full->half into blurFrame.halfSrc
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, downsamplePipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {blurFrame.downsampleSet}, {});
                commandBuffer.dispatch(halfX, halfY, 1);

                // Prepare halfSrc for read, halfDst for write
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        )
                    }
                );

                // Pass B: horizontal blur (half) into blurFrame.halfDst
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, blurPipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {blurFrame.halfBlurSet}, {});
                constants.size = std::max(1, targetRadius / 2);
                constants.axis = 0;
                commandBuffer.pushConstants(blurPipelineLayout.get(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(BlurConstants), &constants);
                commandBuffer.dispatch(halfX, halfY, 1);

                // Ping-pong: copy halfDst -> halfSrc
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eTransferWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferDstOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        )
                    }
                );
                {
                    vk::ImageCopy ic2{};
                    ic2.setSrcSubresource({vk::ImageAspectFlagBits::eColor, 0, 0, 1});
                    ic2.setDstSubresource({vk::ImageAspectFlagBits::eColor, 0, 0, 1});
                    ic2.setExtent(vk::Extent3D{
                        static_cast<uint32_t>(blurFrame.halfSrc->width),
                        static_cast<uint32_t>(blurFrame.halfSrc->height),
                        1u
                    });
                    commandBuffer.copyImage(blurFrame.halfDst->image, vk::ImageLayout::eTransferSrcOptimal,
                                            blurFrame.halfSrc->image, vk::ImageLayout::eTransferDstOptimal,
                                            ic2);
                }
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                            vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        )
                    }
                );

                // Pass C: vertical blur (half) into blurFrame.halfDst
                constants.axis = 1;
                commandBuffer.pushConstants(blurPipelineLayout.get(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(BlurConstants), &constants);
                commandBuffer.dispatch(halfX, halfY, 1);

                // Prepare for upsample: halfDst read, fullDst write
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.fullDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        )
                    }
                );

                // Pass D: upsample half -> full into blurFrame.fullDst
                int fullX = static_cast<int>(std::ceil(blurFrame.fullDst->width/16.0));
                int fullY = static_cast<int>(std::ceil(blurFrame.fullDst->height/16.0));
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, upsamplePipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {blurFrame.upsampleSet}, {});
                commandBuffer.dispatch(fullX, fullY, 1);

                // Transition for sampling
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eFragmentShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.fullDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        )
                    }
                );
            } else {
                // Two-level: Full -> Half -> Quarter, blur at quarter, then upsample back
                int halfX = static_cast<int>(std::ceil(blurFrame.halfSrc->width/16.0));
                int halfY = static_cast<int>(std::ceil(blurFrame.halfSrc->height/16.0));
                int qX = static_cast<int>(std::ceil(blurFrame.quarterSrc->width/16.0));
                int qY = static_cast<int>(std::ceil(blurFrame.quarterSrc->height/16.0));

                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.quarterSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.quarterDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        )
                    }
                );

                // A: downsample full->half
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, downsamplePipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {blurFrame.downsampleSet}, {});
                commandBuffer.dispatch(halfX, halfY, 1);
                // make halfSrc readable
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {}, { vk::ImageMemoryBarrier(
                        vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                        vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        blurFrame.halfSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)) });

                // B: downsample half->quarter
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, downsamplePipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {blurFrame.downsample2Set}, {});
                commandBuffer.dispatch(qX, qY, 1);
                // Make quarterSrc read, quarterDst write
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.quarterSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.quarterDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        )
                    }
                );

                // C: horizontal blur (quarter) into quarterDst
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, blurPipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {blurFrame.quarterBlurSet}, {});
                constants.size = std::max(1, targetRadius / 4);
                constants.axis = 0;
                commandBuffer.pushConstants(blurPipelineLayout.get(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(BlurConstants), &constants);
                commandBuffer.dispatch(qX, qY, 1);

                // Ping-pong quarter: copy dst->src
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.quarterDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eTransferWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferDstOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.quarterSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        )
                    }
                );
                {
                    vk::ImageCopy ic{};
                    ic.setSrcSubresource({vk::ImageAspectFlagBits::eColor,0,0,1});
                    ic.setDstSubresource({vk::ImageAspectFlagBits::eColor,0,0,1});
                    ic.setExtent(vk::Extent3D{ static_cast<uint32_t>(blurFrame.quarterSrc->width), static_cast<uint32_t>(blurFrame.quarterSrc->height), 1u });
                    commandBuffer.copyImage(blurFrame.quarterDst->image, vk::ImageLayout::eTransferSrcOptimal,
                                            blurFrame.quarterSrc->image, vk::ImageLayout::eTransferDstOptimal,
                                            ic);
                }
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                            vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.quarterSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        ),
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.quarterDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        )
                    }
                );

                // D: vertical blur (quarter) into quarterDst
                constants.axis = 1;
                commandBuffer.pushConstants(blurPipelineLayout.get(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(BlurConstants), &constants);
                commandBuffer.dispatch(qX, qY, 1);

                // E: upsample quarter -> half (into halfDst) using compute
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.quarterDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        )
                    }
                );
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, upsamplePipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {blurFrame.upsample2Set}, {});
                commandBuffer.dispatch(halfX, halfY, 1);

                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.fullDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        )
                    }
                );

                // G: upsample half -> full into blurFrame.fullDst
                int fullX = static_cast<int>(std::ceil(blurFrame.fullDst->width/16.0));
                int fullY = static_cast<int>(std::ceil(blurFrame.fullDst->height/16.0));
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, upsamplePipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {blurFrame.upsampleSet}, {});
                commandBuffer.dispatch(fullX, fullY, 1);

                // Transition for sampling
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eFragmentShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.fullDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        )
                    }
                );
            }
        }
        {
            vk::ClearValue color(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
            // No manual transition needed; attachment initial/final layouts handle swapchain transitions in render pass
            commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(shellRenderPass.get(), framebuffers[frame].get(),
                vk::Rect2D({0, 0}, win->swapchainExtent), color), vk::SubpassContents::eInline);
            vk::Viewport viewport(0.0f, 0.0f, static_cast<float>(win->swapchainExtent.width), static_cast<float>(win->swapchainExtent.height), 0.0f, 1.0f);
            vk::Rect2D scissor({0,0}, win->swapchainExtent);
            commandBuffer.setViewport(0, viewport);
            commandBuffer.setScissor(0, scissor);

            const auto dim_setting=config::CONFIG.preference("power.dim","Off");
            const double dim_seconds=dim_setting=="1 minute"?60:dim_setting=="5 minutes"?300:dim_setting=="10 minutes"?600:0;
            const bool dimmed=dim_seconds>0&&std::chrono::duration<double>(std::chrono::steady_clock::now()-last_activity).count()>dim_seconds;
            const float brightness=std::clamp(static_cast<float>(config::CONFIG.number("display.brightness",1))*(dimmed?0.3F:1.0F),0.0F,1.2F);
            if(!overlay_ingame) {
                image_render->renderImageSized(commandBuffer, frame, shellRenderPass.get(), compositedBackgroundView,
                    0.0f, 0.0f, static_cast<int>(win->swapchainExtent.width), static_cast<int>(win->swapchainExtent.height),glm::vec4(brightness,brightness,brightness,1.0F));
            } else if(still_backdrop) {
                // The game's own frame: no menu brightness on top of it.
                image_render->renderImageSized(commandBuffer, frame, shellRenderPass.get(), compositedBackgroundView,
                    0.0f, 0.0f, static_cast<int>(win->swapchainExtent.width), static_cast<int>(win->swapchainExtent.height));
            }
            gui_renderer ctx(commandBuffer, frame, shellRenderPass.get(), win->swapchainExtent, font_render.get(), image_render.get(), simple_render.get());
            if(!overlay_ingame && menu_dim_alpha > 0.001F) {
                ctx.draw_rect(glm::vec2{0.0F, 0.0F}, glm::vec2{1.0F, 1.0F},
                    glm::vec4{0.0F, 0.0F, 0.0F, menu_dim_alpha});
            }
            image_render->setGlassBackground(compositedBackgroundView);
            {
                const auto index = static_cast<std::size_t>(frame);
                const bool has_backdrop = index < glassBackdrops.size() && glassBackdrops[index].view && (!overlay_ingame || still_backdrop);
                image_render->setGlassBackdrop(has_backdrop ? glassBackdrops[index].view.get() : vk::ImageView{},
                    has_backdrop ? glass_backdrop_levels : 0u);
            }
            image_render->setGlassMaterial(
                iconGlassAmbientTexture && iconGlassAmbientTexture->loaded
                    ? iconGlassAmbientTexture->imageView.get()
                    : vk::ImageView{},
                iconGlassEnvironmentTexture && iconGlassEnvironmentTexture->loaded
                    ? iconGlassEnvironmentTexture->imageView.get()
                    : vk::ImageView{});

            // Interface/FX debug overlays: draw font atlas for verification
            if(openxmb::debug::interfacefx_debug) {
                const auroreengine::texture* atlas = font_render->get_atlas();
                if(atlas && atlas->loaded) {
                    if(!openxmb::debug::interfacefx_debug_once_atlas_logged) {
                        spdlog::info("[InterfaceFXDEBUG] Font atlas: {}x{}", atlas->width, atlas->height);
                        openxmb::debug::interfacefx_debug_once_atlas_logged = true;
                    }
                    int dw = std::min(256, atlas->width);
                    int dh = std::min(256, atlas->height);
                    ctx.draw_image_sized(*atlas, 0.02f, 0.02f, dw, dh);
                }
                // Text probe: ensure font rendering path emits visible geometry
                ctx.draw_text("InterfaceFX TEXT PROBE: The quick brown fox jumps over the lazy dog.", 0.02f, 0.14f, 0.06f, glm::vec4(1.0f, 1.0f, 0.0f, 1.0f));
            }
            if(!background_only) {
                render_gui(ctx);
                flush_hint_bar(ctx);
            }

            commandBuffer.endRenderPass();
        }
        font_render->finish(frame);
        image_render->finish(frame);
        simple_render->finish(frame);
        commandBuffer.end();

        vk::PipelineStageFlags waitFlags = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        vk::SubmitInfo submit_info(imageAvailable, waitFlags, commandBuffer, renderFinished);
        submit_graphics(submit_info, fence);
    }

    void shell::render_gui(gui_renderer& renderer) {
        renderer.text_color = glm::vec4(config::CONFIG.textColor, 1.0F);
        renderer.text_scale = static_cast<float>(config::CONFIG.textScale);
        // Applying persisted settings here also covers a profile with no active
        // imported theme. Unchanged parameters preserve the sampler's history.
        if(procedural_wave_render) {
            static const void* previous_renderer = nullptr;
            static decltype(config::CONFIG.waveParameters) previous_parameters;
            if(previous_renderer != procedural_wave_render.get() || previous_parameters != config::CONFIG.waveParameters) {
                procedural_wave_render->reset_parameters();
                for(const auto& [group, parameters] : config::CONFIG.waveParameters)
                    for(const auto& [key, number] : parameters) procedural_wave_render->set_parameter(group, key, number);
                previous_renderer = procedural_wave_render.get();
                previous_parameters = config::CONFIG.waveParameters;
            }
            procedural_wave_render->waves_enabled = config::CONFIG.wavesEnabled;
            procedural_wave_render->particles_enabled = config::CONFIG.particlesEnabled&&!config::CONFIG.enabled("power.reduced.motion",false);
            procedural_wave_render->particle_color = config::CONFIG.particleColor;
        }
        bool render_menu = true;
        unsigned int overlay_begin = 0;
        bool has_overlay = !overlays.empty();
        bool top_is_message = has_overlay && (dynamic_cast<app::message_overlay*>(overlays.back().get()) != nullptr);
        bool top_uses_shell_fade = has_overlay && overlays.back()->do_fade_in();

        if(has_overlay) {
            for(int i=static_cast<int>(overlays.size())-1; i >= 0; i--) {
                if(overlays[i]->is_opaque()) {
                    overlay_begin = i;
                    render_menu = false;
                    break;
                }
            }
        }

        auto now = std::chrono::steady_clock::now();
        // TODO: somehow fix this.... god this is gonna be a huge mess
        double overlay_progress = utils::progress(now, overlay_fade_time, overlay_transition_duration);
        double dir_progress = overlay_fade_direction == transition_direction::in ? overlay_progress : 1.0 - overlay_progress;
        bool overlay_transition = overlay_progress < 1.0;
        // Consider fade-out of a message overlay (stored as old_overlay) as part of the transition too
        bool fading_out_message = (!has_overlay && overlay_transition && old_overlay && (dynamic_cast<app::message_overlay*>(old_overlay.get()) != nullptr));
        // If a message overlay is appearing/disappearing, still render menu during transition
        const bool shell_fade_transition = overlay_transition && top_uses_shell_fade;
        bool allow_menu = !is_retiring_pause_overlay(old_overlay) && !game_has_screen && !has_pause_overlay() &&
            (render_menu || ((top_is_message || fading_out_message) && shell_fade_transition));
        if(allow_menu){
            const float startup_reveal = static_cast<float>(startup_sample().ui_reveal);
            renderer.push_color(glm::vec4(startup_reveal));
            if(shell_fade_transition || fading_out_message) {
                // Fade UI to transparency: scale RGB and A together by (1 - progress)
                float s = 1.0f - static_cast<float>(dir_progress);
                renderer.push_color(glm::vec4(s, s, s, s));
            }
            // Quick zoom via gui_renderer helper so viewport/scissor are applied per draw
            const bool pushed_zoom = (top_is_message || fading_out_message);
            if(pushed_zoom) {
                float scale = static_cast<float>(glm::mix(1.0, 0.85, dir_progress));
                renderer.push_zoom(scale);
            }
            // Render the entire XMB UI (menu + time + news) within the zoom scope
            menu.render(renderer);

            const auto text_color = renderer.text_color;
            renderer.text_color = glm::vec4(1.0F);
            renderer.push_color(glm::vec4(config::CONFIG.clockColor, 1.0F));
            if(!status_bar_render.render_local(
                   renderer, openxmb::xmb::wall_clock_now())) {
                static bool status_bar_time_warning_logged = false;
                if(!status_bar_time_warning_logged) {
                    spdlog::warn(
                        "The local civil time is unavailable; hiding the XMB status bar clock");
                    status_bar_time_warning_logged = true;
                }
            }

            renderer.pop_color();
            renderer.text_color = text_color;
            // The reference root scene has no legacy placeholder ticker.
            if(pushed_zoom) renderer.pop_zoom();
            if(shell_fade_transition || fading_out_message) {
                renderer.pop_color();
            }
            renderer.pop_color();

        }

        bool enable_cursor = false;
        for(unsigned int i=overlay_begin; i < overlays.size(); i++) {
            renderer.text_scale = static_cast<float>(config::CONFIG.textScale) * overlays[i]->typography_scale();
            if(i == overlays.size()-1 && shell_fade_transition) {
                renderer.push_color(glm::mix(glm::vec4(0.0), glm::vec4(1.0), dir_progress));
                overlays[i]->render(renderer, this);
                renderer.pop_color();
            } else {
                overlays[i]->render(renderer, this);
            }
            enable_cursor = overlays[i]->enable_cursor();
        }
        if(!allow_menu && has_overlay && overlays[overlay_begin]->show_status_bar()) {
            const auto overlay_text_scale = renderer.text_scale;
            renderer.text_scale = static_cast<float>(config::CONFIG.textScale);
            renderer.push_color(glm::vec4(config::CONFIG.clockColor,1.0F));
            static_cast<void>(status_bar_render.render_local(renderer,openxmb::xmb::wall_clock_now()));
            renderer.pop_color();
            renderer.text_scale = overlay_text_scale;
        }
        if(overlay_transition && overlay_fade_direction == transition_direction::out &&
           old_overlay && old_overlay->do_fade_out()) {
            renderer.push_color(glm::mix(glm::vec4(0.0), glm::vec4(1.0), dir_progress));
            renderer.text_scale = static_cast<float>(config::CONFIG.textScale) * old_overlay->typography_scale();
            hint_bar_locked = true;
            old_overlay->render(renderer, this);
            hint_bar_locked = false;
            renderer.pop_color();
        } else if(old_overlay) {
            // Fade-out finished; if it was a message or pause overlay, drop background blur now
            if(dynamic_cast<app::message_overlay*>(old_overlay.get()) != nullptr ||
               dynamic_cast<app::pause_overlay*>(old_overlay.get()) != nullptr) {
                set_blur_background(false);
            }
            // Earlier submitted frames can still sample the retiring overlay's
            // icon/artwork textures after its fade has finished on the CPU.
            win->waitIdle();
            old_overlay.reset();
        }

        renderer.text_scale = static_cast<float>(config::CONFIG.textScale);
        if(enable_cursor && std::chrono::steady_clock::now()-cursor_active_at < std::chrono::seconds(2)) {
            constexpr float cursor_size = 0.05f;
            if(cursorTexture && cursorTexture->loaded) {
                renderer.draw_image(*cursorTexture,
                    cursor_position.x - (cursor_size/2.0f)/renderer.aspect_ratio,
                    cursor_position.y - cursor_size/2.0f,
                    cursor_size, cursor_size);
            } else {
                renderer.draw_rect(glm::vec2{
                        cursor_position.x - (cursor_size/6.0f)/renderer.aspect_ratio,
                        cursor_position.y - cursor_size/6.0f
                    },
                    glm::vec2{(cursor_size/3.0f)/renderer.aspect_ratio, cursor_size/3.0f},
                    glm::vec4{1.0f, 1.0f, 1.0f, 0.85f});
            }
        }

        if(config::CONFIG.showFPS || config::CONFIG.showMemory) {
            const overlay_canvas canvas(renderer);
            const float fw = static_cast<float>(renderer.frame_size.width);
            const float fh = static_cast<float>(renderer.frame_size.height);
            const float inset_x = openxmb::xmb::ui::safe_inset * canvas.scale / fw;
            float debug_y = openxmb::xmb::ui::safe_inset * canvas.scale / fh;
            if(config::CONFIG.showFPS) {
                renderer.draw_text("FPS: {:.2f}"_(win->currentFPS), inset_x, debug_y, 0.05f, glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));
                debug_y += 0.025f;
            }
            if(config::CONFIG.showMemory) {
                vk::DeviceSize budget{}, usage{};
                for(const auto& b : allocator.getHeapBudgets()) {
                    budget += b.budget;
                    usage += b.usage;
                }
                constexpr double mb = 1024.0*1024.0;
                auto u = static_cast<double>(usage)/mb;
                auto b = static_cast<double>(budget)/mb;
                renderer.draw_text("Video Memory: {:.2f}/{:.2f} MB"_(u, b), inset_x, debug_y, 0.05f, glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));
                debug_y += 0.025f;
            }
        }
    }


    std::shared_ptr<openxmb::themes::ThemeLibrary> shell::theme_library() {
        const auto directory = config::CONFIG.profileConfigPath.empty()
            ? openxmb::desktop::default_data_root() / "themes"
            : config::CONFIG.profileConfigPath.parent_path() / "themes";
        if(!theme->library || theme->library->root() != directory) {
            theme->library = std::make_shared<openxmb::themes::ThemeLibrary>(directory);
            theme->library->set_baseline(nlohmann::json::parse(config::CONFIG.shell_snapshot()));
        }
        return theme->library;
    }

    void shell::reload_menu() {
        if(!fixed_components_loaded) return;
        win->waitIdle();
        menu.preload(device, allocator, *loader);
    }

    void shell::reload_theme_sounds() {
        const auto load=[&](sdl::mix::unique_chunk& chunk,std::initializer_list<const char*> names){for(const auto* name:names){const auto file=config::CONFIG.resolve_sound(config::CONFIG.asset_directory/"sounds"/name);if(auto* replacement=sdl::mix::LoadWAV(file.string().c_str())){chunk=sdl::mix::unique_chunk{replacement};return;}}};
        load(ok_sound,{"ok.wav"});
        load(confirm_sound,{"NSE.ui.Confirm.wav","NSE.ui.Confirm.ogg","confirm.wav"});
        load(cancel_sound,{"NSE.ui.Cancel.wav","NSE.ui.Cancel.ogg","cancel.wav"});
        load(back_sound,{"NSE.clicker.Cancel.wav","NSE.clicker.Cancel.ogg","back.wav"});
    }
    void shell::open_panel(const std::string& name) {
        if(name == "themes") {
            emplace_overlay<programs::theme_manager>(*loader, theme_library(),
                [this](const openxmb::themes::AppliedTheme& value) { apply_theme(value); },
                [] { return nlohmann::json::parse(config::CONFIG.shell_snapshot()); });
        } else if(name == "settings") {
            menu.show_settings();
        } else if(name == "users" || name == "games" || name == "custom" || name == "friends" || name == "account" || name == "trophies") {
            emplace_overlay<programs::library_manager>(name, *loader,
                [this](const std::filesystem::path& path) { switch_profile(path); });
        } else {
            spdlog::warn("Unsupported panel requested: {}", name);
        }
    }

    void shell::refresh_desktop_preferences() {
        // Profile and reset callbacks run in input handling, before the engine
        // acquires a swapchain image. Rebuild here, never inside render/tick.
            if(win->config.sampleCount!=config::CONFIG.sampleCount) {
                const auto supported=win->physicalDevice.getProperties().limits.framebufferColorSampleCounts;
                if(supported&config::CONFIG.sampleCount) {win->waitIdle();win->config.sampleCount=config::CONFIG.sampleCount;win->recreateSwapchain();}
                else {spdlog::warn("Restored MSAA level is unsupported; retaining active level");config::CONFIG.sampleCount=win->config.sampleCount;}
            }
    }

    void shell::reset_settings() {
        win->waitIdle();
        static_cast<void>(theme_library()->revert());
        theme->active.reset();theme->frame={};theme->textures.clear();theme->library.reset();
        config::CONFIG.reset_to_defaults();
        refresh_desktop_preferences();reload_background();reload_fonts();reload_menu();reload_button_icons();reload_theme_sounds();
    }

    void shell::switch_profile(const std::filesystem::path& path) {
        win->waitIdle();
        config::CONFIG.switch_profile(path);
        refresh_desktop_preferences();
        theme->library.reset(); theme->active.reset(); theme->textures.clear();
        config::CONFIG.themeIcons.clear(); config::CONFIG.themeSounds.clear();
        if(auto saved = theme_library()->active()) apply_theme(*saved);
        else { reload_background(); reload_fonts(); reload_menu(); }
        reload_button_icons();reload_theme_sounds();
    }

    void shell::apply_theme(const openxmb::themes::AppliedTheme& theme) {
        win->waitIdle();
        const auto previous_font = config::CONFIG.fontPath;
        auto value = theme.shell;
        const auto& source = theme.settings;
        const auto inherit = [&](const char* from, const char* to) {
            if(source.contains(from) && !source[from].is_null()) value[to] = source[from];
        };
        // Imported packs share validated style setters. Disabled components
        // retain the pre-theme baseline.
        if(theme.components.colors) {
            inherit("icon-color", "icon-color"); inherit("text-color", "text-color");
            inherit("clock-color", "clock-color"); inherit("particle-color", "particle-color");
            inherit("dotColor", "particle-color");
        }
        if(theme.components.icons) { inherit("icon-style", "icon-style"); inherit("icon-glass-refraction", "icon-glass-refraction"); }
        if(theme.components.font) inherit("font-size", "font-size");
        if(theme.components.background) {
            inherit("theme-waves-enabled", "theme-waves-enabled"); inherit("wavesEnabled", "theme-waves-enabled");
            inherit("theme-particles-enabled", "theme-particles-enabled"); inherit("dotsEnabled", "theme-particles-enabled");
        }
        std::unique_ptr<font_renderer> validated_font;
        if(value.contains("font-path")&&value["font-path"].is_string()) {
            const auto requested=value["font-path"].get<std::string>();
            const auto path=requested=="default"?config::CONFIG.fallback_font:std::filesystem::u8path(requested);
            if(path!=previous_font)try {
                if(!openxmb::localization::font_covers(path,u8""))throw std::runtime_error("Cannot load font face");
                const auto rendered=openxmb::localization::select_font(path,config::CONFIG.asset_directory,config::CONFIG.language);
                validated_font=std::make_unique<font_renderer>(rendered.string(),32,device,allocator,win->swapchainExtent,win->gpuFeatures);
                validated_font->set_preload_texts(openxmb::localization::font_catalog_texts(config::CONFIG.locale_directory,config::CONFIG.language));
                validated_font->preload(loader,{shellRenderPass.get()},win->config.sampleCount,win->pipelineCache.get(),nullptr,0x20,0x1ff).get();
                validated_font->prepare(win->swapchainImageCount);
            } catch(...) {
                // The library may have persisted its selection before handing
                // it to the renderer. Restore that selection on load failure.
                try {if(this->theme->active&&!this->theme->active->reverted)static_cast<void>(theme_library()->apply(this->theme->active->id,this->theme->active->components));else static_cast<void>(theme_library()->revert());}
                catch(const std::exception& error){spdlog::error("Could not restore the previous theme selection: {}",error.what());}
                throw;
            }
        }
        this->theme->active = theme;
        this->theme->frame = {};
        this->theme->started = std::chrono::steady_clock::now();
        this->theme->sampled=this->theme->started;this->theme->animation_seconds=0;
        this->theme->textures.clear();
        config::CONFIG.themeIcons = theme.icons;
        config::CONFIG.themeSounds = theme.sounds;
        const auto text = [&](const char* key, auto&& setter) {
            if(value.contains(key) && value[key].is_string()) setter(value[key].get<std::string>());
        };
        text("background-image", [](const auto& v) { config::CONFIG.setBackgroundImage(v); });
        text("background-type", [](const auto& v) { config::CONFIG.setBackgroundType(v); });
        text("background-color", [](const auto& v) { config::CONFIG.setBackgroundColor(v); });
        text("wave-color", [](const auto& v) { config::CONFIG.setWaveColor(v); });
        text("theme-custom-colour", [](const auto& v) { config::CONFIG.setThemeCustomColour(v); });
        text("theme-colour-mode", [](const auto& v) { config::CONFIG.setThemeOriginalColour(v == "original"); });
        text("theme-day-night-mode", [](const auto& v) { config::CONFIG.setThemeDayNightMode(v); });
        text("font-path", [](const auto& v) { config::CONFIG.setFontPath(v); });
        text("icon-color", [](const auto& v) { config::CONFIG.setIconColor(v); });
        text("text-color", [](const auto& v) { config::CONFIG.setTextColor(v); });
        text("clock-color", [](const auto& v) { config::CONFIG.setClockColor(v); });
        text("particle-color", [](const auto& v) { config::CONFIG.setParticleColor(v == "auto" ? "#ffffff" : v); });
        if(value.contains("font-size") && value["font-size"].is_number()) config::CONFIG.setTextScale(value["font-size"].get<double>() / 100.0);
        if(value.contains("icon-glass-refraction") && value["icon-glass-refraction"].is_boolean()) config::CONFIG.setIconGlassRefraction(value["icon-glass-refraction"].get<bool>());
        text("icon-style", [](const auto& v) { config::CONFIG.setIconStyle(v); });
        if(value.contains("theme-waves-enabled") && value["theme-waves-enabled"].is_boolean()) config::CONFIG.setWavesEnabled(value["theme-waves-enabled"].get<bool>());
        if(value.contains("theme-particles-enabled") && value["theme-particles-enabled"].is_boolean()) config::CONFIG.setParticlesEnabled(value["theme-particles-enabled"].get<bool>());
        if(value.contains("wave-parameters") && value["wave-parameters"].is_object()) config::CONFIG.setWaveParameters(value["wave-parameters"].dump());
        else config::CONFIG.setWaveParameters("{}");
        procedural_wave_render->reset_parameters();
        for(const auto& [group, parameters] : config::CONFIG.waveParameters)
            for(const auto& [key, number] : parameters) procedural_wave_render->set_parameter(group, key, number);
        procedural_wave_render->waves_enabled = config::CONFIG.wavesEnabled;
        procedural_wave_render->particles_enabled = config::CONFIG.particlesEnabled&&!config::CONFIG.enabled("power.reduced.motion",false);
        procedural_wave_render->particle_color = config::CONFIG.particleColor;
        if(validated_font)font_render=std::move(validated_font);
        if(fixed_components_loaded) {
            const auto load = [&](auto& sound, const std::filesystem::path& path) {
                sound = sdl::mix::unique_chunk{sdl::mix::LoadWAV(config::CONFIG.resolve_sound(path).string().c_str())};
            };
            const auto dir = config::CONFIG.asset_directory / "sounds";
            load(ok_sound, dir/"ok.wav"); load(confirm_sound, dir/"NSE.ui.Confirm.wav");
            load(back_sound, dir/"NSE.clicker.Cancel.wav"); load(cancel_sound, dir/"NSE.ui.Cancel.wav");
            load(question_sound, dir/"NSE.questionMark.wav");
            reload_menu();
        }
        config::CONFIG.save_config();
    }

    void shell::set_font_file(const std::string& value) {
        const auto path=value=="default"?config::CONFIG.fallback_font:std::filesystem::u8path(value);
        win->waitIdle();
        if(!openxmb::localization::font_covers(path,u8""))throw std::runtime_error("Cannot load font face");
        const auto rendered=openxmb::localization::select_font(path,config::CONFIG.asset_directory,config::CONFIG.language);
        auto replacement=std::make_unique<font_renderer>(rendered.string(),32,device,allocator,win->swapchainExtent,win->gpuFeatures);
        replacement->set_preload_texts(openxmb::localization::font_catalog_texts(config::CONFIG.locale_directory,config::CONFIG.language));
        replacement->preload(loader,{shellRenderPass.get()},win->config.sampleCount,win->pipelineCache.get(),nullptr,0x20,0x1ff).get();
        replacement->prepare(win->swapchainImageCount);
        config::CONFIG.setFontPath(path.string());font_render=std::move(replacement);
    }
    void shell::reload_fonts() {
        win->waitIdle();
        const auto rendered=openxmb::localization::select_font(config::CONFIG.fontPath,config::CONFIG.asset_directory,config::CONFIG.language);
        auto replacement = std::make_unique<font_renderer>(rendered.string(), 32, device, allocator, win->swapchainExtent, win->gpuFeatures);
        replacement->set_preload_texts(openxmb::localization::font_catalog_texts(config::CONFIG.locale_directory,config::CONFIG.language));
        replacement->preload(loader, {shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get(), nullptr, 0x20, 0x1ff).get();
        replacement->prepare(win->swapchainImageCount);
        font_render = std::move(replacement);
    }

    void shell::reload_background() {
        if(config::CONFIG.backgroundType == config::config::background_type::image) {
            backgroundTexture = std::make_unique<texture>(device, allocator);
            loader->loadTexture(backgroundTexture.get(), config::CONFIG.backgroundImage);
        }
    }
    void shell::reload_button_icons() {
        const auto family=get_controller_type();
        for(std::size_t i=1;i<std::to_underlying(action::_length);++i) {
            button_labels[i].clear();buttonTextures[i].reset();if(family=="none")continue;
            const bool keyboard=family=="keyboard";const auto index=input_bindings::glyph_index(config::CONFIG.preferences,!keyboard,i-1);
            const auto code=input_bindings::read(config::CONFIG.preferences,!keyboard,i-1);
            if(family=="xbox") {
                const auto artwork=input_bindings::xbox_artwork(code);
                if(!artwork.empty()) {buttonTextures[i]=std::make_unique<texture>(device,allocator);loader->loadTexture(buttonTextures[i].get(),config::CONFIG.asset_directory/"icons/controller"/(std::string(artwork)+".png"));continue;}
            }
            if(family=="nintendo") {
                // Nintendo-layout text badges matching Nova SDL_GAMECONTROLLERCONFIG
                // label assignments (east=A/0, south=B/1, north=X/2, west=Y/3).
                switch(code) {
                    case 0:button_labels[i]="A";break;
                    case 1:button_labels[i]="B";break;
                    case 2:button_labels[i]="X";break;
                    case 3:button_labels[i]="Y";break;
                    case 4:button_labels[i]="-";break;
                    case 6:button_labels[i]="+";break;
                    case 7:button_labels[i]="L3";break;
                    case 8:button_labels[i]="R3";break;
                    case 9:button_labels[i]="L";break;
                    case 10:button_labels[i]="R";break;
                    default:button_labels[i]="Button "+std::to_string(code);
                }
                continue;
            }
            if(index<0||family=="default") {
                if(keyboard)button_labels[i]=SDL_GetScancodeName(static_cast<SDL_Scancode>(code));
                else {switch(code){case 0:button_labels[i]="South";break;case 1:button_labels[i]="East";break;case 2:button_labels[i]="West";break;case 3:button_labels[i]="North";break;case 4:button_labels[i]=family=="playstation"?"Select":"View";break;case 6:button_labels[i]=family=="playstation"?"Start":"Menu";break;case 9:button_labels[i]=family=="playstation"?"L1":"LB";break;case 10:button_labels[i]=family=="playstation"?"R1":"RB";break;default:button_labels[i]="Button "+std::to_string(code);}}
                if(button_labels[i].empty())button_labels[i]="Key "+std::to_string(code);continue;
            }
            const auto name=input_bindings::actions[static_cast<std::size_t>(index)];
            const auto icon=config::CONFIG.asset_directory/"icons"/std::format("icon_button_{}_{}.png",family,name);
            buttonTextures[i]=std::make_unique<texture>(device,allocator);
            loader->loadTexture(buttonTextures[i].get(),icon);
        }
    }
    std::string shell::get_controller_type() const {
        auto type = config::CONFIG.controllerType;
        if(type == "auto") {
            if(win->controllers.empty()) {
                return "keyboard";
            }
            for(const auto& [id, controller] : win->controllers) {
                sdl::GameControllerType ctype = sdl::GameControllerGetType(controller.get());
                if(ctype == sdl::GameControllerTypeValues::PS3 ||
                   ctype == sdl::GameControllerTypeValues::PS4 ||
                   ctype == sdl::GameControllerTypeValues::PS5) {
                    return "playstation";
                } else if(ctype == sdl::GameControllerTypeValues::XBOX360 ||
                          ctype == sdl::GameControllerTypeValues::XBOXONE) {
                    return "xbox";
                } else if(ctype == sdl::GameControllerTypeValues::NINTENDO_SWITCH_PRO ||
                          ctype == sdl::GameControllerTypeValues::NINTENDO_SWITCH_JOYCON_PAIR) {
                    return "nintendo";
                }
                std::string_view name = sdl::GameControllerName(controller.get());
                if(name == "Steam Virtual Gamepad" || name == "Steam Controller") {
                    return "steam";
                }
            }
            return "default";
        }
        return type;
    }

    void shell::tick() {
        poll_pause_ipc();
        const bool child_active=menu::has_active_child()&&!has_pause_overlay();
        const bool has_input_focus=win->config.headless||(SDL_GetWindowFlags(win->win.get())&SDL_WINDOW_INPUT_FOCUS)!=0;
        const bool focused=!child_active&&has_input_focus;
        // A launch is over when the game has taken the screen (the menu loses
        // input focus), when it failed, or when it never came up at all.
        bool launching=has_launch_overlay();
        if(launching) {
            if(has_input_focus) launch_menu_had_focus=true;
            const auto waited=std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now()-launch_started).count();
            const bool game_took_over=menu::has_active_child()&&launch_menu_had_focus&&!has_input_focus;
            if(!menu::has_active_child()||game_took_over||waited>600) {
                end_launch();
                launching=false;
                game_has_screen=game_took_over;
                game_focus_returned={};
            }
        }
        if(!menu::has_active_child()) {
            game_has_screen=false;
        } else if(game_has_screen&&has_input_focus&&!has_pause_overlay()&&!is_retiring_pause_overlay(old_overlay)) {
            // The menu has the focus again but nobody asked for a pause: the
            // game's window is gone and its process is still finishing (an
            // Android session takes a few seconds to stop). Show the menu.
            const auto now=std::chrono::steady_clock::now();
            if(game_focus_returned.time_since_epoch().count()==0) game_focus_returned=now;
            else if(now-game_focus_returned>std::chrono::milliseconds(800)) game_has_screen=false;
        } else {
            game_focus_returned={};
        }
        const auto cap=config::CONFIG.maxFPS>10000?0:config::CONFIG.maxFPS;
        // While a game has the screen the menu only clears a transparent
        // frame, so it can afford to look at the pause socket often: Home then
        // answers within a frame or two instead of up to a second.
        const double waiting_fps=launching?20.0:(game_has_screen?20.0:(has_input_focus?30.0:2.0));
        win->config.fpsLimit=child_active?waiting_fps:(focused?cap:config::CONFIG.number("power.background.fps",30));
        const bool keep_awake=config::CONFIG.enabled("power.keep.awake")&&std::any_of(overlays.begin(),overlays.end(),[](const auto& overlay){return overlay->keeps_awake();});
        static bool awake=false;
        if(awake!=keep_awake) {awake=keep_awake;if(awake) SDL_DisableScreenSaver();else SDL_EnableScreenSaver();
#ifdef _WIN32
            SetThreadExecutionState(ES_CONTINUOUS|(awake?ES_DISPLAY_REQUIRED|ES_SYSTEM_REQUIRED:0));
#endif
        }
        if(launching) {
            // The overlay ignores input and is not an action receiver; there
            // is nothing else to tick while the game starts.
            for(unsigned int i=0; i<overlays.size(); i++) {
                const auto res=overlays[i]->tick(this);
                if(res & result::close) {remove_overlay(i);i--;}
            }
            return;
        }
        if(background_only || child_active) {
            return;
        }

        if(!menu::has_active_child()) {
            if(const auto err_path = menu::last_launch_error_path(); !err_path.empty()) {
                std::error_code ec;
                if(std::filesystem::is_regular_file(err_path, ec)) {
                    std::ifstream in(err_path);
                    const auto parsed = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json{};
                    std::filesystem::remove(err_path, ec);
                    if(!parsed.is_discarded() && parsed.is_object()) {
                        std::string title;
                        std::string message;
                        if(parsed.contains("title") && parsed["title"].is_string()) {
                            title = parsed["title"].get<std::string>();
                        }
                        if(parsed.contains("message") && parsed["message"].is_string()) {
                            message = parsed["message"].get<std::string>();
                        }
                        emplace_overlay<app::message_overlay>(
                            title.empty() ? "Could Not Start Game" : ("Could Not Start " + title),
                            message.empty() ? "The game exited unexpectedly." : message
                        );
                    }
                }
            }
        }

        if(!pending_panel.empty()) {
            const auto panel = std::exchange(pending_panel, {});
            open_panel(panel);
        }
        for(unsigned int i=0; i<2; i++) {
            if(last_controller_axis_input[i]) {
                auto time_since_input = std::chrono::duration<double>(std::chrono::steady_clock::now() - last_controller_axis_input_time[i]);
                if(time_since_input > std::chrono::milliseconds(static_cast<int>(config::CONFIG.number("input.repeat",180)))) {
                    auto [controller, dir] = *last_controller_axis_input[i];
                    dispatch<events::joystick_axis>(dir, i,
                        controller_axis_position[i].x,
                        controller_axis_position[i].y);
                    last_controller_axis_input_time[i] = std::chrono::steady_clock::now();
                }
            }
        }
        const double button_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if(const auto repeated=button_timing.repeat(button_seconds,config::CONFIG.number("input.repeat",180)/1000.0)) {
            for(std::size_t i=0;i<4;++i)if(static_cast<int>(*repeated)==input_bindings::read(config::CONFIG.preferences,true,i))dispatch(event{static_cast<action>(i+1),events::controller_button_down{to_logical_button(static_cast<sdl::GameControllerButton>(*repeated))}});
        }

        for(unsigned int i=0; i<overlays.size(); i++) {
            auto res = overlays[i]->tick(this);
            if(res & result::close) {
                remove_overlay(i);
                i--;
            }
            handle(res);
        }
        poll_mouse();
        tick_cursor();
    }

    double shell::startup_elapsed_seconds() const {
        if(win->config.headless && force_headless_startup_overlay()) {
            if(const auto fixed = fixed_boot_seconds_from_environment()) return *fixed;
        }
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - shader_time_zero).count();
        if(!config::CONFIG.enabled("startup.intro")) return elapsed+(config::CONFIG.enabled("startup.warning")?openxmb::xmb::BootMilestones::warning_in_seconds:openxmb::xmb::BootMilestones::complete_seconds);
        return elapsed + (win->config.headless && !force_headless_startup_overlay()
            ? openxmb::xmb::BootMilestones::complete_seconds : 0.0);
    }

    openxmb::xmb::BootSample shell::startup_sample() {
        const auto elapsed=startup_elapsed_seconds();
        auto sample=startup_timeline.sample(elapsed);
        if(!config::CONFIG.enabled("startup.warning") && sample.warning_opacity>0) {static_cast<void>(startup_timeline.dismiss_warning(elapsed));sample=startup_timeline.sample(elapsed);}
        return sample;
    }

    void shell::dismiss_startup_warning(const event& input) {
        if(!startup_timeline.dismiss_warning(startup_elapsed_seconds())) return;
        if(const auto* button = input.get<events::controller_button_down>())
            startup_dismiss_button = button->button;
        if(const auto* key = input.get<events::key_down>())
            startup_dismiss_key = key->keycode;
        spdlog::debug("Startup warning dismissed");
    }

    void shell::dispatch(const event& event) {
        if(menu::has_active_child()&&!has_pause_overlay()) return;
        if(event.action!=action::none||event.get<events::key_down>()||event.get<events::controller_button_down>()||event.get<events::mouse_move>()) last_activity=std::chrono::steady_clock::now();
        // A held dismissal must not auto-repeat into the revealed menu.
        if(startup_dismiss_button) {
            if(const auto* up = event.get<events::controller_button_up>();
               up && up->button == *startup_dismiss_button) {
                startup_dismiss_button.reset();
                return;
            }
            if(event.test<events::controller_button_down>([this](const auto& down) {
                return down.button == *startup_dismiss_button;
            })) return;
        }
        if(startup_dismiss_key) {
            if(const auto* up = event.get<events::key_up>();
               up && up->keycode == *startup_dismiss_key) {
                startup_dismiss_key.reset();
                return;
            }
            if(event.test<events::key_down>([this](const auto& down) {
                return down.keycode == *startup_dismiss_key;
            })) return;
        }
        if(background_only) {
            return;
        }

        for(int i=static_cast<int>(overlays.size())-1; i >= 0; i--) {
            auto& e = overlays[i];
            if(e->enable_cursor() && handle_cursor(event)) {
                return;
            }

            result res = result::unsupported;
            if(auto* recv = dynamic_cast<event_receiver*>(e.get())) {
                res = recv->on_event(event);
            } else if(auto* recv = dynamic_cast<action_receiver*>(e.get())) {
                res = recv->on_action(event.action);
            }

            if(res == result::unsupported) {
                if(auto* d = event.get<events::joystick_axis>()) {
                    if(auto* recv = dynamic_cast<joystick_receiver*>(e.get())) {
                        res = recv->on_joystick(static_cast<unsigned int>(d->index), d->x, d->y);
                    }
                } else if(auto* d = event.get<events::mouse_move>()) {
                    if(auto* recv = dynamic_cast<mouse_receiver*>(e.get())) {
                        res = recv->on_mouse_move(d->x, d->y);
                    }
                } else if(auto* d = event.get<events::mouse_scroll>()) {
                    if(auto* recv = dynamic_cast<mouse_receiver*>(e.get())) {
                        res = recv->on_mouse_scroll(d->x);
                    }
                }
            }

            if(res != result::unsupported) {
                if(res & result::close) {
                    remove_overlay(i);
                    i--;
                }
                handle(res);
                return;
            }
        }

        handle(menu.on_action(event.action));
    }

    void shell::poll_mouse()
    {
        int x = 0;
        int y = 0;
        std::uint32_t buttons = SDL_GetMouseState(&x, &y);
        glm::ivec2 position{x, y};

        const float width = static_cast<float>(std::max(1u, win->swapchainExtent.width));
        const float height = static_cast<float>(std::max(1u, win->swapchainExtent.height));
        const glm::vec2 normalized{
            glm::clamp(static_cast<float>(position.x) / width, 0.0f, 1.0f),
            glm::clamp(static_cast<float>(position.y) / height, 0.0f, 1.0f)
        };

        glm::ivec2 relative{0, 0};
        if(mouse_state_initialized) {
            relative = position - last_mouse_position;
        }

        if(!mouse_state_initialized || position != last_mouse_position) {
            dispatch(event{
                action::none,
                events::mouse_move{
                    normalized.x,
                    normalized.y,
                    static_cast<float>(relative.x) / width,
                    static_cast<float>(relative.y) / height
                }
            });
        }

        for(int button = SDL_BUTTON_LEFT; button <= SDL_BUTTON_X2; ++button) {
            const std::uint32_t mask = SDL_BUTTON(button);
            const bool was_down = (last_mouse_buttons & mask) != 0;
            const bool is_down = (buttons & mask) != 0;
            if(is_down && !was_down) {
                dispatch(event{
                    action::none,
                    events::mouse_button_down{to_logical_mouse_button(button)}
                });
            } else if(!is_down && was_down) {
                dispatch(event{
                    action::none,
                    events::mouse_button_up{to_logical_mouse_button(button)}
                });
            }
        }

        last_mouse_position = position;
        last_mouse_buttons = buttons;
        mouse_state_initialized = true;
    }

    void shell::tick_cursor()
    {
        const bool cursor_enabled = std::ranges::any_of(overlays, [](const auto& overlay) {
            return overlay->enable_cursor();
        });
        if(!cursor_enabled) {
            cursor_joystick_delta = glm::vec2{0.0f, 0.0f};
            return;
        }
        if(cursor_joystick_delta.x == 0.0f && cursor_joystick_delta.y == 0.0f) {
            return;
        }
        cursor_active_at=std::chrono::steady_clock::now();
        cursor_position = glm::clamp(cursor_position + cursor_joystick_delta, glm::vec2{0.0f}, glm::vec2{1.0f});
        dispatch<events::cursor_move>(action::none, cursor_position.x, cursor_position.y);
    }

    bool shell::handle_cursor(const event& event)
    {
        if(auto* d = event.get<events::mouse_move>()) {
            cursor_active_at=std::chrono::steady_clock::now();
            cursor_position = glm::vec2{d->x, d->y};
            dispatch<events::cursor_move>(action::none, cursor_position.x, cursor_position.y);
            return true;
        }
        if(auto* d = event.get<events::joystick_axis>()) {
            if(d->index == events::logical_joystick_index::right) {
                constexpr float controller_cursor_speed = 1.0f;
                cursor_joystick_delta = (glm::vec2{d->x, d->y} / 100.0f) * controller_cursor_speed;
                if(std::abs(d->x) < 0.1f) {
                    cursor_joystick_delta.x = 0.0f;
                }
                if(std::abs(d->y) < 0.1f) {
                    cursor_joystick_delta.y = 0.0f;
                }
                return true;
            }
        }
        return false;
    }

    void shell::handle(result result) {
        const int effect_volume=static_cast<int>(128*config::CONFIG.number("audio.master",100)*config::CONFIG.number("audio.effects",100)/10000.0);
        for(auto* sound:{ok_sound.get(),confirm_sound.get(),cancel_sound.get(),back_sound.get(),question_sound.get()}) if(sound)sdl::mix::VolumeChunk(sound,effect_volume);
        if(result & result::error_rumble) {
            if(config::CONFIG.controllerRumble) {
                for(const auto& [id, controller] : win->controllers) {
                    sdl::GameControllerRumble(controller.get(), 1000, 10000, 100);
                }
            }
        }
        if(result & result::ok_sound) {
            if(ok_sound && sdl::mix::PlayChannel(-1, ok_sound.get(), 0) == -1) {
                spdlog::error("sdl::mix::PlayChannel: {}", sdl::mix::GetError());
            }
        }
        if(result & result::confirm_sound) {
            if(confirm_sound && sdl::mix::PlayChannel(-1, confirm_sound.get(), 0) == -1) {
                spdlog::debug("PlayChannel(confirm): {}", sdl::mix::GetError());
            }
        }
        if(result & result::cancel_sound) {
            if(cancel_sound && sdl::mix::PlayChannel(-1, cancel_sound.get(), 0) == -1) {
                spdlog::debug("PlayChannel(cancel): {}", sdl::mix::GetError());
            }
        }
        if(result & result::back_sound) {
            if(back_sound && sdl::mix::PlayChannel(-1, back_sound.get(), 0) == -1) {
                spdlog::debug("PlayChannel(back): {}", sdl::mix::GetError());
            }
        }
    }

    void shell::key_up(sdl::Keysym key)
    {
        spdlog::trace("Key up: {}", key.sym);
        const auto scan=static_cast<unsigned>(std::to_underlying(key.scancode));if(scan<held_scancodes.size())held_scancodes[scan]=false;
        dispatch(event{
            action::none,
            events::key_up{static_cast<unsigned int>(std::to_underlying(key.scancode))}
        });
    }
    void shell::key_down(sdl::Keysym key) {
        spdlog::trace("Key down: symbol={} scancode={}",key.sym,std::to_underlying(key.scancode));
        const auto scan=static_cast<unsigned>(std::to_underlying(key.scancode));
        const bool held=scan<held_scancodes.size()&&held_scancodes[scan];
        if(scan<held_scancodes.size())held_scancodes[scan]=true;
        for(std::size_t i=0;i<input_bindings::actions.size();++i) if(scan==static_cast<unsigned>(input_bindings::read(config::CONFIG.preferences,false,i))) {
            if(held&&i>=4)return;
            dispatch(event{static_cast<action>(i+1),events::key_down{scan}});return;
        }
        dispatch(event{action::none,events::key_down{scan}});
    }

    void shell::add_controller(sdl::GameController* controller)
    {
        if(config::CONFIG.controllerType == "auto") {
            reload_button_icons();
        }
    }
    void shell::remove_controller(sdl::GameController* controller)
    {
        button_timing.clear();
        if(config::CONFIG.controllerType == "auto") {
            reload_button_icons();
        }
    }
    void shell::button_down(sdl::GameController* controller,sdl::GameControllerButton button) {
        const auto code=static_cast<unsigned>(button);
        const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        for(std::size_t i=0;i<input_bindings::actions.size();++i)if(static_cast<int>(button)==input_bindings::read(config::CONFIG.preferences,true,i)) {
            if(button_timing.press(code,i<4,seconds))dispatch(event{static_cast<action>(i+1),events::controller_button_down{to_logical_button(button)}});return;
        }
        if(button_timing.press(code,false,seconds))dispatch(event{action::none,events::controller_button_down{to_logical_button(button)}});
    }

    void shell::button_up(sdl::GameController* controller, sdl::GameControllerButton button)
    {
        spdlog::trace("Button up: {}", fmt::underlying(button));
        button_timing.release(static_cast<unsigned>(button));
        dispatch(event{action::none, events::controller_button_up{to_logical_button(button)}});
    }
    void shell::axis_motion(sdl::GameController* controller, sdl::GameControllerAxis axis, int16_t value)
    {
        spdlog::trace("Axis motion: {} {}", fmt::underlying(axis), value);

        unsigned int stick_index = 0;
        float v = static_cast<float>(value) / std::numeric_limits<int16_t>::max();
        switch(axis) {
            case sdl::GameControllerAxisValues::LEFTX:
                stick_index = 0;
                controller_axis_position[0].x = v;
                break;
            case sdl::GameControllerAxisValues::LEFTY:
                stick_index = 0;
                controller_axis_position[0].y = v;
                break;
            case sdl::GameControllerAxisValues::RIGHTX:
                stick_index = 1;
                controller_axis_position[1].x = v;
                break;
            case sdl::GameControllerAxisValues::RIGHTY:
                stick_index = 1;
                controller_axis_position[1].y = v;
                break;
            default:
                break;
        }

        const auto default_dispatch = [&]() {
            dispatch<events::joystick_axis>(action::none, stick_index,
                controller_axis_position[stick_index].x,
                controller_axis_position[stick_index].y);
        };

        if(!config::CONFIG.controllerAnalogStick) {
            default_dispatch();
            return;
        }

        if(axis == sdl::GameControllerAxisValues::LEFTX || axis == sdl::GameControllerAxisValues::LEFTY) {
            unsigned int index = axis == sdl::GameControllerAxisValues::LEFTX ? 0 : 1;
            if(std::abs(value) < controller_axis_input_threshold) {
                last_controller_axis_input[index] = std::nullopt;
                last_controller_axis_input_time[index] = std::chrono::steady_clock::now();
                default_dispatch();
                return;
            }
            action dir = axis == sdl::GameControllerAxisValues::LEFTX  ? (value > 0 ? action::right : action::left)
                : (value > 0 ? action::down : action::up);
            if(last_controller_axis_input[index] && std::get<1>(*last_controller_axis_input[index]) == dir) {
                default_dispatch();
                return;
            }
            dispatch<events::joystick_axis>(dir, index,
                controller_axis_position[index].x,
                controller_axis_position[index].y);
            last_controller_axis_input[index] = std::make_tuple(controller, dir);
            last_controller_axis_input_time[index] = std::chrono::steady_clock::now();
        } else {
            default_dispatch();
        }
    }
}
