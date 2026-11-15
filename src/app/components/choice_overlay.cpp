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

#include "dialog_text_layout.hpp"
#include "../../localization/shell_locale.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

module openxmb.app;
import :choice_overlay;
import :xmb_presentation;
import openxmb.config;

import auroreengine;
import glm;
import openxmb.utils;
import vulkan_hpp;
import vma;

namespace app {

choice_overlay::choice_overlay(std::vector<std::string> choices, unsigned int selection_index,
    std::function<void(unsigned int)> confirm_callback,
    std::function<void()> cancel_callback,
    std::function<void(unsigned int)> preview_callback)
    : choices{std::move(choices)}, confirm_callback{std::move(confirm_callback)},
      cancel_callback{std::move(cancel_callback)},
      preview_callback{std::move(preview_callback)}
{
    if(this->choices.empty()) {
        this->selection_index = 0;
        this->last_selection_index = 0;
    } else {
        this->selection_index = std::min<unsigned int>(selection_index, static_cast<unsigned int>(this->choices.size() - 1));
        this->last_selection_index = this->selection_index;
    }
}

result choice_overlay::on_action(action action) {
    switch(action) {
        case action::cancel:
            if(cancel_callback) {
                cancel_callback();
            }
            return result::close | result::back_sound;
        case action::ok:
            if(choices.empty()) {
                return result::success | result::error_rumble;
            }
            if(confirm_callback) {
                confirm_callback(selection_index);
            }
            return result::close | result::confirm_sound;
        case action::left:
            text_scroll_line=text_scroll_line>selected_page_lines?text_scroll_line-selected_page_lines:0;
            return result::success;
        case action::right:
            if(selected_total_lines>selected_page_lines)text_scroll_line=std::min(text_scroll_line+selected_page_lines,selected_total_lines-selected_page_lines);
            return result::success;
        case action::up:
            return select_relative(action::up) ? result::success | result::ok_sound : result::success | result::error_rumble;
        case action::down:
            return select_relative(action::down) ? result::success | result::ok_sound : result::success | result::error_rumble;
        default:
            return result::success | result::error_rumble;
    }
}

bool choice_overlay::select_relative(action dir) {
    if(choices.empty()) {
        return false;
    }

    if(dir == action::up) {
        if(selection_index <= 0) {
            return false;
        }
        last_selection_index = selection_index;
        last_selection_time = std::chrono::system_clock::now();
        selection_index = (selection_index + choices.size() - 1) % choices.size();
    } else if(dir == action::down) {
        if(selection_index >= choices.size() - 1) {
            return false;
        }
        last_selection_index = selection_index;
        last_selection_time = std::chrono::system_clock::now();
        selection_index = (selection_index + 1) % choices.size();
    } else {
        return false;
    }
    text_scroll_line=0;
    if(preview_callback) {
        preview_callback(selection_index);
    }
    return true;
}

void choice_overlay::render(auroreengine::gui_renderer& renderer, class shell* xmb) {
    (void)xmb;

    // A glass sheet on the right, in place of the firmware's lavender wash:
    // the menu stays visible beside it, slightly dimmed, and the list sits
    // on smoked glass so it reads over any wallpaper.
    const auto open_elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - opened_time);
    const auto open_linear = std::clamp(
        open_elapsed / std::chrono::duration<double>(open_duration), 0.0, 1.0);
    const auto open_alpha =
        static_cast<float>(open_linear * open_linear * (3.0 - 2.0 * open_linear));
    overlay_canvas canvas(renderer);
    constexpr float panel_right = 1860.0F / 1920.0F;
    const float panel_edge_authored = (panel_right * static_cast<float>(renderer.frame_size.width) - canvas.ox) / canvas.scale;
    const float x = panel_edge_authored - 357.0F + (1.0F - open_alpha) * 36.0F;
    canvas.rect(canvas.visible_left() - 2, canvas.visible_top() - 2,
        canvas.visible_right() - canvas.visible_left() + 4, canvas.visible_bottom() - canvas.visible_top() + 4,
        {0.0F, 0.01F, 0.04F, 0.30F * open_alpha});

    auto now = std::chrono::system_clock::now();
    double selected = selection_index;
    auto time_since_transition = std::chrono::duration<double>(now - last_selection_time);
    if(time_since_transition < transition_duration) {
        double p = std::clamp(time_since_transition / transition_duration, 0.0, 1.0);
        p = p * p * (3.0 - 2.0 * p);
        selected = last_selection_index + (selected - last_selection_index) * p;
    }

