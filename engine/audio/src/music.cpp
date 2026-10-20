// Music, as Godot's interactive_music module (modules/interactive_music): AudioStreamPlaylist,
// AudioStreamSynchronized and AudioStreamInteractive and their playbacks, ported from Godot's
// audio_stream_playlist.cpp, audio_stream_synchronized.cpp and audio_stream_interactive.cpp.

#include "slash/audio/audio.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <utility>

namespace slash::audio {

namespace {

// The next 32-bit value of a xorshift sequence, so shuffles repeat on every platform.
std::uint32_t next_seed(std::uint32_t& state) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
}

Frame operator*(Frame a, float s) {
    return {a.left * s, a.right * s};
}

Frame& operator+=(Frame& a, Frame b) {
    a.left += b.left;
    a.right += b.right;
    return a;
}

// How long a stream plays in a playlist or before a transition at its end: its beats if it has
// them, else its length.
double beat_length(const Stream& stream) {
    const double bpm = stream.bpm();
    const int beat_count = stream.beat_count();
    if (bpm > 0.0 && beat_count > 0) {
        return beat_count * 60.0 / bpm;
    }
    return stream.length();
}

std::vector<std::unique_ptr<Playback>>
instantiate_all(const std::vector<std::shared_ptr<const Stream>>& streams, int mix_rate,
                std::uint32_t& random) {
    std::vector<std::unique_ptr<Playback>> playbacks;
    playbacks.reserve(streams.size());
    for (const auto& stream : streams) {
        playbacks.push_back(stream->instantiate(mix_rate, next_seed(random)));
    }
    return playbacks;
}

// ---- Playlists ----

// Godot's AudioStreamPlaybackPlaylist: each stream for its length in turn, the one before fading
// out over the start of the next.
class PlaylistPlayback final : public Playback {
public:
    PlaylistPlayback(std::vector<std::shared_ptr<const Stream>> streams, bool shuffle, bool loop,
                     float fade_time, int mix_rate, std::uint32_t random)
        : streams_(std::move(streams)), shuffle_(shuffle), loop_(loop), fade_time_(fade_time),
          mix_rate_(mix_rate), random_(random == 0 ? 0x9E3779B9U : random),
          playbacks_(instantiate_all(streams_, mix_rate, random_)), play_order_(streams_.size()) {}

    void start(double from_seconds) override {
        if (active_) {
            stop();
        }
        from_seconds = std::max(0.0, from_seconds);
        double list_length = 0.0;
        for (const auto& stream : streams_) {
            list_length += beat_length(*stream);
        }
        if (from_seconds >= list_length) {
            if (!loop_ || list_length <= 0.0) {
                return; // No loop, end.
            }
            from_seconds = std::fmod(from_seconds, list_length);
        }
        update_order();
        play_index_ = -1;
        double play_ofs = from_seconds;
        for (std::size_t i = 0; i < streams_.size(); ++i) {
            const double length = beat_length(*streams_[static_cast<std::size_t>(play_order_[i])]);
            if (play_ofs < length) {
                play_index_ = static_cast<int>(i);
                stream_todo_ = length - play_ofs;
                break;
            }
            play_ofs -= length;
        }
        if (play_index_ == -1) {
            return;
        }
        current().start(play_ofs);
        fade_index_ = -1;
        loops_ = 0;
        offset_ = from_seconds;
        active_ = true;
    }

    void stop() override {
        active_ = false;
        for (const auto& playback : playbacks_) {
            playback->stop();
        }
    }

    [[nodiscard]] bool is_playing() const override { return active_; }
    [[nodiscard]] double position() const override { return offset_; }
    [[nodiscard]] int loop_count() const override { return loops_; }

