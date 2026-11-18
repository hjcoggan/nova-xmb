/* XMBShell, a console-like desktop shell
 * Copyright (C) 2025 - JCM
 *
 * This file (or substantial portions of it) is derived from XMBShell:
 *   https://github.com/JnCrMx/xmbshell
 *
 * Modified by Syndromatic Ltd for OpenXMB.
 * Portions Copyright (C) 2025 Syndromatic Ltd, Kavish Krishnakumar.
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
#include "input_timing.hpp"
#include "openxmb/xmb/pause_backdrop.hpp"
#include "openxmb/xmb/child_process.hpp"
#include <future>
#include <optional>
#include "openxmb/xmb/glass.hpp"
#include "../localization/shell_locale.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>
#include <map>
namespace openxmb::themes { class ThemeLibrary; struct AppliedTheme; }

#include <glm/vec2.hpp>
#include "openxmb/xmb/boot_timeline.hpp"

export module openxmb.app:main;

import openxmb.render;
import openxmb.utils;
import openxmb.xmb.captured_wave_renderer;
import openxmb.xmb.monthly_background_renderer;
import openxmb.xmb.status_bar_renderer;
import auroreengine;
import glm;
import sdl2;
import vulkan_hpp;

import :component;
import :choice_overlay;
import :launch_overlay;
import :main_menu;
import :message_overlay;
import :pause_overlay;
import :progress_overlay;
import :startup_overlay;
import :xmb_presentation;

namespace app
{
    export using clipboard = std::variant<
        std::string, std::function<std::string()>,
        std::function<bool(std::filesystem::path)>
    >;

    enum class transition_direction {
        in, out
    };

    using namespace auroreengine;
    export class shell : public phase, public input::keyboard_handler, public input::controller_handler
    {
        public:
            shell(window* window);
            ~shell();
            void preload() override;
            void prepare(std::vector<vk::Image> swapchainImages, std::vector<vk::ImageView> swapchainViews) override;
            void render(int frame, vk::Semaphore imageAvailable, vk::Semaphore renderFinished, vk::Fence fence) override;
            void tick();
            openxmb::xmb::BootSample startup_sample();
            void dismiss_startup_warning(const event& input);

            void key_up(sdl::Keysym key) override;
            void key_down(sdl::Keysym key) override;

            void add_controller(sdl::GameController* controller) override;
            void remove_controller(sdl::GameController* controller) override;
            void button_down(sdl::GameController* controller, sdl::GameControllerButton button) override;
            void button_up(sdl::GameController* controller, sdl::GameControllerButton button) override;
            void axis_motion(sdl::GameController* controller, sdl::GameControllerAxis axis, int16_t value) override;

            void reload_background();
            void reload_fonts();
            void set_font_file(const std::string& path);
            void reset_settings();
            void refresh_desktop_preferences();
            void reload_button_icons();
            void reload_theme_sounds();
            void reload_language();
            void reload_menu();
            void open_panel(const std::string& name);
            void switch_profile(const std::filesystem::path& path);
            void apply_theme(const openxmb::themes::AppliedTheme& theme);
            std::shared_ptr<openxmb::themes::ThemeLibrary> theme_library();
            void queue_panel(std::string name) { pending_panel = std::move(name); }

            void dispatch(const event& event);
            template<typename T, typename... Args>
            void dispatch(action action, Args&&... args) {
                dispatch(event{action, T{std::forward<Args>(args)...}});
            }
            void dispatch(action action) {
                dispatch(event{action, std::monostate{}});
            }
            void handle(result result);

            std::string get_controller_type() const;
            const texture* get_button_icon(action value) const { return buttonTextures[std::to_underlying(value)].get(); }
            std::string get_button_label(action value) const { return button_labels[std::to_underlying(value)]; }
            // The console's one hint bar: a glass capsule centred at the
            // bottom of every screen that takes input, in the overlay canvas
            // so it sits in the same place at every scale. Hints are never
            // drawn inside cards.
            // Screens ask for their hints; the shell draws one bar per frame,
            // after everything else, with the topmost screen's hints. A
            // screen that is fading out does not replace a live one's.
            void render_hint_bar(gui_renderer& renderer, std::ranges::range auto buttons) {
                if(hint_bar_locked&&hint_bar_requested) return;
                hint_bar_buttons.clear();
                for(const auto& [a,text]:buttons) hint_bar_buttons.emplace_back(a,std::string(text));
                hint_bar_colour=renderer.current_color();
                hint_bar_requested=true;
            }
            std::vector<std::pair<action,std::string>> hint_bar_buttons;
            glm::vec4 hint_bar_colour{1.0F};
            bool hint_bar_requested=false,hint_bar_locked=false;
            void flush_hint_bar(gui_renderer& renderer) {
                if(!hint_bar_requested) return;
                hint_bar_requested=false;hint_bar_locked=false;
                const auto previous_scale=renderer.text_scale;
                renderer.text_scale=1.0F;
                renderer.push_color_direct(hint_bar_colour);
                draw_hint_bar(renderer,hint_bar_buttons);
                renderer.pop_color();
                renderer.text_scale=previous_scale;
            }
            void draw_hint_bar(gui_renderer& renderer, const std::vector<std::pair<action,std::string>>& buttons) {
                namespace glass=openxmb::xmb::glass;
                const overlay_canvas c(renderer);
                const float fw=static_cast<float>(renderer.frame_size.width),fh=static_cast<float>(renderer.frame_size.height);
                const float s=c.scale;
                const float font=24.0F*s,key_font=17.0F*s,cap=32.0F*s,gap=10.0F*s,spacing=40.0F*s;
                const auto label=[&](action a){return button_labels[std::to_underlying(a)];};
                const auto glyph_width=[&](action a){const auto& l=label(a);return l.empty()?cap:std::max(cap,renderer.text_width_pixels(l,key_font)+12*s);};
                float total=0;
                for(const auto& [a,text]:buttons) if(a!=action::none)
                    total+=glyph_width(a)+gap+renderer.text_width_pixels(::openxmb::localization::shell_locale::translate(text),font)+spacing;
                if(total<=0) return;
                total-=spacing;
                const float bar_h=54.0F*s,pad_x=24.0F*s;
                const float bar_w=total+2*pad_x;
                const float bar_y=c.oy+(c.visible_bottom()-28.0F)*s-bar_h;
                const float bar_x=fw*0.5F-bar_w*0.5F;
                const float cy=bar_y+bar_h*0.5F;
                c.glass((bar_x-c.ox)/s,(bar_y-c.oy)/s,bar_w/s,bar_h/s,glass::variant::regular,bar_h/s*0.5F);
                const auto metrics=renderer.text_line_metrics_pixels(font),key_metrics=renderer.text_line_metrics_pixels(key_font);
                const auto ring=[&](float px,float py,float pw,float ph) {
                    // A white ring with no fill: a lit glass capsule whose
                    // rim is white, so the bar shows through the key cap.
                    renderer.draw_glass_panel({px/fw,py/fh},{pw/fw,ph/fh},{1,1,1,0.85F},
                        xmb_canvas::panel_params(glass::variant::focus,pw,ph,ph*0.5F,1));
                };
                float current=bar_x+pad_x;
                for(const auto& [a,text]:buttons) {
                    if(a==action::none) continue;
                    const auto i=std::to_underlying(a);const auto width=glyph_width(a);
                    if(!button_labels[i].empty()) {
                        ring(current,cy-cap*0.5F,width,cap);
                        renderer.draw_text_pixels(button_labels[i],current+width*0.5F,cy+(key_metrics.x-key_metrics.y)*0.5F,key_font,{1,1,1,1},0.7F,0,true);
                    } else if(auto* icon=buttonTextures[i].get(); icon!=nullptr&&icon->loaded) {
                        float draw_w=cap,draw_h=cap;
                        if(icon->width>0&&icon->height>0) {
                            const float fit=std::min(cap/static_cast<float>(icon->width),cap/static_cast<float>(icon->height));
                            draw_w=static_cast<float>(icon->width)*fit;draw_h=static_cast<float>(icon->height)*fit;
                        }
                        renderer.draw_image(*icon,(current+(width-draw_w)*0.5F)/fw,cy/fh-draw_h/(2*fh),draw_w/fh,draw_h/fh);
                    }
                    const auto translated=::openxmb::localization::shell_locale::translate(text);
                    renderer.draw_text_pixels(translated,current+width+gap,cy+(metrics.x-metrics.y)*0.5F,font,{1,1,1,glass::text_primary},0.7F,0,false);
                    current+=width+gap+renderer.text_width_pixels(translated,font)+spacing;
                }
            }
            // Free-standing hints for the desktop programs (src/programs);
            // console screens use render_hint_bar.
            void render_controller_buttons(gui_renderer& renderer, float x, float y, std::ranges::range auto buttons) {
                const float fw=static_cast<float>(renderer.frame_size.width),fh=static_cast<float>(renderer.frame_size.height);
                const float ui=static_cast<float>(config::CONFIG.ui_scale());
                const float scale=(fh/1080.0F)*ui,height=28.0F*scale,gap=10.0F*scale,spacing=52.0F*scale;
                const float font=24.0F*scale,key_font=17.0F*scale;
                // Button prompts are round: a circle for a face button, a pill
                // for a longer label such as Start.
                const float cap=32.0F*scale;
                const auto glyph_width=[&](action a){const auto& label=button_labels[std::to_underlying(a)];return label.empty()?cap:std::max(cap,renderer.text_width_pixels(label,key_font)+12*scale);};
                float total=0;for(const auto& [a,text]:buttons)if(a!=action::none)total+=glyph_width(a)+gap+renderer.text_width_pixels(::openxmb::localization::shell_locale::translate(text),font)+spacing;
                if(total>0)total-=spacing;float current=x*fw-total*0.5F;
                const auto metrics=renderer.text_line_metrics_pixels(font),key_metrics=renderer.text_line_metrics_pixels(key_font);
                const auto rounded=[&](float px,float py,float pw,float ph,glm::vec4 colour,float radius) {
                    auroreengine::simple_params parameters{};
                    parameters.aspect_ratio=pw/ph;parameters.border_radius.fill(std::min(radius/ph,0.5F));
                    renderer.draw_rect({px/fw,py/fh},{pw/fw,ph/fh},colour,parameters);
                };
                for(const auto& [a,text]:buttons) {
                    if(a==action::none)continue;const auto i=std::to_underlying(a);const auto width=glyph_width(a);
                    if(!button_labels[i].empty()) {
                        // The key cap: a pale ring around a dark face.
                        rounded(current,y*fh-cap*0.5F,width,cap,{1,1,1,0.85F},cap*0.5F);
                        rounded(current+1.8F*scale,y*fh-cap*0.5F+1.8F*scale,width-3.6F*scale,cap-3.6F*scale,{0.012F,0.02F,0.05F,0.92F},cap*0.5F-1.8F*scale);
                        renderer.draw_text_pixels(button_labels[i],current+width*0.5F,y*fh+(key_metrics.x-key_metrics.y)*0.5F,key_font,{1,1,1,1},0.7F,0,true);
                    } else if(auto* icon=buttonTextures[i].get(); icon!=nullptr&&icon->loaded) {
                        float draw_w=cap,draw_h=cap;
                        if(icon->width>0&&icon->height>0) {
                            const float fit=std::min(cap/static_cast<float>(icon->width),cap/static_cast<float>(icon->height));
                            draw_w=static_cast<float>(icon->width)*fit;
                            draw_h=static_cast<float>(icon->height)*fit;
                        }
                        renderer.draw_image(*icon,(current+(width-draw_w)*0.5F)/fw,y-draw_h/(2*fh),draw_w/fh,draw_h/fh);
                    }
                    renderer.draw_text_pixels(::openxmb::localization::shell_locale::translate(text),current+width+gap,y*fh+(metrics.x-metrics.y)*0.5F,font,{1,1,1,1},0.7F,0,false);
                    current+=width+gap+renderer.text_width_pixels(::openxmb::localization::shell_locale::translate(text),font)+spacing;
                }
            }

            auroreengine::window* get_window() const { return this->win; }

            void set_ingame_mode(bool ingame_mode) { this->ingame_mode = ingame_mode; }
            bool get_ingame_mode() const { return ingame_mode; }

            void set_background_only(bool background_only) {
                this->background_only = background_only;
                if(!this->background_only && !fixed_components_loaded) {
                    preload_fixed_components();
                }
            }
            bool get_background_only() const { return background_only; }

            void set_blur_background(bool blur) {
                if (blur == blur_background) return;
                blur_background = blur;
                last_blur_background_change = std::chrono::steady_clock::now();
            }
            bool get_blur_background() const { return blur_background; }
            void set_startup_background_blur_px(float blur_px) {
                startup_background_blur_px = std::max(0.0f, blur_px);
            }
            bool has_choice_overlay() const {
                for(const auto& overlay : overlays) {
                    if(dynamic_cast<app::choice_overlay*>(overlay.get()) != nullptr) {
                        return true;
                    }
                }
                return old_overlay &&
                    dynamic_cast<app::choice_overlay*>(old_overlay.get()) != nullptr;
            }
            bool has_launch_overlay() const {
                for(const auto& overlay : overlays) {
                    if(dynamic_cast<app::launch_overlay*>(overlay.get()) != nullptr) {
                        return true;
                    }
                }
                return false;
            }
            // Shows "Starting <title>" until the game has a window of its own.
            // `artwork` is the game's banner or icon file, if it has one.
            void begin_launch(const std::string& title, const std::string& artwork = {});
            void end_launch();
            // Lets the player reorder the selected library item or move it to
            // another column.
            void begin_library_move() { menu.begin_move(); }

            bool has_pause_overlay() const {
                for(const auto& overlay : overlays) {
                    if(dynamic_cast<app::pause_overlay*>(overlay.get()) != nullptr) {
                        return true;
                    }
                }
                return false;
            }

            app::component* push_overlay(std::unique_ptr<app::component>&& component) {
                auto ptr = component.get();
                overlays.emplace_back(std::move(component));

                if(ptr->do_fade_in()) {
                    overlay_fade_direction = transition_direction::in;
                    overlay_fade_time = std::chrono::steady_clock::now();
                } else {
                    overlay_fade_time = std::chrono::steady_clock::now() - overlay_transition_duration;
                }

                // Auto-enable background blur for modal message/pause overlays + play question sound
                if(dynamic_cast<app::message_overlay*>(ptr) != nullptr ||
                   dynamic_cast<app::pause_overlay*>(ptr) != nullptr) {
                    set_blur_background(true);
                    if(question_sound && sdl::mix::PlayChannel(-1, question_sound.get(), 0) == -1) {
                        spdlog::debug("PlayChannel(question): {}", sdl::mix::GetError());
                    }
                }
                return ptr;
            }
            template<typename T, typename... Args>
            T* emplace_overlay(Args&&... args) {
                auto p = std::make_unique<T>(std::forward<Args>(args)...);
                auto ptr = p.get();
                overlays.emplace_back(std::move(p));

                if(ptr->do_fade_in()) {
                    overlay_fade_direction = transition_direction::in;
                    overlay_fade_time = std::chrono::steady_clock::now();
                } else {
                    overlay_fade_time = std::chrono::steady_clock::now() - overlay_transition_duration;
                }

                if(dynamic_cast<app::message_overlay*>(ptr) != nullptr ||
                   dynamic_cast<app::pause_overlay*>(ptr) != nullptr) {
                    set_blur_background(true);
                    if(question_sound && sdl::mix::PlayChannel(-1, question_sound.get(), 0) == -1) {
                        spdlog::debug("PlayChannel(question): {}", sdl::mix::GetError());
                    }
                }
                return ptr;
            }
            void remove_overlay(unsigned int index) {
                if(index >= overlays.size()) return;
                if(index == overlays.size()-1 && overlays[index]->do_fade_out()) {
                    overlay_fade_direction = transition_direction::out;
                    overlay_fade_time = std::chrono::steady_clock::now();
                    if(old_overlay) win->waitIdle();
                    old_overlay = std::move(overlays[index]);
                } else {
                    overlay_fade_time = std::chrono::steady_clock::now() - overlay_transition_duration;
                    win->waitIdle();
                }
                overlays.erase(overlays.begin()+index);

                // Disable blur when no more message/pause overlays are present (including fading old_overlay)
                auto has_msg = false;
                for(auto& ov : overlays) {
                    if(dynamic_cast<app::message_overlay*>(ov.get()) != nullptr ||
                       dynamic_cast<app::pause_overlay*>(ov.get()) != nullptr) { has_msg = true; break; }
                }
                if(!has_msg) {
                    if(old_overlay &&
                       (dynamic_cast<app::message_overlay*>(old_overlay.get()) != nullptr ||
                        dynamic_cast<app::pause_overlay*>(old_overlay.get()) != nullptr) &&
                       overlay_fade_direction == transition_direction::out) {
                        // keep blur until fade-out completes
                    } else {
                        set_blur_background(false);
                    }
                }
            }

            void set_clipboard(clipboard&& clipboard) {
                this->clipboard = std::move(clipboard);
            }
            const std::optional<clipboard>& get_clipboard() const { return clipboard; }
        private:
            friend class blur_layer;

            // Monotonic timing for input and fades
            using time_point = std::chrono::time_point<std::chrono::steady_clock>;

            std::unique_ptr<font_renderer> font_render;
            std::unique_ptr<image_renderer> image_render;
            std::unique_ptr<simple_renderer> simple_render;
            std::unique_ptr<render::wave_renderer> wave_render;
            std::unique_ptr<render::procedural_wave_renderer> procedural_wave_render;
            std::unique_ptr<openxmb::xmb::MonthlyBackgroundRenderer> monthly_background_render;
            openxmb::xmb::StatusBarRenderer status_bar_render;
            bool monthly_background_failed{};

            vk::UniqueRenderPass backgroundRenderPass, shellRenderPass;
            vk::SampleCountFlagBits renderPassSamples{};

            // Per-frame resolve target for background (offscreen, avoids reusing swapchain mid-frame)
            std::vector<std::unique_ptr<texture>> backgroundResolve;
            std::vector<vk::UniqueImageView> backgroundAttachmentViews,backgroundMsaaViews;
            std::vector<std::unique_ptr<texture>> backgroundDepth;
            std::vector<vk::UniqueFramebuffer> backgroundFramebuffers;

            vk::UniqueDescriptorSetLayout blurDescriptorSetLayout;
            vk::UniquePipelineLayout blurPipelineLayout;
            vk::UniquePipeline blurPipeline;
            vk::UniquePipeline downsamplePipeline;
            vk::UniquePipeline upsamplePipeline;

            struct blur_frame_resources {
                std::unique_ptr<texture> fullSrc;
                std::unique_ptr<texture> fullDst;
                std::unique_ptr<texture> halfSrc;
                std::unique_ptr<texture> halfDst;
                std::unique_ptr<texture> quarterSrc;
                std::unique_ptr<texture> quarterDst;

                vk::DescriptorSet downsampleSet{};
                vk::DescriptorSet halfBlurSet{};
                vk::DescriptorSet upsampleSet{};
                vk::DescriptorSet downsample2Set{};
                vk::DescriptorSet quarterBlurSet{};
                vk::DescriptorSet upsample2Set{};
            };

            // What glass panels sample: a copy of the scene behind the
            // interface with a mip chain, rebuilt each frame (see
            // build_glass_backdrop). Empty when the format cannot be blitted.
            static constexpr std::uint32_t glass_backdrop_levels = 6;
            struct glass_backdrop_resources {
                vma::UniqueImage image;
                vma::UniqueAllocation allocation;
                vk::UniqueImageView view;
            };
            std::vector<glass_backdrop_resources> glassBackdrops;
            void build_glass_backdrop(vk::CommandBuffer commandBuffer, int frame);
            std::vector<vk::Image> swapchainImages;
            std::vector<std::unique_ptr<texture>> renderImages;
            std::vector<blur_frame_resources> blurFrames;
            std::vector<vk::UniqueFramebuffer> framebuffers;
            vk::UniqueDescriptorPool blurDescriptorPool;
            std::vector<vk::DescriptorSet> blurDescriptorSets;
            // Extra descriptor pool for downsample/upsample chain
            vk::UniqueDescriptorPool blurExtraDescriptorPool;

            std::unique_ptr<texture> backgroundTexture;
            // Keep JSON-heavy theme state out of the exported shell BMI.
            struct theme_state;
            std::unique_ptr<theme_state> theme;
            std::string pending_panel;
            main_menu menu{this};
            std::array<std::unique_ptr<texture>, std::to_underlying(action::_length)> buttonTextures;
            std::array<std::string,std::to_underlying(action::_length)> button_labels;
            std::unique_ptr<texture> cursorTexture;
            std::array<std::unique_ptr<texture>, 15> statusIconTextures;
            std::unique_ptr<texture> iconGlassAmbientTexture;
            std::unique_ptr<texture> iconGlassEnvironmentTexture;

            sdl::mix::unique_chunk ok_sound;
            sdl::mix::unique_chunk question_sound;
            sdl::mix::unique_chunk confirm_sound;
            sdl::mix::unique_chunk cancel_sound;
            sdl::mix::unique_chunk back_sound;

            std::chrono::steady_clock::time_point cursor_active_at{};
            glm::vec2 cursor_position{0.5f, 0.5f};
            glm::vec2 cursor_joystick_delta{0.0f, 0.0f};
            glm::ivec2 last_mouse_position{0, 0};
            std::uint32_t last_mouse_buttons = 0;
            bool mouse_state_initialized = false;
            void poll_mouse();
            void tick_cursor();
            bool handle_cursor(const event& event);

            bool fixed_components_loaded = false;
            void create_render_passes();
            void preload_fixed_components();

            void render_gui(gui_renderer& renderer);
            void ensure_blur(int frame);

            // input handling
            constexpr static int controller_axis_input_threshold = 10000;
            std::array<glm::vec2, 2> controller_axis_position;
            std::array<time_point, 2> last_controller_axis_input_time;
            std::array<std::optional<std::tuple<sdl::GameController*, action>>, 2> last_controller_axis_input;
            constexpr static auto controller_axis_input_duration = std::chrono::milliseconds(200);

            time_point last_controller_button_input_time;
            std::optional<std::tuple<sdl::GameController*, sdl::GameControllerButton>> last_controller_button_input;
            constexpr static auto controller_button_input_duration = std::chrono::milliseconds(200);

            // state
            std::array<bool,513> held_scancodes{};
            ButtonTiming button_timing;
            time_point last_activity=std::chrono::steady_clock::now();
            bool background_only = false;
            bool ingame_mode = false;
            bool launch_menu_had_focus = false;
            // True from the moment a launched game takes the screen until its
            // process is gone. The menu then draws nothing and its surface is
            // transparent, so when Home floats it over the frozen game the first
            // thing on screen is the game, not a stale frame of the menu.
            bool game_has_screen = false;
            bool in_game_refresh_active = false;
            std::chrono::steady_clock::time_point game_focus_returned{};
            std::chrono::steady_clock::time_point launch_started{};

            bool blur_background = false;
            time_point last_blur_background_change;
            float startup_background_blur_px = 0.0f;

            std::vector<std::unique_ptr<app::component>> overlays;

            transition_direction overlay_fade_direction{};
            time_point overlay_fade_time;
            std::unique_ptr<app::component> old_overlay;

            void poll_pause_ipc();
            int pause_sock_fd = -1;
            // The still of the paused game that the pause panel's glass sits
            // on (see openxmb/xmb/pause_backdrop.hpp). PAUSE starts a capture;
            // the panel opens when it is ready, failed, or after 250 ms.
            openxmb::xmb::pause_backdrop pause_backdrop;
            std::optional<std::string> pending_pause_game;
            std::chrono::steady_clock::time_point pause_requested_at;
            openxmb::xmb::child_process pause_capture;
            std::unique_ptr<texture> pause_still;
            std::future<void> pause_still_loading;
            bool pause_still_loading_started = false;
            void start_pause_capture();
            void poll_pause_capture();
            void drop_pause_still();
            [[nodiscard]] bool pause_still_drawn() const { return pause_backdrop.draw_still() && pause_still && pause_still->loaded; }
            std::string pause_sock_path;

            std::optional<clipboard> clipboard;

            // transition duration constants
            constexpr static auto blur_background_transition_duration = std::chrono::milliseconds(500);
            // Slightly longer fade, PS3-like but still snappy
            constexpr static auto overlay_transition_duration = std::chrono::milliseconds(400);

            // Monotonic start time for shader time uniforms (avoid epoch-based float precision loss)
            time_point shader_time_zero { std::chrono::steady_clock::now() };
            openxmb::xmb::BootTimeline startup_timeline;
            std::optional<events::logical_controller_button> startup_dismiss_button;
            std::optional<unsigned int> startup_dismiss_key;
            double startup_elapsed_seconds() const;
    };
}
