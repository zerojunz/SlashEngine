#include "slash/ui/ui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace slash::ui {
namespace {

const world::Control kDefaultControl{};

std::uint8_t to_byte(float channel) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(channel, 0.0F, 1.0F) * 255.0F));
}

Vec4 times(Vec4 a, Vec4 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}

struct Rect {
    float x0 = 0.0F;
    float y0 = 0.0F;
    float x1 = 0.0F;
    float y1 = 0.0F;
};

class Drawer {
public:
    explicit Drawer(DrawList& list) : list_(list) {}

    // A rect in a control's space, its texture coordinates uv, cut to clip if there is one.
    void quad(const DrawCall& source, const Transform& transform, Rect rect, Rect uv, Vec4 color,
              const std::optional<Rect>& clip) {
        if (clip) {
            const Rect cut{std::max(rect.x0, clip->x0), std::max(rect.y0, clip->y0),
                           std::min(rect.x1, clip->x1), std::min(rect.y1, clip->y1)};
            if (cut.x0 >= cut.x1 || cut.y0 >= cut.y1) {
                return;
            }
            const float du = (uv.x1 - uv.x0) / (rect.x1 - rect.x0);
            const float dv = (uv.y1 - uv.y0) / (rect.y1 - rect.y0);
            uv = {uv.x0 + ((cut.x0 - rect.x0) * du), uv.y0 + ((cut.y0 - rect.y0) * dv),
                  uv.x1 - ((rect.x1 - cut.x1) * du), uv.y1 - ((rect.y1 - cut.y1) * dv)};
            rect = cut;
        }
        DrawCall& call = call_for(source);
        const std::array<std::uint8_t, 4> rgba{to_byte(color.x), to_byte(color.y), to_byte(color.z),
                                               to_byte(color.w)};
        const auto corner = [&](float x, float y, float u, float v) {
            const Vec2 p = transform.apply({x, y});
            return render::Vertex2D{.x = p.x, .y = p.y, .u = u, .v = v, .rgba = rgba};
        };
        const render::Vertex2D top_left = corner(rect.x0, rect.y0, uv.x0, uv.y0);
        const render::Vertex2D top_right = corner(rect.x1, rect.y0, uv.x1, uv.y0);
        const render::Vertex2D bottom_left = corner(rect.x0, rect.y1, uv.x0, uv.y1);
        const render::Vertex2D bottom_right = corner(rect.x1, rect.y1, uv.x1, uv.y1);
        call.vertices.insert(call.vertices.end(), {top_left, top_right, bottom_left, top_right,
                                                   bottom_right, bottom_left});
    }

private:
    // The last call if it draws the same way, so draws in a row share one; else a new one.
    DrawCall& call_for(const DrawCall& source) {
        if (!list_.calls.empty()) {
            DrawCall& last = list_.calls.back();
            if (last.source == source.source && last.texture == source.texture &&
                last.page == source.page && last.field == source.field) {
                return last;
            }
        }
        DrawCall& call = list_.calls.emplace_back();
        call.source = source.source;
        call.texture = source.texture;
        call.page = source.page;
        call.field = source.field;
        return call;
    }

    DrawList& list_;
};

enum class Pass : std::uint8_t { ShadowOutline, Shadow, Outline, Text };

