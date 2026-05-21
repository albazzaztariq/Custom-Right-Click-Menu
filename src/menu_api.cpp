// menu_api.cpp
// Macro helpers + tree builder + handler registry.

#include "menu_api.h"
#include "menu_node.h"

namespace _menu_internal {

    MenuNode rootMenu;
    thread_local MenuNode* currentMenu = &rootMenu;
    std::vector<std::function<void()>> handlerRegistry;

    void reset_menu_state() {
        rootMenu = MenuNode{};
        rootMenu.handlerId = -1;
        currentMenu        = &rootMenu;
        handlerRegistry.clear();
    }

    SelectionTag selection_helper(const char* text) {
        return SelectionTag{text, nullptr, nullptr};
    }
    SelectionTag selection_helper(const char* text, const char* shortcut) {
        return SelectionTag{text, shortcut, nullptr};
    }
    SelectionTag selection_helper(const char* text, const char* shortcut,
                                  const char* icon) {
        return SelectionTag{text, shortcut, icon};
    }

    SubmenuTag submenu_helper(const char* text) {
        return SubmenuTag{text, nullptr};
    }
    SubmenuTag submenu_helper(const char* text, const char* icon) {
        return SubmenuTag{text, icon};
    }

    void push_separator() {
        MenuNode sep;
        sep.is_sep    = true;
        sep.handlerId = -1;
        currentMenu->children.push_back(std::move(sep));
    }

    void operator+(SelectionTag tag, std::function<void()> body) {
        int id = static_cast<int>(handlerRegistry.size());
        handlerRegistry.push_back(std::move(body));

        MenuNode leaf;
        leaf.text      = tag.text;
        if (tag.shortcut) leaf.shortcut  = tag.shortcut;
        if (tag.icon)     leaf.icon_path = tag.icon;
        leaf.handlerId = id;
        currentMenu->children.push_back(std::move(leaf));
    }

    void operator+(SubmenuTag tag, std::function<void()> body) {
        MenuNode submenu;
        submenu.text      = tag.text;
        if (tag.icon) submenu.icon_path = tag.icon;
        submenu.handlerId = -1;
        currentMenu->children.push_back(std::move(submenu));

        MenuNode* newParent = &currentMenu->children.back();
        MenuNode* saved     = currentMenu;
        currentMenu         = newParent;
        body();
        currentMenu = saved;
    }

}  // namespace _menu_internal