    int mix(Frame* buffer, float /*rate_scale*/, int frames) override {
        if (!active_) {
            return 0;
        }
        const double time_dec = 1.0 / mix_rate_;
        const double fade_dec = fade_time_ > 0.0F ? (1.0 / fade_time_) / mix_rate_ : 1.0;
        int done = 0;
        while (done < frames) {
            const int to_mix = std::min(frames - done, kMixBufferSize);
            // As Godot's, the streams of a playlist play at their own speed.
            static_cast<void>(current().mix(mix_buffer_.data(), 1.0F, to_mix));
            if (fade_index_ != -1) {
                static_cast<void>(playbacks_[static_cast<std::size_t>(fade_index_)]->mix(
                    fade_buffer_.data(), 1.0F, to_mix));
            }
            offset_ += time_dec * to_mix;
            for (int i = 0; i < to_mix; ++i) {
                // As Godot's, this frame is the last stream's even when the next starts here.
                Frame out = mix_buffer_[static_cast<std::size_t>(i)];
                stream_todo_ -= time_dec;
                if (stream_todo_ < 0 && !next_stream(i, to_mix)) {
                    std::fill(buffer + done + i, buffer + frames, Frame{});
                    return done + i;
                }
                if (fade_index_ != -1) {
                    out += fade_buffer_[static_cast<std::size_t>(i)] *
                           static_cast<float>(fade_volume_);
                    fade_volume_ -= fade_dec;
                    if (fade_volume_ <= 0.0) {
                        playbacks_[static_cast<std::size_t>(fade_index_)]->stop();
                        fade_index_ = -1;
                    }
                }
                buffer[done + i] = out;
            }
            done += to_mix;
        }
        return frames;
    }

private:
    static constexpr int kMixBufferSize = 128; // Godot's MIX_BUFFER_SIZE

    Playback& current() {
        return *playbacks_[static_cast<std::size_t>(
            play_order_[static_cast<std::size_t>(play_index_)])];
    }

    void update_order() {
        const auto count = static_cast<std::uint32_t>(streams_.size());
        for (std::size_t i = 0; i < streams_.size(); ++i) {
            play_order_[i] = static_cast<int>(i);
        }
        if (shuffle_) {
            for (std::size_t i = 0; i < streams_.size(); ++i) {
                std::swap(play_order_[i], play_order_[next_seed(random_) % count]);
            }
        }
    }

    // Moves on to the next stream at frame i of the to_mix frames mixed; false at the end of a
    // playlist that doesn't loop.
    bool next_stream(int i, int to_mix) {
        const int prev = play_order_[static_cast<std::size_t>(play_index_)];
        ++play_index_;
        if (play_index_ >= static_cast<int>(streams_.size())) {
            if (!loop_) {
                active_ = false;
                return false;
            }
            update_order();
            play_index_ = 0;
            ++loops_;
            offset_ = (1.0 / mix_rate_) * (to_mix - i);
        }
        const int idx = play_order_[static_cast<std::size_t>(play_index_)];
        bool restart = true;
        if (prev == idx) {
            // Back to the same one: it goes on if it loops, else starts over.
            restart = !streams_[static_cast<std::size_t>(prev)]->has_loop();
            fade_index_ = -1;
        } else {
            // What the last stream mixed after this frame fades out under the next one.
            fade_buffer_[static_cast<std::size_t>(i)] = {};
            std::copy(mix_buffer_.begin() + i + 1, mix_buffer_.begin() + to_mix,
                      fade_buffer_.begin() + i + 1);
            fade_index_ = prev;
            fade_volume_ = 1.0;
        }
        if (restart) {
            Playback& next = *playbacks_[static_cast<std::size_t>(idx)];
            next.start(0);
            static_cast<void>(next.mix(mix_buffer_.data() + i, 1.0F, to_mix - i));
        }
        stream_todo_ = beat_length(*streams_[static_cast<std::size_t>(idx)]);
        return true;
    }

    std::vector<std::shared_ptr<const Stream>> streams_;
    bool shuffle_;
    bool loop_;
    float fade_time_;
    int mix_rate_;
    std::uint32_t random_;
    std::vector<std::unique_ptr<Playback>> playbacks_;
    std::vector<int> play_order_;
    std::array<Frame, kMixBufferSize> mix_buffer_{};
    std::array<Frame, kMixBufferSize> fade_buffer_{};
    int play_index_ = -1;
    int fade_index_ = -1;
    double fade_volume_ = 1.0;
    double stream_todo_ = 0.0;
    double offset_ = 0.0;
    int loops_ = 0;
    bool active_ = false;
};

class PlaylistStream final : public Stream {
public:
    PlaylistStream(std::vector<std::shared_ptr<const Stream>> streams, const StreamFile& settings)
        : streams_(std::move(streams)), shuffle_(settings.shuffle), loop_(settings.loop),
          fade_time_(settings.fade_time) {}

