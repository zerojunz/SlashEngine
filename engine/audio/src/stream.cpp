#include "slash/audio/audio.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <miniaudio.h>
#include <utility>

namespace slash::audio {

namespace {

// A value from 0 to 1 from a 32-bit seed, and the next seed (a xorshift step), so a randomizer
// draws the same numbers from the same seed on every platform.
float next_random(std::uint32_t& state) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return static_cast<float>(state >> 8U) / 16777216.0F;
}

// Godot's AudioStreamPlaybackResampled: a stream mixes at its own rate into a small buffer, which
// cubic interpolation reads at the server's rate times the pitch scale.
class Resampled : public Playback {
public:
    explicit Resampled(int mix_rate) : mix_rate_(mix_rate) {}

    int mix(Frame* buffer, float rate_scale, int frames) final {
        const auto target_rate = static_cast<float>(mix_rate_);
        const auto mix_increment = static_cast<std::uint64_t>(
            ((stream_rate() * rate_scale) / static_cast<double>(target_rate)) *
            static_cast<double>(kFpLen));
        int mixed_frames_total = -1;
        int i = 0;
        for (; i < frames; ++i) {
            const auto idx = static_cast<std::uint32_t>(kCubicHistory + (mix_offset_ >> kFpBits));
            // Standard cubic interpolation.
            const float mu = static_cast<float>(mix_offset_ & kFpMask) / static_cast<float>(kFpLen);
            const Frame y0 = internal_[idx - 3];
            const Frame y1 = internal_[idx - 2];
            const Frame y2 = internal_[idx - 1];
            const Frame y3 = internal_[idx - 0];
            if (static_cast<int>(idx) >= internal_end_ && internal_end_ >= 0 &&
                mixed_frames_total == -1) {
                // The internal buffer ends somewhere in this range.
                mixed_frames_total = i;
            }
            const float mu2 = mu * mu;
            const float h11 = mu2 * (mu - 1);
            const float z = mu2 - h11;
            const float h01 = z - h11;
            const float h10 = mu - z;
            buffer[i].left = y1.left + (y2.left - y1.left) * h01 +
                             ((y2.left - y0.left) * h10 + (y3.left - y1.left) * h11) * 0.5F;
            buffer[i].right = y1.right + (y2.right - y1.right) * h01 +
                              ((y2.right - y0.right) * h10 + (y3.right - y1.right) * h11) * 0.5F;
            mix_offset_ += mix_increment;
            while ((mix_offset_ >> kFpBits) >= kInternalLen) {
                for (int k = 0; k < kCubicHistory; ++k) {
                    internal_[static_cast<std::size_t>(k)] =
                        internal_[static_cast<std::size_t>(k) + kInternalLen];
                }
                const int mixed = mix_internal(internal_.data() + kCubicHistory, kInternalLen);
                // internal_[mixed] is the first frame of silence, if the stream ended.
                internal_end_ = mixed != kInternalLen ? mixed : -1;
                mix_offset_ -= static_cast<std::uint64_t>(kInternalLen) << kFpBits;
            }
        }
        if (mixed_frames_total == -1 && i == frames) {
            mixed_frames_total = frames;
        }
        return mixed_frames_total;
    }

protected:
    void begin_resample() {
        // Clear the cubic interpolation history, then fill the buffer.
        for (int k = 0; k < kCubicHistory; ++k) {
            internal_[static_cast<std::size_t>(k)] = {};
        }
        // As Godot's, this first fill doesn't mark where the stream ends; the next one does.
        static_cast<void>(mix_internal(internal_.data() + kCubicHistory, kInternalLen));
        mix_offset_ = 0;
    }

    // Writes frames at stream_rate(); returns how many came from the stream, the rest silent.
    virtual int mix_internal(Frame* buffer, int frames) = 0;
    [[nodiscard]] virtual float stream_rate() const = 0;

private:
    static constexpr int kFpBits = 16; // fixed point used for resampling
    static constexpr std::uint64_t kFpLen = 1ULL << kFpBits;
    static constexpr std::uint64_t kFpMask = kFpLen - 1;
    static constexpr int kInternalLen = 128; // 3 ms of positional jitter at 44100 Hz
    static constexpr int kCubicHistory = 4;

