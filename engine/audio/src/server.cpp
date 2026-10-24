// The mixer, following Godot's AudioServer (servers/audio/audio_server.cpp): _driver_process,
// _mix_step and _mix_step_for_channel over each speaker pair, the playback list's states and
// lookahead fade, and the bus setters. Godot mixes on the audio driver's thread; here mix() runs on
// the game's, once per tick, so the same ticks make the same sound.

#include "filter.hpp"

#include "slash/audio/audio.hpp"
#include "slash/audio/effects.hpp"
#include "slash/core/log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

namespace slash::audio {

namespace {

constexpr int kBufferSize = Server::kMixStep;
constexpr int kLookahead = 64;                       // AuSC::LOOKAHEAD_BUFFER_SIZE
constexpr std::size_t kMaxBusesPerPlayback = 6;      // AuSC::MAX_BUSES_PER_PLAYBACK
constexpr float kPeakOffset = 1e-10F;                // AuSC::AUDIO_PEAK_OFFSET
constexpr float kMinPeakDb = -200.0F;                // AuSC::AUDIO_MIN_PEAK_DB
constexpr float kChannelDisableThresholdDb = -60.0F; // audio/buses/channel_disable_threshold_db
constexpr double kChannelDisableTime = 2.0;          // audio/buses/channel_disable_time
// Seconds of microphone input kept, enough for a slow tick's frames and the microphone's delay.
constexpr double kInputBufferSeconds = 1.0;

Frame operator*(Frame a, Frame b) {
    return {a.left * b.left, a.right * b.right};
}
Frame operator*(Frame a, float s) {
    return {a.left * s, a.right * s};
}
Frame operator+(Frame a, Frame b) {
    return {a.left + b.left, a.right + b.right};
}

constexpr std::array<std::pair<std::string_view, SpeakerMode>, 4> kSpeakerModes{{
    {"stereo", SpeakerMode::Stereo},
    {"surround_31", SpeakerMode::Surround31},
    {"surround_51", SpeakerMode::Surround51},
    {"surround_71", SpeakerMode::Surround71},
}};

} // namespace

int channel_count(SpeakerMode mode) {
    switch (mode) {
    case SpeakerMode::Stereo:
        return 1;
    case SpeakerMode::Surround31:
        return 2;
    case SpeakerMode::Surround51:
        return 3;
    case SpeakerMode::Surround71:
        return 4;
    }
    return 1;
}

std::string_view speaker_mode_name(SpeakerMode mode) {
    for (const auto& [name, each] : kSpeakerModes) {
        if (each == mode) {
            return name;
        }
    }
    return "stereo";
}

std::optional<SpeakerMode> parse_speaker_mode(std::string_view name) {
    for (const auto& [each, mode] : kSpeakerModes) {
        if (each == name) {
            return mode;
        }
    }
    return std::nullopt;
}

SpeakerMode speaker_mode_for_channels(int channels) {
    constexpr int k51 = 6;
    constexpr int k71 = 8;
    if (channels >= k71) {
        return SpeakerMode::Surround71;
    }
    return channels >= k51 ? SpeakerMode::Surround51 : SpeakerMode::Stereo;
}

std::vector<Frame> downmix_stereo(std::span<const Frame> frames, int pairs) {
    constexpr float kMinus3Db = 0.70710678F;
    const auto cs = static_cast<std::size_t>(std::max(pairs, 1));
    std::vector<Frame> out(frames.size() / cs);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const Frame* frame = &frames[i * cs];
        Frame mixed = frame[0];
        if (cs > 1) {
            mixed.left += kMinus3Db * frame[1].left; // the center, into both; the LFE is left out
            mixed.right += kMinus3Db * frame[1].left;
        }
        for (std::size_t k = 2; k < cs; ++k) { // rear, then side
            mixed.left += kMinus3Db * frame[k].left;
            mixed.right += kMinus3Db * frame[k].right;
        }
        out[i] = mixed;
    }
    return out;
}

struct Server::Impl final : MixContext {
    struct Effect {
        std::string type;
        bool enabled = true;
    };

    // A speaker pair's buffer on a bus, as Godot's Bus::Channel: its own copy of each effect.
    struct Channel {
        std::vector<Frame> buffer = std::vector<Frame>(kBufferSize);
        // One per effect slot; nullptr when it couldn't be made.
        std::vector<std::unique_ptr<EffectInstance>> effect_instances;
        bool used = false;
        bool active = false;
        Frame peak{kMinPeakDb, kMinPeakDb};
        std::uint64_t last_mix_with_audio = 0;
    };