    [[nodiscard]] std::unique_ptr<Playback> instantiate(int mix_rate,
                                                        std::uint32_t random) const override {
        return std::make_unique<PlaylistPlayback>(streams_, shuffle_, loop_, fade_time_, mix_rate,
                                                  random);
    }
    [[nodiscard]] double length() const override {
        double total = 0.0;
        for (const auto& stream : streams_) {
            total += beat_length(*stream);
        }
        return total;
    }
    [[nodiscard]] double bpm() const override {
        for (const auto& stream : streams_) {
            if (stream->bpm() != 0.0) {
                return stream->bpm();
            }
        }
        return 0.0;
    }
    [[nodiscard]] bool has_loop() const override { return loop_; }
    // Godot's music streams keep AudioStream's default: play() starts them over.
    [[nodiscard]] bool is_monophonic() const override { return true; }

private:
    std::vector<std::shared_ptr<const Stream>> streams_;
    bool shuffle_;
    bool loop_;
    float fade_time_;
};

// ---- Synchronized streams ----

// Godot's AudioStreamPlaybackSynchronized: every stream from the same start, each at its volume.
// A player's stream_<index>_volume_db parameter changes a volume, ramping over a mix step where
// Godot steps, so a layer fading in doesn't click.
class SynchronizedPlayback final : public Playback {
public:
    SynchronizedPlayback(const std::vector<std::shared_ptr<const Stream>>& streams,
                         const std::vector<float>& volumes_db, int mix_rate, std::uint32_t random)
        : playbacks_(instantiate_all(streams, mix_rate, random)), volumes_db_(volumes_db) {
        for (const float db : volumes_db_) {
            gains_.push_back(db_to_linear(db));
        }
    }

    void start(double from_seconds) override {
        if (active_) {
            stop();
        }
        for (const auto& playback : playbacks_) {
            playback->start(from_seconds);
            active_ = true;
        }
    }

    void stop() override {
        active_ = false;
        for (const auto& playback : playbacks_) {
            playback->stop();
        }
    }

    [[nodiscard]] bool is_playing() const override { return active_; }

    [[nodiscard]] double position() const override {
        double max_pos = 0.0;
        for (const auto& playback : playbacks_) {
            if (playback->is_playing()) {
                max_pos = std::max(max_pos, playback->position());
            }
        }
        return max_pos;
    }

    [[nodiscard]] int loop_count() const override {
        int min_loops = 0;
        bool found = false;
        for (const auto& playback : playbacks_) {
            if (playback->is_playing() && (!found || playback->loop_count() < min_loops)) {
                min_loops = playback->loop_count();
                found = true;
            }
        }
        return min_loops;
    }

    int mix(Frame* buffer, float rate_scale, int frames) override {
        if (!active_) {
            return 0;
        }
        bool any_active = false;
        int done = 0;
        while (done < frames) {
            const int to_mix = std::min(frames - done, kMixBufferSize);
            Frame* out = buffer + done;
            std::fill(out, out + to_mix, Frame{});
            for (std::size_t i = 0; i < playbacks_.size(); ++i) {
                const float from = gains_[i];
                const float to = db_to_linear(volumes_db_[i]);
                gains_[i] = to;
                if (!playbacks_[i]->is_playing()) {
                    continue;
                }
                any_active = true;
                static_cast<void>(playbacks_[i]->mix(mix_buffer_.data(), rate_scale, to_mix));
                const float step = (to - from) / static_cast<float>(to_mix);
                for (int j = 0; j < to_mix; ++j) {
                    out[j] += mix_buffer_[static_cast<std::size_t>(j)] *
                              (from + step * static_cast<float>(j + 1));
                }
            }
            done += to_mix;
        }
        if (!any_active) {
            active_ = false;
        }
        return frames;
    }

    void set_parameter(std::string_view name, const json::Value& value) override {
        const std::optional<std::size_t> index = stream_index(name);
        if (index.has_value() && value.is_number()) {
            volumes_db_[*index] = static_cast<float>(value.as_double());
        }
    }

    [[nodiscard]] json::Value parameter(std::string_view name) const override {
        const std::optional<std::size_t> index = stream_index(name);
        return index.has_value() ? json::Value(static_cast<double>(volumes_db_[*index]))
                                 : json::Value();
    }

private:
    static constexpr int kMixBufferSize = 128; // Godot's MIX_BUFFER_SIZE

