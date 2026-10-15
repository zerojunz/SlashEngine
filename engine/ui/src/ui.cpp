#include "slash/ui/ui.hpp"

#include "internal.hpp"

#include "slash/core/text.hpp"
#include "slash/render/image.hpp"
#include "slash/world/scene.hpp"

#include <algorithm>
#include <array>
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

// What check() reports a control's override names against: the items of every class it is.
struct Known {
    ThemeItems items;
    std::string subject; // "Label has", "Label and Panel have"
};

Known known_items(const std::vector<std::string_view>& classes) {
    Known known;
    for (std::size_t i = 0; i < classes.size(); ++i) {
        const ThemeItems& items = default_theme(classes[i]);
        known.items.colors.insert(items.colors.begin(), items.colors.end());
        known.items.constants.insert(items.constants.begin(), items.constants.end());
        known.items.fonts.insert(items.fonts.begin(), items.fonts.end());
        known.items.font_sizes.insert(items.font_sizes.begin(), items.font_sizes.end());
        known.items.styles.insert(items.styles.begin(), items.styles.end());
        if (i > 0) {
            known.subject += i + 1 == classes.size() ? " and " : ", ";
        }
        known.subject += classes[i];
    }
    known.subject += classes.size() > 1 ? " have" : " has";
    return known;
}

