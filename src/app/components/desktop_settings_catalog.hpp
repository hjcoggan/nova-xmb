#pragma once
#include "openxmb/xmb/catalog.hpp"
#include "../../localization/language_options.hpp"
#include <initializer_list>
#include <functional>
#include <string>
#include <vector>
#include <array>
#include <algorithm>

namespace app::desktop_settings {
struct Entry {
    std::string group, key, label, description;
    std::vector<std::string> values{};
};
inline const std::vector<Entry>& entries() {
    static const std::vector<Entry> data{
        {"Theme","theme.install","Install Theme","Import a theme archive or theme folder."},
        {"Theme","theme.wallpaper","Background File","Choose a photo or video background."},
        {"Theme","theme.waves","Wave","Show the animated background ribbon.",{"On","Off"}},
        {"Theme","theme.particles","Sparkles","Show background particles.",{"On","Off"}},
        {"Theme","theme.particle.color","Sparkle Colour","Use a hexadecimal colour such as #ffeefc."},
        {"Theme","theme.icon.style","Icon Style","Choose the icon material.",{"default","flat"}},
        {"Theme","theme.icon.color","Icon Colour","Use auto or a hexadecimal colour."},
        {"Theme","theme.text.color","Text Colour","Use a hexadecimal colour."},
        {"Theme","theme.font","Font File","Choose a TrueType or OpenType font file."},
        {"Theme","theme.sounds","Sound Set Folder","Use a folder containing ok.wav, confirm.wav, cancel.wav and back.wav."},
        {"Theme","theme.preset.save","Save Theme Preset","Save the current appearance with a name."},
        {"Theme","theme.preset.load","Saved Theme Presets","Apply a saved appearance preset."},
        {"Theme","theme.reset","Restore Original Theme","Restore the original OpenXMB appearance."},
        {"System","system.name","System Name","The name displayed by OpenXMB."},
        {"System","language","Language","Preferred interface language.",openxmb::localization::shell_locale::language_values()},
        {"System","startup.intro","Startup Intro","Play the startup sequence.",{"On","Off"}},
        {"System","startup.warning","Photosensitivity Warning","Show the dismissible startup warning.",{"On","Off"}},
        {"System","autostart","Launch at Login","Start OpenXMB when you sign in.",{"Off","On"}},
        {"System","about","System Information","OpenXMB version and information about this computer."},
        {"System","licenses","Licenses","Read the program and third-party license notices."},
        {"System","reset","Restore Default Settings","Restore this profile's shell settings; preserve media."},
        {"Display","display.mode","Window Mode","Choose how OpenXMB uses the display.",{"Windowed","Borderless","Fullscreen"}},
        {"Display","display.monitor","Display","Move OpenXMB to a connected display."},
        {"Display","display.resolution","Resolution","Choose the fullscreen display resolution."},
        {"Display","text.scale","Interface Text Size","Scale text throughout OpenXMB.",{"0.8","1.0","1.2","1.4"}},
        {"Display","ui.scale","Interface Scale","Scale icons, text, and layout together.",{"1.0","1.25","1.35","1.5","1.75","2.0"}},
        {"Display","display.brightness","Brightness","Adjust the OpenXMB scene brightness.",{"0.6","0.8","1.0","1.2"}},
        {"Display","display.screen.brightness","Screen Brightness","Hardware panel backlight level.",{"10","20","30","40","50","60","70","80","90","100"}},
        {"Display","vsync","VSync","Synchronise presentation to the display.",{"On","Off"}},
        {"Display","fps","Frame Limit","Maximum shell rendering rate.",{"30","60","120","144","240","Unlimited"}},
        {"Display","samples","Anti-aliasing","Multisample anti-aliasing for supported graphics hardware.",{"1","2","4","8"}},
        {"Display","glass","Glass Icons","Use refractive icon materials.",{"On","Off"}},
        {"Sound","audio.device","Audio Output Device","Select an available playback device."},
        {"Sound","audio.master","Master Volume","System audio output volume.",{"0","10","20","30","40","50","60","70","80","90","100"}},
        {"Sound","audio.effects","Interface Volume","Volume of navigation and confirmation sounds.",{"0","25","50","75","100"}},
        {"Sound","audio.music","Music Volume","Default volume for music playback.",{"0","25","50","75","100"}},
        {"Sound","audio.video","Video Volume","Default volume for video playback.",{"0","25","50","75","100"}},
        {"Sound","audio.startup","Startup Sound","Play the startup cue in full.",{"On","Off"}},
        {"Sound","audio.background","Background Audio","Continue audio when OpenXMB loses focus.",{"On","Off"}},
        {"Sound","audio.duck","Quiet Interface During Music","Reduce navigation sound volume while music is playing.",{"On","Off"}},
        {"Date and Time","date.format","Date Format","Order of year, month and day.",{"DD/MM/YYYY","MM/DD/YYYY","YYYY-MM-DD"}},
        {"Date and Time","time.format","Time Format","Choose a 12-hour or 24-hour clock.",{"24-hour","12-hour"}},
        {"Date and Time","time.seconds","Show Seconds","Show seconds in the clock.",{"Off","On"}},
        {"Date and Time","time.offset","Time Zone Offset","Hours added to local system time for display.",{"-12","-11","-10","-9","-8","-7","-6","-5","-4","-3","-2","-1","0","1","2","3","4","5","5.5","6","7","8","9","9.5","10","11","12","13"}},
        {"Power Save","power.dim","Idle Dimming","Dim the scene after inactivity.",{"Off","1 minute","5 minutes","10 minutes"}},
        {"Power Save","power.background.fps","Background Frame Limit","Reduce rendering while unfocused.",{"15","30","60"}},
        {"Power Save","power.reduced.motion","Reduced Motion","Reduce background movement and sparkles.",{"Off","On"}},
        {"Power Save","power.keep.awake","Prevent Sleep During Playback","Keep this computer awake while media plays.",{"On","Off"}},
        {"Browser","browser.homepage","Homepage","Page opened when launching the browser."},
        {"Browser","browser.search","Search Engine","Search provider used for non-address text.",{"DuckDuckGo","Google","Bing"}},
        {"Browser","browser.zoom","Page Zoom","Default browser page scale.",{"0.8","1.0","1.25","1.5","2.0"}},
        {"Browser","browser.downloads","Downloads Folder","Directory for browser downloads."},
        {"Browser","browser.clear","Clear Browsing Data","Remove browser cookies and cached site data."},
        {"Network","network.wifi","Wi-Fi","Turn the Wi-Fi radio on or off.",{"On","Off"}},
        {"Network","network.wifi.join","Join Wi-Fi Network","Scan for and connect to a Wi-Fi network."},
        {"Network","network.bluetooth","Bluetooth","Turn the Bluetooth radio on or off.",{"On","Off"}},
        {"Network","network.bluetooth.devices","Bluetooth Devices","Scan, pair and connect Bluetooth devices."},
        {"Network","network.aeroplane","Aeroplane Mode","Disable all radios (Wi-Fi and Bluetooth).",{"Off","On"}},
        {"Game","games.folder","Game Library Folder","Choose the folder scanned for games."},
        {"Game","games.rescan","Rescan Game Library","Refresh the configured game library."},
        {"Game","games.launch","After Launching a Game","Choose how the shell behaves while a game runs.",{"Keep Running","Minimise"}},
        {"Game","games.emulation","Emulation Configuration","Select an installed core and game content."},
        {"Advanced","debug.fps","Show FPS","Display the shell frame rate.",{"Off","On"}},
        {"Advanced","debug.memory","Show GPU Memory","Display GPU memory use.",{"Off","On"}},
        {"Advanced","debug.logs","Open Log Folder","Open the location of diagnostic logs."},
        {"Advanced","debug.decoder","Decoder Information","View active audio and video playback diagnostics."},
    };
    return data;
}
inline void populate(openxmb::xmb::Catalog& catalog,
    const std::function<std::string(std::string_view)>& translate =
        [](std::string_view text){return std::string(text);}, bool console = false) {
    using namespace openxmb::xmb;
    struct ConsoleRow { std::string_view key, group, label; };
    static constexpr std::array<ConsoleRow,27> console_rows{{
        ConsoleRow{"theme.waves","Theme",""}, {"theme.particles","Theme",""}, {"theme.reset","Theme",""},
        {"display.screen.brightness","Display",""}, {"ui.scale","Display",""}, {"fps","Display",""}, {"debug.fps","Display",""},
        {"audio.master","Sound","Volume"}, {"audio.device","Sound",""}, {"audio.effects","Sound","Menu Sounds"}, {"audio.startup","Sound",""},
        {"network.wifi","Network",""}, {"network.wifi.join","Network",""}, {"network.bluetooth","Network",""}, {"network.bluetooth.devices","Network",""}, {"network.aeroplane","Network",""},
        {"date.format","Date and Time",""}, {"time.format","Date and Time",""}, {"time.offset","Date and Time",""},
        {"power.dim","Power Save",""}, {"power.reduced.motion","Power Save",""},
        {"language","System",""}, {"startup.intro","System",""}, {"games.rescan","System",""}, {"about","System",""}, {"licenses","System",""}, {"reset","System",""}
    }};
    auto& root=catalog.nodes.at("category.settings");
    root.children={"category.settings.system.update","category.settings.theme.settings"};root.default_selection=1;
    catalog.nodes.at("category.settings.system.update").action_id="desktop.system.update";
    for(const auto& entry:entries()) {
        const ConsoleRow* selected=nullptr;
        if(console) for(const auto& row:console_rows) if(row.key==entry.key) { selected=&row; break; }
        if(console && !selected) continue;
        const auto group_name=selected?std::string(selected->group):entry.group;
        const auto group=group_name=="Theme"?std::string{"category.settings.theme.settings"}:"category.settings.desktop."+group_name;
        if(!catalog.nodes.contains(group)) {
            CatalogNode node;node.id=group;node.kind=NodeKind::menu;node.label_key=group+".label";
            const std::vector<std::pair<std::string,std::string>> icons{
                {"System","category.settings.system.settings"},{"Display","category.settings.display.settings"},
                {"Sound","category.settings.sound.settings"},{"Music","category.settings.music.settings"},
                {"Date and Time","category.settings.date.and.time.settings"},{"Power Save","category.settings.power.save.settings"},
                {"Game","category.settings.game.settings"},{"Network","category.settings.network.settings"}};
            node.icon_ref="compat.icon.xmb.icon.001";
            for(const auto& [name,id]:icons)if(name==entry.group&&catalog.nodes.contains(id))node.icon_ref=catalog.nodes.at(id).icon_ref;
            node.action_id="navigate.children";
            catalog.strings[node.label_key]=group_name+" Settings";catalog.nodes.emplace(group,std::move(node));
            root.children.push_back(group);
        }
        CatalogNode node;node.id="category.settings.desktop."+entry.key;node.kind=NodeKind::setting;
        node.label_key=node.id+".label";node.description_key=node.id+".description";
        node.icon_ref="compat.icon.xmb.icon.022";node.action_id="desktop."+entry.key;
        catalog.strings[node.label_key]=selected&&!selected->label.empty()?std::string(selected->label):entry.label;catalog.strings[node.description_key]=entry.description;
        for(std::size_t i=0;i<entry.values.size();++i) {
            const auto key=node.id+".choice."+std::to_string(i);catalog.strings[key]=entry.values[i];
            if(entry.key=="language") {
                catalog.strings[key]=openxmb::localization::shell_locale::languages[i].second;
            }
            node.choices.push_back({key,key,entry.values[i]});
        }
        catalog.nodes.at(group).children.push_back(node.id);catalog.nodes.emplace(node.id,std::move(node));
    }
    if(console) {
        root.children={"category.settings.system.update","category.settings.theme.settings"};
        for(const auto& row:console_rows) {
            const auto group=row.group=="Theme"?std::string{"category.settings.theme.settings"}:"category.settings.desktop."+std::string(row.group);
            if(std::find(root.children.begin(),root.children.end(),group)==root.children.end()) root.children.push_back(group);
        }
        root.default_selection=1;
        for(const auto& group:root.children) if(group!="category.settings.system.update") {
            auto& children=catalog.nodes.at(group).children;
            children.erase(std::remove_if(children.begin(),children.end(),[&](const auto& id){
                if(id=="category.settings.theme.settings.colour" || id=="category.settings.theme.settings.background" || id=="category.settings.theme.settings.day.night") return false;
                return std::none_of(console_rows.begin(),console_rows.end(),[&](const auto& row){return id=="category.settings.desktop."+std::string(row.key);});
            }),children.end());
            // Rows follow console_rows, so a group opens on the row the
            // console lists first; the theme's own rows keep their place.
            const auto rank=[&](const std::string& id) {
                for(std::size_t i=0;i<console_rows.size();++i)
                    if(id=="category.settings.desktop."+std::string(console_rows[i].key)) return static_cast<long>(i);
                return -1L;
            };
            std::stable_sort(children.begin(),children.end(),[&](const auto& a,const auto& b){return rank(a)<rank(b);});
        }
    }
    catalog.strings["category.settings.system.settings.description"]="Adjust settings for the OpenXMB system.";
    // Replace the legacy console language selector with actual shipped locales.
    for(auto& [id,node]:catalog.nodes) if(node.action_id=="simulate.system.language") {
        node.action_id="desktop.language"; node.choices.clear(); node.default_selection=0;
        for(const auto& [value,name]:openxmb::localization::shell_locale::languages) {
            const auto key=id+".choice."+std::string(value);catalog.strings[key]=name;
            node.choices.push_back({key,key,std::string(value)});
        }
    }
    for(auto& [key,text]:catalog.strings) text=translate(text);
}
}
