// The mixer, following Godot's AudioServer (servers/audio/audio_server.cpp): _driver_process,
// _mix_step and _mix_step_for_channel, the playback list's states and lookahead fade, and the
// bus setters. Godot mixes on the audio driver's thread; here mix() runs on the game's, once per
// tick, so the same ticks make the same sound.

#include "slash/audio/audio.hpp"
#include "slash/audio/effects.hpp"
#include "slash/core/log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace slash::audio {

namespace {

constexpr int kBufferSize = Server::kMixStep;
constexpr int kLookahead = 64;                       // AuSC::LOOKAHEAD_BUFFER_SIZE
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

} // namespace

struct Server::Impl final : MixContext {
    struct Effect {
        std::string type;
        std::unique_ptr<EffectInstance> instance; // nullptr when it couldn't be made
        bool enabled = true;
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
        // Godot keeps a buffer per speaker pair; stereo has one.
        std::vector<Frame> buffer = std::vector<Frame>(kBufferSize);
        bool used = false;
        bool active = false;
        Frame peak{kMinPeakDb, kMinPeakDb};
        std::uint64_t last_mix_with_audio = 0;
    };

    enum class State : std::uint8_t {
        Playing,
        Paused,
        FadeOutToPause,
        FadeOutToDeletion,
        AwaitingDeletion
    };

    // The bus a playback mixes into and its volume there. Godot allows several per playback, for
    // 3D areas; 2D players use one.
    struct BusDetails {
        std::string bus;
        Frame volume;
        bool active = false;
    };

    struct Node {
        PlaybackId id = 0;
        std::unique_ptr<Playback> playback;
        State state = State::Playing;
        BusDetails bus;
        BusDetails prev;
        float pitch_scale = 1.0F;
        std::array<Frame, kLookahead> lookahead{};
    };

    explicit Impl(int rate)
        : rate_(rate),
          channel_disable_frames(static_cast<std::uint64_t>(kChannelDisableTime * rate)) {}

    [[nodiscard]] int mix_rate() const override { return rate_; }

    Frame* bus_buffer(std::string_view name) override { return channel_mix_buffer(find_bus(name)); }

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
    Frame* channel_mix_buffer(int index) {
        Bus& bus = *buses[static_cast<std::size_t>(index)];
        if (!bus.used) {
            bus.used = true;
            bus.active = true;
            bus.last_mix_with_audio = mix_frames;
            std::ranges::fill(bus.buffer, Frame{});
        }
        return bus.buffer.data();
    }

    Node* find(PlaybackId id) {
        const auto it = std::ranges::find(playbacks, id, &Node::id);
        return it != playbacks.end() ? &*it : nullptr;
    }
    [[nodiscard]] const Node* find(PlaybackId id) const {
        const auto it = std::ranges::find(playbacks, id, &Node::id);
        return it != playbacks.end() ? &*it : nullptr;
    }

