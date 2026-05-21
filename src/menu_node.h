// menu_node.h
// The tree-node type the framework builds when the consumer's
// RegisterMenu() runs, plus the registry where Selection bodies are
// stored for later dispatch.
//
// Not consumer-facing. Lives in the host DLL.

#pragma once

#include <string>
#include <vector>
#include <functional>

struct MenuNode {
    std::string             text;
    std::string             shortcut;     // empty = no shortcut shown
    std::string             icon_path;    // empty = no icon
    std::vector<MenuNode>   children;     // empty for leaf Selections
    int                     handlerId = -1;  // -1 = submenu OR separator
    bool                    is_sep = false;  // separator row

    bool is_separator() const { return is_sep; }
    bool is_selection() const { return !is_sep && handlerId >= 0; }
    bool is_submenu()   const { return !is_sep && handlerId <  0; }
};

namespace _menu_internal {

    extern thread_local MenuNode* currentMenu;
    extern MenuNode rootMenu;
    extern std::vector<std::function<void()>> handlerRegistry;

    void reset_menu_state();

}  // namespace _menu_internal
