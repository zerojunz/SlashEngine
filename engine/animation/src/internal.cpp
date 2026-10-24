#include "internal.hpp"

#include "slash/core/json.hpp"
#include "slash/scene3d/model.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace slash::animation::detail {

bool is_equal_approx(double a, double b) {
    if (a == b) {
        return true;
    }
    const double tolerance = std::max(kCmpEpsilon, kCmpEpsilon * std::abs(a));
    return std::abs(a - b) < tolerance;
}

bool is_zero_approx(double value) {
    return std::abs(value) < kCmpEpsilon;
}

bool is_equal_approx(float a, float b) {
    if (a == b) {
        return true;
    }
    const auto epsilon = static_cast<float>(kCmpEpsilon);
    const float tolerance = std::max(epsilon, epsilon * std::abs(a));
    return std::abs(a - b) < tolerance;
}

bool is_zero_approx(float value) {
    return std::abs(value) < static_cast<float>(kCmpEpsilon);
}

bool is_less_approx(double a, double b) {
    return a < b && !is_equal_approx(a, b);
}

bool is_less_or_equal_approx(double a, double b) {
    return a < b || is_equal_approx(a, b);
}

bool is_greater_approx(double a, double b) {
    return a > b && !is_equal_approx(a, b);
}

bool is_greater_or_equal_approx(double a, double b) {
    return a > b || is_equal_approx(a, b);
}

double fposmod(double x, double y) {
    double value = std::fmod(x, y);
    if ((value < 0.0 && y > 0.0) || (value > 0.0 && y < 0.0)) {
        value += y;
    }
    return value + 0.0;
}

double pingpong(double value, double length) {
    if (length == 0.0) {
        return 0.0;
    }
    const double cycle = (value - length) / (length * 2.0);
    return std::abs(((cycle - std::floor(cycle)) * length * 2.0) - length);
}

namespace {

// Godot's Animation::_track_get_key_indices_in_range: the keys after from_time up to and
// including to_time, or from to_time back to and including from_time when backward.
void keys_between(const std::vector<Key>& keys, double from_time, double to_time, bool backward,
                  std::vector<const Key*>& out) {
    const int count = static_cast<int>(keys.size());
    if (count == 0) {
        return;
    }
    int from = 0;
    int to = count - 1;
    const auto time_of = [&](int i) { return keys[static_cast<std::size_t>(i)].time; };
    if (!backward) {
        while (time_of(from) < from_time || is_equal_approx(time_of(from), from_time)) {
            if (++from > to) {
                return;
            }
        }
        while (time_of(to) > to_time && !is_equal_approx(time_of(to), to_time)) {
            if (--to < from) {
                return;
            }
        }
    } else {
        while (time_of(from) < from_time && !is_equal_approx(time_of(from), from_time)) {
            if (++from > to) {
                return;
            }
        }
        while (time_of(to) > to_time || is_equal_approx(time_of(to), to_time)) {
            if (--to < from) {
                return;
            }
        }
    }
    if (!backward) {
        for (int i = from; i <= to; ++i) {
            out.push_back(&keys[static_cast<std::size_t>(i)]);
        }
    } else {
        for (int i = to; i >= from; --i) {
            out.push_back(&keys[static_cast<std::size_t>(i)]);
        }
    }
}

// Godot's Animation::_find: the key at a time, within CMP_EPSILON, or else the one before it
// (after it when backward); -1 before the first, or for one outside the animation with limit.
int find_index(const std::vector<Key>& keys, double length, double time, bool backward,
               bool limit) {
    const int count = static_cast<int>(keys.size());
    if (count == 0) {
        return -2;
    }
    int low = 0;
    int high = count - 1;
    int middle = 0;
    const auto time_of = [&](int i) { return keys[static_cast<std::size_t>(i)].time; };
    while (low <= high) {
        middle = (low + high) / 2;
        if (is_equal_approx(time, time_of(middle))) {
            return middle;
        }
        if (time < time_of(middle)) {
            high = middle - 1;
        } else {
            low = middle + 1;
        }
    }
    if (!backward) {
        if (time_of(middle) > time) {
            --middle;
        }
    } else if (time_of(middle) < time) {
        ++middle;
    }
    if (limit && middle >= 0 && middle < count) {
        const double key_time = time_of(middle);
        const double diff = length - key_time;
        if ((std::signbit(key_time) && !is_zero_approx(key_time)) ||
            (std::signbit(diff) && !is_zero_approx(diff))) {
            return -1;
        }
    }
    return middle;
}

} // namespace

