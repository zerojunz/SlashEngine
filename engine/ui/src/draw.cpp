#include "slash/ui/ui.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <numbers>
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

    // Triangles already in viewport pixels, three vertices each.
    void triangles(const DrawCall& source, const std::vector<render::Vertex2D>& vertices) {
        if (vertices.empty()) {
            return;
        }
        DrawCall& call = call_for(source);
        call.vertices.insert(call.vertices.end(), vertices.begin(), vertices.end());
    }

private:
    // The last call if it draws the same way, so draws in a row share one; else a new one.
    DrawCall& call_for(const DrawCall& source) {
        if (!list_.calls.empty()) {
            DrawCall& last = list_.calls.back();
            if (last.source == source.source && last.texture == source.texture &&
                last.filter == source.filter && last.page == source.page &&
                last.field == source.field) {
                return last;
            }
        }
        DrawCall& call = list_.calls.emplace_back();
        call.source = source.source;
        call.texture = source.texture;
        call.filter = source.filter;
        call.page = source.page;
        call.field = source.field;
        return call;
    }

    DrawList& list_;
};

// ---- Flat style boxes, as Godot's StyleBoxFlat::draw (scene/resources/style_box_flat.cpp) ----

// A rect as Godot's Rect2: a position and a size.
struct Area {
    float x = 0.0F;
    float y = 0.0F;
    float w = 0.0F;
    float h = 0.0F;
};

Area grow_individual(Area r, float left, float top, float right, float bottom) {
    return {r.x - left, r.y - top, r.w + left + right, r.h + top + bottom};
}

Area grow_side(Area r, std::size_t side, float amount) {
    switch (side) {
    case 0:
        return grow_individual(r, amount, 0.0F, 0.0F, 0.0F);
    case 1:
        return grow_individual(r, 0.0F, amount, 0.0F, 0.0F);
    case 2:
        return grow_individual(r, 0.0F, 0.0F, amount, 0.0F);
    default:
        return grow_individual(r, 0.0F, 0.0F, 0.0F, amount);
    }
}

constexpr std::size_t kLeft = 0;
constexpr std::size_t kTop = 1;
constexpr std::size_t kRight = 2;
constexpr std::size_t kBottom = 3;
constexpr std::size_t kTopLeft = 0;
constexpr std::size_t kTopRight = 1;
constexpr std::size_t kBottomRight = 2;
constexpr std::size_t kBottomLeft = 3;

using Four = std::array<float, 4>;

Vec2 min_each(Vec2 v, float m) {
    return {std::min(v.x, m), std::min(v.y, m)};
}

Vec2 max_each(Vec2 v, float m) {
    return {std::max(v.x, m), std::max(v.y, m)};
}

// Triangles with a color at each point.
struct Mesh {
    std::vector<Vec2> points;
    std::vector<Vec4> colors;
    std::vector<std::size_t> indices;
};

Four inner_corner_radius(Area style, Area inner, const Four& radius) {
    const float left = inner.x - style.x;
    const float top = inner.y - style.y;
    const float right = style.w - inner.w - left;
    const float bottom = style.h - inner.h - top;
    return {std::max(radius[kTopLeft] - std::min(top, left), 0.0F),
            std::max(radius[kTopRight] - std::min(top, right), 0.0F),
            std::max(radius[kBottomRight] - std::min(bottom, right), 0.0F),
            std::max(radius[kBottomLeft] - std::min(bottom, left), 0.0F)};
}

// How much each corner's curve shrinks where opposite corners overlap, as set_corner_scale.
std::array<Vec2, 4> corner_scale(Area style, Area inner, const Four& radius) {
    const float left = inner.x - style.x;
    const float top = inner.y - style.y;
    const float right = style.w - inner.w - left;
    const float bottom = style.h - inner.h - top;
    const Four overflow{
        -std::min(0.0F, inner.h - radius[kTopLeft] - radius[kBottomLeft]),
        -std::min(0.0F, inner.w - radius[kTopLeft] - radius[kTopRight]),
        -std::min(0.0F, inner.h - radius[kTopRight] - radius[kBottomRight]),
        -std::min(0.0F, inner.w - radius[kBottomLeft] - radius[kBottomRight]),
    };
    const float h_sum = left + right;
    const float v_sum = top + bottom;
    const Four ratios{h_sum > 0.0F ? left / h_sum : 0.0F, v_sum > 0.0F ? top / v_sum : 0.0F,
                      h_sum > 0.0F ? right / h_sum : 0.0F, v_sum > 0.0F ? bottom / v_sum : 0.0F};
    const std::array<Vec2, 4> reduction{
        Vec2{overflow[kTop] * ratios[kLeft], overflow[kLeft] * ratios[kTop]},
        Vec2{overflow[kTop] * ratios[kRight], overflow[kRight] * ratios[kTop]},
        Vec2{overflow[kBottom] * ratios[kRight], overflow[kRight] * ratios[kBottom]},
        Vec2{overflow[kBottom] * ratios[kLeft], overflow[kLeft] * ratios[kBottom]},
    };
    std::array<Vec2, 4> pcr{};
    std::array<Vec2, 4> leftovers{};
    for (std::size_t i = 0; i < 4; ++i) {
        pcr[i] = {radius[i], radius[i]};
        leftovers[i] = Vec2{} - min_each(pcr[i] - reduction[i], 0.0F);
    }
    std::array<Vec2, 4> distributed{};
    for (std::size_t i = 0; i < 4; ++i) {
        distributed[i] =
            max_each(pcr[i] - reduction[i] - leftovers[(i + 3) % 4] - leftovers[(i + 1) % 4], 0.0F);
    }
    std::array<Vec2, 4> scale{};
    for (std::size_t i = 0; i < 4; ++i) {
        const Vec2 unshrinkable =
            max_each(leftovers[(i + 1) % 4] + leftovers[(i + 3) % 4] - distributed[i], 0.0F);
        const Vec2 divisor = max_each(pcr[i] - unshrinkable, FLT_EPSILON);
        scale[i] = {distributed[i].x / divisor.x, distributed[i].y / divisor.y};
    }
    return scale;
}