template <typename Map>
void check_names(std::vector<Problem>& problems, const std::string& id, std::string_view field,
                 const Map& overrides, const Known& known_items, const auto& names_of) {
    const auto& known = names_of(known_items.items);
    std::vector<std::string_view> names;
    names.reserve(known.size());
    for (const auto& [name, value] : known) {
        names.emplace_back(name);
    }
    for (const auto& [name, value] : overrides) {
        if (known.contains(name)) {
            continue;
        }
        std::string message = known_items.subject + " no ";
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
    add_asset_types(assets);
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
           (world.ecs()
                .any_of<world::Control, world::Label, world::ColorRect, world::Panel,
                        world::TextureRect, world::NinePatchRect, world::ProgressBar>(entity) ||
            world.ecs()
                .any_of<world::BoxContainer, world::GridContainer, world::MarginContainer,
                        world::CenterContainer, world::FlowContainer, world::PanelContainer>(
                    entity));
}

std::vector<std::string_view> control_classes(const world::World& world, entt::entity entity) {
    std::vector<std::string_view> classes;
    const auto& ecs = world.ecs();
    if (ecs.all_of<world::Label>(entity)) {
        classes.emplace_back("Label");
    }
    if (ecs.all_of<world::ColorRect>(entity)) {
        classes.emplace_back("ColorRect");
    }
    if (ecs.all_of<world::Panel>(entity)) {
        classes.emplace_back("Panel");
    }
    if (ecs.all_of<world::TextureRect>(entity)) {
        classes.emplace_back("TextureRect");
    }
    if (ecs.all_of<world::NinePatchRect>(entity)) {
        classes.emplace_back("NinePatchRect");
    }
    if (ecs.all_of<world::ProgressBar>(entity)) {
        classes.emplace_back("ProgressBar");
    }
    if (const auto* box = ecs.try_get<world::BoxContainer>(entity); box != nullptr) {
        classes.emplace_back(box->vertical ? "VBoxContainer" : "HBoxContainer");
    }
    if (ecs.all_of<world::GridContainer>(entity)) {
        classes.emplace_back("GridContainer");
    }
    if (ecs.all_of<world::MarginContainer>(entity)) {
        classes.emplace_back("MarginContainer");
    }
    if (ecs.all_of<world::CenterContainer>(entity)) {
        classes.emplace_back("CenterContainer");
    }
    if (const auto* flow = ecs.try_get<world::FlowContainer>(entity); flow != nullptr) {
        classes.emplace_back(flow->vertical ? "VFlowContainer" : "HFlowContainer");
    }
    if (ecs.all_of<world::PanelContainer>(entity)) {
        classes.emplace_back("PanelContainer");
    }
    if (classes.empty()) {
        classes.emplace_back("Control");
    }
    return classes;
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
    // Fonts and textures reported once each.
    std::map<std::string, std::string, std::less<>> font_errors;
    std::map<std::string, std::string, std::less<>> texture_errors;
    std::vector<std::pair<std::string, entt::entity>> entities;
    for (const auto [entity] : world.ecs().storage<entt::entity>()->each()) {
        if (is_control(world, entity) && !world.id_of(entity).empty()) {
            entities.emplace_back(world.id_of(entity), entity);
        }
    }
    std::ranges::sort(entities);
    for (const auto& [id, entity] : entities) {
        const auto& ecs = world.ecs();
        // One container places the children; the first in control_classes' order.
        const std::array<std::pair<std::string_view, bool>, 6> containers{{
            {"BoxContainer", ecs.all_of<world::BoxContainer>(entity)},
            {"GridContainer", ecs.all_of<world::GridContainer>(entity)},
            {"MarginContainer", ecs.all_of<world::MarginContainer>(entity)},
            {"CenterContainer", ecs.all_of<world::CenterContainer>(entity)},
            {"FlowContainer", ecs.all_of<world::FlowContainer>(entity)},
            {"PanelContainer", ecs.all_of<world::PanelContainer>(entity)},
        }};
        std::string_view first;
        for (const auto& [type, has] : containers) {
            if (!has) {
                continue;
            }
            if (first.empty()) {
                first = type;
                continue;
            }
            problems.push_back({.id = id,
                                .path = "/components/" + std::string(type),
                                .message = "an entity has one container; its " +
                                           std::string(first) + " places its children"});
        }

        const auto* control = ecs.try_get<world::Control>(entity);
        if (control == nullptr) {
            continue;
        }
        const Known known = known_items(control_classes(world, entity));
        check_names(problems, id, "theme_override_colors", control->theme_override_colors, known,
                    [](const ThemeItems& t) -> const auto& { return t.colors; });
        check_names(problems, id, "theme_override_constants", control->theme_override_constants,
                    known, [](const ThemeItems& t) -> const auto& { return t.constants; });
        check_names(problems, id, "theme_override_fonts", control->theme_override_fonts, known,
                    [](const ThemeItems& t) -> const auto& { return t.fonts; });
        check_names(problems, id, "theme_override_font_sizes", control->theme_override_font_sizes,
                    known, [](const ThemeItems& t) -> const auto& { return t.font_sizes; });
        check_names(problems, id, "theme_override_styles", control->theme_override_styles, known,
                    [](const ThemeItems& t) -> const auto& { return t.styles; });
        for (const auto& [name, path] : control->theme_override_fonts) {
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
        for (const auto& [name, doc] : control->theme_override_styles) {
            const std::string at =
                "/components/Control/theme_override_styles/" + json::pointer_escape(name);
            const std::vector<json::ValidationError> errors = validate_style_box(doc);
            for (const json::ValidationError& error : errors) {
                problems.push_back({.id = id, .path = at + error.path, .message = error.message});
            }
            if (!errors.empty()) {
                continue;
            }
            const auto style = parse_style_box(doc);
            if (!style || style.value().type != StyleBox::Type::Texture) {
                continue;
            }
            const std::string& texture = style.value().texture;
            auto error = texture_errors.find(texture);
            if (error == texture_errors.end()) {
                auto loaded = fonts.assets().load<render::Image>(texture);
                error = texture_errors.emplace(texture, loaded ? "" : loaded.error().message).first;
            }
            if (!error->second.empty()) {
                problems.push_back({.id = id, .path = at + "/texture", .message = error->second});
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
    // The project's window and theme, when the scene is in one.
    Settings project;
    const auto project_file =
        reader ? reader("project.json") : Result<std::string>(make_error(ErrorCode::NotFound, ""));
    const auto parsed = project_file ? json::parse(project_file.value())
                                     : Result<json::Value>(project_file.error());
    if (parsed && parsed.value().is_object()) {
        const json::Value& doc = parsed.value();
        const json::Value* window = doc.find("window");
        const json::Value* width =
            window != nullptr && window->is_object() ? window->find("width") : nullptr;
        const json::Value* height =
            window != nullptr && window->is_object() ? window->find("height") : nullptr;
        if (width != nullptr && height != nullptr && width->is_number() && height->is_number() &&
            width->as_double() > 0.0 && height->as_double() > 0.0) {
            project.viewport = {static_cast<float>(width->as_double()),
                                static_cast<float>(height->as_double())};
        }
        const json::Value* gui = doc.find("gui");
        const json::Value* theme =
            gui != nullptr && gui->is_object() ? gui->find("theme") : nullptr;
        if (theme != nullptr && theme->is_string()) {
            project.theme = theme->as_string();
        }
    }
    install(world, assets, project);
    for (Problem& problem : check(world, *fonts(world))) {
        errors.push_back({"/entities/" + json::pointer_escape(problem.id) + problem.path,
                          std::move(problem.message)});
    }
    return errors;
}

} // namespace slash::ui