const Key* find_key(const Track& track, double length, double time, bool nearest, bool limit) {
    const int k = find_index(track.keys, length, time, false, limit);
    if (k < 0 || k >= static_cast<int>(track.keys.size())) {
        return nullptr;
    }
    const Key& key = track.keys[static_cast<std::size_t>(k)];
    if (!nearest && key.time != time) {
        return nullptr;
    }
    return &key;
}

std::vector<const Key*> keys_in_range(const Track& track, double length, LoopMode loop, double time,
                                      double delta, LoopedFlag looped) {
    std::vector<const Key*> out;
    if (delta == 0.0) {
        return out;
    }
    double from_time = time - delta;
    double to_time = time;
    bool backward = false;
    if (from_time > to_time) {
        backward = true;
        std::swap(from_time, to_time);
    }
    switch (loop) {
    case LoopMode::None:
        from_time = std::clamp(from_time, 0.0, std::max(length, 0.0));
        to_time = std::clamp(to_time, 0.0, std::max(length, 0.0));
        break;
    case LoopMode::Linear: {
        if (from_time > length || from_time < 0.0) {
            from_time = fposmod(from_time, length);
        }
        if (to_time > length || to_time < 0.0) {
            to_time = fposmod(to_time, length);
        }
        if (from_time > to_time) {
            // Round the loop's end, in two parts.
            const double end = length + kCmpEpsilon;
            const double start = -kCmpEpsilon;
            if (!backward) {
                keys_between(track.keys, from_time, end, backward, out);
                keys_between(track.keys, start, to_time, backward, out);
            } else {
                keys_between(track.keys, start, to_time, backward, out);
                keys_between(track.keys, from_time, end, backward, out);
            }
            return out;
        }
        // Just looped: the key at the edge playback came round to.
        if (looped != LoopedFlag::None) {
            if (!backward && is_equal_approx(from_time, 0.0)) {
                if (const Key* edge = find_key(track, length, 0.0, false, false); edge != nullptr) {
                    out.push_back(edge);
                }
            } else if (backward && is_equal_approx(to_time, length)) {
                if (const Key* edge = find_key(track, length, length, false, false);
                    edge != nullptr) {
                    out.push_back(edge);
                }
            }
        }
        break;
    }
    case LoopMode::PingPong:
        if (from_time > length || from_time < 0.0) {
            from_time = pingpong(from_time, length);
        }
        if (to_time > length || to_time < 0.0) {
            to_time = pingpong(to_time, length);
        }
        if (looped == LoopedFlag::Start) {
            keys_between(track.keys, 0.0, from_time, true, out);
            keys_between(track.keys, 0.0, to_time, false, out);
            return out;
        }
        if (looped == LoopedFlag::End) {
            keys_between(track.keys, from_time, length, false, out);
            // NOLINTNEXTLINE(readability-suspicious-call-argument): back from the end to to_time.
            keys_between(track.keys, to_time, length, true, out);
            return out;
        }
        // An edge is passed on the next tick, when playback turns round there.
        if (!backward && is_equal_approx(to_time, length)) {
            to_time -= kCmpEpsilon;
        } else if (backward && is_equal_approx(from_time, 0.0)) {
            from_time += kCmpEpsilon;
        }
        break;
    }
    keys_between(track.keys, from_time, to_time, backward, out);
    return out;
}