// A ring between two rounded rects, or a filled rounded rect, as draw_rounded_rectangle.
void rounded_rectangle(Mesh& mesh, Area style, const Four& corner_radius, Area ring, Area inner,
                       Vec4 inner_color, Vec4 outer_color, int corner_detail, Vec2 skew,
                       bool filled = false) {
    const std::size_t offset = mesh.points.size();
    const bool rounded = std::ranges::any_of(corner_radius, [](float r) { return r > 0.0F; });
    const int detail = rounded ? std::max(corner_detail, 1) : 1;
    const bool border = !filled;

    const Four ring_radius = inner_corner_radius(style, ring, corner_radius);
    const std::array<Vec2, 4> ring_scale = corner_scale(style, ring, ring_radius);
    const std::array<Vec2, 4> outer_points{
        Vec2{ring.x + (ring_radius[0] * ring_scale[0].x),
             ring.y + (ring_radius[0] * ring_scale[0].y)},
        Vec2{ring.x + ring.w - (ring_radius[1] * ring_scale[1].x),
             ring.y + (ring_radius[1] * ring_scale[1].y)},
        Vec2{ring.x + ring.w - (ring_radius[2] * ring_scale[2].x),
             ring.y + ring.h - (ring_radius[2] * ring_scale[2].y)},
        Vec2{ring.x + (ring_radius[3] * ring_scale[3].x),
             ring.y + ring.h - (ring_radius[3] * ring_scale[3].y)},
    };
    const Four inner_radius = inner_corner_radius(style, inner, corner_radius);
    const std::array<Vec2, 4> inner_scale = corner_scale(style, inner, inner_radius);
    const std::array<Vec2, 4> inner_points{
        Vec2{inner.x + (inner_radius[0] * inner_scale[0].x),
             inner.y + (inner_radius[0] * inner_scale[0].y)},
        Vec2{inner.x + inner.w - (inner_radius[1] * inner_scale[1].x),
             inner.y + (inner_radius[1] * inner_scale[1].y)},
        Vec2{inner.x + inner.w - (inner_radius[2] * inner_scale[2].x),
             inner.y + inner.h - (inner_radius[2] * inner_scale[2].y)},
        Vec2{inner.x + (inner_radius[3] * inner_scale[3].x),
             inner.y + inner.h - (inner_radius[3] * inner_scale[3].y)},
    };

    const Vec2 center{style.x + (style.w / 2.0F), style.y + (style.h / 2.0F)};
    const auto skewed = [&](float x, float y) {
        return Vec2{x - (skew.x * (y - center.y)), y - (skew.y * (x - center.x))};
    };
    constexpr float kQuarter = std::numbers::pi_v<float> / 2.0F;
    for (std::size_t c = 0; c < 4; ++c) {
        for (int d = 0; d <= detail; ++d) {
            const float angle =
                ((static_cast<float>(c) + (static_cast<float>(d) / static_cast<float>(detail))) *
                 kQuarter) +
                std::numbers::pi_v<float>;
            const Vec2 cs = cos_sin(angle);
            mesh.points.push_back(
                skewed((inner_radius[c] * cs.x * inner_scale[c].x) + inner_points[c].x,
                       (inner_radius[c] * cs.y * inner_scale[c].y) + inner_points[c].y));
            mesh.colors.push_back(inner_color);
            if (border) {
                mesh.points.push_back(
                    skewed((ring_radius[c] * cs.x * ring_scale[c].x) + outer_points[c].x,
                           (ring_radius[c] * cs.y * ring_scale[c].y) + outer_points[c].y));
                mesh.colors.push_back(outer_color);
            }
        }
    }
    const std::size_t count = mesh.points.size() - offset;
    if (border) {
        for (std::size_t i = 0; i < count; ++i) {
            mesh.indices.insert(
                mesh.indices.end(),
                {offset + (i % count), offset + ((i + 2) % count), offset + ((i + 1) % count)});
        }
    } else {
        // Vertical stripes of two triangles each.
        const std::size_t stripes = (count / 2) - 1;
        const std::size_t last = count - 1;
        for (std::size_t i = 0; i < stripes; ++i) {
            mesh.indices.insert(mesh.indices.end(),
                                {offset + i, offset + last - i - 1, offset + i + 1, offset + i,
                                 offset + last - i, offset + last - i - 1});
        }
    }
}

