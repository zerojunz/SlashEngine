#include "slash/runtime/gameplay.hpp"

#include "slash/gameplay/host.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <utility>

namespace slash::runtime {
namespace {

bool identifier_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool source_file(std::string_view path) {
    return path.ends_with(".cpp") || path.ends_with(".hpp") || path.ends_with(".h");
}

// Adds the SLASH_COMPONENT declarations in a file's text.
void scan(std::string_view text,
          std::map<std::string, std::vector<std::string>, std::less<>>& found) {
    constexpr std::string_view kMacro = "SLASH_COMPONENT";
    for (std::size_t at = text.find(kMacro); at != std::string_view::npos;
         at = text.find(kMacro, at + kMacro.size())) {
        if (at > 0 && identifier_char(text[at - 1])) {
            continue;
        }
        // Not in a macro definition such as "#define DECLARE(T) SLASH_COMPONENT(T, ...)".
        const std::size_t line = text.rfind('\n', at);
        const std::size_t first =
            text.find_first_not_of(" \t", line == std::string_view::npos ? 0 : line + 1);
        if (first != std::string_view::npos && text[first] == '#') {
            continue;
        }
        std::size_t i = at + kMacro.size();
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
            ++i;
        }
        if (i >= text.size() || text[i] != '(') {
            continue;
        }
        const std::size_t close = text.find(')', i);
        if (close == std::string_view::npos) {
            break;
        }
        // Name, then fields, each an identifier between commas.
        std::vector<std::string> parts;
        bool valid = true;
        std::string_view inside = text.substr(i + 1, close - i - 1);
        while (valid) {
            const std::size_t comma = inside.find(',');
            std::string_view part = inside.substr(0, comma);
            while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front())) != 0) {
                part.remove_prefix(1);
            }
            while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back())) != 0) {
                part.remove_suffix(1);
            }
            valid = !part.empty() && std::ranges::all_of(part, identifier_char) &&
                    std::isdigit(static_cast<unsigned char>(part.front())) == 0;
            parts.emplace_back(part);
            if (comma == std::string_view::npos) {
                break;
            }
            inside.remove_prefix(comma + 1);
        }
        // "SLASH_COMPONENT(Type, ...)", as in the macro's own documentation, declares nothing.
        if (valid && !parts.empty()) {
            std::string name = std::move(parts.front());
            parts.erase(parts.begin());
            found[std::move(name)] = std::move(parts);
        }
    }
}

} // namespace

std::vector<DeclaredComponent> declared_components(std::string_view folder,
                                                   const Project& project) {
    std::map<std::string, std::vector<std::string>, std::less<>> found;
    for (const std::string& source : project.gameplay_sources) {
        const std::string path = join_path(folder, source);
        if (auto text = platform::read_file(path); text && source_file(source)) {
            scan(text.value(), found);
            continue;
        }
        const auto files = platform::list_files(path);
        if (!files) {
            continue;
        }
        for (const std::string& file : files.value()) {
            if (!source_file(file)) {
                continue;
            }
            if (auto text = platform::read_file(join_path(path, file)); text) {
                scan(text.value(), found);
            }
        }
    }
    std::vector<DeclaredComponent> out;
    out.reserve(found.size());
    for (auto& [name, fields] : found) {
        out.push_back({name, std::move(fields)});
    }
    return out;
}

GameplayTypes add_gameplay_types(world::ComponentRegistry& registry, std::string_view folder,
                                 const Project& project, bool load_library) {
    if (project.gameplay_sources.empty()) {
        return GameplayTypes::None;
    }
    if (load_library && platform::can_load_libraries()) {
        world::World scratch(registry);
        gameplay::Host host(registry, gameplay::library_path(folder));
        if (host.attach(scratch).error.empty() && host.loaded()) {
            return GameplayTypes::Loaded;
        }
    }
    for (DeclaredComponent& component : declared_components(folder, project)) {
        if (registry.find(component.name) == nullptr) {
            registry.add_stand_in(std::move(component.name), std::move(component.fields));
        }
    }
    return GameplayTypes::Declared;
}

} // namespace slash::runtime
