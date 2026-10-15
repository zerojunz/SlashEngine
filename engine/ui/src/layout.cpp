#include "slash/ui/ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Controls are placed as Godot's Control::_size_changed places them, and labels laid out as
// Godot's Label shapes and draws them (scene/gui/label.cpp): the same minimum sizes, lines shown,
// trimming and alignment, down to where Godot rounds to whole pixels.
namespace slash::ui {
namespace {

constexpr int kMaxZ = 4096;

const world::Control kDefaultControl{};

std::string name_of(const world::World& world, entt::entity entity) {
    const std::string_view id = world.id_of(entity);
    return id.empty() ? "entity " + std::to_string(entt::to_entity(entity))
                      : "\"" + std::string(id) + "\"";
}

template <typename Map, typename Items>
auto theme_item(const Map& overrides, const Items& defaults, const std::string& name) {
    if (const auto found = overrides.find(name); found != overrides.end()) {
        return found->second;
    }
    const auto found = defaults.find(name);
    return found != defaults.end() ? found->second : typename Items::mapped_type{};
}

Vec4 times(Vec4 a, Vec4 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}

// A Label being laid out.
struct LabelWork {
    const world::Label* label = nullptr;
    text::Autowrap autowrap = text::Autowrap::Off;
    text::Overrun overrun = text::Overrun::NoTrimming;
    int line_spacing = 0;
    int paragraph_spacing = 0;
    float line_height = 0.0F;
    LabelText out;
};

LabelWork start_label(const world::Label& label, const world::Control& control) {
    const ThemeItems& theme = default_theme("Label");
    LabelWork work;
    work.label = &label;
    work.autowrap = text::parse_autowrap(label.autowrap_mode).value_or(text::Autowrap::Off);
    work.overrun =
        text::parse_overrun(label.text_overrun_behavior).value_or(text::Overrun::NoTrimming);
    const auto constant = [&](const std::string& name) {
        return theme_item(control.theme_override_constants, theme.constants, name);
    };
    work.line_spacing = constant("line_spacing");
    work.paragraph_spacing = constant("paragraph_spacing");
    LabelText& out = work.out;
    out.font = theme_item(control.theme_override_fonts, theme.fonts, "font");
    out.font_size =
        std::clamp(theme_item(control.theme_override_font_sizes, theme.font_sizes, "font_size"), 1,
                   text::kMaxFontSize);
    out.font_color = theme_item(control.theme_override_colors, theme.colors, "font_color");
    out.outline_color =
        theme_item(control.theme_override_colors, theme.colors, "font_outline_color");
    out.shadow_color = theme_item(control.theme_override_colors, theme.colors, "font_shadow_color");
    out.outline_size = std::max(constant("outline_size"), 0);
    out.shadow_outline_size = std::max(constant("shadow_outline_size"), 0);
    out.shadow_offset = {static_cast<float>(constant("shadow_offset_x")),
                         static_cast<float>(constant("shadow_offset_y"))};
    // Godot takes the ellipsis's first character, or U+2026.
    if (work.overrun == text::Overrun::TrimEllipsis ||
        work.overrun == text::Overrun::TrimWordEllipsis) {
        out.ellipsis = std::string(text::first_characters(label.ellipsis_char, 1));
        if (out.ellipsis.empty()) {
            out.ellipsis = "\u2026";
        }
    }
    out.source = label.uppercase ? text::to_upper(label.text) : label.text;
    if (label.visible_characters >= 0) {
        const auto count = static_cast<std::size_t>(label.visible_characters);
        if (label.visible_characters_behavior == "chars_after_shaping") {
            out.visible_bytes = text::first_characters(out.source, count).size();
        } else {
            out.source = std::string(text::first_characters(out.source, count));
        }
    }
    return work;
}

// Shapes a label's text, wrapped to a width when it wraps; a font that doesn't load is
// reported and the default font used instead.
void shape(text::Fonts& fonts, LabelWork& work, float width) {
    text::LayoutOptions options{
        .font = work.out.font,
        .font_size = work.out.font_size,
        .autowrap = work.autowrap,
        // Godot wraps to the width in whole pixels.
        .width = work.autowrap == text::Autowrap::Off ? 0.0F : std::max(std::trunc(width), 1.0F)};
    auto shaped = fonts.layout(work.out.source, options);
    if (!shaped && !options.font.empty()) {
        work.out.error = shaped.error().message;
        work.out.font.clear();
        options.font.clear();
        shaped = fonts.layout(work.out.source, options);
    }
    if (shaped) {
        work.out.text = std::move(shaped.value());
    } else {
        work.out.text = {};
        work.out.text.lines.emplace_back();
    }
    work.line_height = work.out.text.metrics.height();
}

// The first line shown and the one after the last that max_lines_visible lets show.
std::pair<std::size_t, std::size_t> lines_allowed(const LabelWork& work) {
    const std::size_t total = work.out.text.lines.size();
    const std::size_t skipped =
        std::min(static_cast<std::size_t>(std::max(work.label->lines_skipped, 0)), total);
    std::size_t shown = total - skipped;
    if (work.label->max_lines_visible >= 0) {
        shown = std::min(shown, static_cast<std::size_t>(work.label->max_lines_visible));
    }
    return {skipped, skipped + shown};
}

// The height of lines [first, last): line spacing between lines, paragraph spacing between
// paragraphs, as Label::_update_visible adds them up.
float lines_height(const LabelWork& work, std::size_t first, std::size_t last) {
    const auto& lines = work.out.text.lines;
    const auto ls = static_cast<float>(work.line_spacing);
    const auto ps = static_cast<float>(work.paragraph_spacing);
    float height = 0.0F;
    for (std::size_t l = first; l < last; ++l) {
        height += work.line_height + ls;
        if (lines[l].paragraph_end || l + 1 == last) {
            height += ps;
        }
    }
    return height > 0.0F ? height - (ls + ps) : 0.0F;
}

// A label's minimum size, as Label::get_minimum_size: its lines' width (1 when it wraps, clips
// or trims) and height (at least a line; 1 when it clips or trims with wrapping).
Vec2 label_minimum(const LabelWork& work) {
    const world::Label& label = *work.label;
    const float lh = work.line_height;
    Vec2 size{1.0F, lh};
    if (!label.text.empty()) {
        const auto [first, last] = lines_allowed(work);
        size = {work.out.text.width, std::max(lines_height(work, first, last), lh)};
    }
    const bool trims = work.overrun != text::Overrun::NoTrimming;
    if (work.autowrap != text::Autowrap::Off) {
        if (!label.clip_text && trims && label.max_lines_visible > 0) {
            size.y = std::min(size.y, (lh + static_cast<float>(work.line_spacing)) *
                                          static_cast<float>(label.max_lines_visible));
        } else if (label.clip_text || trims) {
            size.y = 1.0F;
        }
        return {1.0F, size.y};
    }
    if (label.clip_text || trims) {
        size.x = 1.0F;
    }
    return size;
}

// Cuts, stretches and places a label's lines in its rect, as Label's _shape_internal,
// get_layout_data and NOTIFICATION_DRAW do.
void finish_label(text::Fonts& fonts, LabelWork& work, Vec2 size) {
    const world::Label& label = *work.label;
    LabelText& out = work.out;
    text::TextLayout& layout = out.text;
    const auto ls = static_cast<float>(work.line_spacing);
    const auto ps = static_cast<float>(work.paragraph_spacing);
    const float lh = work.line_height;
    const std::size_t total = layout.lines.size();
    const auto [skipped, allowed] = lines_allowed(work);

    // The lines that fit the rect's height.
    std::size_t fit = 0;
    float height = 0.0F;
    const float room = std::ceil(size.y + ls);
    for (std::size_t l = skipped; l < total; ++l) {
        height += lh + ls;
        if (height > room) {
            break;
        }
        ++fit;
        if (layout.lines[l].paragraph_end) {
            height += ps;
        }
    }
    const std::size_t visible = std::min(fit, allowed - skipped);
    out.first = skipped;
    out.last = skipped + visible;
    out.overflow = visible < allowed - skipped;

    // Trimming: every line too wide when lines don't wrap; when they do, only the last line
    // shown when more are hidden, with the ellipsis whether it fits or not.
    const float width = std::trunc(size.x);
    if (work.autowrap == text::Autowrap::Off) {
        for (std::size_t l = 0; l < total; ++l) {
            if (fonts.trim(layout, l, width, work.overrun, out.ellipsis)) {
                out.trimmed.push_back(l);
            }
        }
    } else if (visible > 0 && visible < total - skipped) {
        if (fonts.trim(layout, out.last - 1, width, work.overrun, out.ellipsis, true)) {
            out.trimmed.push_back(out.last - 1);
        }
    }

    // Down the rect.
    const float lines_h = lines_height(work, out.first, out.last);
    float vbegin = 0.0F;
    float vsep = 0.0F;
    if (visible > 0) {
        if (label.vertical_alignment == "center") {
            vbegin = static_cast<float>(static_cast<int>((size.y - lines_h) / 2.0F));
        } else if (label.vertical_alignment == "bottom") {
            vbegin = static_cast<float>(static_cast<int>(size.y - lines_h));
        } else if (label.vertical_alignment == "fill" && visible > 1) {
            vsep = static_cast<float>(
                static_cast<int>((size.y - lines_h) / static_cast<float>(visible - 1)));
        }
    }

    // Across it, each line stretched to the width for fill except a wrapped paragraph's last.
    const bool fill = label.horizontal_alignment == "fill";
    float y = vbegin;
    for (std::size_t l = out.first; l < out.last; ++l) {
        const text::Line& line = layout.lines[l];
        float line_width = line.width;
        float stretch = 0.0F;
        const bool wrapped_last = line.paragraph_end && l > 0 && !layout.lines[l - 1].paragraph_end;
        if (fill && !wrapped_last && line_width < size.x) {
            std::uint32_t end = line.count;
            while (end > 0 && layout.glyphs[line.first + end - 1].space) {
                --end;
            }
            const auto spaces = std::count_if(layout.glyphs.begin() + line.first,
                                              layout.glyphs.begin() + line.first + end,
                                              [](const text::Glyph& glyph) { return glyph.space; });
            if (spaces > 0) {
                stretch = (size.x - line_width) / static_cast<float>(spaces);
                line_width = size.x;
            }
        }
        float x = 0.0F;
        if (label.horizontal_alignment == "center") {
            x = std::trunc(std::trunc(size.x - line_width) / 2.0F);
        } else if (label.horizontal_alignment == "right") {
            x = std::trunc(size.x - line_width);
        }
        if (line_width > size.x + 0.01F) {
            out.overflow = true;
        }
        y += layout.metrics.ascent;
        out.pens.push_back({x, y});
        out.stretch.push_back(stretch);
        y += layout.metrics.descent + ls + vsep;
        if (line.paragraph_end) {
            y += ps;
        }
    }
}

Transform local_transform(const world::Control& control, Vec2 position) {
    const Vec2 cs = control.rotation == 0.0F ? Vec2{1.0F, 0.0F} : cos_sin(control.rotation);
    Transform t{.a = cs.x * control.scale.x,
                .b = cs.y * control.scale.x,
                .c = -cs.y * control.scale.y,
                .d = cs.x * control.scale.y};
    const Vec2 pivot = control.pivot_offset;
    t.tx = position.x + pivot.x - ((t.a * pivot.x) + (t.c * pivot.y));
    t.ty = position.y + pivot.y - ((t.b * pivot.x) + (t.d * pivot.y));
    return t;
}

// Moves an edge pair out to a minimum length, the way grow says.
void grow(float& position, float& length, float minimum, std::string_view direction) {
    if (minimum <= length) {
        return;
    }
    if (direction == "begin") {
        position += length - minimum;
    } else if (direction == "both") {
        position += 0.5F * (length - minimum);
    }
    length = minimum;
}

struct Builder {
    const world::World& world;
    text::Fonts& fonts;
    Layout& out;
    std::unordered_map<entt::entity, std::vector<entt::entity>> children;
    std::unordered_map<entt::entity, entt::entity> parents;