// Godot's adapt_values: shrinks a pair of opposite borders or corners to fit a width.
void adapt_values(std::size_t a, std::size_t b, Four& adapted, const Four& values, float width,
                  float max_a, float max_b) {
    const float sum = values[a] + values[b];
    const float factor = sum == 0.0F ? 1.0F : std::min(1.0F, width / sum);
    adapted[a] = std::min(std::min(values[a] * factor, max_a), adapted[a]);
    adapted[b] = std::min(std::min(values[b] * factor, max_b), adapted[b]);
}

Vec4 transparent(Vec4 color) {
    return {color.x, color.y, color.z, 0.0F};
}

// A flat style box's triangles in its control's space.
Mesh flat_mesh(const StyleBox& style, Area rect) {
    Mesh mesh;
    const bool draw_border = std::ranges::any_of(style.border_width, [](int w) { return w > 0; });
    const bool draw_shadow = style.shadow_size > 0;
    if (!draw_border && !style.draw_center && !draw_shadow) {
        return mesh;
    }
    const Area style_rect =
        grow_individual(rect, style.expand_margin[kLeft], style.expand_margin[kTop],
                        style.expand_margin[kRight], style.expand_margin[kBottom]);
    if (std::abs(style_rect.w) < 1e-5F || std::abs(style_rect.h) < 1e-5F) {
        return mesh;
    }
    const bool rounded = std::ranges::any_of(style.corner_radius, [](int r) { return r > 0; });
    const bool skewed = std::abs(style.skew.x) > 1e-5F || std::abs(style.skew.y) > 1e-5F;
    const bool aa_on = (rounded || skewed) && style.anti_aliasing;
    const bool blend_on = style.border_blend && draw_border;
    const Vec4 border_alpha = transparent(style.border_color);
    const Vec4 border_blend = style.draw_center ? style.bg_color : border_alpha;
    const Vec4 border_inner = blend_on ? border_blend : style.border_color;
    const float aa = aa_on ? style.anti_aliasing_size : 1.0F;
    const int detail = style.corner_detail;

    const float aa_shrink = aa_on ? aa * 2.0F : 0.0F;
    const float width = std::max(style_rect.w - aa_shrink, 0.0F);
    const float height = std::max(style_rect.h - aa_shrink, 0.0F);
    Four border_width{};
    Four corner_radius{};
    for (std::size_t i = 0; i < 4; ++i) {
        border_width[i] = static_cast<float>(style.border_width[i]);
        corner_radius[i] = static_cast<float>(style.corner_radius[i]);
    }
    Four border{1e6F, 1e6F, 1e6F, 1e6F};
    adapt_values(kTop, kBottom, border, border_width, height, height, height);
    adapt_values(kLeft, kRight, border, border_width, width, width, width);
    Four corner{1e6F, 1e6F, 1e6F, 1e6F};
    adapt_values(kTopRight, kBottomRight, corner, corner_radius, height, height - border[kBottom],
                 height - border[kTop]);
    adapt_values(kTopLeft, kBottomLeft, corner, corner_radius, height, height - border[kBottom],
                 height - border[kTop]);
    adapt_values(kTopLeft, kTopRight, corner, corner_radius, width, width - border[kRight],
                 width - border[kLeft]);
    adapt_values(kBottomLeft, kBottomRight, corner, corner_radius, width, width - border[kRight],
                 width - border[kLeft]);

    const Area infill = grow_individual(style_rect, -border[kLeft], -border[kTop], -border[kRight],
                                        -border[kBottom]);
    Area border_style = style_rect;
    if (aa_on) {
        for (std::size_t i = 0; i < 4; ++i) {
            if (style.border_width[i] > 0) {
                border_style = grow_side(border_style, i, -aa);
            }
        }
    }

    if (draw_shadow) {
        const auto size = static_cast<float>(style.shadow_size);
        Area inner = style_rect;
        inner.x += style.shadow_offset.x;
        inner.y += style.shadow_offset.y;
        Area outer = grow_individual(style_rect, size, size, size, size);
        outer.x += style.shadow_offset.x;
        outer.y += style.shadow_offset.y;
        rounded_rectangle(mesh, inner, corner, outer, inner, style.shadow_color,
                          transparent(style.shadow_color), detail, style.skew);
        if (style.draw_center) {
            rounded_rectangle(mesh, inner, corner, inner, inner, style.shadow_color,
                              style.shadow_color, detail, style.skew, true);
        }
    }
    if (draw_border && !aa_on) {
        rounded_rectangle(mesh, border_style, corner, border_style, infill, border_inner,
                          style.border_color, detail, style.skew);
    }
    if (style.draw_center && (!aa_on || blend_on)) {
        rounded_rectangle(mesh, border_style, corner, infill, infill, style.bg_color,
                          style.bg_color, detail, style.skew, true);
    }
    if (aa_on) {
        Four border_aa{};
        Four border_aa_half{};
        Four fill_aa{};
        Four fill_aa_half{};
        for (std::size_t i = 0; i < 4; ++i) {
            if (draw_border && style.border_width[i] > 0) {
                border_aa[i] = aa;
                border_aa_half[i] = aa * 0.5F;
            } else {
                fill_aa[i] = aa;
                fill_aa_half[i] = aa * 0.5F;
            }
        }
        if (style.draw_center) {
            const Area fill_transparent =
                grow_individual(infill, fill_aa_half[kLeft], fill_aa_half[kTop],
                                fill_aa_half[kRight], fill_aa_half[kBottom]);
            const Area fill_colored =
                grow_individual(fill_transparent, -fill_aa[kLeft], -fill_aa[kTop], -fill_aa[kRight],
                                -fill_aa[kBottom]);
            if (!blend_on) {
                rounded_rectangle(mesh, border_style, corner, fill_colored, fill_colored,
                                  style.bg_color, style.bg_color, detail, style.skew, true);
            }
            if (!blend_on || !draw_border) {
                rounded_rectangle(mesh, border_style, corner, fill_transparent, fill_colored,
                                  style.bg_color, transparent(style.bg_color), detail, style.skew);
            }
        }
        if (draw_border) {
            const Area inner_colored =
                grow_individual(infill, border_aa_half[kLeft], border_aa_half[kTop],
                                border_aa_half[kRight], border_aa_half[kBottom]);
            const Area inner_transparent =
                grow_individual(inner_colored, -border_aa[kLeft], -border_aa[kTop],
                                -border_aa[kRight], -border_aa[kBottom]);
            const Area outer_transparent =
                grow_individual(style_rect, border_aa_half[kLeft], border_aa_half[kTop],
                                border_aa_half[kRight], border_aa_half[kBottom]);
            const Area outer_colored =
                grow_individual(border_style, border_aa_half[kLeft], border_aa_half[kTop],
                                border_aa_half[kRight], border_aa_half[kBottom]);
            rounded_rectangle(mesh, border_style, corner, outer_colored,
                              blend_on ? infill : inner_colored, border_inner, style.border_color,
                              detail, style.skew);
            if (!blend_on) {
                rounded_rectangle(mesh, border_style, corner, inner_colored, inner_transparent,
                                  border_blend, style.border_color, detail, style.skew);
            }
            rounded_rectangle(mesh, border_style, corner, outer_transparent, outer_colored,
                              style.border_color, border_alpha, detail, style.skew);
        }
    }
    return mesh;
}