    static void mix_step_for_channel(Frame* out, const Frame* source, Frame vol_start,
                                     Frame vol_final) {
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
            bus->used = false;
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

        // Each playback mixes into its bus, keeping a lookahead buffer so a sudden end can
        // fade out.
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

            BusDetails details = node.bus;
            if (details.active) {
                Frame* channel = channel_mix_buffer(find_bus(details.bus));
                if (fading_out) {
                    details.volume = {};
                }
                // Starting at full volume keeps transients; a change ramps over the step.
                const Frame start = node.prev.active && node.prev.bus == details.bus
                                        ? node.prev.volume
                                        : details.volume;
                mix_step_for_channel(channel, buf, start, details.volume);
            }
            // A bus it left fades out.
            if (node.prev.active && !(details.active && details.bus == node.prev.bus)) {
                Frame* channel = channel_mix_buffer(find_bus(node.prev.bus));
                mix_step_for_channel(channel, buf, node.prev.volume, {});
            }
            node.prev = details;

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
            if (bus.active && !bus.used) {
                // Not mixed into, but still active, so it must be cleared.
                std::ranges::fill(bus.buffer, Frame{});
            }
            if (!bus.bypass) {
                for (Effect& effect : bus.effects) {
                    if (!effect.enabled || effect.instance == nullptr ||
                        !(bus.active || effect.instance->process_silence())) {
                        continue;
                    }
                    effect.instance->process(bus.buffer.data(), temp_buffer.data(), kBufferSize);
                    std::swap(bus.buffer, temp_buffer);
                }
            }

            Bus* send = nullptr;
            if (i > 0) {
                const Bus* named = bus_named(bus.send);
                send = named == nullptr || named->index_cache >= bus.index_cache
                           ? buses[0].get()
                           : buses[static_cast<std::size_t>(named->index_cache)].get();
            }

            if (!bus.active) {
                bus.peak = {kMinPeakDb, kMinPeakDb};
                if (listener) {
                    listener(i, {});
                }
                continue;
            }
            Frame peak{};
            float volume = db_to_linear(bus.volume_db);
            if (solo_mode ? !bus.soloed : bus.mute) {
                volume = 0.0F;
            }
            for (Frame& frame : bus.buffer) {
                frame = frame * volume;
                peak.left = std::max(peak.left, std::abs(frame.left));
                peak.right = std::max(peak.right, std::abs(frame.right));
            }
            bus.peak = {linear_to_db(peak.left + kPeakOffset),
                        linear_to_db(peak.right + kPeakOffset)};
            if (!bus.used) {
                // See if any sound is left, since nothing was mixed in.
                if (std::max(peak.right, peak.left) > db_to_linear(kChannelDisableThresholdDb)) {
                    bus.last_mix_with_audio = mix_frames;
                } else if (mix_frames - bus.last_mix_with_audio > channel_disable_frames) {
                    bus.active = false;
                    if (listener) {
                        listener(i, {});
                    }
                    continue; // went inactive, don't mix
                }
            }
            if (listener) {
                listener(i, bus.buffer);
            }
            if (send != nullptr) {
                Frame* target = channel_mix_buffer(send->index_cache);
                for (int j = 0; j < kBufferSize; ++j) {
                    target[j] = target[j] + bus.buffer[static_cast<std::size_t>(j)];
                }
            }
        }
        mix_frames += kBufferSize;
        to_mix = kBufferSize;
    }

    std::unique_ptr<Bus> make_bus(const BusSettings& settings, bool master) {
        auto bus = std::make_unique<Bus>();
        bus->name = master ? "Master" : settings.name;
        bus->send = settings.send;
        bus->volume_db = settings.volume_db;
        bus->solo = settings.solo;
        bus->mute = settings.mute;
        bus->bypass = settings.bypass_effects;
        for (const EffectSlot& slot : settings.effects) {
            auto instance = make_effect(slot, *this);
            if (!instance) {
                log::warn(
                    "an audio effect can't run",
                    {{"bus", bus->name}, {"type", slot.type}, {"error", instance.error().message}});
                bus->effects.push_back({slot.type, nullptr, slot.enabled});
                continue;
            }
            bus->effects.push_back({slot.type, std::move(instance).value(), slot.enabled});
        }
        return bus;
    }

    int rate_;
    std::vector<std::unique_ptr<Bus>> buses;
    std::vector<Node> playbacks;
    std::vector<Frame> mix_buffer = std::vector<Frame>(kBufferSize + kLookahead);
    std::vector<Frame> temp_buffer = std::vector<Frame>(kBufferSize);
    int to_mix = 0;
    std::uint64_t mix_frames = 0;
    std::uint64_t channel_disable_frames;
    PlaybackId next_id = 1;
};

Server::Server(int mix_rate) : impl_(std::make_unique<Impl>(mix_rate)), mix_rate_(mix_rate) {
    set_layout(default_bus_layout());
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

EffectInstance* Server::effect_instance(int bus, int effect, std::string_view type) {
    if (bus < 0 || bus >= static_cast<int>(impl_->buses.size())) {
        return nullptr;
    }
    auto& effects = impl_->buses[static_cast<std::size_t>(bus)]->effects;
    if (effect < 0 || effect >= static_cast<int>(effects.size())) {
        return nullptr;
    }
    const Impl::Effect& found = effects[static_cast<std::size_t>(effect)];
    return found.type == type ? found.instance.get() : nullptr;
}

Frame Server::bus_peak_db(int bus) const {
    if (bus < 0 || bus >= static_cast<int>(impl_->buses.size())) {
        return {kMinPeakDb, kMinPeakDb};
    }
    return impl_->buses[static_cast<std::size_t>(bus)]->peak;
}

Server::PlaybackId Server::start(std::unique_ptr<Playback> playback, std::string bus, Frame volume,
                                 double from_seconds, float pitch_scale, bool paused) {
    Impl::Node node;
    node.id = impl_->next_id++;
    node.state = paused ? Impl::State::Paused : Impl::State::Playing;
    node.playback = std::move(playback);
    node.playback->attach(*this);
    node.playback->start(from_seconds);
    node.bus = {std::move(bus), volume, true};
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

void Server::set_bus_exclusive(PlaybackId id, std::string bus, Frame volume) {
    if (Impl::Node* node = impl_->find(id); node != nullptr) {
        node->bus = {std::move(bus), volume, true};
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
    std::size_t done = 0;
    while (done < out.size()) {
        if (impl_->to_mix == 0) {
            impl_->mix_step(bus_listener_);
        }
        const auto to_copy =
            std::min<std::size_t>(static_cast<std::size_t>(impl_->to_mix), out.size() - done);
        const Impl::Bus& master = *impl_->buses.front();
        const std::size_t from = kBufferSize - static_cast<std::size_t>(impl_->to_mix);
        for (std::size_t j = 0; j < to_copy; ++j) {
            if (master.active) {
                const Frame& frame = master.buffer[from + j];
                out[done + j] = {std::clamp(frame.left, -1.0F, 1.0F),
                                 std::clamp(frame.right, -1.0F, 1.0F)};
            } else {
                out[done + j] = {};
            }
        }
        done += to_copy;
        impl_->to_mix -= static_cast<int>(to_copy);
    }
    mixed_frames_ += out.size();
}

} // namespace slash::audio
