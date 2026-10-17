#include "slash/canvas/canvas.hpp"
#include "slash/render/device.hpp"
#include "slash/ui/ui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace slash::canvas {

namespace {

// The null device and renderer summaries draw with, kept between summaries.
struct Summarizer {
    asset::Assets* assets = nullptr;
    std::unique_ptr<render::RenderDevice> device;
    std::unique_ptr<CanvasRenderer> renderer;
};

// Copies of a world's context hold this, so it is shared rather than copied.
using SummarizerHandle = std::shared_ptr<Summarizer>;

// Barycentric weights a hair below zero still count as inside, so a pixel center on the edge
// two triangles share hits one of them.
constexpr float kEdge = -1e-5F;
// Things are sampled at up to this many pixels across and down.
constexpr float kMaxSamples = 64.0F;
// Bins are at least this many pixels square, and at most this many go across or down.
constexpr float kMinBin = 16.0F;
constexpr float kMaxBins = 128.0F;

struct Bounds {
    float x0 = std::numeric_limits<float>::max();
    float y0 = std::numeric_limits<float>::max();
    float x1 = std::numeric_limits<float>::lowest();
    float y1 = std::numeric_limits<float>::lowest();

    void add(float x, float y) {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    void add(const Bounds& other) {
        add(other.x0, other.y0);
        add(other.x1, other.y1);
    }
    [[nodiscard]] bool empty() const { return x0 > x1 || y0 > y1; }
    [[nodiscard]] bool touches(const Bounds& other) const {
        return x0 < other.x1 && x1 > other.x0 && y0 < other.y1 && y1 > other.y0;
    }
    [[nodiscard]] bool contains(float x, float y) const {
        return x >= x0 && x < x1 && y >= y0 && y < y1;
    }
};

// One of an item's triangles.
struct Triangle {
    std::size_t item = 0;
    const FramePiece* piece = nullptr;
    const render::Vertex2D* corners = nullptr; // three
};

// The alpha a triangle draws at a point: its texture's there times its color's, or -1 when the
// point is outside it.
float alpha_at(const Triangle& triangle, float x, float y) {
    const render::Vertex2D& a = triangle.corners[0];
    const render::Vertex2D& b = triangle.corners[1];
    const render::Vertex2D& c = triangle.corners[2];
    const float area = ((b.x - a.x) * (c.y - a.y)) - ((c.x - a.x) * (b.y - a.y));
    if (area == 0.0F) {
        return -1.0F;
    }
    const float wa = (((b.x - x) * (c.y - y)) - ((c.x - x) * (b.y - y))) / area;
    const float wb = (((c.x - x) * (a.y - y)) - ((a.x - x) * (c.y - y))) / area;
    const float wc = 1.0F - wa - wb;
    if (wa < kEdge || wb < kEdge || wc < kEdge) {
        return -1.0F;
    }
    const float color =
        ((wa * static_cast<float>(a.rgba[3])) + (wb * static_cast<float>(b.rgba[3])) +
         (wc * static_cast<float>(c.rgba[3]))) /
        255.0F;
    const FramePiece& piece = *triangle.piece;
    if (piece.pixels == nullptr || piece.width == 0 || piece.height == 0) {
        return color;
    }
    // The texel the point samples, nearest.
    const float u = (wa * a.u) + (wb * b.u) + (wc * c.u);
    const float v = (wa * a.v) + (wb * b.v) + (wc * c.v);
    const auto width = static_cast<float>(piece.width);
    const auto height = static_cast<float>(piece.height);
    const auto column =
        static_cast<std::size_t>(std::clamp(std::floor(u * width), 0.0F, width - 1));
    const auto row = static_cast<std::size_t>(std::clamp(std::floor(v * height), 0.0F, height - 1));
    const std::uint8_t* texel = piece.pixels + (((row * piece.width) + column) * 4);
    if (piece.field.pixel_range > 0.0F) {
        // Pixels of the window per texel, from the triangle's size in each.
        const float texels =
            (((b.u - a.u) * (c.v - a.v)) - ((c.u - a.u) * (b.v - a.v))) * width * height;
        const float scale = texels != 0.0F ? std::sqrt(std::abs(area / texels)) : 1.0F;
        const std::array<float, 4> sample{
            static_cast<float>(texel[0]) / 255.0F, static_cast<float>(texel[1]) / 255.0F,
            static_cast<float>(texel[2]) / 255.0F, static_cast<float>(texel[3]) / 255.0F};
        return color * render::distance_field_coverage(sample, piece.field, scale);
    }
    return color * static_cast<float>(texel[3]) / 255.0F;
}

// Triangles by where they are: each bin lists those whose bounds reach it, in drawing order.
class Bins {
public:
    Bins(const Bounds& area, std::span<const Triangle> triangles) : triangles_(triangles) {
        if (area.empty() || triangles.empty()) {
            return;
        }
        origin_x_ = area.x0;
        origin_y_ = area.y0;
        size_ = std::max({kMinBin, (area.x1 - area.x0) / kMaxBins, (area.y1 - area.y0) / kMaxBins});
        columns_ = std::max<std::size_t>(
            static_cast<std::size_t>(std::ceil((area.x1 - area.x0) / size_)), 1);
        rows_ = std::max<std::size_t>(
            static_cast<std::size_t>(std::ceil((area.y1 - area.y0) / size_)), 1);
        bins_.resize(columns_ * rows_);
        for (std::size_t i = 0; i < triangles.size(); ++i) {
            Bounds bounds;
            for (std::size_t corner = 0; corner < 3; ++corner) {
                bounds.add(triangles[i].corners[corner].x, triangles[i].corners[corner].y);
            }
            const std::size_t left = column(bounds.x0);
            const std::size_t right = column(bounds.x1);
            const std::size_t top = row(bounds.y0);
            const std::size_t bottom = row(bounds.y1);
            for (std::size_t r = top; r <= bottom; ++r) {
                for (std::size_t c = left; c <= right; ++c) {
                    bins_[(r * columns_) + c].push_back(i);
                }
            }
        }
    }

