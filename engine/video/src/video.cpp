#include "slash/video/video.hpp"

#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace slash::video {
namespace {

constexpr std::size_t kNoFrame = std::numeric_limits<std::size_t>::max();

std::string key_of(const world::World& world, entt::entity entity) {
    const std::string_view id = world.id_of(entity);
    return id.empty() ? "#" + std::to_string(entt::to_integral(entity)) : std::string(id);
}

// One VideoStreamPlayer's playback, as Godot's VideoStreamPlayback: its stream, the decoder and
// the frame it shows.
struct Player {
    std::string stream;
    asset::Handle<VideoFile> handle;
    std::shared_ptr<const VideoFile> file; // the version being decoded
    std::uint64_t file_version = 0;
    Decoder decoder;
    render::Image image;
    std::size_t frame = kNoFrame;
    std::uint64_t version = 0;
    std::string error;
    bool seen = false;        // autoplay has had its chance
    bool was_playing = false; // playing at the end of the last tick
    bool fresh = false;       // just started: the first tick shows the frame it starts at
};

struct VideoSystem {
    asset::Assets* assets = nullptr;
    std::map<std::string, Player, std::less<>> players;

    void open(Player& player, const std::string& stream) const {
        player.stream = stream;
        player.handle = {};
        player.file.reset();
        player.file_version = 0;
        player.frame = kNoFrame;
        player.error.clear();
        if (stream.empty()) {
            return;
        }
        auto loaded = assets->load<VideoFile>(stream);
        if (!loaded) {
            player.error = loaded.error().message;
            return;
        }
        player.handle = std::move(loaded).value();
    }

    void run(world::World& world, double tick_seconds) {
        auto& ecs = world.ecs();
        std::set<std::string, std::less<>> seen;
        std::vector<std::string> finished;
        for (const entt::entity entity : ecs.view<world::VideoStreamPlayer>()) {
            auto& component = ecs.get<world::VideoStreamPlayer>(entity);
            std::string key = key_of(world, entity);
            Player& player = players[key];
            seen.insert(key);
            if (!player.seen || player.stream != component.stream) {
                open(player, component.stream);
            }
            if (player.handle && player.handle.version() != player.file_version) {
                // Loaded, or reloaded from an edited file.
                player.file = player.handle.share();
                player.file_version = player.handle.version();
                player.frame = kNoFrame;
                player.error.clear();
            }
            if (!player.seen) {
                player.seen = true;
                if (component.autoplay) {
                    component.playing = true;
                    component.stream_position = 0.0;
                }
            }
            if (component.playing && !player.was_playing) {
                player.fresh = true;
            }
            if (component.playing && !component.paused && player.file) {
                // As Godot's NOTIFICATION_INTERNAL_PROCESS: no time passes on the first tick.
                if (!player.fresh) {
                    component.stream_position += tick_seconds * component.speed_scale;
                }
                player.fresh = false;
                const double length = player.file->length();
                if (component.stream_position >= length) {
                    if (component.loop) {
                        component.stream_position = 0.0;
                        player.fresh = true;
                    } else {
                        component.playing = false;
                        component.stream_position = length;
                        finished.push_back(std::move(key));
                    }
                }
            }
            player.was_playing = component.playing;
            if (player.file) {
                const std::size_t index =
                    player.file->frame_at(std::max(component.stream_position, 0.0));
                if (index != player.frame) {
                    if (auto decoded = player.decoder.decode(*player.file, index, player.image);
                        !decoded) {
                        player.error = decoded.error().message;
                    } else {
                        player.frame = index;
                        ++player.version;
                        player.error.clear();
                    }
                }
            }
        }
        std::erase_if(players, [&](const auto& entry) { return !seen.contains(entry.first); });
        for (const std::string& id : finished) {
            world.emit("finished", id, {});
        }
    }

    [[nodiscard]] world::MemoryPart memory() const {
        world::MemoryPart part{"video", 0, {}, {}};
        std::uint64_t decoded = 0;
        for (const auto& [key, player] : players) {
            part.bytes += world::kMapNodeBytes<decltype(players)> + json::heap_bytes(key) +
                          json::heap_bytes(player.stream) + player.image.pixels.capacity();
            decoded += player.frame != kNoFrame ? 1 : 0;
        }
        part.counts = {{"players", players.size()}, {"decoded", decoded}};
        return part;
    }
};

const VideoSystem* system_of(const world::World& world) {
    const auto* system = world.ecs().ctx().find<std::shared_ptr<VideoSystem>>();
    return system != nullptr ? system->get() : nullptr;
}

} // namespace

void add_asset_types(asset::Assets& assets) {
    assets.add_type<VideoFile>(
        "video", {".ivf"},
        [](std::string_view text, std::string_view /*path*/) {
            return read_ivf({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
        },
        [](const VideoFile& video) {
            return sizeof(VideoFile) + video.data.capacity() +
                   (video.frames.capacity() * sizeof(VideoFile::Frame));
        });
}

void install(world::World& world, asset::Assets& assets) {
    add_asset_types(assets);
    if (world.ecs().ctx().contains<std::shared_ptr<VideoSystem>>()) {
        return;
    }
    auto system = std::make_shared<VideoSystem>();
    system->assets = &assets;
    world.ecs().ctx().emplace<std::shared_ptr<VideoSystem>>(system);
    world.add_reset([system](world::World& /*world*/) { system->players.clear(); });
    world.add_memory_source("video",
                            [system](const world::World& /*world*/) { return system->memory(); });
    world.add_system(
        "video", [system](world::World& w, double tick_seconds) { system->run(w, tick_seconds); },
        {.doc = "Plays each VideoStreamPlayer and decodes the frame it shows.", .priority = 1140});
}

Shown shown(const world::World& world, std::string_view entity) {
    const VideoSystem* system = system_of(world);
    if (system == nullptr) {
        return {};
    }
    const auto found = system->players.find(entity);
    if (found == system->players.end()) {
        // A player the system hasn't run yet has nothing to show either.
        const entt::entity id = world.find(entity);
        Shown none;
        none.player = world.valid(id) && world.ecs().all_of<world::VideoStreamPlayer>(id);
        return none;
    }
    const Player& player = found->second;
    return {.image = player.frame != kNoFrame ? &player.image : nullptr,
            .version = player.version,
            .frame = player.frame != kNoFrame ? player.frame : 0,
            .error = player.error,
            .player = true};
}

double stream_length(const world::World& world, std::string_view entity) {
    const VideoSystem* system = system_of(world);
    if (system == nullptr) {
        return 0.0;
    }
    const auto found = system->players.find(entity);
    return found != system->players.end() && found->second.file ? found->second.file->length()
                                                                : 0.0;
}

} // namespace slash::video