    [[nodiscard]] const world::Control& control_of(entt::entity entity) const {
        const auto* control = world.ecs().try_get<world::Control>(entity);
        return control != nullptr ? *control : kDefaultControl;
    }

    void problem(entt::entity entity, std::size_t child, std::string message) {
        out.problems.push_back({.id = std::string(world.id_of(entity)),
                                .path = "/components/Control/children/" + std::to_string(child),
                                .message = std::move(message)});
    }

    void link(const std::vector<entt::entity>& controls) {
        for (const entt::entity entity : controls) {
            const auto* control = world.ecs().try_get<world::Control>(entity);
            if (control == nullptr) {
                continue;
            }
            for (std::size_t i = 0; i < control->children.size(); ++i) {
                const entt::entity child = control->children[i];
                if (child == entt::null || !world.valid(child)) {
                    problem(entity, i, "names no entity");
                } else if (child == entity) {
                    problem(entity, i, "a control can't be its own child");
                } else if (!is_control(world, child)) {
                    problem(entity, i,
                            name_of(world, child) +
                                " isn't a control; give it a Control, Label or ColorRect");
                } else if (const auto found = parents.find(child); found != parents.end()) {
                    problem(entity, i,
                            name_of(world, child) + " is already a child of " +
                                name_of(world, found->second));
                } else {
                    parents.emplace(child, entity);
                    children[entity].push_back(child);
                }
            }
        }
    }

