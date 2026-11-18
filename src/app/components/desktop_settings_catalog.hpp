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
        {"Theme","theme.waves","Wave","Show the animated background ribbon.",{"On","Off"}},
        {"Theme","theme.particles","Sparkles","Show background particles.",{"On","Off"}},
        {"Display","display.screen.brightness","Screen Brightness","Hardware panel backlight level.",{"10","20","30","40","50","60","70","80","90","100"}},
        {"Display","debug.fps","Show FPS","Display the shell frame rate.",{"Off","On"}},
        {"Display","status.analog.clock","Analog Clock","Show an analog face in the status bar.",{"Off","On"}},
        {"Sound","audio.master","Volume","System audio output volume.",{"0","10","20","30","40","50","60","70","80","90","100"}},
        {"Sound","audio.device","Audio Output Device","Select an available playback device."},
        {"Sound","audio.effects","Menu Sounds","Volume of navigation and confirmation sounds.",{"0","25","50","75","100"}},
        {"Sound","audio.startup","Startup Sound","Play the startup cue in full.",{"On","Off"}},
        {"Network","network.wifi","Wi-Fi","Turn the Wi-Fi radio on or off.",{"On","Off"}},
        {"Network","network.wifi.join","Join Wi-Fi Network","Scan for and connect to a Wi-Fi network."},
        {"Network","network.bluetooth","Bluetooth","Turn the Bluetooth radio on or off.",{"On","Off"}},
        {"Network","network.bluetooth.devices","Bluetooth Devices","Scan, pair and connect Bluetooth devices."},
        {"Network","network.aeroplane","Aeroplane Mode","Disable all radios (Wi-Fi and Bluetooth).",{"Off","On"}},
        {"Date and Time","date.format","Date Format","Order of year, month and day.",{"DD/MM/YYYY","MM/DD/YYYY","YYYY-MM-DD"}},
        {"Date and Time","time.format","Time Format","Choose a 12-hour or 24-hour clock.",{"24-hour","12-hour"}},
        {"Date and Time","time.offset","Time Zone Offset","Hours added to local system time for display.",{"-12","-11","-10","-9","-8","-7","-6","-5","-4","-3","-2","-1","0","1","2","3","4","5","5.5","6","7","8","9","9.5","10","11","12","13"}},
        {"Power Save","power.dim","Idle Dimming","Dim the scene after inactivity.",{"Off","1 minute","5 minutes","10 minutes"}},
        {"Power Save","power.reduced.motion","Reduced Motion","Reduce background movement and sparkles.",{"Off","On"}},
        {"System","language","Language","Preferred interface language.",openxmb::localization::shell_locale::language_values()},
        {"System","games.rescan","Rescan Game Library","Refresh the configured game library."},
    };
    return data;
}
inline void populate(openxmb::xmb::Catalog& catalog,
    const std::function<std::string(std::string_view)>& translate =
        [](std::string_view text){return std::string(text);}, bool /*console*/ = true) {
    using namespace openxmb::xmb;
    auto& root=catalog.nodes.at("category.settings");
    root.children={"category.settings.system.update","category.settings.theme.settings"};root.default_selection=1;
    catalog.nodes.at("category.settings.system.update").action_id="desktop.system.update";
    const auto& rows = entries();
    for(const auto& entry:rows) {
        const auto group=entry.group=="Theme"?std::string{"category.settings.theme.settings"}:"category.settings.desktop."+entry.group;
        if(!catalog.nodes.contains(group)) {
            CatalogNode node;node.id=group;node.kind=NodeKind::menu;node.label_key=group+".label";
            const std::vector<std::pair<std::string,std::string>> icons{
                {"System","category.settings.system.settings"},{"Display","category.settings.display.settings"},
                {"Sound","category.settings.sound.settings"},{"Date and Time","category.settings.date.and.time.settings"},
                {"Power Save","category.settings.power.save.settings"},{"Network","category.settings.network.settings"}};
            node.icon_ref="compat.icon.xmb.icon.001";
            for(const auto& [name,id]:icons)if(name==entry.group&&catalog.nodes.contains(id))node.icon_ref=catalog.nodes.at(id).icon_ref;
            node.action_id="navigate.children";
            catalog.strings[node.label_key]=entry.group+" Settings";catalog.nodes.emplace(group,std::move(node));
            root.children.push_back(group);
        }
        CatalogNode node;node.id="category.settings.desktop."+entry.key;node.kind=NodeKind::setting;
        node.label_key=node.id+".label";node.description_key=node.id+".description";
        node.icon_ref="compat.icon.xmb.icon.022";node.action_id="desktop."+entry.key;
        catalog.strings[node.label_key]=entry.label;catalog.strings[node.description_key]=entry.description;
        for(std::size_t i=0;i<entry.values.size();++i) {
            const auto key=node.id+".choice."+std::to_string(i);catalog.strings[key]=entry.values[i];
            if(entry.key=="language") {
                catalog.strings[key]=openxmb::localization::shell_locale::languages[i].second;
            }
            node.choices.push_back({key,key,entry.values[i]});
        }
        catalog.nodes.at(group).children.push_back(node.id);catalog.nodes.emplace(node.id,std::move(node));
    }
    for(const auto& group:root.children) if(group!="category.settings.system.update") {
        auto& children=catalog.nodes.at(group).children;
        children.erase(std::remove_if(children.begin(),children.end(),[&](const auto& id){
            if(id=="category.settings.theme.settings.colour" || id=="category.settings.theme.settings.background" || id=="category.settings.theme.settings.day.night") return false;
            return std::none_of(rows.begin(),rows.end(),[&](const auto& row){return id=="category.settings.desktop."+row.key;});
        }),children.end());
        const auto rank=[&](const std::string& id) {
            for(std::size_t i=0;i<rows.size();++i)
                if(id=="category.settings.desktop."+rows[i].key) return static_cast<long>(i);
            return -1L;
        };
        std::stable_sort(children.begin(),children.end(),[&](const auto& a,const auto& b){return rank(a)<rank(b);});
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