    std::array<Frame, kInternalLen + kCubicHistory> internal_{};
    int internal_end_ = -1;
    std::uint64_t mix_offset_ = 0;
    int mix_rate_;
};

// Godot's AudioStreamPlaybackWAV, over samples already decoded to floats.
class PcmPlayback final : public Resampled {
public:
    PcmPlayback(std::shared_ptr<const Sound> sound, const StreamFile& settings, int mix_rate)
        : Resampled(mix_rate), sound_(std::move(sound)), loop_mode_(settings.loop_mode),
          loop_begin_(std::clamp<std::int64_t>(settings.loop_begin, 0, sound_->frames)),
          loop_end_(settings.loop_end < 0 ? sound_->frames
                                          : std::min(settings.loop_end, sound_->frames)) {
        if (loop_end_ <= loop_begin_) {
            loop_mode_ = LoopMode::Disabled;
        }
    }

    void start(double from_seconds) override {
        seek(from_seconds);
        sign_ = 1;
        active_ = true;
        begin_resample();
    }
    void stop() override { active_ = false; }
    [[nodiscard]] bool is_playing() const override { return active_; }
    [[nodiscard]] double position() const override {
        return static_cast<double>(offset_) / sound_->mix_rate;
    }

private:
    void seek(double seconds) {
        const double max = sound_->length();
        if (seconds < 0) {
            seconds = 0;
        } else if (seconds >= max) {
            seconds = max - 0.001;
        }
        offset_ = static_cast<std::int64_t>(seconds * sound_->mix_rate);
    }

    // Godot's decode_samples: reads frames from offset_, stepping by increment.
    void decode(Frame* dst, std::int8_t increment, std::int32_t amount) {
        const float* data = sound_->samples.data();
        const bool stereo = sound_->channels == 2;
        while (amount > 0) {
            --amount;
            // Godot pads its data, so the loop end may read one frame past it.
            if (offset_ >= 0 && offset_ < sound_->frames) {
                const std::int64_t pos = stereo ? offset_ * 2 : offset_;
                dst->left = data[pos];
                dst->right = stereo ? data[pos + 1] : dst->left;
            } else {
                *dst = {};
            }
            ++dst;
            offset_ += increment;
        }
    }

    int mix_internal(Frame* buffer, int frames) override {
        if (sound_->samples.empty() || !active_) {
            std::fill(buffer, buffer + frames, Frame{});
            return 0;
        }
        const std::int64_t len = sound_->frames;
        const std::int64_t loop_begin = loop_begin_;
        const std::int64_t loop_end = loop_end_;
        const bool looping = loop_mode_ != LoopMode::Disabled;
        const std::int64_t begin_limit = looping ? loop_begin : 0;
        const std::int64_t end_limit = looping ? loop_end : len - 1;
        std::int32_t todo = frames;
        if (loop_mode_ == LoopMode::Backward) {
            sign_ = -1;
        }
        auto increment = static_cast<std::int8_t>(sign_ < 0 ? -1 : 1);
        Frame* dst = buffer;
        while (todo > 0) {
            // Loop checking.
            if (increment < 0) {
                // Going backwards.
                if (looping && offset_ < loop_begin) {
                    if (loop_mode_ == LoopMode::PingPong) {
                        offset_ = loop_begin + (loop_begin - offset_);
                        increment = static_cast<std::int8_t>(-increment);
                        sign_ *= -1;
                    } else {
                        offset_ = loop_end - (loop_begin - offset_);
                    }
                } else if (offset_ < 0) {
                    active_ = false;
                    break;
                }
            } else {
                // Going forward.
                if (looping && offset_ >= loop_end) {
                    if (loop_mode_ == LoopMode::PingPong) {
                        offset_ = loop_end - (offset_ - loop_end);
                        increment = static_cast<std::int8_t>(-increment);
                        sign_ *= -1;
                    } else {
                        offset_ = loop_begin + (offset_ - loop_end);
                    }
                } else if (offset_ >= len) {
                    active_ = false;
                    break;
                }
            }
            // The next limit: a loop point or the sound's start or end.
            const std::int64_t limit = increment < 0 ? begin_limit : end_limit;
            const std::int64_t aux = (limit - offset_) / increment + 1;
            const auto target = static_cast<std::int32_t>(std::min<std::int64_t>(aux, todo));
            if (target <= 0) {
                active_ = false;
                break;
            }
            todo -= target;
            decode(dst, increment, target);
            dst += target;
        }
        if (todo > 0) {
            std::fill(buffer + (frames - todo), buffer + frames, Frame{});
            return frames - todo;
        }
        return frames;
    }