    struct Bus {
        std::string name;
        std::string send;
        float volume_db = 0.0F;
        bool solo = false;
        bool mute = false;
        bool bypass = false;
        bool soloed = false;
        int index_cache = 0;
        std::vector<Effect> effects;
        std::vector<Channel> channels;
    };

    enum class State : std::uint8_t {
        Playing,
        Paused,
        FadeOutToPause,
        FadeOutToDeletion,
        AwaitingDeletion
    };

    struct Node {
        PlaybackId id = 0;
        std::unique_ptr<Playback> playback;
        State state = State::Playing;
        // The buses it mixes into and its volume in each, as Godot's bus details: one for most
        // players, more for a 3D player in an area with a reverb bus.
        std::vector<BusVolume> buses;
        std::vector<BusVolume> prev;
        float pitch_scale = 1.0F;
        // A 3D player's high-shelf filter, one processor per speaker, shared by its buses as
        // Godot's filter_process is.
        float highshelf_gain = 0.0F;
        float attenuation_cutoff_hz = 0.0F;
        std::array<Filter::Processor, 2 * static_cast<std::size_t>(kMaxChannels)> filter_process{};
        std::array<Frame, kLookahead> lookahead{};
    };

    Impl(int rate, int channel_count)
        : rate_(rate), channels(channel_count),
          channel_disable_frames(static_cast<std::uint64_t>(kChannelDisableTime * rate)) {}

    [[nodiscard]] int mix_rate() const override { return rate_; }

    Frame* bus_buffer(std::string_view name, int channel) override {
        return channel_mix_buffer(find_bus(name), std::clamp(channel, 0, channels - 1));
    }

    // Godot's thread_find_bus_index: Master for a name it doesn't know.
    [[nodiscard]] int find_bus(std::string_view name) const {
        for (const auto& bus : buses) {
            if (bus->name == name) {
                return bus->index_cache;
            }
        }
        return 0;
    }

    [[nodiscard]] const Bus* bus_named(std::string_view name) const {
        for (const auto& bus : buses) {
            if (bus->name == name) {
                return bus.get();
            }
        }
        return nullptr;
    }

    // Godot's thread_get_channel_mix_buffer: the first use in a mix step clears it.
    Frame* channel_mix_buffer(int index, int channel) {
        Channel& out =
            buses[static_cast<std::size_t>(index)]->channels[static_cast<std::size_t>(channel)];
        if (!out.used) {
            out.used = true;
            out.active = true;
            out.last_mix_with_audio = mix_frames;
            std::ranges::fill(out.buffer, Frame{});
        }
        return out.buffer.data();
    }

    Node* find(PlaybackId id) {
        const auto it = std::ranges::find(playbacks, id, &Node::id);
        return it != playbacks.end() ? &*it : nullptr;
    }
    [[nodiscard]] const Node* find(PlaybackId id) const {
        const auto it = std::ranges::find(playbacks, id, &Node::id);
        return it != playbacks.end() ? &*it : nullptr;
    }

    // Godot's _mix_step_for_channel: the source into out at a volume ramping from vol_start to
    // vol_final, through the node's high-shelf filter for that speaker pair when it has one.
    void mix_step_for_channel(Frame* out, const Frame* source, Frame vol_start, Frame vol_final,
                              Node& node, int channel) const {
        if (node.highshelf_gain != 0.0F) {
            Filter filter;
            filter.set_mode(Filter::Mode::HighShelf);
            filter.set_sampling_rate(static_cast<float>(rate_));
            filter.set_cutoff(node.attenuation_cutoff_hz);
            filter.set_resonance(1);
            filter.set_stages(1);
            filter.set_gain(node.highshelf_gain);
            const bool is_just_started = vol_start.left == 0 && vol_start.right == 0;
            Filter::Processor& processor_l =
                node.filter_process.at(static_cast<std::size_t>(channel) * 2);
            Filter::Processor& processor_r =
                node.filter_process.at((static_cast<std::size_t>(channel) * 2) + 1);
            processor_l.set_filter(&filter, is_just_started);
            processor_l.update_coeffs(kBufferSize);
            processor_r.set_filter(&filter, is_just_started);
            processor_r.update_coeffs(kBufferSize);
            for (int frame_idx = 0; frame_idx < kBufferSize; ++frame_idx) {
                const float lerp_param =
                    static_cast<float>(frame_idx) / static_cast<float>(kBufferSize);
                const Frame vol =
                    vol_final * lerp_param + Frame{1 - lerp_param, 1 - lerp_param} * vol_start;
                Frame mixed = vol * source[frame_idx];
                processor_l.process_one_interp(mixed.left);
                processor_r.process_one_interp(mixed.right);
                out[frame_idx] = out[frame_idx] + mixed;
            }
            return;
        }
        for (int frame_idx = 0; frame_idx < kBufferSize; ++frame_idx) {
            const float lerp_param =
                static_cast<float>(frame_idx) / static_cast<float>(kBufferSize);
            out[frame_idx] = out[frame_idx] + (vol_final * lerp_param +
                                               Frame{1 - lerp_param, 1 - lerp_param} * vol_start) *
                                                  source[frame_idx];
        }
    }

