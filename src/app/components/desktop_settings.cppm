module;
#include "../../localization/ui_literal_text.hpp"
#include "openxmb/xmb/catalog.hpp"
#include "../../localization/shell_locale.hpp"
#include "../input_bindings.hpp"
#include "openxmb/music/player.hpp"
#include "openxmb/desktop/library.hpp"
#include "openxmb/themes/library.hpp"
#include "../../desktop/log_path.hpp"
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_image.h>
#include <cctype>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include "openxmb/integrations/desktop.hpp"
#include <SDL2/SDL.h>
#undef main
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif
export module openxmb.app:desktop_settings;
import i18n;
import auroreengine;
import vulkan_hpp;
import openxmb.config;
import :component;
import :main;
import :choice_overlay;
import :message_overlay;
import :progress_overlay;
import :text_entry;
import :xmb_presentation;

namespace app {
export class binding_capture final : public component,public event_receiver {
public:
    explicit binding_capture(std::string key) : key_(std::move(key)) {}
    float typography_scale() const override {return 2.5F;}
    result on_event(const event& input) override {
        if(input.action==action::cancel) return result::close|result::back_sound;

        if(!armed_) return result::success;
        int code=-1;
        if(key_.starts_with("pad.")) {if(const auto* button=input.get<events::controller_button_down>())code=static_cast<int>(button->button);}
        else if(const auto* key=input.get<events::key_down>())code=static_cast<int>(key->keycode);
        if(code<0) return result::success;
        input_bindings::assign(config::CONFIG.preferences,key_,code);config::CONFIG.save_config();if(host_)host_->reload_button_icons();
        return result::close|result::confirm_sound;
    }
    result tick(shell* host) override {
        host_=host;
        if(!armed_) {
            int count{};const auto* keys=SDL_GetKeyboardState(&count);bool pressed=false;
            for(int i=0;i<count;++i)pressed=pressed||keys[i]!=0;
            for(const auto& [id,controller]:host->get_window()->controllers)for(int i=0;i<SDL_CONTROLLER_BUTTON_MAX;++i)pressed=pressed||SDL_GameControllerGetButton(controller.get(),static_cast<SDL_GameControllerButton>(i))!=0;
            armed_=!pressed;
        }
        return result::success;
    }
    void render(auroreengine::gui_renderer& r,shell* shell) override {
        overlay_canvas c(r);c.rect(0,0,1920,1080,{0,0,0,0.28F});
        c.rect(0,199,1920,1,{1,1,1,0.75F});c.rect(0,879,1920,1,{1,1,1,0.75F});
        c.text("Change Binding",960,350,36,{1,1,1,1},0,true);
        c.text("Release the controls, then press a "+std::string(key_.starts_with("pad.")?"controller button":"keyboard key")+" for "+key_.substr(4),960,500,28,{1,1,1,1},0,true);
        shell->render_controller_buttons(r,0.5F,0.81F,std::array{std::pair{action::cancel,"Cancel"}});
    }
private:
    std::string key_;bool armed_{};shell* host_{};
};
}