    [[nodiscard]] float stream_rate() const override {
        return static_cast<float>(sound_->mix_rate);
    }

    std::shared_ptr<const Sound> sound_;
    LoopMode loop_mode_;
    std::int64_t loop_begin_;
    std::int64_t loop_end_;
    std::int64_t offset_ = 0;
    int sign_ = 1;
    bool active_ = false;
};

// Godot's AudioStreamPlaybackOggVorbis and AudioStreamPlaybackMP3: a compressed sound decoded as
// it plays, looping back to loop_offset at its end or after beat_count beats.
class DecoderPlayback final : public Resampled {
public:
    DecoderPlayback(std::shared_ptr<const Sound> sound, StreamFile settings, int mix_rate)
        : Resampled(mix_rate), sound_(std::move(sound)), settings_(std::move(settings)) {
        ma_decoder_config config =
            ma_decoder_config_init(ma_format_f32, static_cast<ma_uint32>(sound_->channels), 0);
        config.encodingFormat = sound_->format == Sound::Format::Vorbis ? ma_encoding_format_vorbis
                                                                        : ma_encoding_format_mp3;
        ready_ = ma_decoder_init_memory(sound_->data.data(), sound_->data.size(), &config,
                                        &decoder_) == MA_SUCCESS;
    }
    ~DecoderPlayback() override {
        if (ready_) {
            ma_decoder_uninit(&decoder_);
        }
    }
    DecoderPlayback(const DecoderPlayback&) = delete;
    DecoderPlayback& operator=(const DecoderPlayback&) = delete;
    DecoderPlayback(DecoderPlayback&&) = delete;
    DecoderPlayback& operator=(DecoderPlayback&&) = delete;

    void start(double from_seconds) override {
        active_ = ready_;
        seek(from_seconds);
        loops_ = 0;
        begin_resample();
    }
    void stop() override { active_ = false; }
    [[nodiscard]] bool is_playing() const override { return active_; }
    [[nodiscard]] double position() const override {
        return static_cast<double>(frames_mixed_) / sound_->mix_rate;
    }
    [[nodiscard]] int loop_count() const override { return loops_; }

private:
    void seek(double seconds) {
        if (!active_) {
            return;
        }
        if (seconds >= sound_->length()) {
            seconds = 0;
        }
        frames_mixed_ = static_cast<std::uint32_t>(sound_->mix_rate * seconds);
        ma_decoder_seek_to_pcm_frame(&decoder_, frames_mixed_);
    }

    // Reads up to count frames as stereo; returns how many there were.
    int read(Frame* out, int count) {
        std::array<float, static_cast<std::size_t>(kChunk) * 2> chunk{};
        int done = 0;
        while (done < count) {
            const int want = std::min(count - done, kChunk);
            ma_uint64 got = 0;
            ma_decoder_read_pcm_frames(&decoder_, chunk.data(), static_cast<ma_uint64>(want), &got);
            const bool stereo = sound_->channels == 2;
            for (ma_uint64 k = 0; k < got; ++k) {
                Frame& frame = out[done + static_cast<int>(k)];
                frame.left = chunk[stereo ? k * 2 : k];
                frame.right = chunk[stereo ? k * 2 + 1 : k];
            }
            done += static_cast<int>(got);
            if (static_cast<int>(got) < want) {
                break;
            }
        }
        return done;
    }

