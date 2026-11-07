#include "slash/scene3d/blend_shapes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>

namespace slash::scene3d {
namespace {

std::array<float, 3> unit(const std::array<float, 3>& v) {
    const float length = std::sqrt((v[0] * v[0]) + (v[1] * v[1]) + (v[2] * v[2]));
    if (length == 0.0F) {
        return v;
    }
    return {v[0] / length, v[1] / length, v[2] / length};
}

// Godot's skeleton shader skips a shape whose weight is this close to 0.
constexpr float kNoWeight = 0.0001F;

} // namespace

float sample_blend_track(const BlendShapeTrack& track, double time, double length, bool loop) {
    const std::vector<float>& times = track.times;
    const std::size_t count = std::min(times.size(), track.values.size());
    if (count == 0) {
        return 0.0F;
    }
    if (count == 1) {
        return track.values.front();
    }
    time = std::clamp(time, 0.0, length);
    loop = loop && track.loop_wrap;
    const auto before = static_cast<std::size_t>(
        std::upper_bound(times.begin(), times.begin() + static_cast<std::ptrdiff_t>(count), time,
                         [](double t, float key) { return t < static_cast<double>(key); }) -
        times.begin());
    if (track.step) {
        if (before == 0) {
            return loop ? track.values[count - 1] : track.values.front();
        }
        return track.values[before - 1];
    }
    // The keys either side of the time, on the same clock, as sample_track finds them.
    std::size_t a = before - 1;
    std::size_t b = before;
    double a_time = 0.0;
    double b_time = 0.0;
    if (before == 0) {
        if (!loop) {
            return track.values.front();
        }
        a = count - 1;
        b = 0;
        a_time = static_cast<double>(times[count - 1]) - length;
        b_time = times[0];
    } else if (before == count) {
        if (!loop || time == static_cast<double>(times[count - 1])) {
            return track.values[count - 1];
        }
        b = 0;
        a_time = times[count - 1];
        b_time = static_cast<double>(times[0]) + length;
    } else {
        a_time = times[a];
        b_time = times[b];
    }
    const double span = b_time - a_time;
    const auto weight = static_cast<float>(span > 0.0 ? (time - a_time) / span : 0.0);
    return track.values[a] + ((track.values[b] - track.values[a]) * weight);
}

std::map<std::string, float> blend_shape_weights(const Model& model,
                                                 std::span<const ClipSample> clips,
                                                 const std::map<std::string, float>& set,
                                                 const ModelLoader& load) {
    // Each name's value before the clips: its first mesh's weight, or the one set.
    std::map<std::string, float> base;
    for (const ModelMesh& mesh : model.meshes) {
        for (std::size_t k = 0; k < mesh.blend_shapes.size(); ++k) {
            base.emplace(mesh.blend_shapes[k],
                         k < mesh.blend_weights.size() ? mesh.blend_weights[k] : 0.0F);
        }
    }
    if (base.empty()) {
        return base;
    }
    for (auto& [name, value] : base) {
        if (const auto found = set.find(name); found != set.end()) {
            value = found->second;
        }
    }
    // Godot's mixer: each track moves its value by blend times the way from the start to the
    // track's value (AnimationMixer::_blend_process, TYPE_BLEND_SHAPE).
    std::map<std::string, float> out = base;
    for (const ClipSample& clip : clips) {
        std::shared_ptr<const Model> source = clip.source;
        if (source == nullptr) {
            if (!load) {
                continue;
            }
            auto loaded = load(clip.model);
            if (!loaded || loaded.value() == nullptr) {
                continue;
            }
            source = std::move(loaded).value();
        }
        const Model& from = *source;
        const auto found =
            std::ranges::find(from.animations, clip.animation, &ModelAnimation::name);
        if (found == from.animations.end()) {
            continue;
        }
        for (const BlendShapeTrack& track : found->blend_tracks) {
            const auto start = base.find(track.shape);
            if (start == base.end()) {
                continue;
            }
            float blend = clip.weight;
            if (!clip.node_weights.empty() && track.node < from.nodes.size()) {
                if (const auto weight = clip.node_weights.find(from.nodes[track.node].name);
                    weight != clip.node_weights.end()) {
                    blend = weight->second;
                }
            }
            if (std::abs(blend) <= kNoWeight) {
                continue;
            }
            const float value = sample_blend_track(track, clip.time, found->length, found->loop);
            out[track.shape] += (value - start->second) * blend;
        }
    }
    return out;
}

std::vector<float> mesh_weights(const ModelMesh& mesh,
                                const std::map<std::string, float>& weights) {
    std::vector<float> out(mesh.blend_shapes.size(), 0.0F);
    for (std::size_t k = 0; k < out.size(); ++k) {
        const auto found = weights.find(mesh.blend_shapes[k]);
        if (found != weights.end()) {
            out[k] = found->second;
        } else if (k < mesh.blend_weights.size()) {
            out[k] = mesh.blend_weights[k];
        }
    }
    return out;
}

bool any_blend(std::span<const float> weights) {
    return std::ranges::any_of(weights, [](float w) { return std::abs(w) > kNoWeight; });
}

void blend_vertices(const ModelSurface& surface, std::span<const float> weights,
                    std::vector<render::Vertex3D>& out) {
    out = surface.vertices;
    const std::size_t count = out.size();
    std::vector<std::array<float, 3>> normals(count, {0.0F, 0.0F, 0.0F});
    std::vector<std::array<float, 3>> tangents(count, {0.0F, 0.0F, 0.0F});
    bool turned = false;
    for (std::size_t k = 0; k < surface.blend_shapes.size() && k < weights.size(); ++k) {
        const float w = weights[k];
        if (std::abs(w) <= kNoWeight) {
            continue;
        }
        const SurfaceBlendShape& shape = surface.blend_shapes[k];
        for (std::size_t i = 0; i < count && i < shape.positions.size(); ++i) {
            for (std::size_t c = 0; c < 3; ++c) {
                out[i].position.at(c) += shape.positions[i].at(c) * w;
            }
        }
        for (std::size_t i = 0; i < count && i < shape.normals.size(); ++i) {
            for (std::size_t c = 0; c < 3; ++c) {
                normals[i].at(c) += shape.normals[i].at(c) * w;
            }
            turned = true;
        }
        for (std::size_t i = 0; i < count && i < shape.tangents.size(); ++i) {
            for (std::size_t c = 0; c < 3; ++c) {
                tangents[i].at(c) += shape.tangents[i].at(c) * w;
            }
            turned = true;
        }
    }
    if (!turned) {
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        render::Vertex3D& v = out[i];
        v.normal = unit({v.normal[0] + normals[i][0], v.normal[1] + normals[i][1],
                         v.normal[2] + normals[i][2]});
        const std::array<float, 3> t =
            unit({v.tangent[0] + tangents[i][0], v.tangent[1] + tangents[i][1],
                  v.tangent[2] + tangents[i][2]});
        v.tangent = {t[0], t[1], t[2], v.tangent[3]};
    }
}

} // namespace slash::scene3d