    // The stream a "stream_<index>_volume_db" parameter names.
    [[nodiscard]] std::optional<std::size_t> stream_index(std::string_view name) const {
        constexpr std::string_view kPrefix = "stream_";
        constexpr std::string_view kSuffix = "_volume_db";
        if (!name.starts_with(kPrefix) || !name.ends_with(kSuffix) ||
            name.size() <= kPrefix.size() + kSuffix.size()) {
            return std::nullopt;
        }
        const std::string_view digits =
            name.substr(kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size());
        std::size_t index = 0;
        for (const char c : digits) {
            if (c < '0' || c > '9' || digits.size() > 2) {
                return std::nullopt;
            }
            index = index * 10 + static_cast<std::size_t>(c - '0');
        }
        if (index >= volumes_db_.size()) {
            return std::nullopt;
        }
        return index;
    }

    std::vector<std::unique_ptr<Playback>> playbacks_;
    std::vector<float> volumes_db_;
    std::vector<float> gains_; // each stream's gain at the end of the last mix
    std::array<Frame, kMixBufferSize> mix_buffer_{};
    bool active_ = false;
};

class SynchronizedStream final : public Stream {
public:
    SynchronizedStream(std::vector<std::shared_ptr<const Stream>> streams,
                       const StreamFile& settings)
        : streams_(std::move(streams)) {
        for (std::size_t i = 0; i < streams_.size(); ++i) {
            volumes_db_.push_back(i < settings.streams.size() ? settings.streams[i].volume_db
                                                              : 0.0F);
        }
    }

    [[nodiscard]] std::unique_ptr<Playback> instantiate(int mix_rate,
                                                        std::uint32_t random) const override {
        return std::make_unique<SynchronizedPlayback>(streams_, volumes_db_, mix_rate,
                                                      random == 0 ? 0x9E3779B9U : random);
    }
    [[nodiscard]] double length() const override {
        double max_length = 0.0;
        for (const auto& stream : streams_) {
            max_length = std::max(max_length, stream->length());
        }
        return max_length;
    }
    [[nodiscard]] double bpm() const override {
        for (const auto& stream : streams_) {
            if (stream->bpm() != 0.0) {
                return stream->bpm();
            }
        }
        return 0.0;
    }
    [[nodiscard]] int beat_count() const override {
        int max_beats = 0;
        for (const auto& stream : streams_) {
            max_beats = std::max(max_beats, stream->beat_count());
        }
        return max_beats;
    }
    [[nodiscard]] int bar_beats() const override {
        for (const auto& stream : streams_) {
            if (stream->bar_beats() != 0) {
                return stream->bar_beats();
            }
        }
        return 0;
    }
    [[nodiscard]] bool has_loop() const override {
        return std::ranges::any_of(streams_, [](const auto& stream) { return stream->has_loop(); });
    }
    [[nodiscard]] bool is_monophonic() const override { return true; }

private:
    std::vector<std::shared_ptr<const Stream>> streams_;
    std::vector<float> volumes_db_;
};

// ---- Interactive music ----

// An interactive stream's clips and transitions, shared by its playbacks.
struct Interactive {
    std::vector<std::shared_ptr<const Stream>> streams; // one per clip
    std::vector<StreamFile::Clip> clips;
    std::vector<StreamFile::Transition> transitions;
    int initial_clip = 0;

    [[nodiscard]] StreamFile::Transition transition(int from, int to) const {
        const int index = transition_index(transitions, from, to);
        // With none, Godot's empty transition, with its defaults.
        return index >= 0 ? transitions[static_cast<std::size_t>(index)] : StreamFile::Transition{};
    }
};

// Godot's AudioStreamPlaybackInteractive: one clip plays, and a switch waits for its transition's
// time, fades as it says and may play a filler clip between.
class InteractivePlayback final : public Playback {
public:
    InteractivePlayback(std::shared_ptr<const Interactive> stream, int mix_rate,
                        std::uint32_t random)
        : stream_(std::move(stream)), mix_rate_(mix_rate), states_(stream_->streams.size()) {
        std::uint32_t state = random == 0 ? 0x9E3779B9U : random;
        for (std::size_t i = 0; i < states_.size(); ++i) {
            states_[i].stream = stream_->streams[i].get();
            states_[i].playback = stream_->streams[i]->instantiate(mix_rate, next_seed(state));
        }
    }