    int mix_internal(Frame* buffer, int frames) override {
        if (!active_) {
            return 0;
        }
        int todo = frames;
        int frames_mixed_this_step = frames;
        int beat_length_frames = -1;
        const bool use_loop = settings_.loop;
        const bool beat_loop = use_loop && settings_.bpm > 0 && settings_.beat_count > 0;
        if (beat_loop) {
            beat_length_frames =
                static_cast<int>(settings_.beat_count * sound_->mix_rate * 60 / settings_.bpm);
        }
        while (todo > 0 && active_) {
            // Up to the end of the beats, then one frame at a time as Godot reads.
            int want = todo;
            if (beat_loop) {
                want = std::clamp(beat_length_frames - static_cast<int>(frames_mixed_), 1, todo);
            }
            Frame* out = buffer + (frames - todo);
            const int got = read(out, want);
            for (int k = 0; k < got; ++k) {
                if (loop_fade_remaining_ < kFadeSize) {
                    const float weight = static_cast<float>(kFadeSize - loop_fade_remaining_) /
                                         static_cast<float>(kFadeSize);
                    const Frame& faded = loop_fade_[static_cast<std::size_t>(loop_fade_remaining_)];
                    out[k].left += faded.left * weight;
                    out[k].right += faded.right * weight;
                    ++loop_fade_remaining_;
                }
            }
            todo -= got;
            frames_mixed_ += static_cast<std::uint32_t>(got);
            if (got > 0) {
                if (beat_loop && static_cast<int>(frames_mixed_) >= beat_length_frames) {
                    // What follows the loop point fades out over the start of the loop.
                    const int faded = read(loop_fade_.data(), kFadeSize);
                    std::fill(loop_fade_.begin() + faded, loop_fade_.end(), Frame{});
                    loop_fade_remaining_ = 0;
                    seek(settings_.loop_offset);
                    ++loops_;
                }
                continue;
            }
            // The end of the sound.
            if (use_loop) {
                seek(settings_.loop_offset);
                ++loops_;
            } else {
                frames_mixed_this_step = frames - todo;
                std::fill(buffer + (frames - todo), buffer + frames, Frame{});
                active_ = false;
                todo = 0;
            }
        }
        return frames_mixed_this_step;
    }

    [[nodiscard]] float stream_rate() const override {
        return static_cast<float>(sound_->mix_rate);
    }

    static constexpr int kChunk = 512;
    static constexpr int kFadeSize = 256;

    std::shared_ptr<const Sound> sound_;
    StreamFile settings_;
    ma_decoder decoder_{};
    std::array<Frame, kFadeSize> loop_fade_{};
    int loop_fade_remaining_ = kFadeSize;
    std::uint32_t frames_mixed_ = 0;
    int loops_ = 0;
    bool ready_ = false;
    bool active_ = false;
};

// Godot's AudioStreamPlaybackRandomizer: the stream it picked, at a random pitch and volume.
class RandomizerPlayback final : public Playback {
public:
    RandomizerPlayback(std::unique_ptr<Playback> playing, float pitch_scale, float volume_scale)
        : playing_(std::move(playing)), pitch_scale_(pitch_scale), volume_scale_(volume_scale) {}

    void start(double from_seconds) override {
        if (playing_) {
            playing_->start(from_seconds);
        }
    }
    void stop() override {
        if (playing_) {
            playing_->stop();
        }
    }
    [[nodiscard]] bool is_playing() const override { return playing_ && playing_->is_playing(); }
    [[nodiscard]] double position() const override { return playing_ ? playing_->position() : 0.0; }
    [[nodiscard]] int loop_count() const override { return playing_ ? playing_->loop_count() : 0; }
    int mix(Frame* buffer, float rate_scale, int frames) override {
        if (!playing_) {
            std::fill(buffer, buffer + frames, Frame{});
            return frames;
        }
        const int mixed = playing_->mix(buffer, rate_scale * pitch_scale_, frames);
        for (int i = 0; i < mixed; ++i) {
            buffer[i].left *= volume_scale_;
            buffer[i].right *= volume_scale_;
        }
        return mixed;
    }

private:
    std::unique_ptr<Playback> playing_;
    float pitch_scale_;
    float volume_scale_;
};

class SoundStream final : public Stream {
public:
    SoundStream(std::shared_ptr<const Sound> sound, StreamFile settings)
        : sound_(std::move(sound)), settings_(std::move(settings)) {}

