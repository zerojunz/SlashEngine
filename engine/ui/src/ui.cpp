#include "slash/ui/ui.hpp"

#include "internal.hpp"
#include "syntax_highlighter.hpp"

#include "slash/core/text.hpp"
#include "slash/input/input.hpp"
#include "slash/render/image.hpp"
#include "slash/world/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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
        for (const ThemeItems* items : {&default_theme(classes[i]), &optional_items(classes[i])}) {
            known.items.colors.insert(items->colors.begin(), items->colors.end());
            known.items.constants.insert(items->constants.begin(), items->constants.end());
            known.items.fonts.insert(items->fonts.begin(), items->fonts.end());
            known.items.font_sizes.insert(items->font_sizes.begin(), items->font_sizes.end());
            known.items.icons.insert(items->icons.begin(), items->icons.end());
            known.items.styles.insert(items->styles.begin(), items->styles.end());
        }
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

// A CodeEdit makes the TextEdit on its entity a code editor; its lines, and the lines of the
// TextEdit's gutters, are lines of the text, and a gutter shows text or icons.
void check_code_edits(const world::World& world, std::vector<Problem>& problems) {
    std::vector<std::pair<std::string, entt::entity>> found;
    for (const auto [entity] : world.ecs().storage<entt::entity>()->each()) {
        if (!world.id_of(entity).empty() &&
            world.ecs().any_of<world::CodeEdit, world::TextEdit>(entity)) {
            found.emplace_back(world.id_of(entity), entity);
        }
    }
    std::ranges::sort(found);
    for (const auto& [id, entity] : found) {
        const auto* edit = world.ecs().try_get<world::TextEdit>(entity);
        const auto* code = world.ecs().try_get<world::CodeEdit>(entity);
        if (edit == nullptr) {
            problems.push_back({.id = id,
                                .path = "/components/CodeEdit",
                                .message = "a CodeEdit makes the TextEdit on its entity a code "
                                           "editor, and this one has none"});
            continue;
        }
        const int last = static_cast<int>(std::ranges::count(edit->text, '\n'));
        const auto past = [&](const std::string& path, int line) {
            if (line > last) {
                problems.push_back({.id = id,
                                    .path = path,
                                    .message = "line " + std::to_string(line) +
                                               " is past the text's last line, " +
                                               std::to_string(last)});
            }
        };
        if (code != nullptr) {
            for (const auto& [field, lines] :
                 {std::pair{"breakpointed_lines", &code->breakpointed_lines},
                  std::pair{"bookmarked_lines", &code->bookmarked_lines},
                  std::pair{"executing_lines", &code->executing_lines}}) {
                for (std::size_t i = 0; i < lines->size(); ++i) {
                    past("/components/CodeEdit/" + std::string(field) + "/" + std::to_string(i),
                         (*lines)[i]);
                }
            }
        }
        for (std::size_t g = 0; g < edit->gutters.size(); ++g) {
            const world::TextEditGutter& gutter = edit->gutters[g];
            const std::string at = "/components/TextEdit/gutters/" + std::to_string(g);
            if (gutter.type != "string" && gutter.type != "icon") {
                std::string message =
                    R"(a gutter shows "string" or "icon", not ")" + gutter.type + "\"";
                const std::array<std::string_view, 2> types{"string", "icon"};
                if (const std::string_view hint = closest_match(gutter.type, types);
                    !hint.empty()) {
                    message += "; did you mean \"" + std::string(hint) + "\"?";
                }
                problems.push_back({.id = id, .path = at + "/type", .message = message});
            }
            if (gutter.width < 0) {
                problems.push_back({.id = id,
                                    .path = at + "/width",
                                    .message = "a gutter's width can't be less "
                                               "than 0"});
            }
            for (std::size_t i = 0; i < gutter.lines.size(); ++i) {
                const std::string entry = at + "/lines/" + std::to_string(i);
                if (gutter.lines[i].line < 0) {
                    problems.push_back(
                        {.id = id, .path = entry + "/line", .message = "lines count from 0"});
                }
                past(entry + "/line", gutter.lines[i].line);
            }
        }
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
        (*found)->fonts->set_system_fallback(settings.allow_system_fallback);
        return;
    }
    add_asset_types(assets);
    auto state = std::make_shared<State>();
    state->fonts = std::make_unique<text::Fonts>(assets);
    state->fonts->set_system_fallback(settings.allow_system_fallback);
    state->settings = settings;
    // The glyph atlas pages text draws from; the fonts themselves are assets.
    world.add_memory_source("text", [state](const world::World& /*world*/) {
        world::MemoryPart part{"text", 0, {}, {}};
        for (const text::AtlasPage& page : state->fonts->pages()) {
            part.bytes += page.rgba.capacity();
        }
        part.counts = {{"pages", state->fonts->pages().size()}};
        return part;
    });
    world.ecs().ctx().emplace<std::shared_ptr<State>>(std::move(state));
    install_gui(world);
}