// One pass over a label's shown lines, as Label draws them: the shadow's outline, the shadow,
// the outline, then the text, each over the last.
void draw_label_pass(Drawer& drawer, DrawList& list, text::Fonts& fonts, const LabelText& label,
                     const Placed& placed, Vec4 tint, Pass pass, const std::optional<Rect>& clip) {
    Vec4 color = label.font_color;
    Vec2 offset;
    int outline = 0;
    switch (pass) {
    case Pass::ShadowOutline:
        color = label.shadow_color;
        offset = label.shadow_offset;
        outline = label.shadow_outline_size;
        break;
    case Pass::Shadow:
        color = label.shadow_color;
        offset = label.shadow_offset;
        break;
    case Pass::Outline:
        color = label.outline_color;
        outline = label.outline_size;
        break;
    case Pass::Text:
        break;
    }
    color = times(color, tint);
    const auto& pages = fonts.pages();
    const bool snap = placed.transform.translation_only();
    std::vector<float> spaces;
    for (std::size_t l = label.first; l < label.last; ++l) {
        const text::Line& line = label.text.lines[l];
        const Vec2 pen = label.pens[l - label.first] + offset;
        const float stretch = label.stretch[l - label.first];
        // Where each space between words is, for fill's stretching.
        spaces.clear();
        if (stretch != 0.0F) {
            std::uint32_t end = line.count;
            while (end > 0 && label.text.glyphs[line.first + end - 1].space) {
                --end;
            }
            for (std::uint32_t g = 0; g < end; ++g) {
                const text::Glyph& glyph = label.text.glyphs[line.first + g];
                if (glyph.space) {
                    spaces.push_back(glyph.x);
                }
            }
            std::ranges::sort(spaces);
        }
        for (std::uint32_t g = 0; g < line.count; ++g) {
            const text::Glyph& glyph = label.text.glyphs[line.first + g];
            if (glyph.space || glyph.cluster >= label.visible_bytes) {
                continue;
            }
            const auto image = fonts.glyph(glyph.face, glyph.index, label.font_size, outline);
            if (!image) {
                continue;
            }
            const text::FaceInfo info = fonts.face_info(glyph.face);
            const auto before = std::ranges::lower_bound(spaces, glyph.x) - spaces.begin();
            Vec2 at{pen.x + glyph.x + (stretch * static_cast<float>(before)), pen.y + glyph.y};
            if (snap && !info.msdf) {
                // Glyph images are made for whole pixels, so they draw on them, as Godot's do.
                const Vec2 screen = placed.transform.apply(at);
                at = at + Vec2{std::floor(screen.x + 0.5F) - screen.x,
                               std::floor(screen.y + 0.5F) - screen.y};
            }
            const text::AtlasPage& page = pages[image->page];
            const auto pw = static_cast<float>(page.width);
            const auto ph = static_cast<float>(page.height);
            const Rect rect{
                at.x + (image->left * image->scale), at.y + (image->top * image->scale),
                at.x + ((image->left + static_cast<float>(image->width)) * image->scale),
                at.y + ((image->top + static_cast<float>(image->height)) * image->scale)};
            const Rect uv{static_cast<float>(image->x) / pw, static_cast<float>(image->y) / ph,
                          static_cast<float>(image->x + image->width) / pw,
                          static_cast<float>(image->y + image->height) / ph};
            DrawCall source;
            source.source = DrawCall::Source::Atlas;
            source.page = image->page;
            if (info.msdf) {
                // Godot's outline sizes are four times the pixels they widen by.
                source.field = {.pixel_range = static_cast<float>(info.msdf_pixel_range),
                                .outline = static_cast<float>(outline) / 4.0F / image->scale};
            }
            drawer.quad(source, placed.transform, rect, uv, color, clip);
            ++list.glyphs;
        }
    }
}

} // namespace

DrawList draw_calls(const Layout& layout, const world::World& world, text::Fonts& fonts) {
    DrawList list;
    Drawer drawer(list);
    const auto& ecs = world.ecs();
    for (const Placed& placed : layout.controls) {
        if (!placed.visible || !world.valid(placed.entity)) {
            continue;
        }
        const auto* control = ecs.try_get<world::Control>(placed.entity);
        const Vec4 tint =
            times(placed.modulate, (control != nullptr ? *control : kDefaultControl).self_modulate);
        const Rect bounds{0.0F, 0.0F, placed.size.x, placed.size.y};
        bool drawn = false;
        if (const auto* rect = ecs.try_get<world::ColorRect>(placed.entity); rect != nullptr) {
            drawer.quad({}, placed.transform, bounds, {0.0F, 0.0F, 1.0F, 1.0F},
                        times(rect->color, tint), std::nullopt);
            drawn = true;
        }
        if (const LabelText* label = layout.label(placed.entity); label != nullptr) {
            const auto* component = ecs.try_get<world::Label>(placed.entity);
            const std::optional<Rect> clip =
                component != nullptr && component->clip_text ? std::optional(bounds) : std::nullopt;
            if (label->shadow_color.w != 0.0F && label->shadow_outline_size > 0) {
                draw_label_pass(drawer, list, fonts, *label, placed, tint, Pass::ShadowOutline,
                                clip);
            }
            if (label->shadow_color.w > 0.0F) {
                draw_label_pass(drawer, list, fonts, *label, placed, tint, Pass::Shadow, clip);
            }
            if (label->outline_size > 0 && label->outline_color.w != 0.0F) {
                draw_label_pass(drawer, list, fonts, *label, placed, tint, Pass::Outline, clip);
            }
            draw_label_pass(drawer, list, fonts, *label, placed, tint, Pass::Text, clip);
            drawn = true;
        }
        if (drawn) {
            ++list.controls;
        }
    }
    return list;
}

} // namespace slash::ui