namespace app::desktop_settings {
using namespace mfk::i18n::literals;
// Nova hardware settings helpers (backlight / iwd / BlueZ / PipeWire / updates).
#ifndef _WIN32
// read(2) by way of readv(2): glibc's fortified read() is an inline wrapper
// that a C++ module interface unit references but does not emit, which fails
// at link time.
inline ssize_t nova_read_fd(int fd,char* buffer,std::size_t size) {
    struct iovec vec{buffer,size};
    return ::readv(fd,&vec,1);
}
#endif
inline int nova_exec_capture(const std::vector<std::string>& args,std::string& stdout_out) {
#ifdef _WIN32
    (void)args; stdout_out.clear(); return -1;
#else
    stdout_out.clear();
    if(args.empty()) return -1;
    int pipefd[2]{-1,-1};
    if(::pipe(pipefd)!=0) return -1;
    const pid_t pid=::fork();
    if(pid==0) {
        ::close(pipefd[0]);
        const int devnull=::open("/dev/null",O_RDWR);
        if(devnull>=0) {
            ::dup2(devnull,STDIN_FILENO);
            ::dup2(devnull,STDERR_FILENO);
            if(devnull>STDERR_FILENO) ::close(devnull);
        }
        ::dup2(pipefd[1],STDOUT_FILENO);
        ::close(pipefd[1]);
        std::vector<char*> c_args;
        c_args.reserve(args.size()+1);
        for(const auto& s:args) c_args.push_back(const_cast<char*>(s.c_str()));
        c_args.push_back(nullptr);
        ::execvp(args.front().c_str(),c_args.data());
        ::_exit(127);
    }
    ::close(pipefd[1]);
    if(pid<0) { ::close(pipefd[0]); return -1; }
    char buf[4096];
    for(;;) {
        const ssize_t n=nova_read_fd(pipefd[0],buf,sizeof(buf));
        if(n>0) stdout_out.append(buf,static_cast<std::size_t>(n));
        else if(n==0 || (n<0 && errno!=EINTR)) break;
    }
    ::close(pipefd[0]);
    int status=0;
    while(::waitpid(pid,&status,0)<0 && errno==EINTR) {}
    return WIFEXITED(status)?WEXITSTATUS(status):-1;
#endif
}
inline int nova_exec_quiet(const std::vector<std::string>& args) {
    std::string discarded;
    return nova_exec_capture(args,discarded);
}
inline std::string nova_run_read(const char* cmd) {
    std::string out;
    nova_exec_capture({"/bin/sh","-c",cmd},out);
    return out;
}
inline int nova_run_quiet(const std::string& cmd) {
    return nova_exec_quiet({"/bin/sh","-c",cmd});
}
inline std::string nova_strip_ansi(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for(std::size_t i=0;i<input.size();++i) {
        if(input[i]=='\x1b' && i+1<input.size() && input[i+1]=='[') {
            i+=2;
            while(i<input.size() && !(input[i]>='@' && input[i]<='~')) ++i;
            continue;
        }
        out.push_back(input[i]);
    }
    return out;
}
inline std::string nova_trim(std::string_view sv) {
    while(!sv.empty() && (sv.front()==' '||sv.front()=='\t'||sv.front()=='\r'||sv.front()=='\n')) sv.remove_prefix(1);
    while(!sv.empty() && (sv.back()==' '||sv.back()=='\t'||sv.back()=='\r'||sv.back()=='\n')) sv.remove_suffix(1);
    return std::string(sv);
}
inline std::filesystem::path nova_brightness_state_path() {
    if(const char* xdg=std::getenv("XDG_STATE_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg)/"nova/brightness";
    }
    if(const char* home=std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home)/".local/state/nova/brightness";
    }
    return {};
}
inline std::filesystem::path nova_backlight_dir() {
    const std::filesystem::path preferred("/sys/class/backlight/ae94000.dsi.0");
    std::error_code ec;
    if(std::filesystem::is_directory(preferred,ec)) return preferred;
    for(const auto& entry:std::filesystem::directory_iterator("/sys/class/backlight",ec))
        if(entry.is_directory(ec)||entry.is_symlink(ec)) return entry.path();
    return {};
}
inline int nova_read_int(const std::filesystem::path& path,int fallback) {
    std::ifstream in(path);int value=fallback;
    return (in>>value)?value:fallback;
}
inline int nova_backlight_pct() {
    const auto dir=nova_backlight_dir();
    if(dir.empty()) return 50;
    const int max_b=std::max(1,nova_read_int(dir/"max_brightness",100));
    return std::clamp(static_cast<int>(std::lround(100.0*nova_read_int(dir/"brightness",max_b/2)/max_b)),5,100);
}
inline void nova_write_backlight_pct(int pct,bool persist=true) {
    pct=std::clamp(pct,5,100);
    if(persist) {
        const auto state=nova_brightness_state_path();
        if(!state.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(state.parent_path(),ec);
            std::ofstream out(state);
            if(out) out<<pct<<'\n';
        }
    }
    const auto dir=nova_backlight_dir();
    if(dir.empty()) return;
    const int max_b=std::max(1,nova_read_int(dir/"max_brightness",100));
    const int raw=std::clamp(static_cast<int>(std::lround(max_b*(pct/100.0))),1,max_b);
    { std::ofstream out(dir/"brightness"); if(out) { out<<raw<<'\n'; if(out.good()) return; } }
    nova_exec_quiet({"/bin/sh","-c","printf '%d\\n' \"$1\" | sudo -n tee \"$2\" >/dev/null 2>&1","sh",std::to_string(raw),(dir/"brightness").string()});
}
export void restore_saved_brightness() {
    const auto state=nova_brightness_state_path();
    if(state.empty() || !std::filesystem::is_regular_file(state)) return;
    const int pct=nova_read_int(state,-1);
    if(pct>=5 && pct<=100) nova_write_backlight_pct(pct,false);
}
inline int nova_pipewire_volume_pct() {
    std::string out;
    if(nova_exec_capture({"wpctl","get-volume","@DEFAULT_AUDIO_SINK@"},out)!=0) return -1;
    const auto pos=out.find("Volume:");
    if(pos==std::string::npos) return -1;
    try {
        const double vol=std::stod(out.substr(pos+7));
        return std::clamp(static_cast<int>(std::lround(vol*100.0)),0,100);
    } catch(...) {
        return -1;
    }
}
struct NovaAudioSink {
    std::string id;
    std::string name;
    bool is_default=false;
};
inline std::vector<NovaAudioSink> nova_pipewire_sinks() {
    std::string out;
    if(nova_exec_capture({"wpctl","status"},out)!=0) return {};
    std::vector<NovaAudioSink> sinks;
    std::istringstream in(out);
    std::string line;
    bool in_audio=false;
    bool in_sinks=false;
    while(std::getline(in,line)) {
        const auto clean=nova_trim(line);
        if(line.rfind("Audio",0)==0) { in_audio=true; continue; }
        if(in_audio && !line.empty() && line[0]!=' ' && line[0]!='\t' && line.find("Audio")==std::string::npos) {
            if(line.rfind("Video",0)==0 || line.rfind("Settings",0)==0) break;
        }
        if(!in_audio) continue;
        if(clean.find("Sinks:")!=std::string::npos) { in_sinks=true; continue; }
        if(in_sinks && (clean.find("Sources:")!=std::string::npos || clean.find("Filters:")!=std::string::npos || clean.find("Streams:")!=std::string::npos || clean.find("Devices:")!=std::string::npos)) {
            in_sinks=false; continue;
        }
        if(!in_sinks) continue;
        std::string_view sv(line);
        while(!sv.empty() && (sv.front()==' '||sv.front()=='\t'||static_cast<unsigned char>(sv.front())>=0x80)) {
            if(static_cast<unsigned char>(sv.front())>=0x80) {
                while(!sv.empty() && static_cast<unsigned char>(sv.front())>=0x80) sv.remove_prefix(1);
            } else {
                sv.remove_prefix(1);
            }
        }
        bool is_def=false;
        if(!sv.empty() && sv.front()=='*') { is_def=true; sv.remove_prefix(1); }
        while(!sv.empty() && sv.front()==' ') sv.remove_prefix(1);
        const auto dot=sv.find('.');
        if(dot==std::string_view::npos || dot==0) continue;
        const auto id_sv=sv.substr(0,dot);
        if(!std::all_of(id_sv.begin(),id_sv.end(),[](unsigned char ch){return std::isdigit(ch)!=0;})) continue;
        auto rest=sv.substr(dot+1);
        const auto bracket=rest.rfind("[vol:");
        if(bracket!=std::string_view::npos) rest=rest.substr(0,bracket);
        const auto name=nova_trim(rest);
        if(!name.empty()) sinks.push_back({std::string(id_sv),name,is_def});
    }
    return sinks;
}
struct NovaWifiNetwork {
    std::string ssid;
    std::string security;
    std::string signal;
    bool connected=false;
};
inline std::vector<NovaWifiNetwork> nova_scan_wifi_networks() {
    (void)nova_exec_quiet({"iwctl","station","wlan0","scan"});
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    std::string out;
    if(nova_exec_capture({"iwctl","station","wlan0","get-networks"},out)!=0) return {};
    const auto stripped=nova_strip_ansi(out);
    std::vector<NovaWifiNetwork> result;
    std::istringstream in(stripped);
    std::string line;
    while(std::getline(in,line)) {
        if(line.find("----")!=std::string::npos || line.find("Available networks")!=std::string::npos || line.find("Network name")!=std::string::npos) continue;
        bool connected=false;
        std::string trimmed=line;
        const auto first_non=trimmed.find_first_not_of(" \t");
        if(first_non==std::string::npos) continue;
        trimmed=trimmed.substr(first_non);
        if(trimmed.front()=='>') {
            connected=true;
            trimmed=nova_trim(trimmed.substr(1));
        } else {
            trimmed=nova_trim(trimmed);
        }
        if(trimmed.empty()) continue;
        std::string security;
        std::string signal;
        for(const char* sec:{"psk","open","8021x","wep"}) {
            const std::string needle=std::string(" ")+sec;
            auto pos=trimmed.rfind(needle);
            while(pos!=std::string::npos) {
                const auto after=pos+needle.size();
                if(after==trimmed.size() || trimmed[after]==' ') {
                    const auto ssid=nova_trim(std::string_view(trimmed).substr(0,pos));
                    const auto sig=after<trimmed.size()?nova_trim(std::string_view(trimmed).substr(after)):std::string{};
                    if(!ssid.empty()) {
                        result.push_back({ssid,sec,sig,connected});
                        break;
                    }
                }
                pos=pos==0?std::string::npos:trimmed.rfind(needle,pos-1);
            }
            if(!result.empty() && result.back().security==sec) break;
        }
    }
    return result;
}
struct NovaBtDevice {
    std::string mac;
    std::string name;
    bool connected=false;
    bool paired=false;
};
inline std::vector<NovaBtDevice> nova_scan_bt_devices() {
    (void)nova_exec_quiet({"bluetoothctl","power","on"});
    (void)nova_exec_quiet({"bluetoothctl","--timeout","4","scan","on"});
    std::string all_out, conn_out, paired_out;
    (void)nova_exec_capture({"bluetoothctl","devices"},all_out);
    (void)nova_exec_capture({"bluetoothctl","devices","Connected"},conn_out);
    (void)nova_exec_capture({"bluetoothctl","devices","Paired"},paired_out);
    auto parse_list=[](const std::string& raw) {
        std::vector<std::pair<std::string,std::string>> items;
        std::istringstream in(raw);
        std::string line;
        while(std::getline(in,line)) {
            line=nova_trim(nova_strip_ansi(line));
            if(!line.starts_with("Device ")) continue;
            const auto rest=std::string_view(line).substr(7);
            const auto sp=rest.find(' ');
            if(sp==std::string_view::npos) continue;
            const auto mac=std::string(rest.substr(0,sp));
            const auto name=nova_trim(rest.substr(sp+1));
            if(!mac.empty()) items.emplace_back(mac,name.empty()?mac:name);
        }
        return items;
    };
    const auto conn=parse_list(conn_out);
    const auto paired=parse_list(paired_out);
    auto has_mac=[](const auto& vec,const std::string& mac) {
        return std::any_of(vec.begin(),vec.end(),[&](const auto& p){return p.first==mac;});
    };
    std::vector<NovaBtDevice> out;
    for(const auto& [mac,name]:parse_list(all_out)) {
        out.push_back({mac,name,has_mac(conn,mac),has_mac(paired,mac)});
    }
    return out;
}
inline bool nova_wifi_powered() {
    const auto out=nova_run_read("iwctl device wlan0 show 2>/dev/null");
    const auto pos=out.find("Powered");
    return pos!=std::string::npos && out.find("on",pos)!=std::string::npos;
}
inline std::string nova_wifi_network() {
    const auto out=nova_run_read("iwctl station wlan0 show 2>/dev/null");
    const auto pos=out.find("Connected network");
    if(pos==std::string::npos) return {};
    auto line=out.substr(pos+std::string("Connected network").size());
    line=line.substr(0,line.find('\n'));
    const auto first=line.find_first_not_of(" \t");
    return first==std::string::npos?std::string{}:line.substr(first);
}
inline bool nova_bt_powered() {
    return nova_run_read("bluetoothctl show 2>/dev/null").find("Powered: yes")!=std::string::npos;
}
export std::string value(const std::string& key,shell* host=nullptr) {
    const auto& c=config::CONFIG;
    const auto hex=[](const auto& color) {std::string value="#000000";constexpr char digits[]="0123456789abcdef";for(int i=0;i<3;++i){const auto byte=std::clamp(static_cast<int>(color[i]*255+0.5F),0,255);value[1+i*2]=digits[byte>>4];value[2+i*2]=digits[byte&15];}return value;};
    if(key=="theme.icon.color")return c.iconColorAuto?"auto":hex(c.iconColor);
    if(key=="theme.text.color")return hex(c.textColor);
    if(key=="theme.particle.color")return hex(c.particleColor);
    if(host&&!host->get_window()->config.headless) {
        if(key=="display.mode") {const auto flags=SDL_GetWindowFlags(host->get_window()->win.get());return (flags&SDL_WINDOW_FULLSCREEN_DESKTOP)==SDL_WINDOW_FULLSCREEN_DESKTOP?"Borderless":(flags&SDL_WINDOW_FULLSCREEN)?"Fullscreen":"Windowed";}
        if(key=="display.monitor")return std::to_string(SDL_GetWindowDisplayIndex(host->get_window()->win.get()));
        if(key=="display.resolution") {int w{},h{};SDL_GetWindowSize(host->get_window()->win.get(),&w,&h);return std::to_string(w)+"x"+std::to_string(h);}
        if(key=="audio.device") {const auto name=host->audio_device_name();return name.empty()?"System Default":name;}
    }
    if(key=="language") return c.language;
    if(key=="text.scale") return std::to_string(c.textScale).substr(0,3);
    if(key=="ui.scale") {
        auto s = std::to_string(c.ui_scale());
        if(s.find('.')!=std::string::npos) { while(s.back()=='0') s.pop_back(); if(s.back()=='.') s.push_back('0'); }
        return s;
    }
    if(key=="vsync") return c.preferredPresentMode==vk::PresentModeKHR::eFifoRelaxed?"On":"Off";
    if(key=="fps") return c.maxFPS>10000?"Unlimited":std::to_string(static_cast<int>(c.maxFPS));
    if(key=="samples") return std::to_string(static_cast<int>(c.sampleCount));
    if(key=="glass") return c.iconGlassRefraction?"On":"Off";
    if(key=="theme.waves") return c.wavesEnabled?"On":"Off";
    if(key=="theme.particles") return c.particlesEnabled?"On":"Off";
    if(key=="theme.icon.style") return c.iconStyle;
    if(key=="theme.font") return c.fontPath.string();
    if(key=="controller.type") return c.controllerType;
    if(key=="controller.rumble") return c.controllerRumble?"On":"Off";
    if(key=="controller.analog") return c.controllerAnalogStick?"On":"Off";
    if(key=="pictures.folder") return c.pictures_path().string();
    if(key=="music.folder") return c.music_path().string();
    if(key=="videos.folder") return c.videos_path().string();
    if(key=="debug.fps") return c.showFPS?"On":"Off";
    if(key=="debug.memory") return c.showMemory?"On":"Off";
    if(key=="about") return "OpenXMB " OPENXMB_VERSION_STRING;
    if(key=="system.name") return c.preference(key,"OpenXMB system");
    if(key=="autostart") return c.autostart?"On":"Off";
    if(key=="display.screen.brightness") return std::to_string(nova_backlight_pct());
    if(key=="audio.master") {
        const int pw=nova_pipewire_volume_pct();
        if(pw>=0) return std::to_string(pw);
    }
    if(key=="audio.device") {
        for(const auto& s:nova_pipewire_sinks()) if(s.is_default) return s.name;
    }
    if(key=="network.wifi") return nova_wifi_powered()?"On":"Off";
    if(key=="network.wifi.join") {const auto net=nova_wifi_network();return net.empty()?std::string("Not connected"):net;}
    if(key=="network.bluetooth") return nova_bt_powered()?"On":"Off";
    if(key=="network.bluetooth.devices") return "Scan & Pair";
    if(key=="network.aeroplane") return (!nova_wifi_powered()&&!nova_bt_powered())?"On":"Off";
    const auto saved=c.preference(key);if(!saved.empty()) return saved;
    if(key=="audio.device") return "System Default";
    if(key=="browser.homepage") return "https://duckduckgo.com";
    if(key=="browser.downloads") return (openxmb::integrations::user_data_directory()/"downloads").string();
    if(key=="games.folder") return (openxmb::integrations::user_data_directory()/"games").string();
    if(key.starts_with("audio.") && key!="audio.startup" && key!="audio.background" && key!="audio.duck") return "100";
    if(key=="music.visualizer") return "Wave";
    if(key=="music.group") return "Folders";
    if(key=="music.sort") return "Name";
    if(key=="video.hardware") return "auto";
    if(key=="photo.duration") return "5";
    if(key=="photo.transition") return "Fade";
    if(key=="photo.fit"||key=="video.fit") return "Fit";
    if(key=="video.subtitle.size"||key=="display.brightness"||key=="browser.zoom") return "1.0";
    if(key=="video.audio.language") return "Default";
    if(key=="video.subtitle.language") return "Off";
    if(key=="time.offset") return "0";
    if(key=="time.format") return "24-hour";
    if(key=="date.format") return "DD/MM/YYYY";
    if(key=="input.repeat") return "180";
    if(key=="browser.search") return "DuckDuckGo";
    if(key=="display.mode") return "Windowed";
    if(key=="games.launch") return "Keep Running";
    if(key=="power.background.fps") return "30";
    if(key=="power.dim"||key=="music.shuffle"||key=="music.repeat"||key=="photo.shuffle"||key=="time.seconds"||key=="power.reduced.motion") return "Off";
    if(key=="startup.intro"||key=="startup.warning"||key=="audio.startup"||key=="audio.background"||key=="audio.duck"||key=="music.background"||key=="music.artwork"||key=="video.resume"||key=="photo.repeat"||key=="input.osk"||key=="power.keep.awake") return "On";
    return {};
}
export std::string display_value(const std::string& key,shell* host=nullptr) {
    const auto raw=value(key,host);
    return key=="language" ? openxmb::localization::shell_locale::language_label(raw) :
        openxmb::localization::shell_locale::translate(raw);
}
void show_error(shell* shell,const std::string& message) {
    shell->emplace_overlay<message_overlay>("Settings",message);
}
class nova_update_item final : public progress_item {
    public:
        ~nova_update_item() final {
            if(reader.joinable()) reader.join();
        }
        status init(std::string& message) final {
#ifdef _WIN32
            message="System update is unavailable on this platform.";
            return status::error;
#else
            message="Checking for system updates...";
            int pipefd[2]{-1,-1};
            if(::pipe(pipefd)!=0) {
                message="Could not open pipe for nova-update.";
                return status::error;
            }
            const pid_t pid=::fork();
            if(pid==0) {
                ::close(pipefd[0]);
                const int devnull=::open("/dev/null",O_RDWR);
                if(devnull>=0) {
                    ::dup2(devnull,STDIN_FILENO);
                    if(devnull>STDERR_FILENO) ::close(devnull);
                }
                ::dup2(pipefd[1],STDOUT_FILENO);
                ::dup2(pipefd[1],STDERR_FILENO);
                ::close(pipefd[1]);
                const char* const argv[]={"nova-update","--json",nullptr};
                ::execvp("nova-update",const_cast<char* const*>(argv));
                ::_exit(127);
            }
            ::close(pipefd[1]);
            if(pid<0) {
                ::close(pipefd[0]);
                message="Could not start nova-update.";
                return status::error;
            }
            child_pid=pid;
            read_fd=pipefd[0];
            reader=std::thread([this] { read_events(); });
            return status::running;
#endif
        }
        status progress(double& value,std::string& message) final {
            std::lock_guard<std::mutex> lock(mutex);
            if(step_progress) value=*step_progress;
            message=last_message;
            if(failed) return status::error;
            if(finished) return status::success;
            return status::running;
        }
    private:
        void handle_line(std::string line) {
            while(!line.empty() && (line.back()=='\n' || line.back()=='\r')) line.pop_back();
            if(line.empty()) return;
            const auto parsed=nlohmann::json::parse(line,nullptr,false);
            if(parsed.is_discarded() || !parsed.is_object()) return;
            std::lock_guard<std::mutex> lock(mutex);
            const auto event=parsed.value("event",std::string{});
            const auto detail=parsed.value("detail",std::string{});
            if(event=="progress") {
                const int index=parsed.value("index",0);
                const int total=parsed.value("total",0);
                if(total>0) step_progress=std::max(0,index-1)/static_cast<double>(total);
                last_message=total>0?std::format("Step {} of {}: {}",index,total,detail):detail;
            } else if(event=="note") {
                if(!detail.empty()) last_message=detail;
            } else if(event=="error") {
                last_message=detail.empty()?"System update failed.":detail;
                failed=true;
            } else if(event=="done") {
                step_progress=1.0;
                last_message=detail.empty()?"System is up to date.":detail;
                finished=true;
            }
        }
        void read_events() {
#ifndef _WIN32
            std::string pending;
            char buf[1024];
            for(;;) {
                const ssize_t n=nova_read_fd(read_fd,buf,sizeof(buf));
                if(n>0) {
                    pending.append(buf,static_cast<std::size_t>(n));
                    for(;;) {
                        const auto nl=pending.find('\n');
                        if(nl==std::string::npos) break;
                        auto line=pending.substr(0,nl);
                        pending.erase(0,nl+1);
                        handle_line(std::move(line));
                    }
                } else if(n==0 || (n<0 && errno!=EINTR)) {
                    break;
                }
            }
            ::close(read_fd);
            read_fd=-1;
            handle_line(std::move(pending));
            int raw=0;
            while(::waitpid(child_pid,&raw,0)<0 && errno==EINTR) {}
            const int code=WIFEXITED(raw)?WEXITSTATUS(raw):-1;
            std::lock_guard<std::mutex> lock(mutex);
            if(failed || finished) return;
            if(code!=0) {
                failed=true;
                last_message=code>0?std::format("nova-update failed with exit code {}.",code):"nova-update could not be started.";
            } else {
                finished=true;
                step_progress=1.0;
                if(last_message.empty()) last_message="System update finished.";
            }
#endif
        }
        std::mutex mutex;
        std::thread reader;
        int read_fd=-1;
        long child_pid=-1;
        std::optional<double> step_progress;
        std::string last_message="Checking for system updates...";
        bool finished=false;
        bool failed=false;
};
template<typename Fn>
class nova_async_task_item final : public progress_item {
    public:
        nova_async_task_item(std::string wait_msg,Fn fn)
            : wait_message(std::move(wait_msg)), task(std::move(fn)) {}
        ~nova_async_task_item() final {
            if(worker.joinable()) worker.join();
        }
        status init(std::string& message) final {
            message=wait_message;
            worker=std::thread([this] {
                task();
                std::lock_guard<std::mutex> lock(mutex);
                completed=true;
            });
            return status::running;
        }
        status progress(double& value,std::string& message) final {
            std::lock_guard<std::mutex> lock(mutex);
            value=completed?1.0:0.45;
            message=wait_message;
            return completed?status::success:status::running;
        }
    private:
        std::string wait_message;
        Fn task;
        std::mutex mutex;
        std::thread worker;
        bool completed=false;
};
void apply(shell* shell,const std::string& key,const std::string& selected) {
    auto& c=config::CONFIG;auto* window=shell->get_window();
    if(key=="theme.icon.color"||key=="theme.text.color"||key=="theme.particle.color") {
        const bool automatic=key=="theme.icon.color"&&(selected=="auto"||selected=="original");
        const bool hex=selected.size()==7&&selected.front()=='#'&&std::all_of(selected.begin()+1,selected.end(),[](unsigned char c){return std::isxdigit(c)!=0;});
        if(!automatic&&!hex){show_error(shell,"Use a six-digit hexadecimal colour such as #ffeefc.");return;}
    }
    if(key=="browser.homepage") {const auto checked=openxmb::integrations::validate_browser_url(selected);if(!checked){show_error(shell,checked.message);return;}}
    if(key=="display.screen.brightness") {nova_write_backlight_pct(std::stoi(selected));return;}
    if(key=="audio.master") {
        const int pct=std::clamp(std::stoi(selected),0,100);
        if(nova_exec_quiet({"wpctl","set-volume","@DEFAULT_AUDIO_SINK@",std::to_string(pct)+"%"})==0) {
            c.preferences["audio.master"]="100";
            c.save_config();
            return;
        }
    }
    if(key=="network.wifi") {nova_exec_quiet({"iwctl","device","wlan0","set-property","Powered",selected=="On"?"on":"off"});return;}
    if(key=="network.bluetooth") {nova_exec_quiet({"bluetoothctl","power",selected=="On"?"on":"off"});return;}
    if(key=="network.aeroplane") {nova_exec_quiet({"rfkill",selected=="On"?"block":"unblock","all"});return;}
    if(key=="system.name") SDL_SetWindowTitle(window->win.get(),selected.c_str());
    if(key=="language") {c.setLanguage(selected);c.save_config();return;}
    if(key=="text.scale") {c.textScale=std::stod(selected);shell->reload_fonts();}
    else if(key=="vsync") c.setVSync(selected=="On");
    else if(key=="fps") {c.setMaxFPS(selected=="Unlimited"?0:std::stod(selected));window->config.fpsLimit=selected=="Unlimited"?0:std::stod(selected);}
    else if(key=="samples") {
        const auto samples=static_cast<vk::SampleCountFlagBits>(std::stoi(selected));
        const auto supported=window->physicalDevice.getProperties().limits.framebufferColorSampleCounts;
        if(!(supported&samples)) {show_error(shell,"This sample count is unavailable on the active GPU.");return;}
        window->waitIdle();c.setSampleCount(samples);window->config.sampleCount=samples;window->recreateSwapchain();
    }
    else if(key=="theme.waves") c.setWavesEnabled(selected=="On");
    else if(key=="theme.particles") c.setParticlesEnabled(selected=="On");
    else if(key=="theme.icon.style") c.setIconStyle(selected);
    else if(key=="theme.icon.color") c.setIconColor(selected);
    else if(key=="theme.particle.color") c.setParticleColor(selected);
    else if(key=="theme.text.color") c.setTextColor(selected);
    else if(key=="theme.font") {if(!std::filesystem::is_regular_file(std::filesystem::u8path(selected))) {show_error(shell,"Font file not found.");return;}shell->set_font_file(selected);}
    else if(key=="theme.wallpaper") {if(!std::filesystem::is_regular_file(std::filesystem::u8path(selected))) {show_error(shell,"Background file not found.");return;}auto ext=std::filesystem::u8path(selected).extension().string();std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        const std::array<std::string_view,13> videos{".mp4",".mkv",".webm",".mov",".m4v",".avi",".wmv",".mpg",".mpeg",".ts",".m2ts",".vob",".ogv"};
        const bool video=std::find(videos.begin(),videos.end(),ext)!=videos.end();
        if(video) {AVFormatContext* context{};const bool opened=avformat_open_input(&context,selected.c_str(),nullptr,nullptr)>=0;const bool valid=opened&&avformat_find_stream_info(context,nullptr)>=0&&av_find_best_stream(context,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0)>=0;avformat_close_input(&context);if(!valid){show_error(shell,"This video cannot be opened.");return;}}
        else {auto* image=IMG_Load(selected.c_str());if(!image){show_error(shell,"This file is not a supported image or video.");return;}SDL_FreeSurface(image);}
        c.setBackgroundImage(selected);c.setBackgroundType(video?config::config::background_type::video:config::config::background_type::image);}
    else if(key=="theme.sounds") {const auto folder=std::filesystem::u8path(selected);if(!std::filesystem::is_directory(folder)) {show_error(shell,"Sound folder not found.");return;}for(const auto* name:{"ok.wav","confirm.wav","cancel.wav","back.wav"}) {auto* sound=Mix_LoadWAV((folder/name).string().c_str());if(!sound){show_error(shell,std::string("Sound set needs a playable ")+name);return;}Mix_FreeChunk(sound);}}
    else if(key=="glass") c.setIconGlassRefraction(selected=="On");
    else if(key=="controller.type") c.setControllerType(selected);
    else if(key=="controller.rumble") c.setControllerRumble(selected=="On");
    else if(key=="controller.analog") c.setControllerAnalogStick(selected=="On");
    else if(key=="debug.fps") c.setShowFPS(selected=="On");
    else if(key=="debug.memory") c.setShowMemory(selected=="On");
    else if(key=="pictures.folder"||key=="music.folder"||key=="videos.folder"||key=="games.folder"||key=="browser.downloads") {
        if(!std::filesystem::is_directory(std::filesystem::u8path(selected))) {show_error(shell,"Choose an existing folder.");return;}
        if(key=="pictures.folder") c.picturesPath=std::filesystem::u8path(selected);
        if(key=="music.folder") c.musicPath=std::filesystem::u8path(selected);
        if(key=="videos.folder") c.videosPath=std::filesystem::u8path(selected);
        if(key=="games.folder") {openxmb::desktop::Library library;library.add_game_root(std::filesystem::u8path(selected));}
    }
    else if(key=="display.mode") {
        const auto flags=selected=="Windowed"?0:selected=="Borderless"?SDL_WINDOW_FULLSCREEN_DESKTOP:SDL_WINDOW_FULLSCREEN;
        if(SDL_SetWindowFullscreen(window->win.get(),flags)!=0) {show_error(shell,SDL_GetError());return;}
        window->config.fullscreen=selected!="Windowed";
    }
    else if(key=="display.monitor") {const int index=std::stoi(selected);SDL_Rect bounds{};if(SDL_GetDisplayBounds(index,&bounds)==0) SDL_SetWindowPosition(window->win.get(),bounds.x,bounds.y);}
    else if(key=="display.resolution") {int w=0,h=0;const auto x=selected.find('x');if(x!=std::string::npos) {w=std::stoi(selected.substr(0,x));h=std::stoi(selected.substr(x+1));}if(w>0&&h>0) {SDL_DisplayMode mode{};mode.w=w;mode.h=h;SDL_SetWindowDisplayMode(window->win.get(),&mode);SDL_SetWindowSize(window->win.get(),w,h);}}
    else if(key=="autostart") {
#ifdef _WIN32
        HKEY handle{};if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,nullptr,0,KEY_SET_VALUE,nullptr,&handle,nullptr)!=ERROR_SUCCESS) {show_error(shell,"Unable to update launch at login.");return;}
        const auto command=L"\""+(c.exe_directory/L"OpenXMB.exe").wstring()+L"\"";
        const auto result=selected=="On"?RegSetValueExW(handle,L"OpenXMB",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),static_cast<DWORD>((command.size()+1)*sizeof(wchar_t))):RegDeleteValueW(handle,L"OpenXMB");RegCloseKey(handle);
        if(result!=ERROR_SUCCESS&&result!=ERROR_FILE_NOT_FOUND) {show_error(shell,"Unable to update launch at login.");return;}