    // The triangles that may hold a point, in drawing order.
    [[nodiscard]] std::span<const std::size_t> at(float x, float y) const {
        if (bins_.empty()) {
            return {};
        }
        return bins_[(row(y) * columns_) + column(x)];
    }

    [[nodiscard]] const Triangle& triangle(std::size_t index) const { return triangles_[index]; }

private:
    [[nodiscard]] std::size_t column(float x) const {
        return static_cast<std::size_t>(std::clamp(std::floor((x - origin_x_) / size_), 0.0F,
                                                   static_cast<float>(columns_ - 1)));
    }
    [[nodiscard]] std::size_t row(float y) const {
        return static_cast<std::size_t>(
            std::clamp(std::floor((y - origin_y_) / size_), 0.0F, static_cast<float>(rows_ - 1)));
    }

    std::span<const Triangle> triangles_;
    float origin_x_ = 0.0F;
    float origin_y_ = 0.0F;
    float size_ = kMinBin;
    std::size_t columns_ = 0;
    std::size_t rows_ = 0;
    std::vector<std::vector<std::size_t>> bins_;
};

constexpr std::size_t kNothing = std::numeric_limits<std::size_t>::max();
// The share a thing too thin for its samples gets when the map finds it.
constexpr float kLeastShare = 0.01F;

// Samples a rect at pixel centers, at most kMaxSamples across and down, calling visit(x, y) for
// each. Returns the pixels each sample stands for.
template <typename Visit>
float sample(const Bounds& rect, const Visit& visit) {
    const float across = std::max(1.0F, std::ceil((rect.x1 - rect.x0) / kMaxSamples));
    const float down = std::max(1.0F, std::ceil((rect.y1 - rect.y0) / kMaxSamples));
    const float left = std::floor(rect.x0) + 0.5F;
    const float top = std::floor(rect.y0) + 0.5F;
    for (int row = 0; top + (static_cast<float>(row) * down) < rect.y1; ++row) {
        const float y = top + (static_cast<float>(row) * down);
        for (int column = 0; left + (static_cast<float>(column) * across) < rect.x1; ++column) {
            visit(left + (static_cast<float>(column) * across), y);
        }
    }
    return across * down;
}

// The items drawn at a point, last drawn first, each with its alpha there: its triangles' over
// one another, as blending lays each over the last. Calls visit(item, alpha) for each until it
// returns false.
template <typename Visit>
void layers_at(const Bins& bins, float x, float y, const Visit& visit) {
    const std::span<const std::size_t> near = bins.at(x, y);
    std::size_t item = kNothing;
    float through = 1.0F; // what the item's triangles so far let through
    for (const std::size_t index : std::ranges::reverse_view(near)) {
        const Triangle& triangle = bins.triangle(index);
        const float alpha = alpha_at(triangle, x, y);
        if (alpha < 0.0F) {
            continue;
        }
        if (triangle.item != item) {
            if (item != kNothing && !visit(item, 1.0F - through)) {
                return;
            }
            item = triangle.item;
            through = 1.0F;
        }
        through *= 1.0F - std::clamp(alpha, 0.0F, 1.0F);
    }
    if (item != kNothing) {
        (void)visit(item, 1.0F - through);
    }
}

// The item that shows most on top at a point: the last drawn whose share of the pixel's color
// is kShownShare or more, or kNothing.
std::size_t top_at(const Bins& bins, float x, float y) {
    std::size_t top = kNothing;
    float through = 1.0F; // what the items drawn over this one let through
    layers_at(bins, x, y, [&](std::size_t item, float alpha) {
        if (alpha * through >= kShownShare) {
            top = item;
            return false;
        }
        through *= 1.0F - alpha;
        return through >= kShownShare;
    });
    return top;
}

// A control's rect on screen, turned and scaled as it draws, as ui.layout reports it.
Bounds control_rect(const ui::Placed& placed) {
    Bounds bounds;
    for (const Vec2 corner : {Vec2{0.0F, 0.0F}, Vec2{placed.size.x, 0.0F},
                              Vec2{0.0F, placed.size.y}, Vec2{placed.size.x, placed.size.y}}) {
        const Vec2 at = placed.transform.apply(corner);
        bounds.add(at.x, at.y);
    }
    return bounds;
}

} // namespace

Result<FrameSummary> summarize(world::World& world, asset::Assets& assets,
                               const SummaryOptions& options) {
    auto& ctx = world.ecs().ctx();
    if (!ctx.contains<SummarizerHandle>() || ctx.get<SummarizerHandle>()->assets != &assets) {
        auto made = std::make_shared<Summarizer>();
        made->assets = &assets;
        made->device = render::create_null_device();
        made->renderer = std::make_unique<CanvasRenderer>(*made->device, assets);
        ctx.insert_or_assign<SummarizerHandle>(std::move(made));
    }
    Summarizer& summarizer = *ctx.get<SummarizerHandle>();
    const Settings frame = settings(world);
    const Vec2 viewport = ui::settings(world).viewport;

    FrameSummary summary;
    CanvasOptions draw;
    draw.clear = frame.clear_color;
    draw.width = viewport.x;
    draw.height = viewport.y;
    draw.interpolation = 1.0;
    draw.texture_filter = frame.texture_filter;
    draw.time = material_time(world.tick(), frame.tick_seconds, 1.0);
    draw.contents = &summary.contents;
    if (auto drawn = summarizer.renderer->draw(world, draw); !drawn) {
        return drawn.error();
    }

    const std::vector<FrameItem>& items = summary.contents.items;
    const Bounds window{.x0 = 0.0F, .y0 = 0.0F, .x1 = viewport.x, .y1 = viewport.y};
    Bounds area = window;
    if (options.region) {
        const auto [x, y, width, height] = *options.region;
        area = {.x0 = x, .y0 = y, .x1 = x + width, .y1 = y + height};
    }

    // What each item drew and where; those that reach the area are sampled, their triangles in
    // drawing order.
    std::vector<Bounds> drawn(items.size());
    std::vector<std::pair<std::size_t, Triangle>> ordered;
    Bounds reach;
    for (std::size_t i = 0; i < items.size(); ++i) {
        for (const FramePiece& piece : items[i].pieces) {
            for (const render::Vertex2D& vertex : piece.vertices) {
                drawn[i].add(vertex.x, vertex.y);
            }
        }
        if (drawn[i].empty() || !drawn[i].touches(area)) {
            continue;
        }
        reach.add(drawn[i]);
        for (const FramePiece& piece : items[i].pieces) {
            for (std::size_t first = 0; first + 3 <= piece.vertices.size(); first += 3) {
                ordered.emplace_back(
                    piece.order,
                    Triangle{.item = i, .piece = &piece, .corners = &piece.vertices[first]});
            }
        }
    }
    std::ranges::stable_sort(ordered, {}, &std::pair<std::size_t, Triangle>::first);
    std::vector<Triangle> triangles;
    triangles.reserve(ordered.size());
    for (const auto& [order, triangle] : ordered) {
        triangles.push_back(triangle);
    }
    const Bins bins(reach, triangles);

    // How much of each item shows, by its pixels: drawn, in the area, and not under another.
    // The part in the area is sampled on its own, so a large thing that reaches far outside it
    // is read as closely there as a small one.
    enum class Fate : std::uint8_t { Unseen, Shown, Offscreen, Covered, Clear, Clipped };
    std::vector<Fate> fates(items.size(), Fate::Unseen);
    std::vector<float> shares(items.size(), 0.0F);
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Bounds& bounds = drawn[i];
        // A control's pixels as it would draw whole, for one a clip cut.
        Bounds whole;
        std::vector<Triangle> uncut;
        for (const FramePiece& piece : items[i].uncut) {
            for (std::size_t first = 0; first + 3 <= piece.vertices.size(); first += 3) {
                uncut.push_back({.item = i, .piece = &piece, .corners = &piece.vertices[first]});
                for (std::size_t corner = first; corner < first + 3; ++corner) {
                    whole.add(piece.vertices[corner].x, piece.vertices[corner].y);
                }
            }
        }
        if (bounds.empty()) {
            if (!whole.empty() && (!options.region || whole.touches(area))) {
                fates[i] = Fate::Clipped;
            }
            continue;
        }
        if (!bounds.touches(area)) {
            if (!options.region) {
                fates[i] = Fate::Offscreen;
            }
            continue;
        }
        // Its alpha at a point, and what the items drawn over it let through there.
        const auto layers = [&](float x, float y, float& own, float& through) {
            own = 0.0F;
            through = 1.0F;
            layers_at(bins, x, y, [&](std::size_t item, float alpha) {
                if (item == i) {
                    own = alpha;
                    return false;
                }
                through *= 1.0F - alpha;
                return true;
            });
        };
        const Bounds inner{.x0 = std::max(bounds.x0, area.x0),
                           .y0 = std::max(bounds.y0, area.y0),
                           .x1 = std::min(bounds.x1, area.x1),
                           .y1 = std::min(bounds.y1, area.y1)};
        float inside = 0.0F; // its pixels in the area
        float shows = 0.0F;  // those that show
        const float inner_weight = sample(inner, [&](float x, float y) {
            if (!area.contains(x, y)) {
                return;
            }
            float own = 0.0F;
            float through = 1.0F;
            layers(x, y, own, through);
            if (own >= kShownShare) {
                inside += 1.0F;
                if (own * through >= kShownShare) {
                    shows += 1.0F;
                }
            }
        });
        inside *= inner_weight;
        shows *= inner_weight;
        float all = inside; // all its pixels
        if (bounds.x0 < area.x0 || bounds.y0 < area.y0 || bounds.x1 > area.x1 ||
            bounds.y1 > area.y1) {
            float count = 0.0F;
            const float weight = sample(bounds, [&](float x, float y) {
                float own = 0.0F;
                float through = 1.0F;
                layers(x, y, own, through);
                if (own >= kShownShare) {
                    count += 1.0F;
                }
            });
            all = std::max(all, count * weight);
        }
        if (!uncut.empty()) {
            // Cut by a clip: its share is of all it would draw.
            const Bins own_bins(whole, uncut);
            float count = 0.0F;
            const float weight = sample(whole, [&](float x, float y) {
                float alpha = 0.0F;
                layers_at(own_bins, x, y, [&](std::size_t, float a) {
                    alpha = a;
                    return false;
                });
                if (alpha >= kShownShare) {
                    count += 1.0F;
                }
            });
            all = std::max(all, count * weight);
        }
        if (all == 0.0F) {
            fates[i] = Fate::Clear;
        } else if (inside == 0.0F) {
            fates[i] = options.region ? Fate::Unseen : Fate::Offscreen;
        } else if (shows == 0.0F) {
            fates[i] = Fate::Covered;
        } else {
            fates[i] = Fate::Shown;
            shares[i] = std::min(shows / all, 1.0F);
        }
    }

