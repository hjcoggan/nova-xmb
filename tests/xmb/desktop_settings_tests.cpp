#include "../../src/app/components/desktop_settings_catalog.hpp"
#include "openxmb/xmb/settings_controller.hpp"
#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>

void require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
int main(int argc,char** argv) try {
    require(argc==2,"catalog fixture path required");
    auto loaded=openxmb::xmb::load_catalog_file(argv[1]);require(static_cast<bool>(loaded),"catalog loads");
    auto catalog=std::move(*loaded.value);
    const auto system_icon=catalog.nodes.at("category.settings.system.settings").icon_ref;
    const auto update_icon=catalog.nodes.at("category.settings.system.update").icon_ref;
    app::desktop_settings::populate(catalog);
    const auto& root=catalog.nodes.at("category.settings");
    require(root.children.front()=="category.settings.system.update","System Update remains visible");
    require(catalog.nodes.at(root.children.front()).icon_ref==update_icon,"update artwork retained");
    require(catalog.nodes.at("category.settings.desktop.System").icon_ref==system_icon,"system artwork retained");
    require(catalog.nodes.contains("category.settings.printer.settings"),"future pane assets and source definitions preserved");
    auto made=openxmb::xmb::SettingsCatalogController::create(catalog);require(static_cast<bool>(made),"desktop tree creates controller");
    auto& controller=*made.controller;
    const auto group=std::find(root.children.begin(),root.children.end(),"category.settings.desktop.System");
    require(group!=root.children.end(),"system group exposed");
    const auto selection=static_cast<std::size_t>(group-root.children.begin());
    require(static_cast<bool>(controller.set_selection(selection,1)),"select group");
    require(static_cast<bool>(controller.activate(2)),"activate group");
    require(controller.route().id==*group,"nested settings open selected group");
    require(static_cast<bool>(controller.set_selection(3,3)),"select child");
    require(static_cast<bool>(controller.back(4)),"return to parent");
    require(controller.route().selection==selection,"Back restores parent choice");
    require(controller.route().id=="category.settings","Back returns to category");
    std::set<std::string> actions;
    for(const auto& entry:app::desktop_settings::entries()) {
        const auto& node=catalog.nodes.at("category.settings.desktop."+entry.key);
        require(actions.insert(node.action_id).second,"desktop actions unique");
        require(!catalog.text(node.label_key).empty(),"every row has readable text");
        require(!node.action_id.starts_with("simulate."),"desktop row never simulates successful config writes");
    }
    // Console mode keeps each group's rows in the curated order, so a group
    // opens on the row the console lists first.
    {
        auto console_loaded=openxmb::xmb::load_catalog_file(argv[1]);require(static_cast<bool>(console_loaded),"catalog loads again");
        auto console_catalog=std::move(*console_loaded.value);
        app::desktop_settings::populate(console_catalog,[](std::string_view text){return std::string(text);},true);
        const auto& display=console_catalog.nodes.at("category.settings.desktop.Display").children;
        require(display.size()==3,"console Display has only the rows a handheld needs");
        require(display[0]=="category.settings.desktop.display.screen.brightness","Display opens on Screen Brightness");
        require(display[1]=="category.settings.desktop.debug.fps","Show FPS second");
        require(display[2]=="category.settings.desktop.status.analog.clock","Analog Clock third");
        const auto& sound=console_catalog.nodes.at("category.settings.desktop.Sound").children;
        require(!sound.empty()&&sound[0]=="category.settings.desktop.audio.master","Sound opens on Volume");
    }
    std::cout<<"desktop_settings_tests: retained assets, live tree and parent navigation passed\n";
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
