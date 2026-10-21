#include "slash/soundtrack/plan.hpp"

#include "slash/storyboard/storyboard.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::soundtrack {
namespace {

constexpr std::string_view kSceneSuffix = ".scene.json";

std::string_view kind_name(std::string_view id) {
    return id.substr(0, id.find('/'));
}

std::string text_of(const json::Value& entry, std::string_view field) {
    const json::Value* value = entry.find(field);
    return value != nullptr && value->is_string() ? value->as_string() : std::string();
}

std::vector<std::string> strings(const json::Value& entry, std::string_view field) {
    std::vector<std::string> out;
    if (const json::Value* list = entry.find(field); list != nullptr && list->is_array()) {
        for (const json::Value& item : list->as_array()) {
            if (item.is_string()) {
                out.push_back(item.as_string());
            }
        }
    }
    return out;
}

// Whether a link names the path, or a folder the path is in.
bool covers(std::string_view link, std::string_view path) {
    return path.starts_with(link) && (path.size() == link.size() || path[link.size()] == '/');
}

// Text as an ID's name: lowercase letters, digits, '_' and '-', with the rest as one '_'.
std::string slug(std::string_view text) {
    std::string out;
    for (char c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out.push_back(c);
        } else if (!out.empty() && out.back() != '_') {
            out.push_back('_');
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    if (out.size() > storyboard::kMaxNameLength) {
        out.resize(storyboard::kMaxNameLength);
    }
    return out.empty() ? "music" : out;
}

// "level_2" as "Level 2".
std::string title(std::string_view name) {
    std::string out(name);
    std::ranges::replace(out, '_', ' ');
    std::ranges::replace(out, '-', ' ');
    if (!out.empty() && out.front() >= 'a' && out.front() <= 'z') {
        out.front() = static_cast<char>(out.front() - 'a' + 'A');
    }
    return out;
}

// Which subjects pick a cue's theme first: where it is set, then who.
int rank(std::string_view id) {
    const std::string_view kind = kind_name(id);
    if (kind == "place") {
        return 0;
    }
    if (kind == "faction") {
        return 1;
    }
    return kind == "character" ? 2 : 3;
}

class Planner {
public:
    explicit Planner(const json::Object& entries) : entries_(entries) {
        for (const auto& [id, entry] : entries) {
            taken_.insert(id);
            const std::string_view kind = kind_name(id);
            if (kind == "theme") {
                themes_.push_back(id);
            } else if (kind == "cue") {
                for (std::string& scene : strings(entry, "scenes")) {
                    cue_scenes_.push_back(std::move(scene));
                }
                for (std::string& beat : strings(entry, "beats")) {
                    covered_beats_.insert(std::move(beat));
                }
                for (std::string& state : strings(entry, "states")) {
                    covered_states_.insert(std::move(state));
                }
            }
        }
    }

    void scene(const PlanScene& scene) {
        if (std::ranges::any_of(
                cue_scenes_, [&](const std::string& link) { return covers(link, scene.path); })) {
            return;
        }
        std::string_view stem = scene.path;
        stem = stem.substr(stem.rfind('/') + 1);
        if (stem.ends_with(kSceneSuffix)) {
            stem.remove_suffix(kSceneSuffix.size());
        }
        // The beats set in this scene that no cue plays during yet.
        json::Array beats;
        const auto take = [&](const std::string& beat) {
            if (kind_name(beat) == "beat" && entries_.contains(beat) &&
                covered_beats_.insert(beat).second) {
                beats.emplace_back(beat);
            }
        };
        for (const std::string& id : scene.story_ids) {
            take(id);
        }
        for (const auto& [id, entry] : entries_) {
            if (kind_name(id) == "beat" &&
                std::ranges::any_of(strings(entry, "links"), [&](const std::string& link) {
                    return covers(link, scene.path);
                })) {
                take(id);
            }
        }

        const std::string name = slug(stem);
        json::Value set;
        set["name"] = title(name);
        set["summary"] = "Plays in " + scene.path + ".";
        set["scenes"] = json::Array{scene.path};
        if (!beats.empty()) {
            set["beats"] = std::move(beats);
        }
        add_theme(set, subjects(scene.story_ids), scene.path);
        add(name, std::move(set));
    }

    void beats() {
        for (const auto& [id, entry] : entries_) {
            if (kind_name(id) != "beat" || !covered_beats_.insert(id).second) {
                continue;
            }
            const std::string_view name = std::string_view(id).substr(id.find('/') + 1);
            std::string label = text_of(entry, "name");
            json::Value set;
            set["name"] = label.empty() ? title(name) : std::move(label);
            set["summary"] = "Plays during " + id + ".";
            set["beats"] = json::Array{id};
            add_theme(set, subjects({id}), {});
            add(std::string(name), std::move(set));
        }
    }

    void state(const std::string& state) {
        if (!covered_states_.insert(state).second) {
            return;
        }
        const std::string name = slug(state);
        json::Value set;
        set["name"] = title(name);
        set["summary"] = "Plays in the " + state + " state.";
        set["states"] = json::Array{state};
        add(name, std::move(set));
    }

    Plan finish() && {
        return {.edits = std::move(edits_),
                .unthemed = std::vector<std::string>(unthemed_.begin(), unthemed_.end())};
    }

private:
    [[nodiscard]] const json::Value* find(std::string_view id) const {
        const auto it = entries_.find(std::string(id));
        return it != entries_.end() ? &it->second : nullptr;
    }

    // The entries ids name and those they name in turn (a beat's place and characters, a
    // character's home and faction, a place's region), places first, then factions, then
    // characters, each kind in the order found.
    [[nodiscard]] std::vector<std::string> subjects(const std::vector<std::string>& ids) const {
        std::vector<std::string> found;
        const auto add = [&](const std::string& id) {
            if (!id.empty() && std::ranges::find(found, id) == found.end()) {
                found.push_back(id);
            }
        };
        for (const std::string& id : ids) {
            add(id);
        }
        // found grows as it is read.
        for (std::size_t next = 0; next < found.size();) {
            const std::string id = found[next++];
            const json::Value* entry = find(id);
            if (entry == nullptr) {
                continue;
            }
            const std::string_view kind = kind_name(id);
            if (kind == "beat") {
                add(text_of(*entry, "place"));
                for (const std::string& character : strings(*entry, "characters")) {
                    add(character);
                }
            } else if (kind == "character") {
                add(text_of(*entry, "home"));
                add(text_of(*entry, "faction"));
            } else if (kind == "place") {
                add(text_of(*entry, "region"));
            }
        }
        std::ranges::stable_sort(found, {}, [](const std::string& id) { return rank(id); });
        return found;
    }

    // Sets the cue's theme: the one whose links name the scene, else the first subject's.
    // A cue left without one records the subject a theme should be written for.
    void add_theme(json::Value& set, const std::vector<std::string>& about,
                   std::string_view scene) {
        for (const std::string& theme : themes_) {
            if (!scene.empty() &&
                std::ranges::any_of(strings(*find(theme), "links"),
                                    [&](const std::string& link) { return covers(link, scene); })) {
                set["theme"] = theme;
                return;
            }
        }
        for (const std::string& subject : about) {
            for (const std::string& theme : themes_) {
                const std::vector<std::string> stands_for = strings(*find(theme), "subjects");
                if (std::ranges::find(stands_for, subject) != stands_for.end()) {
                    set["theme"] = theme;
                    return;
                }
            }
        }
        if (!about.empty() && rank(about.front()) < 3) {
            unthemed_.insert(about.front());
        }
    }

    // A new cue named name, or name_2 and so on if that ID is taken.
    void add(const std::string& name, json::Value set) {
        std::string id = "cue/" + name;
        for (int n = 2; taken_.contains(id); ++n) {
            const std::string suffix = "_" + std::to_string(n);
            id = "cue/" + name.substr(0, storyboard::kMaxNameLength - suffix.size()) + suffix;
        }
        taken_.insert(id);
        json::Value edit;
        edit["id"] = id;
        edit["set"] = std::move(set);
        edit["version"] = nullptr;
        edits_.push_back(std::move(edit));
    }

    const json::Object& entries_;
    std::vector<std::string> themes_;
    std::vector<std::string> cue_scenes_;
    std::set<std::string> covered_beats_;
    std::set<std::string> covered_states_;
    std::set<std::string> taken_;
    std::set<std::string> unthemed_;
    json::Array edits_;
};

} // namespace

Plan plan_cues(const json::Object& entries, const std::vector<PlanScene>& scenes,
               const std::vector<std::string>& states) {
    Planner planner(entries);
    for (const PlanScene& scene : scenes) {
        planner.scene(scene);
    }
    planner.beats();
    for (const std::string& state : states) {
        planner.state(state);
    }
    return std::move(planner).finish();
}

} // namespace slash::soundtrack