const Result<scene3d::Model>& CheckedFiles::model(const std::string& path) {
    if (const auto it = models_.find(path); it != models_.end()) {
        return it->second;
    }
    const auto read = [&]() -> Result<scene3d::Model> {
        auto bytes = reader_(path);
        if (!bytes) {
            return bytes.error();
        }
        return scene3d::load_model(bytes.value(), path, reader_);
    };
    return models_.emplace(path, read()).first->second;
}

const Result<AnimationLibrary>& CheckedFiles::library(const std::string& path) {
    if (const auto it = libraries_.find(path); it != libraries_.end()) {
        return it->second;
    }
    const auto parse = [&]() -> Result<AnimationLibrary> {
        if (is_model_library(path)) {
            const Result<scene3d::Model>& found = model(path);
            if (!found) {
                return found.error();
            }
            return model_library(found.value(), path);
        }
        auto text = reader_(path);
        if (!text) {
            return text.error();
        }
        auto parsed = json::parse(text.value());
        if (!parsed) {
            return parsed.error();
        }
        auto library = parse_animation_library(parsed.value());
        if (!library) {
            return library;
        }
        // Animations from models take their lengths and loops from them.
        for (auto& [name, animation] : library.value().animations) {
            if (animation.from.model.empty()) {
                continue;
            }
            const std::string at = "/animations/" + json::pointer_escape(name) + "/from";
            const Result<scene3d::Model>& found = model(animation.from.model);
            if (!found) {
                return make_error(found.error().code, at + "/model: " + animation.from.model +
                                                          ": " + found.error().message);
            }
            if (auto resolved = resolve_clip(animation, found.value()); !resolved) {
                return make_error(resolved.error().code,
                                  at + "/animation: " + resolved.error().message);
            }
        }
        return library;
    };
    return libraries_.emplace(path, parse()).first->second;
}

const scene3d::ModelAnimation* CheckedFiles::clip(const std::string& model_path,
                                                  const std::string& animation) {
    if (model_path.empty()) {
        return nullptr;
    }
    const Result<scene3d::Model>& found = model(model_path);
    if (!found) {
        return nullptr;
    }
    const auto& animations = found.value().animations;
    const auto it = std::ranges::find(animations, animation, &scene3d::ModelAnimation::name);
    return it == animations.end() ? nullptr : &*it;
}

json::Value zero_of(const json::Value& value) {
    if (value.is_int()) {
        return std::int64_t{0};
    }
    if (value.is_double()) {
        return 0.0;
    }
    if (value.is_bool()) {
        return false;
    }
    if (value.is_array()) {
        json::Array out;
        out.reserve(value.size());
        for (const json::Value& item : value.as_array()) {
            out.push_back(zero_of(item));
        }
        return out;
    }
    return {};
}

LibraryCache::Entry* LibraryCache::get(const std::string& path, const Warn& warn) {
    auto it = entries_.find(path);
    if (it == entries_.end()) {
        Entry entry;
        if (is_model_library(path)) {
            auto loaded = assets_->load<scene3d::Model>(path);
            if (!loaded) {
                warn(path + ": " + loaded.error().message);
                return nullptr;
            }
            entry.model = std::move(loaded).value();
        } else {
            auto loaded = assets_->load<AnimationLibrary>(path);
            if (!loaded) {
                warn(path + ": " + loaded.error().message);
                return nullptr;
            }
            entry.file = std::move(loaded).value();
        }
        it = entries_.emplace(path, std::move(entry)).first;
    }
    Entry& entry = it->second;
    entry.used = true;
    refresh(entry, path, warn);
    return &entry;
}