    [[nodiscard]] std::unique_ptr<Playback> instantiate(int mix_rate,
                                                        std::uint32_t /*random*/) const override {
        if (sound_->format == Sound::Format::Wav) {
            return std::make_unique<PcmPlayback>(sound_, settings_, mix_rate);
        }
        return std::make_unique<DecoderPlayback>(sound_, settings_, mix_rate);
    }
    [[nodiscard]] double length() const override { return sound_->length(); }
    [[nodiscard]] double bpm() const override { return settings_.bpm; }
    [[nodiscard]] int beat_count() const override { return settings_.beat_count; }
    [[nodiscard]] int bar_beats() const override { return settings_.bar_beats; }
    [[nodiscard]] bool has_loop() const override {
        return sound_->format == Sound::Format::Wav ? settings_.loop_mode != LoopMode::Disabled
                                                    : settings_.loop;
    }

private:
    std::shared_ptr<const Sound> sound_;
    StreamFile settings_;
};

// Godot's AudioStreamRandomizer. The stream it played last is kept here, as Godot keeps it on
// the resource, so every player of one randomizer avoids repeats together.
class RandomizerStream final : public Stream {
public:
    RandomizerStream(std::vector<std::pair<std::shared_ptr<const Stream>, float>> pool,
                     const StreamFile& settings)
        : pool_(std::move(pool)), mode_(settings.playback_mode),
          random_pitch_scale_(std::max(1.0F, settings.random_pitch)),
          random_volume_offset_db_(std::max(0.0F, settings.random_volume_offset_db)) {}

    [[nodiscard]] std::unique_ptr<Playback> instantiate(int mix_rate,
                                                        std::uint32_t random) const override {
        std::uint32_t state = random == 0 ? 0x9E3779B9U : random;
        const Stream* picked = nullptr;
        switch (mode_) {
        case PlaybackMode::Random:
            picked = pick_random(state, nullptr);
            break;
        case PlaybackMode::RandomNoRepeats:
            picked = pick_random(state, last_);
            if (picked == nullptr) {
                // There is only one sound to choose from.
                picked = pick_random(state, nullptr);
            }
            break;
        case PlaybackMode::Sequential:
            picked = pick_next();
            break;
        }
        last_ = picked != nullptr ? picked : last_;
        // Pitch is multiplicative, so it is picked on a log scale, as Godot does.
        const float range_from = std::log(1.0F / random_pitch_scale_);
        const float range_to = std::log(random_pitch_scale_);
        const float pitch_scale =
            std::exp(range_from + next_random(state) * (range_to - range_from));
        const float offset_db =
            -random_volume_offset_db_ + next_random(state) * (2.0F * random_volume_offset_db_);
        return std::make_unique<RandomizerPlayback>(
            picked != nullptr ? picked->instantiate(mix_rate, state) : nullptr, pitch_scale,
            db_to_linear(offset_db));
    }

    [[nodiscard]] double length() const override {
        return last_ != nullptr ? last_->length() : 0.0;
    }

private:
    const Stream* pick_random(std::uint32_t& state, const Stream* skip) const {
        double total_weight = 0;
        for (const auto& [stream, weight] : pool_) {
            if (stream.get() != skip && weight > 0) {
                total_weight += weight;
            }
        }
        if (total_weight <= 0) {
            return nullptr;
        }
        const double chosen = next_random(state) * total_weight;
        double cumulative = 0;
        const Stream* last_valid = nullptr;
        for (const auto& [stream, weight] : pool_) {
            if (stream.get() == skip || weight <= 0) {
                continue;
            }
            cumulative += weight;
            last_valid = stream.get();
            if (cumulative > chosen) {
                return stream.get();
            }
        }
        // A floating point error: take the last one.
        return last_valid;
    }

    const Stream* pick_next() const {
        if (pool_.empty()) {
            return nullptr;
        }
        bool found_last = false;
        for (const auto& [stream, weight] : pool_) {
            if (found_last) {
                return stream.get();
            }
            found_last = stream.get() == last_;
        }
        return pool_.front().first.get(); // wrap around
    }