    void mix_step(const BusListener& listener) {
        bool solo_mode = false;
        for (std::size_t i = 0; i < buses.size(); ++i) {
            Bus* bus = buses[i].get();
            bus->index_cache = static_cast<int>(i);
            for (Channel& channel : bus->channels) {
                channel.used = false;
            }
            if (!bus->solo) {
                bus->soloed = false;
                continue;
            }
            // The solo chain.
            solo_mode = true;
            bus->soloed = true;
            while (bus != nullptr) {
                if (bus != buses[0].get()) {
                    // Everything sends somewhere, save the Master bus.
                    const Bus* send = bus_named(bus->send);
                    if (send == nullptr || send->index_cache >= bus->index_cache) {
                        bus = buses[0].get();
                    } else {
                        bus = buses[static_cast<std::size_t>(send->index_cache)].get();
                    }
                    bus->soloed = true;
                } else {
                    bus = nullptr;
                }
            }
        }

        // Each playback mixes into its buses, each speaker pair at its volume, keeping a
        // lookahead buffer so a sudden end can fade out.
        for (auto it = playbacks.begin(); it != playbacks.end();) {
            Node& node = *it;
            if (node.state == State::Paused) {
                ++it;
                continue;
            }
            const bool fading_out =
                node.state == State::FadeOutToDeletion || node.state == State::FadeOutToPause;
            Frame* buf = mix_buffer.data();
            std::ranges::copy(node.lookahead, buf);
            const int mixed = node.playback->mix(&buf[kLookahead], node.pitch_scale, kBufferSize);
            if (mixed != kBufferSize) {
                // 0.94 ^ 64 = 0.01906: there might still be a pop, but a far smaller one.
                constexpr float kFadeoutBase = 0.94F;
                float fadeout = 1;
                for (int idx = std::max(mixed, 0); idx < kBufferSize; ++idx) {
                    fadeout *= kFadeoutBase;
                    buf[idx] = buf[idx] * fadeout;
                }
                node.state = State::AwaitingDeletion;
            } else {
                std::copy(buf + kBufferSize, buf + kBufferSize + kLookahead,
                          node.lookahead.begin());
            }

            std::vector<BusVolume> details = node.buses;
            for (BusVolume& target : details) {
                const int bus_idx = find_bus(target.bus);
                const auto was = std::ranges::find(node.prev, target.bus, &BusVolume::bus);
                for (int k = 0; k < channels; ++k) {
                    Frame* channel = channel_mix_buffer(bus_idx, k);
                    Frame& volume = target.volume.at(static_cast<std::size_t>(k));
                    if (fading_out) {
                        volume = {};
                    }
                    // Starting at full volume keeps transients; a change ramps over the step.
                    const Frame start = was != node.prev.end()
                                            ? was->volume.at(static_cast<std::size_t>(k))
                                            : volume;
                    mix_step_for_channel(channel, buf, start, volume, node, k);
                }
            }
            // Buses it left fade out.
            for (const BusVolume& left : node.prev) {
                if (std::ranges::find(details, left.bus, &BusVolume::bus) != details.end()) {
                    continue;
                }
                const int bus_idx = find_bus(left.bus);
                for (int k = 0; k < channels; ++k) {
                    Frame* channel = channel_mix_buffer(bus_idx, k);
                    mix_step_for_channel(channel, buf, left.volume.at(static_cast<std::size_t>(k)),
                                         {}, node, k);
                }
            }
            node.prev = std::move(details);

            switch (node.state) {
            case State::AwaitingDeletion:
            case State::FadeOutToDeletion:
                it = playbacks.erase(it);
                continue;
            case State::FadeOutToPause:
                node.state = State::Paused;
                break;
            case State::Playing:
            case State::Paused:
                break;
            }
            ++it;
        }

        // Effects and sends, from the last bus to Master.
        for (int i = static_cast<int>(buses.size()) - 1; i >= 0; --i) {
            Bus& bus = *buses[static_cast<std::size_t>(i)];
            for (Channel& channel : bus.channels) {
                if (channel.active && !channel.used) {
                    // Not mixed into, but still active, so it must be cleared.
                    std::ranges::fill(channel.buffer, Frame{});
                }
            }
            if (!bus.bypass) {
                for (std::size_t j = 0; j < bus.effects.size(); ++j) {
                    if (!bus.effects[j].enabled) {
                        continue;
                    }
                    for (std::size_t k = 0; k < bus.channels.size(); ++k) {
                        Channel& channel = bus.channels[k];
                        EffectInstance* effect = channel.effect_instances[j].get();
                        if (effect == nullptr || !(channel.active || effect->process_silence())) {
                            continue;
                        }
                        effect->process(channel.buffer.data(), temp_buffer[k].data(), kBufferSize);
                        // Swap buffers, so the channel's always has the right sound.
                        std::swap(channel.buffer, temp_buffer[k]);
                    }
                }
            }

            Bus* send = nullptr;
            if (i > 0) {
                const Bus* named = bus_named(bus.send);
                send = named == nullptr || named->index_cache >= bus.index_cache
                           ? buses[0].get()
                           : buses[static_cast<std::size_t>(named->index_cache)].get();
            }

            float volume = db_to_linear(bus.volume_db);
            if (solo_mode ? !bus.soloed : bus.mute) {
                volume = 0.0F;
            }
            for (int k = 0; k < static_cast<int>(bus.channels.size()); ++k) {
                Channel& channel = bus.channels[static_cast<std::size_t>(k)];
                if (!channel.active) {
                    channel.peak = {kMinPeakDb, kMinPeakDb};
                    if (listener) {
                        listener(i, k, {});
                    }
                    continue;
                }
                Frame peak{};
                for (Frame& frame : channel.buffer) {
                    frame = frame * volume;
                    peak.left = std::max(peak.left, std::abs(frame.left));
                    peak.right = std::max(peak.right, std::abs(frame.right));
                }
                channel.peak = {linear_to_db(peak.left + kPeakOffset),
                                linear_to_db(peak.right + kPeakOffset)};
                if (!channel.used) {
                    // See if any sound is left, since nothing was mixed in.
                    if (std::max(peak.right, peak.left) >
                        db_to_linear(kChannelDisableThresholdDb)) {
                        channel.last_mix_with_audio = mix_frames;
                    } else if (mix_frames - channel.last_mix_with_audio > channel_disable_frames) {
                        channel.active = false;
                        if (listener) {
                            listener(i, k, {});
                        }
                        continue; // went inactive, don't mix
                    }
                }
                if (listener) {
                    listener(i, k, channel.buffer);
                }
                if (send != nullptr) {
                    Frame* target = channel_mix_buffer(send->index_cache, k);
                    for (int j = 0; j < kBufferSize; ++j) {
                        target[j] = target[j] + channel.buffer[static_cast<std::size_t>(j)];
                    }
                }
            }
        }
        mix_frames += kBufferSize;
        to_mix = kBufferSize;
    }

