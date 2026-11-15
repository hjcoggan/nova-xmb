module;
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "openxmb/xmb/background.hpp"
#include "openxmb/xmb/glass.hpp"
#include "openxmb/xmb/glass_draw.hpp"
#include "../localization/ui_literal_text.hpp"
export module openxmb.app:xmb_presentation;
import auroreengine;
import glm;
import openxmb.config;
import openxmb.utils;
import vulkan_hpp;
export namespace app {
// One contained design space for every XMB scene. Text uses baseline anchors.
struct xmb_canvas {
    auroreengine::gui_renderer& renderer;
    float scale, ox, oy;
    explicit xmb_canvas(auroreengine::gui_renderer& r):renderer(r) {
        const float w=static_cast<float>(r.frame_size.width),h=static_cast<float>(r.frame_size.height);
        const float base_scale=h/1080.0f;
        const float ui=static_cast<float>(config::CONFIG.ui_scale());
        scale=base_scale*ui;
        const float base_ox=(w-1920.0f*base_scale)*0.5f;
        const float anchor_x=(w/std::max(1.0f,h)<16.0f/9.0f)?(w*0.28f):(base_ox+566.0f*base_scale);
        const float target_y=(ui>1.0f)?(h*0.27f):(289.0f*base_scale);
        ox=anchor_x-566.0f*scale;
        oy=target_y-((ui>1.0f)?279.0f:289.0f)*scale;
    }
    void text(std::string_view text,float x,float baseline,float size=28,glm::vec4 color=glm::vec4(1),float glow=0,bool centered=false) const {
        renderer.draw_text_pixels(text,ox+x*scale,oy+baseline*scale,size*scale*config::CONFIG.textScale,color,0.7f,glow,centered);
    }
    template<class Character, std::size_t N>
        requires std::is_same_v<Character, const char>
    void text(Character (&literal)[N],float x,float baseline,float size=28,glm::vec4 color=glm::vec4(1),float glow=0,bool centered=false) const {
        const auto localized=detail::localize_ui_literal(literal);
        text(std::string_view(localized),x,baseline,size,color,glow,centered);
    }
    void rect(float x,float y,float w,float h,glm::vec4 color,float radius=0) const {
        auroreengine::simple_params parameters{};
        if(radius>0&&h>0) {parameters.aspect_ratio=w/h;parameters.border_radius.fill(std::min(radius/h,0.5f));}
        renderer.draw_rect({(ox+x*scale)/renderer.frame_size.width,(oy+y*scale)/renderer.frame_size.height},
            {w*scale/renderer.frame_size.width,h*scale/renderer.frame_size.height},color,parameters);
    }
    void gradient(float x,float y,float w,float h,glm::vec4 top,glm::vec4 bottom) const {
        using V=auroreengine::simple_renderer::vertex_data;
        const float left=(ox+x*scale)/renderer.frame_size.width,right=(ox+(x+w)*scale)/renderer.frame_size.width;
        const float upper=(oy+y*scale)/renderer.frame_size.height,lower=(oy+(y+h)*scale)/renderer.frame_size.height;
        const std::array<V,4> vertices{V{{left,upper},top,{0,0}},V{{right,upper},top,{1,0}},V{{left,lower},bottom,{0,1}},V{{right,lower},bottom,{1,1}}};
        renderer.draw_quad(vertices);
    }
    // --- Glass -------------------------------------------------------------
    // The console's surfaces are smoked glass: a dark translucent body that
    // keeps white text readable over any wallpaper, a bright rim, a sheen on
    // the upper half and a soft shadow. They are a handful of plain quads.
    //
    // A rectangle whose edges fade out over `feather` design units.
    void soft_rect(float x,float y,float w,float h,glm::vec4 color,float feather) const {
        if(w<=0||h<=0) return;
        auroreengine::simple_params parameters{};
        const float fx=std::min(feather/w,0.5f),fy=std::min(feather/h,0.5f);
        parameters.blur={glm::vec2{0,fx},glm::vec2{0,fx},glm::vec2{0,fy},glm::vec2{0,fy}};
        renderer.draw_rect({(ox+x*scale)/renderer.frame_size.width,(oy+y*scale)/renderer.frame_size.height},
            {w*scale/renderer.frame_size.width,h*scale/renderer.frame_size.height},color,parameters);
    }
    // A vertical gradient with its own corner radii (top pair, bottom pair).
    void rounded_gradient(float x,float y,float w,float h,glm::vec4 top,glm::vec4 bottom,float top_radius,float bottom_radius=0) const {
        if(w<=0||h<=0) return;
        using V=auroreengine::simple_renderer::vertex_data;
        auroreengine::simple_params parameters{};
        parameters.aspect_ratio=w/h;
        const float tr=std::min(top_radius/h,0.5f),br=std::min(bottom_radius/h,0.5f);
        parameters.border_radius={tr,tr,br,br};
        const float left=(ox+x*scale)/renderer.frame_size.width,right=(ox+(x+w)*scale)/renderer.frame_size.width;
        const float upper=(oy+y*scale)/renderer.frame_size.height,lower=(oy+(y+h)*scale)/renderer.frame_size.height;
        const std::array<V,4> vertices{V{{left,upper},top,{0,0}},V{{right,upper},top,{1,0}},V{{left,lower},bottom,{0,1}},V{{right,lower},bottom,{1,1}}};
        renderer.draw_quad(vertices,parameters);
    }
    // A small solid arrowhead: the font has no arrow glyphs.
    enum class arrow_direction { left, right, up, down };
    void arrow(float cx,float cy,float size,arrow_direction direction,glm::vec4 color) const {
        using V=auroreengine::simple_renderer::vertex_data;
        const float h=size*0.5f;
        std::array<glm::vec2,3> points{};
        switch(direction) {
            case arrow_direction::left:  points={glm::vec2{cx-h,cy},glm::vec2{cx+h,cy-h},glm::vec2{cx+h,cy+h}};break;
            case arrow_direction::right: points={glm::vec2{cx+h,cy},glm::vec2{cx-h,cy-h},glm::vec2{cx-h,cy+h}};break;
            case arrow_direction::up:    points={glm::vec2{cx,cy-h},glm::vec2{cx-h,cy+h},glm::vec2{cx+h,cy+h}};break;
            case arrow_direction::down:  points={glm::vec2{cx,cy+h},glm::vec2{cx-h,cy-h},glm::vec2{cx+h,cy-h}};break;
        }
        std::array<V,3> vertices{};
        for(std::size_t i=0;i<3;++i)
            vertices[i]=V{{(ox+points[i].x*scale)/renderer.frame_size.width,(oy+points[i].y*scale)/renderer.frame_size.height},color,{0.5f,0.5f}};
        renderer.draw_generic(vertices);
    }
    // Colours are authored as sRGB bytes; the render target is linear.
    static glm::vec4 srgb(float r,float g,float b,float a=1) {
        const auto channel=[](float v){v/=255.0f;return v<=0.04045f?v/12.92f:std::pow((v+0.055f)/1.055f,2.4f);};
        return {channel(r),channel(g),channel(b),a};
    }
    // The one glass primitive (openxmb/xmb/glass.hpp): `regular` for cards
    // and bars, `clear` for a lens straight on the wallpaper, `focus` for
    // the lit row inside a card. The engine panel samples the backdrop,
    // blurs and bends it, adds the rim and keeps white text legible.
    static auroreengine::glass_panel_params panel_params(openxmb::xmb::glass::variant v,float w_px,float h_px,float radius_px,float alpha) {
        namespace glass=openxmb::xmb::glass;
        const auto m=glass::material_for(v);
        auroreengine::glass_panel_params p{};
        p.size_px={w_px,h_px};
        p.radius_px=glass::clamp_radius(radius_px,w_px,h_px);
        p.alpha=alpha;
        p.refraction_px=m.refraction_px;p.blur_lod=m.blur_lod;p.tint=m.tint;p.dim_strength=m.dim_strength;
        p.samples_backdrop=m.samples_backdrop;
        return p;
    }
    static glm::vec4 rim_colour(openxmb::xmb::glass::variant v) {
        const auto r=openxmb::xmb::glass::rim_for(v);
        return {r[0],r[1],r[2],r[3]};
    }
    void glass(float x,float y,float w,float h,openxmb::xmb::glass::variant v=openxmb::xmb::glass::variant::regular,
               float radius=openxmb::xmb::glass::card_radius,float alpha=1) const {
        openxmb::xmb::glass::draw_glass_px<auroreengine::glass_panel_params,auroreengine::simple_params>(
            renderer,ox+x*scale,oy+y*scale,w*scale,h*scale,v,radius*scale,alpha,scale);
    }
    // --------------------------------------------------------------------
    void image(const auroreengine::texture& t,float x,float y,float w,float h,glm::vec4 color=glm::vec4(1)) const {
        renderer.draw_image(t,(ox+x*scale)/renderer.frame_size.width,(oy+y*scale)/renderer.frame_size.height,
            w*scale/renderer.frame_size.height,h*scale/renderer.frame_size.height,color);
    }
    void glass_image(const auroreengine::texture& t,float x,float y,float w,float h,glm::vec4 color) const {
        renderer.draw_image_glass(t,(ox+x*scale)/renderer.frame_size.width,(oy+y*scale)/renderer.frame_size.height,
            w*scale/renderer.frame_size.height,h*scale/renderer.frame_size.height,color);
    }
    static float pulse() {
        const auto t=std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
        return 0.7f+0.55f*(0.5f+0.5f*std::sin(t*1.848f));
    }
};
// Overlay canvas: dialogs, keyboards and choice panels author in the same
// 1920x1080 space, but unlike the crossbar they are centred, not left-anchored.
// The uniform scale cover-crops the authored canvas, so authored full-screen
// layers cover the whole frame with no letterbox bands; overlays derive the
// authored-coordinate window that is on screen from visible_left()/right().
struct overlay_canvas : xmb_canvas {
    explicit overlay_canvas(auroreengine::gui_renderer& r):xmb_canvas(r) {
        const float w=static_cast<float>(r.frame_size.width),h=static_cast<float>(r.frame_size.height);
        scale=std::min((h/1080.0f)*static_cast<float>(config::CONFIG.ui_scale()),w/1100.0f);
        ox=(w-1920.0f*scale)*0.5f;oy=(h-1080.0f*scale)*0.5f;
    }
    [[nodiscard]] float visible_left() const {return -ox/scale;}
    [[nodiscard]] float visible_right() const {return (static_cast<float>(renderer.frame_size.width)-ox)/scale;}
    [[nodiscard]] float visible_top() const {return -oy/scale;}
    [[nodiscard]] float visible_bottom() const {return (static_cast<float>(renderer.frame_size.height)-oy)/scale;}
};
class media_icons {
    struct item { std::unique_ptr<auroreengine::texture> image,normal;std::future<void> ready,normal_ready; };
    std::map<std::string,item,std::less<>> images;
public:
    ~media_icons() { for(auto& [key,value]:images) {if(value.ready.valid()) value.ready.wait();if(value.normal_ready.valid())value.normal_ready.wait();} }
    void load(auroreengine::resource_loader& loader,std::string name) {
        if(images.contains(name)) return;
        auto& value=images[name];
        value.image=std::make_unique<auroreengine::texture>(loader.getDevice(),loader.getAllocator());
        value.ready=loader.loadTexture(value.image.get(),config::CONFIG.asset_directory/"icons"/(name+".png"));
        const auto normals=config::CONFIG.asset_directory/"normalmaps"/(name+".png");
        if(name.starts_with("theme/")&&std::filesystem::is_regular_file(normals)) {
            value.normal=std::make_unique<auroreengine::texture>(loader.getDevice(),loader.getAllocator(),vk::ImageUsageFlagBits::eSampled,vk::Format::eR8G8B8A8Unorm);
            value.normal_ready=loader.loadTexture(value.normal.get(),normals);
        }
    }
    void box(const xmb_canvas& c,std::string_view name,float x,float y,float width,float height,float alpha=1) const {
        const auto it=images.find(name);if(it!=images.end()&&it->second.image->loaded) c.image(*it->second.image,x,y,width,height,{1,1,1,alpha});
    }
    float scaled_width(std::string_view name,float height) const {
        const auto it=images.find(name);
        if(it==images.end()||!it->second.image->loaded) return height;
        const auto& image=*it->second.image;
        return height*image.width/std::max(1,image.height);
    }
    void draw(const xmb_canvas& c,std::string_view name,float cx,float cy,float height,float alpha=0.85f,float focus=1) const {
        const auto it=images.find(name);if(it==images.end()||!it->second.image->loaded) return;
        const bool use_glass=config::CONFIG.iconGlassRefraction&&it->second.normal&&it->second.normal->loaded;
        const auto& t=use_glass?*it->second.normal:*it->second.image;
        const float h=height*focus,w=h*t.width/std::max(1,t.height);
        if(use_glass)
            c.glass_image(t,cx-w*0.5f,cy-h*0.5f,w,h,{utils::xmb_icon_colour(openxmb::xmb::wall_clock_now()),alpha});
        else c.image(t,cx-w*0.5f,cy-h*0.5f,w,h,{1,1,1,alpha});
    }
};
}