    // Breaks each loop of children at the entry that closes it, so every control is reached
    // from a root.
    void break_loops(const std::vector<entt::entity>& controls) {
        std::unordered_set<entt::entity> reached;
        const auto reach = [&](entt::entity root) {
            std::vector<entt::entity> stack{root};
            while (!stack.empty()) {
                const entt::entity entity = stack.back();
                stack.pop_back();
                if (!reached.insert(entity).second) {
                    continue;
                }
                const auto found = children.find(entity);
                if (found != children.end()) {
                    stack.insert(stack.end(), found->second.begin(), found->second.end());
                }
            }
        };
        for (const entt::entity entity : controls) {
            if (!parents.contains(entity)) {
                reach(entity);
            }
        }
        for (const entt::entity entity : controls) {
            if (reached.contains(entity)) {
                continue;
            }
            // Up the parents until one repeats: that one's link to its parent closes the loop.
            std::unordered_set<entt::entity> seen;
            entt::entity at = entity;
            while (seen.insert(at).second) {
                at = parents.at(at);
            }
            const entt::entity parent = parents.at(at);
            auto& list = children[parent];
            list.erase(std::ranges::find(list, at));
            parents.erase(at);
            const auto& listed = world.ecs().get<world::Control>(parent).children;
            const auto index =
                static_cast<std::size_t>(std::ranges::find(listed, at) - listed.begin());
            problem(parent, index,
                    name_of(world, at) + " contains " + name_of(world, parent) +
                        ", so it can't be its child too");
            reach(at);
        }
    }