    void start(double /*from_seconds*/) override {
        if (active_) {
            stop();
        }
        const int current = stream_->initial_clip;
        if (current < 0 || current >= clip_count()) {
            return; // No playback possible.
        }
        active_ = true;
        queue(current, false);
    }

    void stop() override {
        if (!active_) {
            return;
        }
        active_ = false;
        for (State& state : states_) {
            state.playback->stop();
            state.reset_fade();
            state.active = false;
            state.auto_advance = -1;
            state.first_mix = true;
        }
        playback_current_ = -1;
        queued_ = -1;
    }

    [[nodiscard]] bool is_playing() const override { return active_; }

    // Godot's is always 0; the clip playing's position tells an agent more.
    [[nodiscard]] double position() const override {
        return playback_current_ >= 0 ? state(playback_current_).playback->position() : 0.0;
    }

    int mix(Frame* buffer, float /*rate_scale*/, int frames) override {
        // A clip asked for before start, or reached since, may be the one it is going to now.
        if (switch_request_ != -1) {
            if (switch_request_ != heading()) {
                queue(switch_request_, false);
            }
            switch_request_ = -1;
        }
        if (!active_) {
            return 0;
        }
        int done = 0;
        while (done < frames) {
            const int to_mix = std::min(frames - done, kBufferSize);
            mix_internal(to_mix);
            std::copy_n(mix_buffer_.begin(), to_mix, buffer + done);
            done += to_mix;
        }
        return frames;
    }

    // Godot's switch_to_clip_by_name. Switching to the clip it is playing or going to does
    // nothing, where Godot would start that clip over.
    void set_parameter(std::string_view name, const json::Value& value) override {
        if (name != "switch_to_clip" || !value.is_string()) {
            return;
        }
        const std::string& clip = value.as_string();
        if (clip.empty()) {
            switch_request_ = -1;
            return;
        }
        for (int i = 0; i < clip_count(); ++i) {
            if (stream_->clips[static_cast<std::size_t>(i)].name == clip) {
                switch_request_ = i == heading() ? -1 : i;
                return;
            }
        }
    }

    // The clip asked for, else the one it is going to, else the one playing: where Godot's
    // get_parameter skips the clip a transition waits for.
    [[nodiscard]] json::Value parameter(std::string_view name) const override {
        if (name != "switch_to_clip") {
            return {};
        }
        const int clip = switch_request_ != -1 ? switch_request_ : heading();
        return clip >= 0 ? json::Value(stream_->clips[static_cast<std::size_t>(clip)].name)
                         : json::Value("");
    }

    [[nodiscard]] std::string_view clip() const override {
        return playback_current_ >= 0
                   ? std::string_view(
                         stream_->clips[static_cast<std::size_t>(playback_current_)].name)
                   : std::string_view{};
    }

private:
    static constexpr int kBufferSize = 1024; // Godot's BUFFER_SIZE

    struct State {
        const Stream* stream = nullptr;
        std::unique_ptr<Playback> playback;
        bool active = false;
        double fade_wait = 0; // time to wait until the fade kicks in
        double fade_volume = 1.0;
        double fade_speed = 0; // fade speed, negative or positive
        int auto_advance = -1;
        bool first_mix = true;
        double previous_position = 0;

        void reset_fade() {
            fade_wait = 0;
            fade_volume = 1.0;
            fade_speed = 0;
        }
    };

    [[nodiscard]] int clip_count() const { return static_cast<int>(states_.size()); }
    State& state(int index) { return states_[static_cast<std::size_t>(index)]; }
    [[nodiscard]] const State& state(int index) const {
        return states_[static_cast<std::size_t>(index)];
    }
    [[nodiscard]] const StreamFile::Clip& clip_at(int index) const {
        return stream_->clips[static_cast<std::size_t>(index)];
    }
    [[nodiscard]] int heading() const { return queued_ != -1 ? queued_ : playback_current_; }