    // x already computed above with overlay_canvas so the panel and text share bounds.
    const float ui=config::CONFIG.textScale;
    const float pixels=28*canvas.scale*ui;
    const auto font=config::CONFIG.fontPath.string();
    const auto metrics=renderer.text_line_metrics_pixels(pixels)/canvas.scale;
    const float line_height=metrics.x+metrics.y,pitch=line_height+8*ui;
    constexpr float available_width=337;
    if(wrapped_pixels!=pixels||wrapped_font!=font) {
        wrapped_pixels=pixels;wrapped_font=font;
        wrapped_choices.clear();row_heights.clear();
        for(const auto& entry:choices) {
            auto lines=dialog_text::wrap(entry,available_width*canvas.scale,[&](auto v){return renderer.text_width_pixels(v,pixels);});
            row_heights.push_back(line_height+(lines.size()-1)*pitch+12*ui);
            wrapped_choices.push_back(std::move(lines));
        }
    }
    float top=518-metrics.x;
    {
        // Size the sheet to its list: a title if there is one, then the rows,
        // never taller than the screen.
        float rows_height=0;
        for(const auto row:row_heights)rows_height+=row;
        const float sheet_top=(top_action.empty()?top:top_action_y*1080-metrics.x)-34.0F;
        const float sheet_bottom=std::min(canvas.visible_bottom()-26.0F,top+rows_height+26.0F);
        canvas.glass(x-36.0F,sheet_top,357.0F+58.0F,sheet_bottom-sheet_top,34.0F,open_alpha);
    }
    if(!top_action.empty()) {
        auto labels=dialog_text::wrap(top_action,available_width*canvas.scale,[&](auto v){return renderer.text_width_pixels(v,pixels);});
        if(labels.size()>2){labels.resize(2);labels.back()=dialog_text::ellipsize(labels.back()+"…",available_width*canvas.scale,[&](auto v){return renderer.text_width_pixels(v,pixels);});}
        const float baseline=top_action_y*1080;
        for(std::size_t i=0;i<labels.size();++i)canvas.text(labels[i],x,baseline+i*pitch,24,{0.80F,0.88F,1.0F,0.85F*open_alpha});
        top=std::max(top,baseline+(labels.size()-1)*pitch+metrics.y+24*ui);
    }
    const float bottom=std::min(1000.0F,canvas.visible_bottom()-44.0F),viewport=std::max(line_height,bottom-top);
    const auto window=dialog_text::choice_window(row_heights,selection_index,viewport,first_visible);
    first_visible=window.first;
    selected_page_lines=dialog_text::line_capacity(viewport,pitch,line_height);
    selected_total_lines=wrapped_choices.empty()?0:wrapped_choices[selection_index].size();
    text_scroll_line=std::min(text_scroll_line,selected_total_lines>selected_page_lines?selected_total_lines-selected_page_lines:0);
    renderer.set_clip((canvas.ox+(x-24.0F)*canvas.scale)/renderer.frame_size.width,
        (canvas.oy+(top-12.0F)*canvas.scale)/renderer.frame_size.height,
        (357.0F+34.0F)*canvas.scale/renderer.frame_size.width,(viewport+24.0F)*canvas.scale/renderer.frame_size.height);
    float y=top+metrics.x;
    const float swatch_inset=swatches.empty()?0.0F:44.0F;
    for(std::size_t i=window.first;i<window.last;++i) {
        const double focus=1-std::clamp(std::abs(static_cast<double>(i)-selected),0.0,1.0);
        const float eased=static_cast<float>(focus*focus*(3-2*focus));
        const bool focused=i==selection_index;
        const float row_height=row_heights[i]-12*ui;
        // The lit capsule follows the selection from row to row.
        if(eased>0.01F)
            canvas.glass_focus(x-18.0F,y-metrics.x-9.0F,available_width+30.0F,row_height+18.0F,eased*open_alpha,26.0F);
        if(i<swatches.size()) {
            canvas.rect(x-2.0F,y-metrics.x+(line_height-28.0F)*0.5F-1.0F,30,30,{1,1,1,0.55F*open_alpha},8);
            canvas.rect(x,y-metrics.x+(line_height-28.0F)*0.5F+1.0F,26,26,{swatches[i],open_alpha},6);
        }
        const std::size_t start=focused?text_scroll_line:0;
        const auto& lines=wrapped_choices[i];
        const auto count=std::min(lines.size()-start,selected_page_lines);
        for(std::size_t line=0;line<count;++line)
            canvas.text(lines[start+line],x+swatch_inset,y+line*pitch,26,{1,1,1,(0.78F+0.22F*eased)*open_alpha});
        y+=row_heights[i];
    }
    renderer.reset_clip();
    // The font has no arrow glyphs; the markers are drawn.
    using direction=xmb_canvas::arrow_direction;
    const float centre=x+available_width*0.5F-12.0F;
    if(window.first)canvas.arrow(centre,top-20.0F,16.0F,direction::up,{1,1,1,0.85F*open_alpha});
    if(window.last<choices.size())canvas.arrow(centre,top+viewport+20.0F,16.0F,direction::down,{1,1,1,0.85F*open_alpha});
    if(selected_total_lines>selected_page_lines) {
        const float y_more=std::min(1064.0F,canvas.visible_bottom()-8.0F);
        canvas.arrow(x+6.0F,y_more-6.0F,12.0F,direction::left,{1,1,1,open_alpha});
        canvas.text("More text",x+20.0F,y_more,16,{1,1,1,open_alpha});
        canvas.arrow(x+112.0F,y_more-6.0F,12.0F,direction::right,{1,1,1,open_alpha});
    }
}

}