    // The map: what shows at each cell's center. Something too thin for its samples to find
    // shows there all the same.
    std::vector<std::vector<std::size_t>> cells;
    if (options.columns > 0 && options.rows > 0) {
        const float cell_w = (area.x1 - area.x0) / static_cast<float>(options.columns);
        const float cell_h = (area.y1 - area.y0) / static_cast<float>(options.rows);
        cells.resize(static_cast<std::size_t>(options.rows));
        for (std::size_t r = 0; r < cells.size(); ++r) {
            const float y = area.y0 + ((static_cast<float>(r) + 0.5F) * cell_h);
            for (int c = 0; c < options.columns; ++c) {
                const float x = area.x0 + ((static_cast<float>(c) + 0.5F) * cell_w);
                const std::size_t item = top_at(bins, x, y);
                if (item != kNothing && fates[item] != Fate::Shown) {
                    fates[item] = Fate::Shown;
                    shares[item] = kLeastShare;
                }
                cells[r].push_back(item);
            }
        }
    }

    std::vector<std::ptrdiff_t> place(items.size(), -1);
    for (std::size_t i = 0; i < items.size(); ++i) {
        switch (fates[i]) {
        case Fate::Shown: {
            Bounds rect = drawn[i];
            if (items[i].kind == FrameItem::Kind::Control) {
                if (const ui::Placed* placed = summary.contents.layout.find(items[i].entity);
                    placed != nullptr) {
                    rect = control_rect(*placed);
                    if (placed->clip) {
                        // The part its clip leaves.
                        rect.x0 = std::clamp(rect.x0, placed->clip->x, placed->clip->z);
                        rect.y0 = std::clamp(rect.y0, placed->clip->y, placed->clip->w);
                        rect.x1 = std::clamp(rect.x1, placed->clip->x, placed->clip->z);
                        rect.y1 = std::clamp(rect.y1, placed->clip->y, placed->clip->w);
                    }
                }
            } else if (items[i].kind == FrameItem::Kind::Popup) {
                const std::vector<ui::PopupView>& popups = summary.contents.layout.popups;
                if (const auto popup =
                        std::ranges::find(popups, items[i].entity, &ui::PopupView::owner);
                    popup != popups.end()) {
                    rect = {.x0 = popup->position.x,
                            .y0 = popup->position.y,
                            .x1 = popup->position.x + popup->size.x,
                            .y1 = popup->position.y + popup->size.y};
                }
            }
            place[i] = static_cast<std::ptrdiff_t>(summary.shown.size());
            summary.shown.push_back(
                {.item = i,
                 .rect = {rect.x0, rect.y0, rect.x1 - rect.x0, rect.y1 - rect.y0},
                 .shown = shares[i]});
            break;
        }
        case Fate::Offscreen:
            summary.offscreen.push_back(i);
            break;
        case Fate::Covered:
            summary.covered.push_back(i);
            break;
        case Fate::Clear:
            summary.clear.push_back(i);
            break;
        case Fate::Clipped:
            summary.clipped.push_back(i);
            break;
        case Fate::Unseen:
            break;
        }
    }
    for (const std::vector<std::size_t>& row : cells) {
        std::vector<std::ptrdiff_t>& out = summary.map.emplace_back();
        out.reserve(row.size());
        for (const std::size_t item : row) {
            out.push_back(item == kNothing ? -1 : place[item]);
        }
    }
    return summary;
}

} // namespace slash::canvas
