#include "slash/canvas/canvas.hpp"

#include "slash/canvas/sprite_frames.hpp"
#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/render/image.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace slash::canvas {
namespace {

// Plays AnimatedSprite2D components. Its state is only a cache of loaded .frames.json files and
// the warnings already logged, so a world's snapshot, undo or dry run is never out of step with
// it.
struct AnimationState {
    asset::Assets* assets = nullptr;
    struct Cached {
        asset::Handle<SpriteFrames> frames;
        bool used = false;
    };
    std::map<std::string, Cached, std::less<>> cache;
    std::set<std::string, std::less<>> warned;

    void warn_once(std::string_view id, const std::string& message) {
        if (warned.insert(std::string(id) + "\n" + message).second) {
            log::warn("an AnimatedSprite2D can't play", {{"id", id}, {"error", message}});
        }
    }

    const SpriteFrames* frames(const std::string& path, std::string_view id) {
        if (auto it = cache.find(path); it != cache.end()) {
            it->second.used = true;
            return it->second.frames.get();
        }
        auto loaded = assets->load<SpriteFrames>(path);
        if (!loaded) {
            warn_once(id, path + ": " + loaded.error().message);
            return nullptr;
        }
        auto& cached = cache[path];
        cached.frames = std::move(loaded).value();
        cached.used = true;
        return cached.frames.get();
    }

    void run(world::World& world, double tick_seconds) {
        for (auto& [path, cached] : cache) {
            cached.used = false;
        }
        auto& ecs = world.ecs();
        for (const entt::entity entity : ecs.view<world::AnimatedSprite2D>()) {
            auto& player = ecs.get<world::AnimatedSprite2D>(entity);
            auto& sprite = ecs.get_or_emplace<world::Sprite2D>(entity);
            const std::string_view id = world.id_of(entity);
            if (player.sprite_frames.empty()) {
                continue;
            }
            const SpriteFrames* frames = this->frames(player.sprite_frames, id);
            if (frames == nullptr) {
                continue;
            }
            const auto found = frames->animations.find(player.animation);
            if (found == frames->animations.end()) {
                std::vector<std::string_view> names;
                names.reserve(frames->animations.size());
                for (const auto& [name, animation] : frames->animations) {
                    names.emplace_back(name);
                }
                std::string message =
                    player.sprite_frames + " has no animation \"" + player.animation + "\"";
                if (const std::string_view hint = closest_match(player.animation, names);
                    !hint.empty()) {
                    message += "; did you mean \"" + std::string(hint) + "\"?";
                }
                warn_once(id, message);
                continue;
            }
            advance(world, id, player, found->second, tick_seconds);
            const SpriteFrame& frame =
                found->second.frames.at(static_cast<std::size_t>(player.frame));
            sprite.texture = frame.texture;
            sprite.hframes = frame.hframes;
            sprite.vframes = frame.vframes;
            sprite.frame = frame.frame;
            sprite.region = {};
        }
        std::erase_if(cache, [](const auto& entry) { return !entry.second.used; });
    }

    // Moves the animation on by one tick, as Godot does: frames change each 1 / (fps *
    // speed_scale) seconds, and an animation that doesn't loop stops on its last frame.
    static void advance(world::World& world, std::string_view id, world::AnimatedSprite2D& player,
                        const Animation& animation, double tick_seconds) {
        const auto count = static_cast<std::int64_t>(animation.frames.size());
        player.frame = static_cast<int>(std::clamp<std::int64_t>(player.frame, 0, count - 1));
        player.frame_progress = std::clamp(player.frame_progress, 0.0F, 1.0F);
        const double rate = animation.fps * player.speed_scale;
        if (!player.playing || rate == 0.0 || !std::isfinite(rate)) {
            return;
        }
        double progress = player.frame_progress + (tick_seconds * rate);
        if (progress >= 0.0 && progress < 1.0) {
            player.frame_progress = static_cast<float>(progress);
            return;
        }
        const double steps = std::floor(progress);
        progress -= steps;
        const double next = static_cast<double>(player.frame) + steps;
        bool finished = false;
        if (animation.loop) {
            const double wrapped =
                std::fmod(std::fmod(next, static_cast<double>(count)) + static_cast<double>(count),
                          static_cast<double>(count));
            player.frame = static_cast<int>(wrapped);
        } else if (next >= static_cast<double>(count)) {
            player.frame = static_cast<int>(count - 1);
            progress = 1.0;
            finished = true;
        } else if (next < 0.0) {
            player.frame = 0;
            progress = 0.0;
            finished = true;
        } else {
            player.frame = static_cast<int>(next);
        }
        player.frame_progress = static_cast<float>(progress);
        if (finished) {
            player.playing = false;
            json::Value data;
            data["animation"] = player.animation;
            world.emit("animation_finished", id, std::move(data));
        }
    }
};

} // namespace

void add_asset_types(asset::Assets& assets) {
    assets.add_type<render::Image>("texture", {".png", ".bmp"},
                                   [](std::string_view bytes, std::string_view /*path*/) {
                                       return render::decode_image(bytes);
                                   });
    assets.add_type<SpriteFrames>(
        "sprite_frames", {std::string(kSpriteFramesSuffix)},
        [](std::string_view text, std::string_view /*path*/) -> Result<SpriteFrames> {
            auto doc = json::parse(text);
            if (!doc) {
                return doc.error();
            }
            return parse_sprite_frames(doc.value());
        });
}

void add_systems(world::World& world, asset::Assets& assets) {
    auto state = std::make_shared<AnimationState>();
    state->assets = &assets;
    world.add_system(
        "animated_sprite_2d",
        [state](world::World& w, double tick_seconds) { state->run(w, tick_seconds); },
        {.doc = "Plays each AnimatedSprite2D's animation on its Sprite2D.", .priority = 1000});
}

void install(world::World& world, asset::Assets& assets) {
    add_asset_types(assets);
    add_systems(world, assets);
}

} // namespace slash::canvas