void draw_flat(Drawer& drawer, const Box& box, const Placed& placed, Vec4 tint) {
    const StyleBox& style = box.style;
    const Area rect{box.position.x, box.position.y, box.size.x, box.size.y};
    const bool plain = style.draw_center && style.shadow_size <= 0 &&
                       std::ranges::all_of(style.border_width, [](int w) { return w <= 0; }) &&
                       std::ranges::all_of(style.corner_radius, [](int r) { return r <= 0; }) &&
                       style.skew == Vec2{};
    if (plain) {
        // Godot's triangles for a rect with no corners, border, shadow or skew cover just the
        // rect, so it is one quad.
        const Rect bounds{rect.x - style.expand_margin[kLeft], rect.y - style.expand_margin[kTop],
                          rect.x + rect.w + style.expand_margin[kRight],
                          rect.y + rect.h + style.expand_margin[kBottom]};
        if (bounds.x1 - bounds.x0 > 0.0F && bounds.y1 - bounds.y0 > 0.0F) {
            drawer.quad({}, placed.transform, bounds, {0.0F, 0.0F, 1.0F, 1.0F},
                        times(style.bg_color, tint), std::nullopt);
        }
        return;
    }
    const Mesh mesh = flat_mesh(style, rect);
    std::vector<render::Vertex2D> vertices;
    vertices.reserve(mesh.indices.size());
    for (const std::size_t i : mesh.indices) {
        const Vec2 p = placed.transform.apply(mesh.points[i]);
        const Vec4 c = times(mesh.colors[i], tint);
        vertices.push_back({.x = p.x,
                            .y = p.y,
                            .u = 0.0F,
                            .v = 0.0F,
                            .rgba = {to_byte(c.x), to_byte(c.y), to_byte(c.z), to_byte(c.w)}});
    }
    drawer.triangles({}, vertices);
}

// StyleBoxLine::draw: a rect of its thickness along the top of the rect, or down its left side,
// grown at both ends, in whole pixels as Godot's Rect2i keeps it.
void draw_line(Drawer& drawer, const Box& box, const Placed& placed, Vec4 tint) {
    const StyleBox& style = box.style;
    float x = std::trunc(box.position.x);
    float y = std::trunc(box.position.y);
    float w = std::trunc(box.size.x);
    float h = std::trunc(box.size.y);
    const auto thickness = static_cast<float>(style.thickness);
    if (style.vertical) {
        y = std::trunc(y - style.grow_begin);
        h = std::trunc(h + (style.grow_begin + style.grow_end));
        w = thickness;
    } else {
        x = std::trunc(x - style.grow_begin);
        w = std::trunc(w + (style.grow_begin + style.grow_end));
        h = thickness;
    }
    if (w > 0.0F && h > 0.0F) {
        drawer.quad({}, placed.transform, {x, y, x + w, y + h}, {0.0F, 0.0F, 1.0F, 1.0F},
                    times(style.color, tint), std::nullopt);
    }
}