    std::vector<std::pair<std::shared_ptr<const Stream>, float>> pool_;
    PlaybackMode mode_;
    float random_pitch_scale_;
    float random_volume_offset_db_;
    mutable const Stream* last_ = nullptr;
};

constexpr int kMaxNesting = 8;

Result<std::shared_ptr<const Stream>> load_stream_at(asset::Assets& assets, std::string_view path,
                                                     int depth, LoadedStream& loaded) {
    if (depth > kMaxNesting) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + std::string(path) + "\" nests .audio.json files more than " +
                              std::to_string(kMaxNesting) + " deep; does one name itself?");
    }
    const auto load_sound =
        [&](std::string_view sound_path) -> Result<std::shared_ptr<const Sound>> {
        auto sound = assets.load<Sound>(sound_path);
        if (!sound) {
            return sound.error();
        }
        loaded.sounds.push_back(sound.value());
        return sound.value().share();
    };
    if (is_sound_path(path)) {
        auto sound = load_sound(path);
        if (!sound) {
            return sound.error();
        }
        return sound_stream(std::move(sound).value());
    }
    auto file = assets.load<StreamFile>(path);
    if (!file) {
        return file.error();
    }
    loaded.files.push_back(file.value());
    const std::shared_ptr<const StreamFile> settings = file.value().share();
    if (!settings->source.empty()) {
        auto sound = load_sound(settings->source);
        if (!sound) {
            return sound.error();
        }
        return sound_stream(std::move(sound).value(), *settings);
    }
    std::vector<std::string_view> paths;
    if (settings->kind == StreamFile::Kind::Interactive) {
        for (const StreamFile::Clip& clip : settings->clips) {
            paths.emplace_back(clip.stream);
        }
    } else {
        for (const StreamFile::Entry& entry : settings->streams) {
            paths.emplace_back(entry.stream);
        }
    }
    std::vector<std::shared_ptr<const Stream>> streams;
    for (const std::string_view each : paths) {
        auto stream = load_stream_at(assets, each, depth + 1, loaded);
        if (!stream) {
            return stream.error();
        }
        streams.push_back(std::move(stream).value());
    }
    switch (settings->kind) {
    case StreamFile::Kind::Playlist:
        return playlist_stream(std::move(streams), *settings);
    case StreamFile::Kind::Synchronized:
        return synchronized_stream(std::move(streams), *settings);
    case StreamFile::Kind::Interactive:
        return interactive_stream(std::move(streams), *settings);
    case StreamFile::Kind::Sound:
    case StreamFile::Kind::Randomizer:
        break;
    }
    std::vector<std::pair<std::shared_ptr<const Stream>, float>> pool;
    for (std::size_t i = 0; i < streams.size(); ++i) {
        pool.emplace_back(std::move(streams[i]), settings->streams[i].weight);
    }
    return randomizer_stream(std::move(pool), *settings);
}

} // namespace

std::shared_ptr<const Stream> sound_stream(std::shared_ptr<const Sound> sound,
                                           const StreamFile& settings) {
    return std::make_shared<SoundStream>(std::move(sound), settings);
}

std::shared_ptr<const Stream>
randomizer_stream(std::vector<std::pair<std::shared_ptr<const Stream>, float>> streams,
                  const StreamFile& settings) {
    return std::make_shared<RandomizerStream>(std::move(streams), settings);
}

bool LoadedStream::stale() const {
    std::size_t i = 0;
    for (const auto& sound : sounds) {
        if (i >= versions.size() || sound.version() != versions[i]) {
            return true;
        }
        ++i;
    }
    for (const auto& file : files) {
        if (i >= versions.size() || file.version() != versions[i]) {
            return true;
        }
        ++i;
    }
    return false;
}

Result<LoadedStream> load_stream(asset::Assets& assets, std::string_view path) {
    LoadedStream loaded;
    auto stream = load_stream_at(assets, path, 0, loaded);
    if (!stream) {
        return stream.error();
    }
    loaded.stream = std::move(stream).value();
    for (const auto& sound : loaded.sounds) {
        loaded.versions.push_back(sound.version());
    }
    for (const auto& file : loaded.files) {
        loaded.versions.push_back(file.version());
    }
    return loaded;
}

} // namespace slash::audio
