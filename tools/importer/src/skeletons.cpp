#include "skeletons.hpp"

#include "slash/runtime/gameplay.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace slash::importer {

Skeletons::Skeletons(const Existing& existing)
    : existing_(existing), folder_(gameplay_folder(existing.project)) {
    const std::string folder = folder_ + "/imported/";
    for (const auto& [path, text] : existing.code) {
        if (!path.starts_with(folder)) {
            for (const runtime::DeclaredComponent& component : runtime::declared_in(text)) {
                components_taken_.insert(component.name);
            }
        }
    }
}

Skeleton& Skeletons::add(const std::string& source, Script script,
                         world::ComponentRegistry& components) {
    Skeleton skeleton{.script = std::move(script), .code = {}, .kept = false};
    Script& code = skeleton.script;
    // A file name of a folder and the script's, then a number, when that is taken.
    if (namespaces_taken_.contains(code.ns)) {
        const std::size_t slash = source.rfind('/');
        const std::string parent =
            slash == std::string::npos ? "root" : file_stem(source.substr(0, slash));
        std::string base = cpp_identifier(snake_case(parent) + "_" + code.ns);
        std::ranges::replace(base, '-', '_');
        code.ns = base;
        for (int n = 2; namespaces_taken_.contains(code.ns); ++n) {
            code.ns = base + "_" + std::to_string(n);
        }
    }
    namespaces_taken_.insert(code.ns);
    skeleton.code = folder_ + "/imported/" + code.ns + ".cpp";
    code.system = code.ns;
    while (is_engine_system(code.system) || systems_taken_.contains(code.system)) {
        code.system += "_script";
    }
    systems_taken_.insert(code.system);
    while (components.find(code.component) != nullptr ||
           components_taken_.contains(code.component)) {
        code.component += "Script";
    }
    // A skeleton the destination has keeps its component, and its fields take values.
    if (const auto kept = existing_.code.find(skeleton.code); kept != existing_.code.end()) {
        skeleton.kept = true;
        const auto declared = runtime::declared_in(kept->second);
        if (!declared.empty() && components.find(declared.front().name) == nullptr) {
            code.component = declared.front().name;
            std::erase_if(code.fields, [&](const ScriptField& field) {
                return !contains(declared.front().fields, field.name);
            });
        } else {
            code.node = false; // its component is unknown: scenes leave it out
        }
    }
    components_taken_.insert(code.component);
    if (code.node) {
        std::vector<std::string> fields;
        fields.reserve(code.fields.size());
        for (const ScriptField& field : code.fields) {
            fields.push_back(field.name);
        }
        components.add_stand_in(code.component, std::move(fields))
            .set_doc("What the " + std::string(engine_name(code.source)) +
                     (code.source == ScriptSource::UnrealCpp ? " class " : " script ") + code.path +
                     " keeps.");
    }
    return skeletons_.insert_or_assign(source, std::move(skeleton)).first->second;
}

Skeleton* Skeletons::find(const std::string& source) {
    const auto found = skeletons_.find(source);
    return found != skeletons_.end() ? &found->second : nullptr;
}

const Skeleton* Skeletons::find(const std::string& source) const {
    const auto found = skeletons_.find(source);
    return found != skeletons_.end() ? &found->second : nullptr;
}

std::string Skeletons::write(const std::function<void(std::string, std::string)>& add_file,
                             Problems& problems) const {
    if (skeletons_.empty()) {
        return {};
    }
    std::vector<std::string> registered;
    for (const auto& [source, entry] : skeletons_) {
        const Script& code = entry.script;
        if (!entry.kept) {
            add_file(entry.code, skeleton(code));
        }
        // A skeleton kept as the agent left it registers what it has, if anything.
        const bool registers =
            entry.kept ? existing_.code.at(entry.code).find("void register_" + code.ns + "(") !=
                             std::string::npos
                       : code.node;
        if (registers) {
            registered.push_back(code.ns);
        }
    }
    const std::string scripts_file = folder_ + "/imported/scripts.cpp";
    add_file(scripts_file, registration(registered));
    for (const auto& [path, text] : existing_.code) {
        if (path != scripts_file && text.find("SLASH_GAME_MODULE(") != std::string::npos) {
            if (text.find("register_scripts") == std::string::npos) {
                problems.add("game module", path,
                             "call imported::register_scripts(game) from its SLASH_GAME_MODULE "
                             "to register the scripts' components and systems");
            }
            return {};
        }
    }
    std::string module = existing_.code.contains(folder_ + "/game.cpp")
                             ? folder_ + "/imported/module.cpp"
                             : folder_ + "/game.cpp";
    add_file(module, module_source());
    return module;
}

} // namespace slash::importer