    // Godot's _update_bus_effects: each speaker pair gets its own copy of each effect.
    void update_bus_effects(Bus& bus, const BusSettings& settings) {
        bus.effects.clear();
        bus.channels.clear();
        bus.channels.resize(static_cast<std::size_t>(channels));
        for (const EffectSlot& slot : settings.effects) {
            bus.effects.push_back({slot.type, slot.enabled});
            for (int k = 0; k < channels; ++k) {
                auto instance = make_effect(slot, *this, k);
                if (!instance) {
                    if (k == 0) {
                        log::warn("an audio effect can't run",
                                  {{"bus", bus.name},
                                   {"type", slot.type},
                                   {"error", instance.error().message}});
                    }
                    bus.channels[static_cast<std::size_t>(k)].effect_instances.emplace_back();
                    continue;
                }
                bus.channels[static_cast<std::size_t>(k)].effect_instances.push_back(
                    std::move(instance).value());
            }
        }
    }

    std::unique_ptr<Bus> make_bus(const BusSettings& settings, bool master) {
        auto bus = std::make_unique<Bus>();
        bus->name = master ? "Master" : settings.name;
        bus->send = settings.send;
        bus->volume_db = settings.volume_db;
        bus->solo = settings.solo;
        bus->mute = settings.mute;
        bus->bypass = settings.bypass_effects;
        update_bus_effects(*bus, settings);
        return bus;
    }