const scene3d::Model* LibraryCache::model(const std::string& path, const Warn& warn,
                                          std::uint64_t* version) {
    auto it = models_.find(path);
    if (it == models_.end()) {
        auto loaded = assets_->load<scene3d::Model>(path);
        if (!loaded) {
            warn(path + ": " + loaded.error().message);
            return nullptr;
        }
        it = models_.emplace(path, UsedModel{std::move(loaded).value()}).first;
    }
    it->second.used = true;
    if (version != nullptr) {
        *version = it->second.handle ? it->second.handle.version() : 0;
    }
    return it->second.handle ? &*it->second.handle : nullptr;
}

namespace {

// The versions of a library's files: its own, then the models its animations come from.
std::vector<std::uint64_t> versions_of(const LibraryCache::Entry& entry) {
    std::vector<std::uint64_t> out;
    out.reserve(entry.models.size() + 1);
    out.push_back(entry.file ? entry.file.version() : entry.model.version());
    for (const auto& [path, model] : entry.models) {
        out.push_back(model ? model.version() : 0);
    }
    return out;
}

} // namespace

// Rebuilds a library when one of its files changed: a model's animations, or a .anim.json file's
// with those from models given their lengths and loops.
void LibraryCache::refresh(Entry& entry, const std::string& path, const Warn& warn) {
    if (!entry.versions.empty() && entry.versions == versions_of(entry)) {
        return;
    }
    entry.nodes = nullptr;
    if (entry.model) {
        entry.library = model_library(*entry.model, path);
    } else {
        entry.library = *entry.file;
        entry.nodes = node_animations(entry.library);
        for (auto& [name, animation] : entry.library.animations) {
            if (animation.from.model.empty()) {
                continue;
            }
            auto found = entry.models.find(animation.from.model);
            if (found == entry.models.end()) {
                auto loaded = assets_->load<scene3d::Model>(animation.from.model);
                if (!loaded) {
                    warn(animation.from.model + ": " + loaded.error().message);
                }
                found = entry.models
                            .emplace(animation.from.model, loaded ? std::move(loaded).value()
                                                                  : asset::Handle<scene3d::Model>())
                            .first;
            }
            if (!found->second) {
                continue;
            }
            if (auto resolved = resolve_clip(animation, *found->second); !resolved) {
                warn(path + ": " + resolved.error().message);
            }
        }
    }
    entry.versions = versions_of(entry);
    entry.version = ++builds_;
}

const std::vector<LibraryCache::Resolved>&
LibraryCache::targets(Entry& entry, const std::string& name, const Animation& animation,
                      const world::ComponentRegistry& components) {
    if (entry.version != entry.resolved || entry.types != components.revision()) {
        entry.targets.clear();
        entry.resolved = entry.version;
        entry.types = components.revision();
    }
    auto [it, added] = entry.targets.try_emplace(name);
    if (added) {
        it->second.reserve(animation.tracks.size());
        for (const Track& track : animation.tracks) {
            Resolved resolved;
            if (!track.path.empty()) {
                auto target = resolve_target(track.path, components);
                if (target) {
                    resolved.target = std::move(target).value();
                } else {
                    resolved.error = track.path + ": " + target.error().message;
                }
            }
            it->second.push_back(std::move(resolved));
        }
    }
    return it->second;
}

void LibraryCache::begin() {
    for (auto& [path, entry] : entries_) {
        entry.used = false;
    }
    for (auto& [path, model] : models_) {
        model.used = false;
    }
}

void LibraryCache::end() {
    std::erase_if(entries_, [](const auto& entry) { return !entry.second.used; });
    std::erase_if(models_, [](const auto& model) { return !model.second.used; });
}

