// context.cpp
// Storage for the context globals declared in menu_api.h.
// Populated by the host before each right-click.

#include "menu_api.h"
#include "context.h"
#include <algorithm>
#include <cctype>

// ─────────────────────────────────────────────────────────────────────
// Global storage. The names match the `extern` declarations in
// menu_api.h. These are read by the consumer's menu.cpp at the moment
// RegisterMenu() runs.
// ─────────────────────────────────────────────────────────────────────

ClickTarget               target          = ClickTarget::File;
std::vector<std::string>  paths;
std::string               path;
std::string               extension;
std::vector<std::string>  extensions;
std::string               parentFolder;
int                       selectionCount  = 0;
Modifiers                 modifiers       = {false, false, false, false};
Point                     clickLocation   = {0, 0};


// ─────────────────────────────────────────────────────────────────────
// Predicate helpers (consumer-facing, declared in menu_api.h)
// ─────────────────────────────────────────────────────────────────────

bool extension_exists(std::initializer_list<const char*> exts) {
    for (const char* e : exts) {
        if (extension == e) return true;
    }
    return false;
}

// Case-insensitive glob: '*' = any run, '?' = one char. No bracket
// classes, no escape syntax — Windows path globs don't need them.
// Recursive with memoised backtrack; paths are short, no risk.
static bool glob_match(const char* pat, const char* str) {
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') ++pat;
            if (!*pat) return true;
            for (; *str; ++str) {
                if (glob_match(pat, str)) return true;
            }
            return glob_match(pat, str);
        }
        if (!*str) return false;
        if (*pat != '?') {
            unsigned char a = static_cast<unsigned char>(*pat);
            unsigned char b = static_cast<unsigned char>(*str);
            if (std::tolower(a) != std::tolower(b)) return false;
        }
        ++pat; ++str;
    }
    return !*str;
}

bool path_matches(const char* glob) {
    if (!glob) return false;
    return glob_match(glob, path.c_str());
}


// ─────────────────────────────────────────────────────────────────────
// Host-side helpers
// ─────────────────────────────────────────────────────────────────────

namespace _menu_internal {

    static std::string lowercase_extension_of(const std::string& p) {
        // Find the last '.' after the last path separator.
        auto sep    = p.find_last_of("\\/");
        auto dot    = p.find_last_of('.');
        if (dot == std::string::npos)               return {};
        if (sep != std::string::npos && dot < sep)  return {};
        std::string ext = p.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return ext;
    }

    static std::string parent_of(const std::string& p) {
        auto sep = p.find_last_of("\\/");
        if (sep == std::string::npos) return {};
        return p.substr(0, sep);
    }

    void reset_context() {
        target          = ClickTarget::File;
        paths           .clear();
        path            .clear();
        extension       .clear();
        extensions      .clear();
        parentFolder    .clear();
        selectionCount  = 0;
        modifiers       = {false, false, false, false};
        clickLocation   = {0, 0};
    }

    void set_paths(std::vector<std::string> p) {
        paths           = std::move(p);
        selectionCount  = static_cast<int>(paths.size());

        extensions.clear();
        for (const auto& s : paths) {
            extensions.push_back(lowercase_extension_of(s));
        }

        if (selectionCount == 1) {
            path         = paths[0];
            extension    = extensions[0];
            parentFolder = parent_of(paths[0]);
        } else {
            path.clear();
            extension.clear();
            // For multi-select, parentFolder is the common parent of
            // the first item (in practice all selected items share a
            // parent in shell semantics).
            parentFolder = paths.empty() ? std::string{}
                                         : parent_of(paths[0]);
        }
    }

    void set_click_location(int x, int y) {
        clickLocation = {x, y};
    }

    void set_modifiers(bool shift, bool ctrl, bool alt, bool win) {
        modifiers = {shift, ctrl, alt, win};
    }

    void set_target(ClickTarget t) {
        target = t;
    }

}  // namespace _menu_internal