    void place(entt::entity entity, const Placed* parent) {
        const world::Control& control = control_of(entity);
        const Vec2 area = parent != nullptr ? parent->size : out.viewport;
        Placed placed;
        placed.entity = entity;
        placed.parent = parent != nullptr ? parent->entity : entt::null;
        float left = control.offset_left + (control.anchor_left * area.x);
        float top = control.offset_top + (control.anchor_top * area.y);
        float width = control.offset_right + (control.anchor_right * area.x) - left;
        float height = control.offset_bottom + (control.anchor_bottom * area.y) - top;

        Vec2 minimum = control.custom_minimum_size;
        const auto* label = world.ecs().try_get<world::Label>(entity);
        LabelWork work;
        if (label != nullptr) {
            work = start_label(*label, control);
            if (work.autowrap == text::Autowrap::Off) {
                shape(fonts, work, 0.0F);
            }
            minimum.x = std::max(
                minimum.x, work.autowrap == text::Autowrap::Off ? label_minimum(work).x : 1.0F);
        }
        grow(left, width, minimum.x, control.grow_horizontal);
        if (label != nullptr) {
            // A wrapping label's height depends on the width it wraps to.
            if (work.autowrap != text::Autowrap::Off) {
                shape(fonts, work, width);
            }
            minimum.y = std::max(minimum.y, label_minimum(work).y);
        }
        grow(top, height, minimum.y, control.grow_vertical);
        placed.position = {left, top};
        placed.size = {width, height};
        placed.minimum_size = minimum;

        const Transform local = local_transform(control, placed.position);
        placed.transform = parent != nullptr ? parent->transform * local : local;
        placed.visible = control.visible && (parent == nullptr || parent->visible);
        placed.modulate =
            parent != nullptr ? times(parent->modulate, control.modulate) : control.modulate;
        placed.z = std::clamp(control.z_index + (parent != nullptr ? parent->z : 0), -kMaxZ, kMaxZ);
        if (label != nullptr) {
            finish_label(fonts, work, placed.size);
            out.labels.emplace(entity, std::move(work.out));
        }
        out.controls.push_back(placed);
    }
};

} // namespace

Layout layout(const world::World& world, text::Fonts& fonts, Vec2 viewport) {
    Layout out;
    out.viewport = viewport;
    const auto& ecs = world.ecs();
    std::vector<entt::entity> controls;
    for (const entt::entity entity : ecs.view<const world::Control>()) {
        controls.push_back(entity);
    }
    for (const entt::entity entity : ecs.view<const world::Label>(entt::exclude<world::Control>)) {
        controls.push_back(entity);
    }
    for (const entt::entity entity :
         ecs.view<const world::ColorRect>(entt::exclude<world::Control, world::Label>)) {
        controls.push_back(entity);
    }
    std::ranges::sort(controls, [](entt::entity a, entt::entity b) {
        return entt::to_entity(a) < entt::to_entity(b);
    });

    Builder builder{.world = world, .fonts = fonts, .out = out, .children = {}, .parents = {}};
    builder.link(controls);
    builder.break_loops(controls);

    // Parents before children, children in order, roots in the order they were made; the place
    // in out.controls of each one's parent rides along.
    constexpr auto kRoot = static_cast<std::size_t>(-1);
    std::vector<std::pair<entt::entity, std::size_t>> stack;
    for (const entt::entity entity : std::views::reverse(controls)) {
        if (!builder.parents.contains(entity)) {
            stack.emplace_back(entity, kRoot);
        }
    }
    while (!stack.empty()) {
        const auto [entity, parent] = stack.back();
        stack.pop_back();
        builder.place(entity, parent == kRoot ? nullptr : &out.controls[parent]);
        const std::size_t placed = out.controls.size() - 1;
        if (const auto found = builder.children.find(entity); found != builder.children.end()) {
            for (const entt::entity child : std::views::reverse(found->second)) {
                stack.emplace_back(child, placed);
            }
        }
    }

    // Then by z: Godot draws a canvas item over those with a lower z_index, the rest in tree
    // order.
    std::ranges::stable_sort(out.controls,
                             [](const Placed& a, const Placed& b) { return a.z < b.z; });
    for (std::size_t i = 0; i < out.controls.size(); ++i) {
        out.index.emplace(out.controls[i].entity, i);
    }
    return out;
}

} // namespace slash::ui