namespace {

struct Triangle {
    std::array<int, 3> points{};
    Vec2 circum_center;
    float circum_radius_squared = 0.0F;
};

struct Edge {
    std::array<int, 2> points{};
    bool bad = false;
};

float length_squared(Vec2 v) {
    return (v.x * v.x) + (v.y * v.y);
}

// Godot's Delaunay2D::create_triangle: the triangle with its circumcircle.
Triangle create_triangle(const std::vector<Vec2>& vertices, int a, int b, int c) {
    Triangle triangle{.points = {a, b, c}, .circum_center = {}, .circum_radius_squared = 0.0F};
    const Vec2 origin = vertices[static_cast<std::size_t>(a)];
    const Vec2 u = vertices[static_cast<std::size_t>(b)] - origin;
    const Vec2 v = vertices[static_cast<std::size_t>(c)] - origin;
    const Vec2 w = (v * length_squared(u)) - (u * length_squared(v));
    const float cross = (u.x * v.y) - (u.y * v.x);
    // Vector2::orthogonal is (y, -x).
    const float divisor = cross * 2.0F;
    const Vec2 center{w.y / divisor, -w.x / divisor};
    triangle.circum_radius_squared = length_squared(center);
    triangle.circum_center = center + origin;
    return triangle;
}

} // namespace

std::vector<std::array<int, 3>> delaunay(const std::vector<Vec2>& input) {
    std::vector<std::array<int, 3>> out;
    const int point_count = static_cast<int>(input.size());
    if (point_count <= 2) {
        return out;
    }
    std::vector<Vec2> points = input;
    // The bounding rectangle, then a triangle round it.
    Vec2 low = input[0];
    Vec2 high = input[0];
    for (const Vec2 point : input) {
        low = {std::min(low.x, point.x), std::min(low.y, point.y)};
        high = {std::max(high.x, point.x), std::max(high.y, point.y)};
    }
    const float delta_max = std::max(high.x - low.x, high.y - low.y);
    const Vec2 center = low + ((high - low) * 0.5F);
    points.push_back({center.x - (delta_max * 16.0F), center.y - delta_max});
    points.push_back({center.x, center.y + (delta_max * 16.0F)});
    points.push_back({center.x + (delta_max * 16.0F), center.y - delta_max});
    std::vector<Triangle> triangles{
        create_triangle(points, point_count, point_count + 1, point_count + 2)};

    for (int i = 0; i < point_count; ++i) {
        const Vec2 point = points[static_cast<std::size_t>(i)];
        std::vector<Edge> polygon;
        // The edges of the triangles whose circumcircles hold the point; the triangles go.
        for (int j = static_cast<int>(triangles.size()) - 1; j >= 0; --j) {
            const Triangle& triangle = triangles[static_cast<std::size_t>(j)];
            if (length_squared(point - triangle.circum_center) < triangle.circum_radius_squared) {
                const auto edge = [](int a, int b) {
                    return Edge{.points = {std::min(a, b), std::max(a, b)}};
                };
                polygon.push_back(edge(triangle.points[0], triangle.points[1]));
                polygon.push_back(edge(triangle.points[1], triangle.points[2]));
                polygon.push_back(edge(triangle.points[2], triangle.points[0]));
                triangles.erase(triangles.begin() + j);
            }
        }
        // A triangle from each edge only one of them had.
        for (std::size_t j = 0; j < polygon.size(); ++j) {
            if (polygon[j].bad) {
                continue;
            }
            for (std::size_t k = j + 1; k < polygon.size(); ++k) {
                if (polygon[k].points == polygon[j].points) {
                    polygon[j].bad = true;
                    polygon[k].bad = true;
                    break;
                }
            }
            if (!polygon[j].bad) {
                triangles.push_back(
                    create_triangle(points, polygon[j].points[0], polygon[j].points[1], i));
            }
        }
    }
    for (const Triangle& triangle : triangles) {
        if (std::ranges::any_of(triangle.points, [&](int p) { return p >= point_count; })) {
            continue;
        }
        std::array<int, 3> sorted = triangle.points;
        std::ranges::sort(sorted);
        if (std::ranges::find(out, sorted) == out.end()) {
            out.push_back(sorted);
        }
    }
    return out;
}

} // namespace slash::animation::detail