    // Godot's _queue.
    void queue(int to_clip, bool is_auto_advance) {
        if (to_clip < 0 || to_clip >= clip_count()) {
            return;
        }
        queued_ = to_clip;
        if (playback_current_ == -1) {
            // Nothing to do, start.
            State& start_state = state(to_clip);
            start_state.active = true;
            start_state.fade_wait = 0;
            start_state.fade_volume = 1.0;
            start_state.fade_speed = 0;
            start_state.first_mix = true;
            start_state.playback->start(0);
            playback_current_ = to_clip;
            const StreamFile::Clip& clip = clip_at(to_clip);
            if (clip.auto_advance == AutoAdvance::Enabled && clip.next_clip >= 0 &&
                clip.next_clip < clip_count() && clip.next_clip != to_clip) {
                start_state.auto_advance = clip.next_clip;
            }
            return;
        }

        for (int i = 0; i < clip_count(); ++i) {
            if (i == playback_current_ || i == to_clip) {
                continue;
            }
            State& waiting = state(i);
            if (waiting.active && waiting.fade_wait > 0) {
                // Waiting to kick in: a change of plans ends it.
                waiting.playback->stop();
                waiting.reset_fade();
                waiting.active = false;
            }
        }

        State& from_state = state(playback_current_);
        State& to_state = state(to_clip);
        StreamFile::Transition transition = stream_->transition(playback_current_, to_clip);

        if (transition.fade_mode == FadeMode::Automatic) {
            // Adjust automatic mode based on context.
            transition.fade_mode =
                transition.to_time == TransitionTo::Start ? FadeMode::Out : FadeMode::Cross;
        }
        if (is_auto_advance) {
            transition.from_time = TransitionFrom::End;
            if (transition.to_time == TransitionTo::SamePosition) {
                transition.to_time = TransitionTo::Start;
            }
        }

        // Prepare the fade out.
        const auto current_pos = static_cast<float>(from_state.playback->position());
        float src_fade_wait = 0;
        float dst_seek_to = 0;
        float fade_speed = 0;
        bool src_no_loop = false;
        // A fade of no beats is as quick as can be.
        const float fade_beats = std::max(transition.fade_beats, 0.001F);

        if (from_state.stream->bpm() != 0.0) {
            // The source has a BPM, so the transition keeps to its beats.
            const float beat_sec = 60 / static_cast<float>(from_state.stream->bpm());
            switch (transition.from_time) {
            case TransitionFrom::Immediate:
                src_fade_wait = 0;
                break;
            case TransitionFrom::NextBeat:
                src_fade_wait = beat_sec - std::fmod(current_pos, beat_sec);
                break;
            case TransitionFrom::NextBar:
                if (from_state.stream->bar_beats() > 0) {
                    const float bar_sec =
                        beat_sec * static_cast<float>(from_state.stream->bar_beats());
                    src_fade_wait = bar_sec - std::fmod(current_pos, bar_sec);
                } else {
                    // No beats per bar: avoid NaN, and play immediately.
                    src_fade_wait = 0;
                }
                break;
            case TransitionFrom::End: {
                const float end =
                    from_state.stream->beat_count() > 0
                        ? static_cast<float>(from_state.stream->beat_count()) * beat_sec
                        : static_cast<float>(from_state.stream->length());
                // A stream with no length goes at once.
                src_fade_wait = end == 0 ? 0 : end - current_pos;
                src_no_loop = !from_state.stream->has_loop();
                break;
            }
            }
            // The fade keeps to the beats too.
            fade_speed = 1.0F / (fade_beats * beat_sec);
        } else {
            // The source has no BPM, so just a simple transition.
            if (transition.from_time == TransitionFrom::End && from_state.stream->length() > 0) {
                src_fade_wait = static_cast<float>(from_state.stream->length()) - current_pos;
                src_no_loop = !from_state.stream->has_loop();
            } else {
                src_fade_wait = 0;
            }
            fade_speed = 1.0F / fade_beats;
        }

        if (transition.to_time == TransitionTo::PreviousPosition &&
            to_state.stream->length() > 0.0) {
            dst_seek_to = static_cast<float>(to_state.previous_position);
        } else if (transition.to_time == TransitionTo::SamePosition &&
                   transition.from_time != TransitionFrom::End && to_state.stream->length() > 0.0) {
            // About the same position as when the fade starts.
            dst_seek_to = current_pos + src_fade_wait;
            if (static_cast<double>(dst_seek_to) > beat_length(*to_state.stream)) {
                dst_seek_to = 0; // Past the end: back to the beginning.
            }
        } else {
            dst_seek_to = 0.0;
        }

        if (transition.fade_mode == FadeMode::Disabled || transition.fade_mode == FadeMode::In) {
            if (src_no_loop) {
                // With no fade out, a source that doesn't loop plays on to its end.
                from_state.fade_wait = 0;
                from_state.fade_speed = 0;
            } else {
                // Otherwise a very quick fade avoids clicks.
                from_state.fade_wait = src_fade_wait;
                from_state.fade_speed = 1.0 / -0.001;
            }
        } else {
            // A regular fade. The volume stays, since it may have been fading in.
            from_state.fade_wait = src_fade_wait;
            from_state.fade_speed = -fade_speed;
        }

        to_state.playback->start(dst_seek_to);
        to_state.active = true;
        to_state.fade_volume = 0.0;
        to_state.first_mix = true;

        int auto_advance_to = -1;
        const StreamFile::Clip& to = clip_at(to_clip);
        const bool use_filler = transition.filler_clip >= 0;
        if (to.auto_advance == AutoAdvance::Enabled) {
            const int next_clip = to.next_clip;
            if (next_clip >= 0 && next_clip < clip_count() && next_clip != to_clip &&
                (!use_filler || next_clip != transition.filler_clip)) {
                auto_advance_to = next_clip;
            }
        }
        // Godot records the hold after this check, so a hold_previous transition straight into a
        // return_to_hold clip went back to an older hold, or nowhere; recording it first lets a
        // one-clip sting return to the clip it interrupted.
        if (transition.hold_previous) {
            return_memory_ = playback_current_;
        }
        if (return_memory_ != -1 && to.auto_advance == AutoAdvance::ReturnToHold) {
            auto_advance_to = return_memory_;
            return_memory_ = -1;
        }

        const bool fade_in =
            transition.fade_mode != FadeMode::Disabled && transition.fade_mode != FadeMode::Out;
        if (use_filler && transition.filler_clip < clip_count() &&
            playback_current_ != transition.filler_clip && to_clip != transition.filler_clip) {
            State& filler_state = state(transition.filler_clip);
            filler_state.playback->start(0);
            filler_state.active = true;
            // A filler doesn't fade: bake a fade into its sound if it needs one.
            filler_state.fade_volume = 1.0;
            filler_state.fade_speed = 0.0;
            filler_state.fade_wait = src_fade_wait;
            filler_state.first_mix = true;
            const auto filler_end = static_cast<float>(beat_length(*filler_state.stream));
            if (fade_in) {
                to_state.fade_volume = 0.0;
                to_state.fade_speed = fade_speed;
            } else {
                // The filler is the transition, so the next clip starts at full volume.
                to_state.fade_volume = 1.0;
                to_state.fade_speed = 0.0;
            }
            to_state.fade_wait = src_fade_wait + filler_end;
        } else {
            to_state.fade_wait = src_fade_wait;
            if (fade_in) {
                to_state.fade_volume = 0.0;
                to_state.fade_speed = fade_speed;
            } else {
                to_state.fade_volume = 1.0;
                to_state.fade_speed = 0.0;
            }
            to_state.auto_advance = auto_advance_to;
        }
    }

