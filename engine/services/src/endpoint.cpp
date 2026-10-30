#include "slash/services/endpoint.hpp"

#include "slash/core/log.hpp"
#include "slash/platform/file.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <random>

namespace slash::services {
namespace {

constexpr std::string_view kName = "endpoint";
constexpr std::int64_t kMinute = 60'000;
constexpr std::int64_t kMaxRetryWait = 10 * kMinute;
constexpr int kStatusOk = 200;
constexpr int kStatusMultipleChoices = 300;

std::int64_t system_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string random_id() {
    static std::random_device device;
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::string id;
    for (int i = 0; i < 32; ++i) {
        id += kDigits[device() & 0xFU];
    }
    return id;
}

// Milliseconds since 1970 as ISO 8601 in UTC, "2026-10-30T04:50:00.123Z": Howard Hinnant's
// civil_from_days.
std::string iso8601(std::int64_t ms) {
    std::int64_t days = ms >= 0 ? ms / 86'400'000 : ((ms + 1) / 86'400'000) - 1;
    const std::int64_t in_day = ms - (days * 86'400'000);
    days += 719'468;
    const std::int64_t era = (days >= 0 ? days : days - 146'096) / 146'097;
    const std::int64_t day_of_era = days - (era * 146'097);
    const std::int64_t year_of_era =
        (day_of_era - (day_of_era / 1460) + (day_of_era / 36'524) - (day_of_era / 146'096)) / 365;
    const std::int64_t day_of_year =
        day_of_era - ((365 * year_of_era) + (year_of_era / 4) - (year_of_era / 100));
    const std::int64_t month_index = ((5 * day_of_year) + 2) / 153;
    const std::int64_t day = day_of_year - (((153 * month_index) + 2) / 5) + 1;
    const std::int64_t month = month_index < 10 ? month_index + 3 : month_index - 9;
    const std::int64_t year = year_of_era + (era * 400) + (month <= 2 ? 1 : 0);
    // Room for any 64-bit fields, so GCC's format-truncation check holds at -O2.
    std::array<char, 72> text{};
    std::snprintf(
        text.data(), text.size(), "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld.%03lldZ",
        static_cast<long long>(year), static_cast<long long>(month), static_cast<long long>(day),
        static_cast<long long>(in_day / 3'600'000), static_cast<long long>((in_day / 60'000) % 60),
        static_cast<long long>((in_day / 1000) % 60), static_cast<long long>(in_day % 1000));
    return text.data();
}

bool retryable(int status) {
    return status == 0 || status == 408 || status == 429 || status >= 500;
}

// What a sent batch's answer fills in, kept alive by the poster's callback too, so the backend
// can go first.
struct Answer {
    std::mutex mutex;
    std::optional<int> status;
};

class EndpointAnalytics final : public AnalyticsBackend {
public:
    EndpointAnalytics(AnalyticsEndpoint endpoint, EndpointOptions options)
        : endpoint_(std::move(endpoint)), options_(std::move(options)) {
        if (!options_.now_ms) {
            options_.now_ms = system_now_ms;
        }
        if (!options_.new_id) {
            options_.new_id = random_id;
        }
    }
    EndpointAnalytics(const EndpointAnalytics&) = delete;
    EndpointAnalytics& operator=(const EndpointAnalytics&) = delete;
    EndpointAnalytics(EndpointAnalytics&&) = delete;
    EndpointAnalytics& operator=(EndpointAnalytics&&) = delete;
    ~EndpointAnalytics() override { save(); }

    [[nodiscard]] std::string_view name() const override { return kName; }

    void start(const std::shared_ptr<Reports>& /*reports*/) override {
        load();
        if (device_.empty()) {
            device_ = options_.new_id();
            first_open_ = true;
            save();
        }
    }

    void set_consent(const ConsentState& consent) override {
        if (consent.analytics == Consent::Denied && !queue_.empty()) {
            // What waits was logged with consent the player took back.
            const std::size_t kept = in_flight_;
            dropped_ += queue_.size() - kept;
            queue_.resize(kept);
            save();
        }
    }

    void log_event(std::string_view name, const json::Value& params) override {
        const std::int64_t now = options_.now_ms();
        if (session_.empty() ||
            now - last_event_ms_ > static_cast<std::int64_t>(endpoint_.session_minutes * kMinute)) {
            session_ = options_.new_id();
            if (first_open_) {
                first_open_ = false;
                add("first_open", json::Value(), now);
            }
            add("session_start", json::Value(), now);
        }
        last_event_ms_ = now;
        add(name, params, now);
        if (static_cast<int>(queue_.size() - in_flight_) >= endpoint_.batch_size) {
            send();
        }
    }

    void set_user_property(std::string_view name, std::string_view value) override {
        user_properties_[std::string(name)] = std::string(value);
    }

    void set_user_id(std::string_view id) override { user_id_ = id; }

    void flush() override {
        take_answer();
        send();
        save();
    }

    void poll() override {
        take_answer();
        if (queue_.size() == in_flight_ || in_flight_ != 0) {
            return;
        }
        const std::int64_t now = options_.now_ms();
        if (now < retry_at_ms_) {
            return;
        }
        const std::int64_t oldest = queue_.front().find("time")->as_int();
        if (now - oldest >= static_cast<std::int64_t>(endpoint_.flush_seconds * 1000.0)) {
            send();
        }
    }

    [[nodiscard]] json::Value status() const override {
        json::Value out;
        out["device"] = device_;
        out["session"] = session_;
        out["queued"] = static_cast<std::int64_t>(queue_.size());
        out["in_flight"] = static_cast<std::int64_t>(in_flight_);
        out["sent"] = static_cast<std::int64_t>(sent_);
        out["dropped"] = static_cast<std::int64_t>(dropped_);
        out["last_status"] = last_status_;
        return out;
    }

private:
    void add(std::string_view name, const json::Value& params, std::int64_t now) {
        json::Value event;
        event["event"] = name;
        if (params.is_object() && params.size() > 0) {
            event["params"] = params;
        }
        event["time"] = now;
        event["session"] = session_;
        event["seq"] = static_cast<std::int64_t>(++sequence_);
        if (!user_id_.empty()) {
            event["user_id"] = user_id_;
        }
        if (!user_properties_.empty()) {
            json::Value properties;
            for (const auto& [key, value] : user_properties_) {
                properties[key] = value;
            }
            event["user_properties"] = std::move(properties);
        }
        queue_.push_back(std::move(event));
        // The oldest go first, but never those being sent.
        while (queue_.size() > static_cast<std::size_t>(endpoint_.max_queued) &&
               queue_.size() > in_flight_) {
            queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(in_flight_));
            ++dropped_;
        }
    }

    // The batch's body in the endpoint's format.
    [[nodiscard]] std::string body(std::size_t count) const {
        json::Value out;
        if (endpoint_.format == AnalyticsEndpoint::Format::PostHog) {
            out["api_key"] = endpoint_.api_key;
            json::Array batch;
            for (std::size_t i = 0; i < count; ++i) {
                const json::Value& event = queue_[i];
                json::Value item;
                item["event"] = event.find("event")->as_string();
                const json::Value* user = event.find("user_id");
                item["distinct_id"] = user != nullptr ? user->as_string() : device_;
                item["timestamp"] = iso8601(event.find("time")->as_int());
                json::Value properties;
                if (const json::Value* params = event.find("params"); params != nullptr) {
                    properties = *params;
                }
                properties["$session_id"] = event.find("session")->as_string();
                properties["$device_id"] = device_;
                properties["$lib"] = "slashengine";
                properties["$os"] = options_.platform;
                properties["$app_name"] = options_.app_name;
                properties["$app_version"] = options_.app_version;
                if (const json::Value* set = event.find("user_properties"); set != nullptr) {
                    properties["$set"] = *set;
                }
                item["properties"] = std::move(properties);
                batch.push_back(std::move(item));
            }
            out["batch"] = std::move(batch);
        } else {
            out["app"]["name"] = options_.app_name;
            out["app"]["version"] = options_.app_version;
            out["app"]["platform"] = options_.platform;
            out["device"] = device_;
            out["sent_at"] = iso8601(options_.now_ms());
            json::Array events;
            for (std::size_t i = 0; i < count; ++i) {
                json::Value event = queue_[i];
                event["time"] = iso8601(event.find("time")->as_int());
                events.push_back(std::move(event));
            }
            out["events"] = std::move(events);
        }
        return json::write(out, json::Style::Compact);
    }

    void send() {
        if (in_flight_ != 0 || queue_.empty() || !options_.post) {
            return;
        }
        const std::size_t count =
            std::min(queue_.size(), static_cast<std::size_t>(endpoint_.batch_size));
        in_flight_ = count;
        HttpPost post{.url = endpoint_.url, .headers = endpoint_.headers, .body = body(count)};
        auto answer = std::make_shared<Answer>();
        answer_ = answer;
        options_.post(std::move(post), [answer](int status) {
            const std::scoped_lock lock(answer->mutex);
            answer->status = status;
        });
        take_answer();
    }

    // Takes in the sent batch's answer once there is one.
    void take_answer() {
        if (!answer_) {
            return;
        }
        std::optional<int> status;
        {
            const std::scoped_lock lock(answer_->mutex);
            status = answer_->status;
        }
        if (!status) {
            return;
        }
        answer_.reset();
        last_status_ = *status;
        const auto count = static_cast<std::ptrdiff_t>(in_flight_);
        in_flight_ = 0;
        if (*status >= kStatusOk && *status < kStatusMultipleChoices) {
            queue_.erase(queue_.begin(), queue_.begin() + count);
            sent_ += static_cast<std::uint64_t>(count);
            attempts_ = 0;
            retry_at_ms_ = 0;
            save();
        } else if (retryable(*status)) {
            const std::int64_t wait = std::min(kMaxRetryWait, kMinute << std::min(attempts_, 4));
            ++attempts_;
            retry_at_ms_ = options_.now_ms() + wait;
            log::warn("analytics events wait to be sent again",
                      {{"status", *status}, {"seconds", static_cast<double>(wait) / 1000.0}});
        } else {
            queue_.erase(queue_.begin(), queue_.begin() + count);
            dropped_ += static_cast<std::uint64_t>(count);
            log::warn("the analytics server refused events",
                      {{"status", *status}, {"events", static_cast<std::int64_t>(count)}});
            save();
        }
    }

    void load() {
        if (options_.store_path.empty()) {
            return;
        }
        auto text = platform::read_file(options_.store_path);
        if (!text) {
            return;
        }
        auto stored = json::parse(text.value());
        if (!stored || !stored.value().is_object()) {
            return;
        }
        const json::Value& value = stored.value();
        if (const json::Value* device = value.find("device");
            device != nullptr && device->is_string()) {
            device_ = device->as_string();
        }
        if (const json::Value* sequence = value.find("seq");
            sequence != nullptr && sequence->is_number()) {
            sequence_ = static_cast<std::uint64_t>(sequence->as_int());
        }
        if (const json::Value* events = value.find("events");
            events != nullptr && events->is_array()) {
            for (const json::Value& event : events->as_array()) {
                if (event.is_object() && event.find("event") != nullptr &&
                    event.find("time") != nullptr && event.find("session") != nullptr) {
                    queue_.push_back(event);
                }
            }
        }
    }

    void save() const {
        if (options_.store_path.empty() || device_.empty()) {
            return;
        }
        json::Value stored;
        stored["device"] = device_;
        stored["seq"] = static_cast<std::int64_t>(sequence_);
        json::Array events(queue_.begin(), queue_.end());
        stored["events"] = std::move(events);
        // Written whole, then moved over the last, so a game stopped mid-write keeps one.
        const std::string temporary = options_.store_path + ".tmp";
        if (!platform::write_file(temporary, json::write(stored, json::Style::Compact)) ||
            !platform::rename_file(temporary, options_.store_path)) {
            log::warn("couldn't keep analytics events", {{"path", options_.store_path}});
        }
    }

    AnalyticsEndpoint endpoint_;
    EndpointOptions options_;
    std::deque<json::Value> queue_; // oldest first; the first in_flight are being sent
    std::shared_ptr<Answer> answer_;
    std::map<std::string, std::string> user_properties_;
    std::string user_id_;
    std::int64_t last_event_ms_ = 0;
    std::int64_t retry_at_ms_ = 0;
    std::uint64_t sequence_ = 0;
    std::string device_;
    std::string session_;
    std::size_t in_flight_ = 0; // the first events of the queue, being sent
    std::uint64_t sent_ = 0;
    std::uint64_t dropped_ = 0; // refused, over max_queued, or taken back with consent
    int last_status_ = 0;
    int attempts_ = 0;
    bool first_open_ = false;
};

} // namespace

std::unique_ptr<AnalyticsBackend> make_endpoint_analytics(AnalyticsEndpoint endpoint,
                                                          EndpointOptions options) {
    return std::make_unique<EndpointAnalytics>(std::move(endpoint), std::move(options));
}

} // namespace slash::services