void set_settings(world::World& world, const Settings& settings) {
    if (auto* found = world.ecs().ctx().find<std::shared_ptr<State>>(); found != nullptr) {
        (*found)->settings = settings;
        (*found)->fonts->set_system_fallback(settings.allow_system_fallback);
    }
}

void set_viewport(world::World& world, Vec2 viewport) {
    if (auto* found = world.ecs().ctx().find<std::shared_ptr<State>>(); found != nullptr) {
        (*found)->settings.viewport = viewport;
    }
}

Settings settings(const world::World& world) {
    const State* state = state_of(world);
    return state != nullptr ? state->settings : Settings{};
}

void read_settings(const json::Value& gui, Settings& settings) {
    if (!gui.is_object()) {
        return;
    }
    if (const json::Value* theme = gui.find("theme"); theme != nullptr && theme->is_string()) {
        settings.theme = theme->as_string();
    }
    if (const json::Value* system = gui.find("allow_system_fallback");
        system != nullptr && system->is_bool()) {
        settings.allow_system_fallback = system->as_bool();
    }
    if (const json::Value* delay = gui.find("tooltip_delay_sec");
        delay != nullptr && delay->is_number()) {
        settings.tooltip_delay_sec = delay->as_double();
    }
    if (const json::Value* offset = gui.find("tooltip_position_offset");
        offset != nullptr && offset->is_array() && offset->size() == 2 &&
        offset->as_array()[0].is_number() && offset->as_array()[1].is_number()) {
        settings.tooltip_position_offset = {static_cast<float>(offset->as_array()[0].as_double()),
                                            static_cast<float>(offset->as_array()[1].as_double())};
    }
    if (const json::Value* threshold = gui.find("drag_threshold");
        threshold != nullptr && threshold->is_number()) {
        settings.drag_threshold = static_cast<int>(threshold->as_double());
    }
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
                .any_of<world::Button, world::CheckBox, world::CheckButton, world::OptionButton,
                        world::MenuButton, world::TextureButton, world::LineEdit, world::TextEdit,
                        world::Slider, world::ScrollBar>(entity) ||
            world.ecs()
                .any_of<world::BoxContainer, world::GridContainer, world::MarginContainer,
                        world::CenterContainer, world::FlowContainer, world::PanelContainer,
                        world::ScrollContainer>(entity));
}