#else
        const auto home=std::getenv("HOME");if(!home) {show_error(shell,"Home directory unavailable.");return;}
        const auto file=std::filesystem::path(home)/".config/autostart/openxmb.desktop";
        std::filesystem::create_directories(file.parent_path());
        if(selected=="On") {std::ofstream out(file);out<<"[Desktop Entry]\nType=Application\nName=OpenXMB\nExec=\""<<(c.exe_directory/"XMS.bin").string()<<"\"\n";}
        else std::filesystem::remove(file);
#endif
        c.autostart=selected=="On";
    }
    if(key=="audio.device"&&!shell->set_audio_device(selected=="System Default"?std::string{}:selected)) {show_error(shell,openxmb::music::service().snapshot().error);return;}
    c.preferences[key]=selected;c.save_config();
    if(key=="theme.sounds")shell->reload_theme_sounds();
    if(key=="music.group"||key=="music.sort"||key.ends_with(".folder")) shell->reload_menu();
}
export void activate(shell* shell,const openxmb::xmb::CatalogNode& node) {
    const auto key=node.action_id.substr(8);const auto title=key;
    if(key=="system.update") {
        shell->emplace_overlay<message_overlay>("System Update"_(),"Check for and install system updates from the configured flake now?",std::vector<std::string>{"No","Yes"},[shell](unsigned i){
            if(i) shell->emplace_overlay<progress_overlay>("System Update"_(),std::make_unique<nova_update_item>());
        });
        return;
    }
    if(key=="theme.install") {shell->emplace_overlay<text_entry_overlay>("Theme archive or folder","",[shell](std::string path){try {shell->theme_library()->import_theme(std::filesystem::u8path(path));shell->reload_menu();}catch(const std::exception& e){show_error(shell,e.what());}});return;}
    if(key=="theme.preset.save") {shell->emplace_overlay<text_entry_overlay>("Preset name","",[shell](std::string name){try {auto library=shell->theme_library();const auto theme=library->create_custom_theme(name,nlohmann::json::parse(config::CONFIG.shell_snapshot()));library->save_preset(name,theme.id,{});}catch(const std::exception& e){show_error(shell,e.what());}});return;}
    if(key=="theme.preset.load") {const auto names=shell->theme_library()->presets();if(names.empty()){show_error(shell,"No saved theme presets.");return;}shell->emplace_overlay<choice_overlay>(names,0,[shell,names](unsigned i){try{shell->apply_theme(shell->theme_library()->load_preset(names.at(i)));}catch(const std::exception& e){show_error(shell,e.what());}});return;}
    if(key=="theme.reset") {shell->emplace_overlay<message_overlay>("Restore Original Theme?","Restore the original theme appearance?",std::vector<std::string>{"No","Yes"},[shell](unsigned i){if(i)shell->apply_theme(shell->theme_library()->revert());});return;}
    if(key=="about") {shell->open_panel("about");return;}
    if(key=="licenses") {shell->open_panel("licenses");return;}
    if(key=="browser.clear") {shell->emplace_overlay<message_overlay>("Clear Browsing Data?","Remove browser cookies, site storage and cached pages?",std::vector<std::string>{"No","Yes"},[](unsigned i){if(i) {config::CONFIG.preferences["browser.clear_site_data"]=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());config::CONFIG.save_config();}});return;}
    if(key=="reset") {shell->emplace_overlay<message_overlay>("Restore Default Settings?","Restore this profile's preferences? Your media files will be preserved.",std::vector<std::string>{"No","Yes"},[shell](unsigned i){if(i) {apply(shell,"autostart","Off");shell->reset_settings();}});return;}
    if(key=="network.wifi.join") {
        auto found=std::make_shared<std::vector<NovaWifiNetwork>>();
        auto item=std::make_unique<nova_async_task_item<std::function<void()>>>("Scanning for Wi-Fi networks...",[found](){
            *found=nova_scan_wifi_networks();
        });
        shell->emplace_overlay<progress_overlay>("Wi-Fi Networks",std::move(item),false,[shell,found](class shell* s,bool){
            const auto connect_ssid=[](class shell* host,const std::string& ssid,const std::string& psk) {
                std::vector<std::string> cmd{"iwctl"};
                if(!psk.empty()) { cmd.push_back("--passphrase"); cmd.push_back(psk); }
                cmd.push_back("station"); cmd.push_back("wlan0"); cmd.push_back("connect"); cmd.push_back(ssid);
                if(nova_exec_quiet(cmd)!=0) show_error(host,"Could not connect to "+ssid+". Check the name and password.");
                else host->reload_menu();
            };
            const auto prompt_manual=[shell,connect_ssid]() {
                shell->emplace_overlay<text_entry_overlay>("Wi-Fi network name (SSID)","",[shell,connect_ssid](std::string ssid){
                    if(ssid.empty()) return;
                    shell->emplace_overlay<text_entry_overlay>("Wi-Fi password (blank for open)","",[shell,ssid,connect_ssid](std::string psk){
                        connect_ssid(shell,ssid,psk);
                    });
                });
            };
            if(found->empty()) {
                prompt_manual();
                return;
            }
            std::vector<std::string> labels;
            unsigned initial=0;
            for(std::size_t idx=0;idx<found->size();++idx) {
                const auto& net=(*found)[idx];
                if(net.connected) initial=static_cast<unsigned>(idx);
                std::string label=net.ssid;
                if(net.connected) label+=" [Connected]";
                if(!net.security.empty() || !net.signal.empty()) {
                    label+=" (";
                    if(!net.security.empty()) label+=net.security;
                    if(!net.security.empty() && !net.signal.empty()) label+=" ";
                    if(!net.signal.empty()) label+=net.signal;
                    label+=")";
                }
                labels.push_back(std::move(label));
            }
            labels.push_back("Enter SSID manually...");
            auto* overlay=s->emplace_overlay<choice_overlay>(labels,initial,[shell,found,connect_ssid,prompt_manual](unsigned i){
                if(i>=found->size()) {
                    prompt_manual();
                    return;
                }
                const auto net=(*found)[i];
                if(net.security=="open") {
                    connect_ssid(shell,net.ssid,{});
                    return;
                }
                shell->emplace_overlay<text_entry_overlay>("Wi-Fi password for "+net.ssid+" (blank if saved)","",[shell,net,connect_ssid](std::string psk){
                    connect_ssid(shell,net.ssid,psk);
                });
            });
            overlay->set_top_action("Select Wi-Fi Network");
        });
        return;
    }
    if(key=="network.bluetooth.devices") {
        auto found=std::make_shared<std::vector<NovaBtDevice>>();
        auto item=std::make_unique<nova_async_task_item<std::function<void()>>>("Scanning for Bluetooth devices...",[found](){
            *found=nova_scan_bt_devices();
        });
        shell->emplace_overlay<progress_overlay>("Bluetooth Devices",std::move(item),false,[shell,found](class shell*,bool){
            if(found->empty()) {
                show_error(shell,"No Bluetooth devices found. Put your device into pairing mode and try again.");
                return;
            }
            std::vector<std::string> labels;
            for(const auto& d:*found) {
                std::string label=d.name;
                if(d.connected) label+=" [Connected]";
                else if(d.paired) label+=" [Paired]";
                labels.push_back(std::move(label));
            }
            auto* overlay=shell->emplace_overlay<choice_overlay>(labels,0,[shell,found](unsigned i){
                if(i>=found->size()) return;
                const auto dev=(*found)[i];
                std::vector<std::string> actions;
                if(dev.connected) {
                    actions={"Disconnect","Forget Device","Cancel"};
                } else if(dev.paired) {
                    actions={"Connect","Forget Device","Cancel"};
                } else {
                    actions={"Pair & Connect","Cancel"};
                }
                auto* act_overlay=shell->emplace_overlay<choice_overlay>(actions,0,[shell,dev,actions](unsigned a){
                    const auto& act=actions.at(a);
                    if(act=="Disconnect") {
                        if(nova_exec_quiet({"bluetoothctl","disconnect",dev.mac})!=0) show_error(shell,"Could not disconnect "+dev.name+".");
                    } else if(act=="Forget Device") {
                        if(nova_exec_quiet({"bluetoothctl","remove",dev.mac})!=0) show_error(shell,"Could not remove "+dev.name+".");
                    } else if(act=="Connect") {
                        if(nova_exec_quiet({"bluetoothctl","connect",dev.mac})!=0) show_error(shell,"Could not connect to "+dev.name+".");
                    } else if(act=="Pair & Connect") {
                        (void)nova_exec_quiet({"bluetoothctl","trust",dev.mac});
                        (void)nova_exec_quiet({"bluetoothctl","pair",dev.mac});
                        if(nova_exec_quiet({"bluetoothctl","connect",dev.mac})!=0) show_error(shell,"Could not pair/connect "+dev.name+".");
                    }
                    shell->reload_menu();
                });
                act_overlay->set_top_action(dev.name);
            });
            overlay->set_top_action("Bluetooth Devices");
        });
        return;
    }
    if(key=="music.rescan") {shell->reload_menu();return;}
    if(key=="games.rescan") {openxmb::desktop::Library library;const auto count=library.scan_games();show_error(shell,"Added "+std::to_string(count)+" games to the library.");return;}
    if(key=="games.emulation") {shell->open_panel("emulation");return;}
    if(key=="cache.clear") {shell->emplace_overlay<message_overlay>("Clear Thumbnail Cache?","Generated thumbnails will be recreated when needed. Original media is preserved.",std::vector<std::string>{"No","Yes"},[shell](unsigned i){if(i) {const auto root=openxmb::integrations::user_data_directory()/"thumbnails";std::error_code error;std::filesystem::remove_all(root,error);if(error)show_error(shell,error.message());}});return;}
    if(key=="debug.logs") {
#ifdef _WIN32
        const auto directory=openxmb::desktop::log_directory();std::filesystem::create_directories(directory);ShellExecuteW(nullptr,L"open",directory.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
#else
        show_error(shell,"File logging is unavailable on this platform.");
#endif
        return;
    }
    if(key=="debug.decoder") {const auto& m=openxmb::music::service().snapshot().metadata;show_error(shell,"Music codec: "+m.codec+"\nSample rate: "+std::to_string(m.sample_rate)+" Hz\nChannels: "+std::to_string(m.channels)+"\nVideo decoder details are available in the video player's information display.");return;}
    if(key=="input.keyboard"||key=="input.controller") {
        const bool controller=key=="input.controller";
        shell->emplace_overlay<choice_overlay>(::app::detail::localize_ui_choices(std::vector<std::string>{"Left","Right","Up","Down","Confirm","Back","Options","Extra","Restore Defaults"}),0,[shell,controller](unsigned i){
            static const std::array<std::string,8> actions{"left","right","up","down","ok","cancel","options","extra"};
            if(i==8) {for(const auto& action:actions) config::CONFIG.preferences.erase((controller?"pad.":"key.")+action);config::CONFIG.save_config();shell->reload_button_icons();return;}
            shell->queue_panel(std::string(controller?"bind.pad.":"bind.key.")+actions[i]);
        });return;
    }
    if(key=="audio.device") {
        const auto sinks=nova_pipewire_sinks();
        if(!sinks.empty()) {
            std::vector<std::string> labels;
            unsigned selected=0;
            for(std::size_t i=0;i<sinks.size();++i) {
                labels.push_back(sinks[i].name);
                if(sinks[i].is_default) selected=static_cast<unsigned>(i);
            }
            shell->emplace_overlay<choice_overlay>(labels,selected,[shell,sinks](unsigned i){
                if(i>=sinks.size()) return;
                if(nova_exec_quiet({"wpctl","set-default",sinks[i].id})!=0) {
                    show_error(shell,"Could not switch audio output device.");
                } else {
                    config::CONFIG.preferences["audio.device"]=sinks[i].name;
                    config::CONFIG.save_config();
                    shell->reload_menu();
                }
            });
            return;
        }
    }
    std::vector<std::string> values;for(const auto& choice:node.choices) values.push_back(choice.value);
    if(key=="display.monitor") for(int i=0;i<SDL_GetNumVideoDisplays();++i) values.push_back(std::to_string(i));
    if(key=="display.resolution") {for(int i=0;i<SDL_GetNumDisplayModes(0);++i) {SDL_DisplayMode mode{};if(SDL_GetDisplayMode(0,i,&mode)==0) {const auto item=std::to_string(mode.w)+"x"+std::to_string(mode.h);if(std::find(values.begin(),values.end(),item)==values.end())values.push_back(item);}}}
    if(key=="audio.device") {values.push_back("System Default");for(int i=0;i<SDL_GetNumAudioDevices(0);++i) if(auto* name=SDL_GetAudioDeviceName(i,0))values.emplace_back(name);}
    if(!values.empty()) {
        const auto current=value(key,shell);
        auto it=std::find(values.begin(),values.end(),current);
        unsigned selected=it==values.end()?0:static_cast<unsigned>(it-values.begin());
        if(it==values.end() && (key=="audio.master" || key=="display.screen.brightness")) {
            try {
                const int cur_i=std::stoi(current);
                int best_diff=1000000;
                for(std::size_t idx=0;idx<values.size();++idx) {
                    const int diff=std::abs(std::stoi(values[idx])-cur_i);
                    if(diff<best_diff) { best_diff=diff; selected=static_cast<unsigned>(idx); }
                }
            } catch(...) {}
        }
        auto labels=values;
        for(auto& label:labels)label=key=="audio.device"&&label!="System Default"?label:key=="language"?openxmb::localization::shell_locale::language_label(label):openxmb::localization::shell_locale::translate(label);
        shell->emplace_overlay<choice_overlay>(labels,selected,[shell,key,values](unsigned i){if(i<values.size())try{apply(shell,key,values[i]);}catch(const std::exception& e){show_error(shell,e.what());}});
        return;
    }
    shell->emplace_overlay<text_entry_overlay>(node.id.substr(node.id.find_last_of('.')+1),value(key,shell),[shell,key](std::string text){try{apply(shell,key,text);}catch(const std::exception& e){show_error(shell,e.what());}});
}
}