    void mix_internal(int frames) {
        std::fill_n(mix_buffer_.begin(), frames, Frame{});
        bool any_active = false;
        for (int i = 0; i < clip_count(); ++i) {
            if (!state(i).active) {
                continue;
            }
            mix_internal_state(i, frames);
            any_active = state(i).active || any_active;
        }
        if (!any_active) {
            active_ = false;
            playback_current_ = -1;
            queued_ = -1;
        }
    }

    void mix_internal_state(int index, int frames) {
        State& each = state(index);
        const double frame_inc = 1.0 / mix_rate_;
        int from_frame = 0;
        int queue_next = -1;

        if (each.first_mix) {
            // Not mixing yet: wait.
            const double mix_time = frames * frame_inc;
            if (each.fade_wait < mix_time) {
                // Time to start!
                from_frame = static_cast<int>(each.fade_wait * mix_rate_);
                each.fade_wait = 0;
                if (each.fade_speed == 0.0) {
                    queue_next = each.auto_advance;
                }
                playback_current_ = index;
                if (queued_ == index) {
                    queued_ = -1;
                }
                each.first_mix = false;
            } else {
                // The new stream waits to fade in.
                each.fade_wait -= mix_time;
                return;
            }
        }

        each.previous_position = each.playback->position();
        std::fill_n(temp_buffer_.begin(), from_frame, Frame{});
        static_cast<void>(
            each.playback->mix(temp_buffer_.data() + from_frame, 1.0F, frames - from_frame));

        double frame_fade_inc = each.fade_speed * frame_inc;
        for (int i = from_frame; i < frames; ++i) {
            if (each.fade_wait != 0.0) {
                // The fade out of the stream playing waits.
                each.fade_wait -= frame_inc;
                if (each.fade_wait < 0.0) {
                    each.fade_wait = 0.0;
                }
            } else if (frame_fade_inc > 0) {
                each.fade_volume += frame_fade_inc;
                if (each.fade_volume >= 1.0) {
                    each.fade_speed = 0.0;
                    frame_fade_inc = 0.0;
                    each.fade_volume = 1.0;
                    queue_next = each.auto_advance;
                }
            } else if (frame_fade_inc < 0.0) {
                each.fade_volume += frame_fade_inc;
                if (each.fade_volume <= 0.0) {
                    each.fade_speed = 0.0;
                    each.fade_volume = 0.0;
                    each.playback->stop(); // Faded out: no point mixing on.
                    break;
                }
            }
            mix_buffer_[static_cast<std::size_t>(i)] +=
                temp_buffer_[static_cast<std::size_t>(i)] * static_cast<float>(each.fade_volume);
            each.previous_position += frame_inc;
        }

        if (!each.playback->is_playing()) {
            // It reached its end or faded out.
            each.active = false;
        }
        if (queue_next != -1) {
            queue(queue_next, true);
        }
    }