// ---- Texture style boxes, as Godot's nine-patch drawing (map_ninepatch_axis in the canvas
// shaders) ----

// A stretch of a nine-patch along one axis: where it draws and the texture pixels it shows,
// from the region's start.
struct Segment {
    float d0 = 0.0F;
    float d1 = 0.0F;
    float u0 = 0.0F;
    float u1 = 0.0F;
    bool middle = false;
};

// More tiles than this in a row fit this many instead, as tile_fit does.
constexpr int kMaxTiles = 256;

std::vector<Segment> segments(float size, float source, float begin, float end,
                              StyleBox::Stretch mode) {
    std::vector<Segment> out;
    const float begin_end = std::min(begin, size);
    if (begin_end > 0.0F) {
        out.push_back({.d0 = 0.0F, .d1 = begin_end, .u0 = 0.0F, .u1 = begin_end});
    }
    const float middle_end = size - end;
    if (middle_end > begin) {
        const float span = middle_end - begin;
        const float tile = source - begin - end;
        int tiles = 1;
        if (mode == StyleBox::Stretch::Tile && tile > 0.0F) {
            tiles = static_cast<int>(std::ceil(span / tile));
        } else if (mode == StyleBox::Stretch::TileFit) {
            tiles =
                std::max(1, static_cast<int>(std::floor((span / std::max(tile, 1e-7F)) + 0.5F)));
        }
        if (mode == StyleBox::Stretch::Tile && tile > 0.0F && tiles <= kMaxTiles) {
            for (int t = 0; begin + (static_cast<float>(t) * tile) < middle_end; ++t) {
                const float d = begin + (static_cast<float>(t) * tile);
                const float d1 = std::min(d + tile, middle_end);
                out.push_back(
                    {.d0 = d, .d1 = d1, .u0 = begin, .u1 = begin + (d1 - d), .middle = true});
            }
        } else {
            tiles = std::min(tiles, kMaxTiles);
            const float step = span / static_cast<float>(tiles);
            for (int t = 0; t < tiles; ++t) {
                const float d0 = begin + (step * static_cast<float>(t));
                const float d1 = t + 1 == tiles ? middle_end : d0 + step;
                out.push_back(
                    {.d0 = d0, .d1 = d1, .u0 = begin, .u1 = source - end, .middle = true});
            }
        }
    }
    const float end_start = std::max(begin, size - end);
    if (size > end_start) {
        out.push_back(
            {.d0 = end_start, .d1 = size, .u0 = source - (size - end_start), .u1 = source});
    }
    return out;
}

void draw_texture(Drawer& drawer, const Box& box, const Placed& placed, Vec4 tint) {
    const StyleBox& style = box.style;
    const Rect rect{box.position.x - style.expand_margin[kLeft],
                    box.position.y - style.expand_margin[kTop],
                    box.position.x + box.size.x + style.expand_margin[kRight],
                    box.position.y + box.size.y + style.expand_margin[kBottom]};
    const Vec2 texture = box.texture_size;
    if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0 || texture.x <= 0.0F || texture.y <= 0.0F) {
        return;
    }
    Vec4 region = style.region_rect;
    if (region.z <= 0.0F || region.w <= 0.0F) {
        region = {0.0F, 0.0F, texture.x, texture.y};
    }
    const std::vector<Segment> columns =
        segments(rect.x1 - rect.x0, region.z, style.texture_margin[kLeft],
                 style.texture_margin[kRight], style.axis_stretch_horizontal);
    const std::vector<Segment> rows =
        segments(rect.y1 - rect.y0, region.w, style.texture_margin[kTop],
                 style.texture_margin[kBottom], style.axis_stretch_vertical);
    DrawCall source;
    source.source = DrawCall::Source::Texture;
    source.texture = style.texture;
    source.filter = placed.texture_filter;
    const Vec4 color = times(style.modulate_color, tint);
    for (const Segment& row : rows) {
        for (const Segment& column : columns) {
            if (row.middle && column.middle && !style.draw_center) {
                continue;
            }
            // A flip mirrors the whole drawing: each cell moves to the other side and shows its
            // texture backwards.
            float x0 = rect.x0 + column.d0;
            float x1 = rect.x0 + column.d1;
            float u0 = (region.x + column.u0) / texture.x;
            float u1 = (region.x + column.u1) / texture.x;
            if (style.flip_h) {
                x0 = rect.x1 - column.d1;
                x1 = rect.x1 - column.d0;
                std::swap(u0, u1);
            }
            float y0 = rect.y0 + row.d0;
            float y1 = rect.y0 + row.d1;
            float v0 = (region.y + row.u0) / texture.y;
            float v1 = (region.y + row.u1) / texture.y;
            if (style.flip_v) {
                y0 = rect.y1 - row.d1;
                y1 = rect.y1 - row.d0;
                std::swap(v0, v1);
            }
            drawer.quad(source, placed.transform, {x0, y0, x1, y1}, {u0, v0, u1, v1}, color,
                        std::nullopt);
        }
    }
}