    int rate_;
    int channels; // speaker pairs
    std::vector<std::unique_ptr<Bus>> buses;
    std::vector<Node> playbacks;
    std::vector<Frame> mix_buffer = std::vector<Frame>(kBufferSize + kLookahead);
    std::array<std::vector<Frame>, kMaxChannels> temp_buffer{
        std::vector<Frame>(kBufferSize), std::vector<Frame>(kBufferSize),
        std::vector<Frame>(kBufferSize), std::vector<Frame>(kBufferSize)};
    int to_mix = 0;
    std::uint64_t mix_frames = 0;
    std::uint64_t channel_disable_frames;
    PlaybackId next_id = 1;
};

Server::Server(int mix_rate, SpeakerMode speakers)
    : impl_(std::make_unique<Impl>(mix_rate, audio::channel_count(speakers))), mix_rate_(mix_rate),
      speakers_(speakers) {
    set_layout(default_bus_layout());
}

void Server::set_speaker_mode(SpeakerMode speakers) {
    if (speakers == speakers_) {
        return;
    }
    speakers_ = speakers;
    impl_->channels = audio::channel_count(speakers);
    for (std::size_t i = 0; i < impl_->buses.size(); ++i) {
        impl_->update_bus_effects(*impl_->buses[i], layout_.buses[i]);
    }
}

Server::~Server() {
    // Microphone playbacks let go of the input as they go, so they go first.
    impl_->playbacks.clear();
}

void Server::set_layout(const BusLayout& layout) {
    layout_ = layout.buses.empty() ? default_bus_layout() : layout;
    layout_.buses.front().name = "Master";
    impl_->buses.clear();
    for (std::size_t i = 0; i < layout_.buses.size(); ++i) {
        impl_->buses.push_back(impl_->make_bus(layout_.buses[i], i == 0));
        impl_->buses.back()->index_cache = static_cast<int>(i);
    }
}