    std::shared_ptr<const Interactive> stream_;
    int mix_rate_;
    std::vector<State> states_;
    std::array<Frame, kBufferSize> mix_buffer_{};
    std::array<Frame, kBufferSize> temp_buffer_{};
    int playback_current_ = -1;
    int queued_ = -1; // the clip a transition is going to
    bool active_ = false;
    int return_memory_ = -1;
    int switch_request_ = -1;
};

class InteractiveStream final : public Stream {
public:
    explicit InteractiveStream(std::shared_ptr<const Interactive> data) : data_(std::move(data)) {}

    [[nodiscard]] std::unique_ptr<Playback> instantiate(int mix_rate,
                                                        std::uint32_t random) const override {
        return std::make_unique<InteractivePlayback>(data_, mix_rate, random);
    }
    // As Godot's: it plays until it is stopped, or a clip that doesn't go on ends.
    [[nodiscard]] double length() const override { return 0.0; }
    [[nodiscard]] bool is_monophonic() const override { return true; }

private:
    std::shared_ptr<const Interactive> data_;
};

} // namespace

int transition_index(std::span<const StreamFile::Transition> transitions, int from, int to) {
    for (const auto& [want_from, want_to] :
         std::array<std::pair<int, int>, 4>{{{from, to}, {from, -1}, {-1, to}, {-1, -1}}}) {
        for (std::size_t i = 0; i < transitions.size(); ++i) {
            if (transitions[i].from_clip == want_from && transitions[i].to_clip == want_to) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

std::shared_ptr<const Stream> playlist_stream(std::vector<std::shared_ptr<const Stream>> streams,
                                              const StreamFile& settings) {
    return std::make_shared<PlaylistStream>(std::move(streams), settings);
}

std::shared_ptr<const Stream>
synchronized_stream(std::vector<std::shared_ptr<const Stream>> streams,
                    const StreamFile& settings) {
    return std::make_shared<SynchronizedStream>(std::move(streams), settings);
}

std::shared_ptr<const Stream> interactive_stream(std::vector<std::shared_ptr<const Stream>> clips,
                                                 const StreamFile& settings) {
    auto data = std::make_shared<Interactive>();
    data->streams = std::move(clips);
    data->clips = settings.clips;
    data->clips.resize(data->streams.size());
    data->transitions = settings.transitions;
    data->initial_clip = settings.initial_clip;
    return std::make_shared<InteractiveStream>(std::move(data));
}

} // namespace slash::audio