// A style box of any type.
void draw_box(Drawer& drawer, const Box& box, const Placed& placed, Vec4 tint) {
    switch (box.style.type) {
    case StyleBox::Type::Texture:
        draw_texture(drawer, box, placed, tint);
        break;
    case StyleBox::Type::Line:
        draw_line(drawer, box, placed, tint);
        break;
    case StyleBox::Type::Flat:
    case StyleBox::Type::Empty:
        draw_flat(drawer, box, placed, tint);
        break;
    }
}

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
    const Vec4 selected = times(label.selected_color, tint);
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
            for (std::uint32_t g = 0; g < line.count; ++g) {
                const text::Glyph& glyph = label.text.glyphs[line.first + g];
                if (text::stretches(line, glyph)) {
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
            const bool in_selection = pass == Pass::Text && glyph.cluster >= label.selected_start &&
                                      glyph.cluster < label.selected_end;
            drawer.quad(source, placed.transform, rect, uv, in_selection ? selected : color, clip);
            ++list.glyphs;
        }
    }
}

// ---- Clipping, as a scissor cuts what an item with clip_contents and its children draw ----

// A vertex with its color as numbers, for cutting triangles between vertices.
struct Corner {
    float x = 0.0F;
    float y = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
    std::array<float, 4> rgba{};
};

Corner corner_of(const render::Vertex2D& vertex) {
    return {.x = vertex.x,
            .y = vertex.y,
            .u = vertex.u,
            .v = vertex.v,
            .rgba = {static_cast<float>(vertex.rgba[0]), static_cast<float>(vertex.rgba[1]),
                     static_cast<float>(vertex.rgba[2]), static_cast<float>(vertex.rgba[3])}};
}

render::Vertex2D vertex_of(const Corner& corner) {
    const auto byte = [](float value) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 255.0F)));
    };
    return {.x = corner.x,
            .y = corner.y,
            .u = corner.u,
            .v = corner.v,
            .rgba = {byte(corner.rgba[0]), byte(corner.rgba[1]), byte(corner.rgba[2]),
                     byte(corner.rgba[3])}};
}

Corner between(const Corner& a, const Corner& b, float t) {
    const auto mix = [t](float from, float to) { return from + ((to - from) * t); };
    return {.x = mix(a.x, b.x),
            .y = mix(a.y, b.y),
            .u = mix(a.u, b.u),
            .v = mix(a.v, b.v),
            .rgba = {mix(a.rgba[0], b.rgba[0]), mix(a.rgba[1], b.rgba[1]),
                     mix(a.rgba[2], b.rgba[2]), mix(a.rgba[3], b.rgba[3])}};
}

// Cuts triangles, three vertices each, to a rect in viewport pixels [left, top, right,
// bottom]: those inside stay as they are, those outside go, and those across an edge are cut
// to the polygon inside it (Sutherland and Hodgman's way) and drawn as a fan. Returns whether
// any was cut or dropped.
bool clip_triangles(std::vector<render::Vertex2D>& vertices, Vec4 clip) {
    const auto inside = [&](const render::Vertex2D& v) {
        return v.x >= clip.x && v.x <= clip.z && v.y >= clip.y && v.y <= clip.w;
    };
    std::vector<render::Vertex2D> out;
    out.reserve(vertices.size());
    bool cut = false;
    std::vector<Corner> polygon;
    std::vector<Corner> next;
    for (std::size_t first = 0; first + 3 <= vertices.size(); first += 3) {
        const render::Vertex2D* triangle = &vertices[first];
        if (inside(triangle[0]) && inside(triangle[1]) && inside(triangle[2])) {
            out.insert(out.end(), triangle, triangle + 3);
            continue;
        }
        cut = true;
        polygon = {corner_of(triangle[0]), corner_of(triangle[1]), corner_of(triangle[2])};
        // Each edge: the part of the polygon on its inner side.
        for (int edge = 0; edge < 4 && !polygon.empty(); ++edge) {
            const auto distance = [&](const Corner& c) {
                switch (edge) {
                case 0:
                    return c.x - clip.x;
                case 1:
                    return clip.z - c.x;
                case 2:
                    return c.y - clip.y;
                default:
                    return clip.w - c.y;
                }
            };
            next.clear();
            for (std::size_t i = 0; i < polygon.size(); ++i) {
                const Corner& a = polygon[i];
                const Corner& b = polygon[(i + 1) % polygon.size()];
                const float da = distance(a);
                const float db = distance(b);
                if (da >= 0.0F) {
                    next.push_back(a);
                }
                if ((da >= 0.0F) != (db >= 0.0F)) {
                    next.push_back(between(a, b, da / (da - db)));
                }
            }
            std::swap(polygon, next);
        }
        for (std::size_t i = 1; i + 1 < polygon.size(); ++i) {
            out.insert(out.end(),
                       {vertex_of(polygon[0]), vertex_of(polygon[i]), vertex_of(polygon[i + 1])});
        }
    }
    if (cut) {
        vertices = std::move(out);
    }
    return cut;
}