int Server::bus_index(std::string_view name) const {
    for (std::size_t i = 0; i < impl_->buses.size(); ++i) {
        if (impl_->buses[i]->name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Server::set_bus_volume_db(int bus, float volume_db) {
    if (bus >= 0 && bus < static_cast<int>(impl_->buses.size())) {
        impl_->buses[static_cast<std::size_t>(bus)]->volume_db = volume_db;
        layout_.buses[static_cast<std::size_t>(bus)].volume_db = volume_db;
    }
}

void Server::set_bus_mute(int bus, bool mute) {
    if (bus >= 0 && bus < static_cast<int>(impl_->buses.size())) {
        impl_->buses[static_cast<std::size_t>(bus)]->mute = mute;
        layout_.buses[static_cast<std::size_t>(bus)].mute = mute;
    }
}

void Server::set_bus_solo(int bus, bool solo) {
    if (bus >= 0 && bus < static_cast<int>(impl_->buses.size())) {
        impl_->buses[static_cast<std::size_t>(bus)]->solo = solo;
        layout_.buses[static_cast<std::size_t>(bus)].solo = solo;
    }
}

void Server::set_bus_bypass_effects(int bus, bool bypass) {
    if (bus >= 0 && bus < static_cast<int>(impl_->buses.size())) {
        impl_->buses[static_cast<std::size_t>(bus)]->bypass = bypass;
        layout_.buses[static_cast<std::size_t>(bus)].bypass_effects = bypass;
    }
}

void Server::set_bus_effect_enabled(int bus, int effect, bool enabled) {
    if (bus < 0 || bus >= static_cast<int>(impl_->buses.size())) {
        return;
    }
    BusSettings& settings = layout_.buses[static_cast<std::size_t>(bus)];
    if (effect < 0 || effect >= static_cast<int>(settings.effects.size())) {
        return;
    }
    settings.effects[static_cast<std::size_t>(effect)].enabled = enabled;
    impl_->buses[static_cast<std::size_t>(bus)]->effects[static_cast<std::size_t>(effect)].enabled =
        enabled;
}

EffectInstance* Server::effect_instance(int bus, int effect, std::string_view type, int channel) {
    if (bus < 0 || bus >= static_cast<int>(impl_->buses.size())) {
        return nullptr;
    }
    const Impl::Bus& found = *impl_->buses[static_cast<std::size_t>(bus)];
    if (effect < 0 || effect >= static_cast<int>(found.effects.size()) || channel < 0 ||
        channel >= static_cast<int>(found.channels.size())) {
        return nullptr;
    }
    if (found.effects[static_cast<std::size_t>(effect)].type != type) {
        return nullptr;
    }
    return found.channels[static_cast<std::size_t>(channel)]
        .effect_instances[static_cast<std::size_t>(effect)]
        .get();
}

Frame Server::bus_peak_db(int bus, int channel) const {
    if (bus < 0 || bus >= static_cast<int>(impl_->buses.size())) {
        return {kMinPeakDb, kMinPeakDb};
    }
    const auto& channels = impl_->buses[static_cast<std::size_t>(bus)]->channels;
    if (channel < 0 || channel >= static_cast<int>(channels.size())) {
        return {kMinPeakDb, kMinPeakDb};
    }
    return channels[static_cast<std::size_t>(channel)].peak;
}

Server::PlaybackId Server::start(std::unique_ptr<Playback> playback, std::string bus,
                                 const Volumes& volume, double from_seconds, float pitch_scale,
                                 bool paused) {
    Impl::Node node;
    node.id = impl_->next_id++;
    node.state = paused ? Impl::State::Paused : Impl::State::Playing;
    node.playback = std::move(playback);
    node.playback->attach(*this);
    node.playback->start(from_seconds);
    node.buses.push_back({std::move(bus), volume});
    node.pitch_scale = pitch_scale;
    impl_->playbacks.push_back(std::move(node));
    return impl_->playbacks.back().id;
}

void Server::stop(PlaybackId id) {
    Impl::Node* node = impl_->find(id);
    if (node == nullptr) {
        return;
    }
    if (!node->playback->is_playing()) {
        node->playback->stop();
    }
    if (node->state != Impl::State::AwaitingDeletion) {
        node->state = Impl::State::FadeOutToDeletion;
    }
}

void Server::set_paused(PlaybackId id, bool paused) {
    Impl::Node* node = impl_->find(id);
    if (node == nullptr) {
        return;
    }
    const Impl::State old = node->state;
    if (!paused && old == Impl::State::Playing) {
        return;
    }
    if (paused && (old == Impl::State::Paused || old == Impl::State::FadeOutToPause)) {
        return;
    }
    if (old == Impl::State::FadeOutToDeletion || old == Impl::State::AwaitingDeletion) {
        return;
    }
    node->state = paused ? Impl::State::FadeOutToPause : Impl::State::Playing;
}

void Server::set_bus_exclusive(PlaybackId id, std::string bus, const Volumes& volume) {
    if (Impl::Node* node = impl_->find(id); node != nullptr) {
        node->buses.assign(1, {std::move(bus), volume});
    }
}

void Server::set_bus_volumes(PlaybackId id, std::vector<BusVolume> buses) {
    if (Impl::Node* node = impl_->find(id); node != nullptr) {
        if (buses.size() > kMaxBusesPerPlayback) {
            buses.resize(kMaxBusesPerPlayback);
        }
        node->buses = std::move(buses);
    }
}

void Server::set_highshelf(PlaybackId id, float linear_gain, float cutoff_hz) {
    if (Impl::Node* node = impl_->find(id); node != nullptr) {
        node->highshelf_gain = linear_gain;
        node->attenuation_cutoff_hz = cutoff_hz;
    }
}

void Server::set_pitch_scale(PlaybackId id, float pitch_scale) {
    if (Impl::Node* node = impl_->find(id); node != nullptr) {
        node->pitch_scale = pitch_scale;
    }
}

bool Server::is_active(PlaybackId id) const {
    const Impl::Node* node = impl_->find(id);
    return node != nullptr && node->state == Impl::State::Playing;
}

bool Server::is_paused(PlaybackId id) const {
    const Impl::Node* node = impl_->find(id);
    return node != nullptr &&
           (node->state == Impl::State::Paused || node->state == Impl::State::FadeOutToPause);
}

double Server::position(PlaybackId id) const {
    const Impl::Node* node = impl_->find(id);
    return node != nullptr ? node->playback->position() : 0.0;
}

Playback* Server::playback(PlaybackId id) {
    Impl::Node* node = impl_->find(id);
    return node != nullptr ? node->playback.get() : nullptr;
}

const Playback* Server::playback(PlaybackId id) const {
    const Impl::Node* node = impl_->find(id);
    return node != nullptr ? node->playback.get() : nullptr;
}

std::size_t Server::playback_count() const {
    return impl_->playbacks.size();
}

void Server::set_input_enabled(bool enabled) {
    input_.enabled = enabled;
    if (!enabled) {
        input_.game = false;
    }
}

bool Server::input_active() const noexcept {
    return input_.enabled && (input_.game || input_.users > 0);
}

void Server::begin_input() {
    // As Godot's set_input_device_active and the driver's input_start: the buffer starts over.
    input_.buffer.assign(static_cast<std::size_t>(input_buffer_length_frames()), Frame{});
    input_.recorded = 0;
    input_.read = 0;
}

bool Server::set_input_active(bool active) {
    if (active == input_.game) {
        return true;
    }
    if (active) {
        if (!input_.enabled) {
            log::warn("microphone input is off; turn on audio.enable_input in project.json");
            return false;
        }
        if (!input_active()) {
            begin_input();
        }
    }
    input_.game = active;
    return true;
}

bool Server::acquire_input() {
    if (!input_.enabled) {
        log::warn("microphone input is off; turn on audio.enable_input in project.json");
        return false;
    }
    if (!input_active()) {
        begin_input();
    }
    ++input_.users;
    return true;
}

void Server::release_input() {
    input_.users = std::max(input_.users - 1, 0);
}

void Server::push_input(std::span<const Frame> frames) {
    if (!input_active() || input_.buffer.empty()) {
        return;
    }
    for (const Frame& frame : frames) {
        input_.buffer[input_.recorded % input_.buffer.size()] = frame;
        ++input_.recorded;
    }
}

int Server::input_frames_available() const {
    // Frames the buffer has dropped since are lost.
    return static_cast<int>(
        std::min<std::uint64_t>(input_.recorded - input_.read, input_.buffer.size()));
}

int Server::input_buffer_length_frames() const {
    return static_cast<int>(kInputBufferSeconds * mix_rate_);
}

std::vector<Frame> Server::input_frames(int frames) {
    if (frames < 0 || frames > input_frames_available()) {
        return {};
    }
    if (input_.recorded > input_.buffer.size()) {
        input_.read = std::max(input_.read, input_.recorded - input_.buffer.size());
    }
    std::vector<Frame> out(static_cast<std::size_t>(frames));
    for (Frame& frame : out) {
        frame = input_.buffer[input_.read % input_.buffer.size()];
        ++input_.read;
    }
    return out;
}

Frame Server::input_frame(std::uint64_t index) const {
    if (index >= input_.recorded || index + input_.buffer.size() < input_.recorded) {
        return {};
    }
    return input_.buffer[index % input_.buffer.size()];
}

void Server::mix(std::span<Frame> out) {
    // As Godot's _driver_process: each speaker pair of a frame after the one before.
    const auto cs = static_cast<std::size_t>(impl_->channels);
    const std::size_t frames = out.size() / cs;
    std::size_t done = 0;
    while (done < frames) {
        if (impl_->to_mix == 0) {
            impl_->mix_step(bus_listener_);
        }
        const auto to_copy =
            std::min<std::size_t>(static_cast<std::size_t>(impl_->to_mix), frames - done);
        const Impl::Bus& master = *impl_->buses.front();
        const std::size_t from = kBufferSize - static_cast<std::size_t>(impl_->to_mix);
        for (std::size_t k = 0; k < cs; ++k) {
            const Impl::Channel& channel = master.channels[k];
            for (std::size_t j = 0; j < to_copy; ++j) {
                Frame& dest = out[((done + j) * cs) + k];
                if (channel.active) {
                    const Frame& frame = channel.buffer[from + j];
                    dest = {std::clamp(frame.left, -1.0F, 1.0F),
                            std::clamp(frame.right, -1.0F, 1.0F)};
                } else {
                    dest = {};
                }
            }
        }
        done += to_copy;
        impl_->to_mix -= static_cast<int>(to_copy);
    }
    mixed_frames_ += frames;
}

} // namespace slash::audio
