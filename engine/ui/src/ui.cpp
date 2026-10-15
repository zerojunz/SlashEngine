#include "slash/ui/ui.hpp"

#include "slash/core/text.hpp"
#include "slash/world/scene.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::ui {
namespace {

struct State {
    std::unique_ptr<text::Fonts> fonts;
    Settings settings;
};

const State* state_of(const world::World& world) {
    const auto* found = world.ecs().ctx().find<std::shared_ptr<State>>();
    return found != nullptr ? found->get() : nullptr;
}

template <typename Map>
void check_names(std::vector<Problem>& problems, const std::string& id, std::string_view field,
                 const Map& overrides, const ThemeItems& items, std::string_view type,
                 const auto& names_of) {
    const auto& known = names_of(items);
    std::vector<std::string_view> names;
    names.reserve(known.size());
    for (const auto& [name, value] : known) {
        names.emplace_back(name);
    }
    for (const auto& [name, value] : overrides) {
        if (known.contains(name)) {
            continue;
        }
        std::string message = std::string(type) + " has no ";
        message += field.substr(std::string_view("theme_override_").size());
        message += " item \"" + name + "\"";
        if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        } else if (names.empty()) {
            message += "; it has none";
        }
        problems.push_back(
            {.id = id,
             .path = "/components/Control/" + std::string(field) + "/" + json::pointer_escape(name),
             .message = std::move(message)});
    }
}

} // namespace

Transform Transform::operator*(const Transform& o) const {
    return {.a = (a * o.a) + (c * o.b),
            .b = (b * o.a) + (d * o.b),
            .c = (a * o.c) + (c * o.d),
            .d = (b * o.c) + (d * o.d),
            .tx = (a * o.tx) + (c * o.ty) + tx,
            .ty = (b * o.tx) + (d * o.ty) + ty};
}

void install(world::World& world, asset::Assets& assets, const Settings& settings) {
    if (auto* found = world.ecs().ctx().find<std::shared_ptr<State>>(); found != nullptr) {
        (*found)->settings = settings;
        return;
    }
    text::add_asset_types(assets);
    auto state = std::make_shared<State>();
    state->fonts = std::make_unique<text::Fonts>(assets);
    state->settings = settings;
    world.ecs().ctx().emplace<std::shared_ptr<State>>(std::move(state));
}

void set_settings(world::World& world, const Settings& settings) {
    if (auto* found = world.ecs().ctx().find<std::shared_ptr<State>>(); found != nullptr) {
        (*found)->settings = settings;
    }
}

Settings settings(const world::World& world) {
    const State* state = state_of(world);
    return state != nullptr ? state->settings : Settings{};
}

text::Fonts* fonts(const world::World& world) {
    const State* state = state_of(world);
    return state != nullptr ? state->fonts.get() : nullptr;
}

bool is_control(const world::World& world, entt::entity entity) {
    return world.valid(entity) &&
           world.ecs().any_of<world::Control, world::Label, world::ColorRect>(entity);
}

const ThemeItems& default_theme(std::string_view type) {
    // Godot's default theme.
    static const std::map<std::string, ThemeItems, std::less<>> kThemes = [] {
        std::map<std::string, ThemeItems, std::less<>> themes;
        ThemeItems& label = themes["Label"];
        label.colors = {{"font_color", {1.0F, 1.0F, 1.0F, 1.0F}},
                        {"font_shadow_color", {0.0F, 0.0F, 0.0F, 0.0F}},
                        {"font_outline_color", {0.0F, 0.0F, 0.0F, 1.0F}}};
        label.constants = {{"line_spacing", 3},    {"paragraph_spacing", 0},
                           {"outline_size", 0},    {"shadow_offset_x", 1},
                           {"shadow_offset_y", 1}, {"shadow_outline_size", 1}};
        label.fonts = {{"font", ""}};
        label.font_sizes = {{"font_size", text::kDefaultFontSize}};
        themes["ColorRect"];
        themes["Control"];
        return themes;
    }();
    const auto found = kThemes.find(type);
    return found != kThemes.end() ? found->second : kThemes.find("Control")->second;
}

const Placed* Layout::find(entt::entity entity) const {
    const auto found = index.find(entity);
    return found != index.end() ? &controls[found->second] : nullptr;
}

const LabelText* Layout::label(entt::entity entity) const {
    const auto found = labels.find(entity);
    return found != labels.end() ? &found->second : nullptr;
}

std::vector<Problem> check(const world::World& world, text::Fonts& fonts) {
    std::vector<Problem> problems = layout(world, fonts, settings(world).viewport).problems;
    // Fonts reported once each.
    std::map<std::string, std::string, std::less<>> font_errors;
    const auto view = world.ecs().view<const world::Control>();
    std::vector<entt::entity> entities(view.begin(), view.end());
    std::ranges::sort(
        entities, [&](entt::entity a, entt::entity b) { return world.id_of(a) < world.id_of(b); });
    for (const entt::entity entity : entities) {
        const std::string id(world.id_of(entity));
        if (id.empty()) {
            continue;
        }
        const world::Control& control = view.get<const world::Control>(entity);
        std::string_view type = "Control";
        if (world.ecs().all_of<world::Label>(entity)) {
            type = "Label";
        } else if (world.ecs().all_of<world::ColorRect>(entity)) {
            type = "ColorRect";
        }
        const ThemeItems& items = default_theme(type);
        check_names(problems, id, "theme_override_colors", control.theme_override_colors, items,
                    type, [](const ThemeItems& t) -> const auto& { return t.colors; });
        check_names(problems, id, "theme_override_constants", control.theme_override_constants,
                    items, type, [](const ThemeItems& t) -> const auto& { return t.constants; });
        check_names(problems, id, "theme_override_fonts", control.theme_override_fonts, items, type,
                    [](const ThemeItems& t) -> const auto& { return t.fonts; });
        check_names(problems, id, "theme_override_font_sizes", control.theme_override_font_sizes,
                    items, type, [](const ThemeItems& t) -> const auto& { return t.font_sizes; });
        for (const auto& [name, path] : control.theme_override_fonts) {
            auto error = font_errors.find(path);
            if (error == font_errors.end()) {
                auto loaded = fonts.metrics(path, text::kDefaultFontSize);
                error = font_errors.emplace(path, loaded ? "" : loaded.error().message).first;
            }
            if (!error->second.empty()) {
                problems.push_back({.id = id,
                                    .path = "/components/Control/theme_override_fonts/" +
                                            json::pointer_escape(name),
                                    .message = error->second});
            }
        }
    }
    return problems;
}

std::vector<json::ValidationError> check_scene(const world::ComponentRegistry& components,
                                               const json::Value& scene,
                                               const world::FileReader& reader) {
    std::vector<json::ValidationError> errors;
    // The assets outlive the world, whose fonts hold handles to them.
    asset::Assets assets(asset::FileSource{
        .read = [&](std::string_view path) -> Result<std::string> {
            if (!reader) {
                return make_error(ErrorCode::NotFound, "no files to read");
            }
            return reader(path);
        },
        .stamp = [](std::string_view /*path*/) { return std::optional<asset::FileStamp>(); },
    });
    world::World world(components);
    world.set_file_reader(reader);
    if (!world::load_scene(world, scene)) {
        return errors;
    }
    install(world, assets);
    for (Problem& problem : check(world, *fonts(world))) {
        errors.push_back({"/entities/" + json::pointer_escape(problem.id) + problem.path,
                          std::move(problem.message)});
    }
    return errors;
}

} // namespace slash::ui