// Where each control's drawing over its children goes: after the last control drawn within it
// at its z, which come right after it in drawing order. Controls inside others come first.
std::map<std::size_t, std::vector<std::size_t>> over_children_order(const Layout& layout) {
    std::map<std::size_t, std::vector<std::size_t>> order;
    const auto within = [&](entt::entity entity, entt::entity ancestor) {
        for (const Placed* at = layout.find(entity); at != nullptr; at = layout.find(at->parent)) {
            if (at->parent == ancestor) {
                return true;
            }
        }
        return false;
    };
    for (const auto& [entity, boxes] : layout.over_children) {
        const auto found = layout.index.find(entity);
        if (found == layout.index.end()) {
            continue;
        }
        const std::size_t own = found->second;
        const int z = layout.controls[own].z;
        std::size_t last = own;
        while (last + 1 < layout.controls.size() && layout.controls[last + 1].z == z &&
               within(layout.controls[last + 1].entity, entity)) {
            ++last;
        }
        order[last].push_back(own);
    }
    for (auto& [last, controls] : order) {
        std::ranges::sort(controls, std::greater<>());
    }
    return order;
}

} // namespace

DrawList draw_calls(const Layout& layout, const world::World& world, text::Fonts& fonts) {
    DrawList list;
    Drawer drawer(list);
    const auto& ecs = world.ecs();
    const std::map<std::size_t, std::vector<std::size_t>> over = over_children_order(layout);
    std::vector<bool> counted(layout.controls.size(), false);
    // What a control drew since calls_before (and vertices_before of the call before it), cut
    // to its clip, as spans.
    const auto finish = [&](const Placed& placed, std::size_t calls_before,
                            std::size_t vertices_before, bool popup, bool tooltip = false) {
        const std::size_t from = calls_before == 0 ? 0 : calls_before - 1;
        if (placed.clip) {
            for (std::size_t call = from; call < list.calls.size(); ++call) {
                const std::size_t first = call + 1 == calls_before ? vertices_before : 0;
                std::vector<render::Vertex2D>& all = list.calls[call].vertices;
                if (all.size() <= first) {
                    continue;
                }
                const auto start = all.begin() + static_cast<std::ptrdiff_t>(first);
                std::vector<render::Vertex2D> part(start, all.end());
                const std::vector<render::Vertex2D> whole = part;
                if (!clip_triangles(part, *placed.clip)) {
                    continue;
                }
                all.erase(start, all.end());
                all.insert(all.end(), part.begin(), part.end());
                DrawCall uncut = list.calls[call];
                uncut.vertices = whole;
                list.uncut.push_back(
                    {.entity = placed.entity, .call = std::move(uncut), .popup = popup});
            }
            // Its own calls that it cut down to nothing.
            for (std::size_t call = list.calls.size(); call > calls_before; --call) {
                if (list.calls[call - 1].vertices.empty()) {
                    list.calls.erase(list.calls.begin() + static_cast<std::ptrdiff_t>(call - 1));
                }
            }
        }
        // What it added: the rest of the call it may have gone on with, then its own calls.
        for (std::size_t call = from; call < list.calls.size(); ++call) {
            const std::size_t first = call + 1 == calls_before ? vertices_before : 0;
            const std::size_t end = list.calls[call].vertices.size();
            if (end > first) {
                list.spans.push_back({.entity = placed.entity,
                                      .call = call,
                                      .first = first,
                                      .count = end - first,
                                      .popup = popup,
                                      .tooltip = tooltip});
            }
        }
    };
    const auto vertices_now = [&] {
        return list.calls.empty() ? std::size_t{0} : list.calls.back().vertices.size();
    };
    for (std::size_t index = 0; index < layout.controls.size(); ++index) {
        const Placed& placed = layout.controls[index];
        if (placed.visible && world.valid(placed.entity)) {
            const auto* control = ecs.try_get<world::Control>(placed.entity);
            const Vec4 tint = times(
                placed.modulate, (control != nullptr ? *control : kDefaultControl).self_modulate);
            const Rect bounds{0.0F, 0.0F, placed.size.x, placed.size.y};
            const std::size_t calls_before = list.calls.size();
            const std::size_t vertices_before = vertices_now();
            bool drawn = false;
            if (const auto found = layout.boxes.find(placed.entity); found != layout.boxes.end()) {
                for (const Box& box : found->second) {
                    draw_box(drawer, box, placed, tint);
                    ++list.boxes;
                }
                drawn = true;
            }
            if (const auto found = layout.percentages.find(placed.entity);
                found != layout.percentages.end()) {
                const LabelText& percent = found->second;
                if (percent.outline_size > 0 && percent.outline_color.w > 0.0F) {
                    draw_label_pass(drawer, list, fonts, percent, placed, tint, Pass::Outline,
                                    std::nullopt);
                }
                draw_label_pass(drawer, list, fonts, percent, placed, tint, Pass::Text,
                                std::nullopt);
            }
            if (const LabelText* label = layout.label(placed.entity); label != nullptr) {
                const std::optional<Rect> clip = label->clip ? std::optional(bounds) : std::nullopt;
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
            if (const auto found = layout.overlays.find(placed.entity);
                found != layout.overlays.end()) {
                for (const Box& box : found->second) {
                    draw_flat(drawer, box, placed, tint);
                    ++list.boxes;
                }
                drawn = true;
            }
            if (drawn) {
                ++list.controls;
                counted[index] = true;
            }
            finish(placed, calls_before, vertices_before, false);
        }
        // Then what the controls this one ends draw over their children, as Godot draws a
        // ScrollContainer's scroll bars, which are its last children.
        const auto after = over.find(index);
        if (after == over.end()) {
            continue;
        }
        for (const std::size_t own : after->second) {
            const Placed& container = layout.controls[own];
            if (!container.visible || !world.valid(container.entity)) {
                continue;
            }
            const std::size_t calls_before = list.calls.size();
            const std::size_t vertices_before = vertices_now();
            for (const Box& box : layout.over_children.at(container.entity)) {
                draw_box(drawer, box, container, container.modulate);
                ++list.boxes;
            }
            if (!counted[own]) {
                ++list.controls;
                counted[own] = true;
            }
            finish(container, calls_before, vertices_before, false);
        }
    }
    const Vec4 white{1.0F, 1.0F, 1.0F, 1.0F};
    // A label's passes, as a Label draws them.
    const auto draw_label = [&](const LabelText& label, const Placed& at) {
        if (label.shadow_color.w != 0.0F && label.shadow_outline_size > 0) {
            draw_label_pass(drawer, list, fonts, label, at, white, Pass::ShadowOutline,
                            std::nullopt);
        }
        if (label.shadow_color.w > 0.0F) {
            draw_label_pass(drawer, list, fonts, label, at, white, Pass::Shadow, std::nullopt);
        }
        if (label.outline_size > 0 && label.outline_color.w != 0.0F) {
            draw_label_pass(drawer, list, fonts, label, at, white, Pass::Outline, std::nullopt);
        }
        draw_label_pass(drawer, list, fonts, label, at, white, Pass::Text, std::nullopt);
    };
    // A text field's dragged text, at the pointer over the controls.
    if (layout.drag && layout.drag->text && world.valid(layout.drag->from)) {
        Placed placed;
        placed.entity = layout.drag->from;
        placed.transform.tx = layout.drag->position.x;
        placed.transform.ty = layout.drag->position.y;
        const std::size_t calls_before = list.calls.size();
        const std::size_t vertices_before = vertices_now();
        draw_label(*layout.drag->text, placed);
        finish(placed, calls_before, vertices_before, false);
    }
    // The popup menus open, over every control, as Godot draws embedded windows over the
    // viewport's canvas: the panel, then the items cut to the list's room, then the scroll bar.
    for (const PopupView& popup : layout.popups) {
        if (!world.valid(popup.owner)) {
            continue;
        }
        Placed placed;
        placed.entity = popup.owner;
        placed.transform.tx = popup.position.x;
        placed.transform.ty = popup.position.y;
        const auto boxes = [&](const Placed& at, const std::vector<Box>& list_boxes) {
            const std::size_t calls_before = list.calls.size();
            const std::size_t vertices_before = vertices_now();
            for (const Box& box : list_boxes) {
                draw_box(drawer, box, at, white);
                ++list.boxes;
            }
            finish(at, calls_before, vertices_before, true);
        };
        boxes(placed, popup.panel);
        Placed items = placed;
        const std::array<float, 4>& m = popup.margins;
        items.clip =
            Vec4{popup.position.x + m[0], popup.position.y + m[1],
                 popup.position.x + popup.size.x - m[2], popup.position.y + popup.size.y - m[3]};
        const std::size_t calls_before = list.calls.size();
        const std::size_t vertices_before = vertices_now();
        for (const Box& box : popup.boxes) {
            draw_box(drawer, box, items, white);
            ++list.boxes;
        }
        for (const LabelText& label : popup.labels) {
            if (label.outline_size > 0 && label.outline_color.w > 0.0F) {
                draw_label_pass(drawer, list, fonts, label, items, white, Pass::Outline,
                                std::nullopt);
            }
            draw_label_pass(drawer, list, fonts, label, items, white, Pass::Text, std::nullopt);
        }
        finish(items, calls_before, vertices_before, true);
        boxes(placed, popup.bar);
        ++list.controls;
    }
    // The tooltip, a window over the popup menus too.
    if (layout.tooltip && world.valid(layout.tooltip->owner)) {
        const TooltipView& tip = *layout.tooltip;
        Placed placed;
        placed.entity = tip.owner;
        placed.transform.tx = tip.position.x;
        placed.transform.ty = tip.position.y;
        const std::size_t calls_before = list.calls.size();
        const std::size_t vertices_before = vertices_now();
        for (const Box& box : tip.panel) {
            draw_box(drawer, box, placed, white);
            ++list.boxes;
        }
        draw_label(tip.label, placed);
        finish(placed, calls_before, vertices_before, false, true);
        ++list.controls;
    }
    return list;
}

} // namespace slash::ui