std::vector<std::string_view> control_classes(const world::World& world, entt::entity entity) {
    std::vector<std::string_view> classes;
    const auto& ecs = world.ecs();
    if (ecs.all_of<world::Button>(entity)) {
        classes.emplace_back("Button");
    }
    if (ecs.all_of<world::CheckBox>(entity)) {
        classes.emplace_back("CheckBox");
    }
    if (ecs.all_of<world::CheckButton>(entity)) {
        classes.emplace_back("CheckButton");
    }
    if (ecs.all_of<world::OptionButton>(entity)) {
        classes.emplace_back("OptionButton");
    }
    if (ecs.all_of<world::MenuButton>(entity)) {
        classes.emplace_back("MenuButton");
    }
    if (ecs.all_of<world::TextureButton>(entity)) {
        classes.emplace_back("TextureButton");
    }
    if (ecs.all_of<world::LineEdit>(entity)) {
        classes.emplace_back("LineEdit");
    }
    if (ecs.all_of<world::TextEdit, world::CodeEdit>(entity)) {
        classes.emplace_back("CodeEdit");
    }
    if (ecs.all_of<world::TextEdit>(entity)) {
        classes.emplace_back("TextEdit");
    }
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
    if (const auto* slider = ecs.try_get<world::Slider>(entity); slider != nullptr) {
        classes.emplace_back(slider->vertical ? "VSlider" : "HSlider");
    }
    if (const auto* bar = ecs.try_get<world::ScrollBar>(entity); bar != nullptr) {
        classes.emplace_back(bar->vertical ? "VScrollBar" : "HScrollBar");
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
    if (ecs.all_of<world::ScrollContainer>(entity)) {
        classes.emplace_back("ScrollContainer");
    }
    if (classes.empty()) {
        classes.emplace_back("Control");
    }
    return classes;
}

const world::Button* button_of(const world::World& world, entt::entity entity) {
    if (!world.valid(entity)) {
        return nullptr;
    }
    const auto& ecs = world.ecs();
    if (const auto* button = ecs.try_get<world::Button>(entity); button != nullptr) {
        return button;
    }
    if (const auto* box = ecs.try_get<world::CheckBox>(entity); box != nullptr) {
        return box;
    }
    if (const auto* check = ecs.try_get<world::CheckButton>(entity); check != nullptr) {
        return check;
    }
    if (const auto* option = ecs.try_get<world::OptionButton>(entity); option != nullptr) {
        return option;
    }
    return ecs.try_get<world::MenuButton>(entity);
}

world::Button* button_of(world::World& world, entt::entity entity) {
    if (!world.valid(entity)) {
        return nullptr;
    }
    auto& ecs = world.ecs();
    if (auto* button = ecs.try_get<world::Button>(entity); button != nullptr) {
        return button;
    }
    if (auto* box = ecs.try_get<world::CheckBox>(entity); box != nullptr) {
        return box;
    }
    if (auto* check = ecs.try_get<world::CheckButton>(entity); check != nullptr) {
        return check;
    }
    if (auto* option = ecs.try_get<world::OptionButton>(entity); option != nullptr) {
        return option;
    }
    return ecs.try_get<world::MenuButton>(entity);
}

std::string_view button_type(const world::World& world, entt::entity entity) {
    if (!world.valid(entity)) {
        return "";
    }
    const auto& ecs = world.ecs();
    if (ecs.all_of<world::Button>(entity)) {
        return "Button";
    }
    if (ecs.all_of<world::CheckBox>(entity)) {
        return "CheckBox";
    }
    if (ecs.all_of<world::CheckButton>(entity)) {
        return "CheckButton";
    }
    if (ecs.all_of<world::OptionButton>(entity)) {
        return "OptionButton";
    }
    return ecs.all_of<world::MenuButton>(entity) ? "MenuButton" : "";
}

std::vector<world::PopupMenuItem> popup_items(const world::World& world, entt::entity owner) {
    if (!world.valid(owner)) {
        return {};
    }
    const auto& ecs = world.ecs();
    if (const auto* popup = ecs.try_get<world::PopupMenu>(owner); popup != nullptr) {
        return popup->items;
    }
    if (const auto* menu = ecs.try_get<world::MenuButton>(owner); menu != nullptr) {
        return menu->items;
    }
    std::vector<world::PopupMenuItem> items;
    if (const auto* option = ecs.try_get<world::OptionButton>(owner); option != nullptr) {
        items.reserve(option->items.size());
        for (std::size_t i = 0; i < option->items.size(); ++i) {
            const world::OptionButtonItem& each = option->items[i];
            world::PopupMenuItem& item = items.emplace_back();
            item.text = each.text;
            item.icon = each.icon;
            item.tooltip = each.tooltip;
            item.checkable = "radio";
            item.checked = std::cmp_equal(option->selected, i);
            item.id = each.id;
            item.disabled = each.disabled;
            item.separator = each.separator;
            item.auto_translate_mode = each.auto_translate_mode;
        }
    }
    return items;
}

int item_id(const world::PopupMenuItem& item, std::size_t index) {
    return item.id >= 0 ? item.id : static_cast<int>(index);
}

Range range_of(const world::Slider& slider) {
    Range range{.min = slider.min_value,
                .max = std::max(slider.max_value, slider.min_value),
                .step = slider.step,
                .page = 0.0,
                .value = 0.0,
                .rounded = slider.rounded,
                .allow_greater = slider.allow_greater,
                .allow_lesser = slider.allow_lesser};
    range.page = std::clamp(slider.page, 0.0, range.max - range.min);
    range.value = range_value(range, slider.value);
    return range;
}

Range range_of(const world::ScrollBar& bar) {
    Range range{.min = bar.min_value,
                .max = std::max(bar.max_value, bar.min_value),
                .step = bar.step,
                .page = 0.0,
                .value = 0.0,
                .rounded = bar.rounded,
                .allow_greater = bar.allow_greater,
                .allow_lesser = bar.allow_lesser};
    range.page = std::clamp(bar.page, 0.0, range.max - range.min);
    range.value = range_value(range, bar.value);
    return range;
}

double range_value(const Range& range, double value) {
    if (std::isnan(value)) {
        return value;
    }
    if (range.step > 0.0) {
        // Math::snapped, from min unless min is too big to snap from. Godot rounds in decimal
        // (R128) so steps such as 0.1 give 0.3, not 0.30000000000000004: a step that is one
        // over a whole number divides by that number instead of multiplying.
        const double from = std::abs(range.min) > range.step * 1e14 ? 0.0 : range.min;
        const double steps = std::floor(((value - from) / range.step) + 0.5);
        const double per = std::round(1.0 / range.step);
        if (range.step < 1.0 && per > 0.0 && std::abs((per * range.step) - 1.0) < 1e-12) {
            value = ((from * per) + steps) / per;
        } else {
            value = (steps * range.step) + from;
        }
    }
    if (range.rounded) {
        value = std::round(value);
    }
    if (!range.allow_greater && value > range.max - range.page) {
        value = range.max - range.page;
    }
    if (!range.allow_lesser && value < range.min) {
        value = range.min;
    }
    return value;
}

double range_ratio(const Range& range) {
    // Math::is_equal_approx: equal within 0.00001 of their size, or of 1 when smaller.
    const double tolerance = std::max(0.00001 * std::abs(range.max), 0.00001);
    if (range.max == range.min || std::abs(range.max - range.min) < tolerance) {
        return 1.0;
    }
    const double value = std::clamp(range.value, range.min, range.max);
    return std::clamp((value - range.min) / (range.max - range.min), 0.0, 1.0);
}

double range_from_ratio(const Range& range, double ratio) {
    const double percent = (range.max - range.min) * ratio;
    double value = percent + range.min;
    if (range.step > 0.0) {
        value = (std::round(percent / range.step) * range.step) + range.min;
    }
    return std::clamp(value, range.min, range.max);
}

std::string_view control_class(const world::World& world, entt::entity entity) {
    return control_classes(world, entity).front();
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
    check_buttons(world, problems);
    // A CodeHighlighter colors the TextEdit on its entity, by color regions that keep to
    // add_color_region's rules.
    std::vector<std::pair<std::string, entt::entity>> highlighters;
    for (const auto [entity, highlighter] : world.ecs().view<world::CodeHighlighter>().each()) {
        if (!world.id_of(entity).empty()) {
            highlighters.emplace_back(world.id_of(entity), entity);
        }
    }
    std::ranges::sort(highlighters);
    for (const auto& [id, entity] : highlighters) {
        const auto& highlighter = world.ecs().get<world::CodeHighlighter>(entity);
        if (!world.ecs().all_of<world::TextEdit>(entity)) {
            problems.push_back({.id = id,
                                .path = "/components/CodeHighlighter",
                                .message = "a CodeHighlighter colors the TextEdit on its entity, "
                                           "and this one has none"});
        }
        for (const auto& [key, color] : highlighter.color_regions) {
            if (std::string error = color_region_error(highlighter, key); !error.empty()) {
                problems.push_back({.id = id,
                                    .path = "/components/CodeHighlighter/color_regions/" +
                                            json::pointer_escape(key),
                                    .message = std::move(error)});
            }
        }
    }
    check_code_edits(world, problems);
    for (const auto& [id, entity] : entities) {
        const auto& ecs = world.ecs();
        // One container places the children; the first in control_classes' order.
        const std::array<std::pair<std::string_view, bool>, 7> containers{{
            {"BoxContainer", ecs.all_of<world::BoxContainer>(entity)},
            {"GridContainer", ecs.all_of<world::GridContainer>(entity)},
            {"MarginContainer", ecs.all_of<world::MarginContainer>(entity)},
            {"CenterContainer", ecs.all_of<world::CenterContainer>(entity)},
            {"FlowContainer", ecs.all_of<world::FlowContainer>(entity)},
            {"PanelContainer", ecs.all_of<world::PanelContainer>(entity)},
            {"ScrollContainer", ecs.all_of<world::ScrollContainer>(entity)},
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

        // A language that isn't a BCP 47 tag picks no fonts for it.
        const auto check_language = [&](std::string_view type, const std::string& language) {
            if (!language.empty() && !text::is_language_code(language)) {
                problems.push_back({.id = id,
                                    .path = "/components/" + std::string(type) + "/language",
                                    .message = "\"" + language +
                                               "\" isn't a BCP 47 language tag, such as \"ar\" "
                                               "or \"zh-Hant\""});
            }
        };
        if (const auto* label = ecs.try_get<world::Label>(entity); label != nullptr) {
            check_language("Label", label->language);
        }
        if (const world::Button* button = button_of(world, entity); button != nullptr) {
            check_language(button_type(world, entity), button->language);
        }
        if (const auto* edit = ecs.try_get<world::LineEdit>(entity); edit != nullptr) {
            check_language("LineEdit", edit->language);
            if (to_utf32(edit->secret_character).size() > 1) {
                problems.push_back({.id = id,
                                    .path = "/components/LineEdit/secret_character",
                                    .message = "a secret character is one character; the "
                                               "others are left out"});
            }
            if (ecs.any_of<world::Label, world::TextureButton>(entity) ||
                button_of(world, entity) != nullptr) {
                problems.push_back(
                    {.id = id,
                     .path = "/components/LineEdit",
                     .message = "a LineEdit draws its own text and takes its own input; put the "
                                "Label or button in another control"});
            }
        }
        if (const auto* edit = ecs.try_get<world::TextEdit>(entity); edit != nullptr) {
            check_language("TextEdit", edit->language);
            if (ecs.any_of<world::Label, world::TextureButton, world::LineEdit>(entity) ||
                button_of(world, entity) != nullptr) {
                problems.push_back(
                    {.id = id,
                     .path = "/components/TextEdit",
                     .message = "a TextEdit draws its own text and takes its own input; put the "
                                "Label, button or LineEdit in another control"});
            }
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
        check_names(problems, id, "theme_override_icons", control->theme_override_icons, known,
                    [](const ThemeItems& t) -> const auto& { return t.icons; });
        check_names(problems, id, "theme_override_styles", control->theme_override_styles, known,
                    [](const ThemeItems& t) -> const auto& { return t.styles; });
        for (const auto& [name, path] : control->theme_override_icons) {
            const std::string at =
                "/components/Control/theme_override_icons/" + json::pointer_escape(name);
            if (std::string error = icon_path_error(path); !error.empty()) {
                problems.push_back({.id = id, .path = at, .message = std::move(error)});
                continue;
            }
            if (path.empty() || builtin_icon(path) != nullptr) {
                continue;
            }
            auto error = texture_errors.find(path);
            if (error == texture_errors.end()) {
                auto loaded = fonts.assets().load<render::Image>(path);
                error = texture_errors.emplace(path, loaded ? "" : loaded.error().message).first;
            }
            if (!error->second.empty()) {
                problems.push_back({.id = id, .path = at, .message = error->second});
            }
        }
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
        if (const json::Value* gui = doc.find("gui"); gui != nullptr) {
            read_settings(*gui, project);
        }
    }
    install(world, assets, project);
    // The project's input actions, for buttons' shortcuts.
    input::Map actions = input::Map::with_defaults();
    if (const json::Value* map =
            parsed && parsed.value().is_object() ? parsed.value().find("input") : nullptr;
        map != nullptr && input::validate_map(*map).empty()) {
        if (auto read = input::parse_map(*map); read) {
            actions = std::move(read).value();
        }
    }
    input::install(world, std::move(actions));
    for (Problem& problem : check(world, *fonts(world))) {
        errors.push_back({"/entities/" + json::pointer_escape(problem.id) + problem.path,
                          std::move(problem.message)});
    }
    return errors;
}

} // namespace slash::ui
