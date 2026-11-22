#include "slash/command/work.hpp"

#include "checkpoints.hpp"
#include "work_view.hpp"

#include "slash/command/session.hpp"
#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/core/version.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"
#include "slash/storyboard/storyboard.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::command::work {
namespace {

constexpr std::string_view kJournalSuffix = ".jsonl";
// A rotated journal's older part: <id>.1.jsonl.
constexpr std::string_view kRotatedSuffix = ".1.jsonl";
// A run of reads is written once it has this many, whatever the time.
constexpr std::size_t kMaxReadRun = 200;
// Reads are written when the last came this long ago.
constexpr std::int64_t kReadQuietMs = 1000;
// What a short form keeps.
constexpr std::size_t kBriefString = 120;
constexpr std::size_t kBriefArray = 8;
constexpr std::size_t kBriefKeys = 16;
constexpr int kBriefDepth = 4;
// A kept result larger than this, written, is cut to its numbers and counts.
constexpr std::size_t kMaxResultBytes = std::size_t{16} << 10U;
// The entities a line names in each part of its changes.
constexpr std::size_t kMaxIds = 20;

std::string join(std::string_view root, std::string_view relative) {
    std::string path(root.empty() ? "." : root);
    if (path.back() != '/' && path.back() != '\\') {
        path += '/';
    }
    path += relative;
    return path;
}

std::int64_t system_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string pad(std::int64_t value, int width) {
    std::string text = std::to_string(value);
    while (static_cast<int>(text.size()) < width) {
        text.insert(text.begin(), '0');
    }
    return text;
}

// Days since 1970-01-01 of a civil date, and the date of a day, after Howard Hinnant's
// algorithms (public domain).
std::int64_t days_from_civil(std::int64_t year, std::int64_t month, std::int64_t day) {
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const std::int64_t yoe = year - (era * 400);
    const std::int64_t doy = (((153 * (month + (month > 2 ? -3 : 9))) + 2) / 5) + day - 1;
    const std::int64_t doe = (yoe * 365) + (yoe / 4) - (yoe / 100) + doy;
    return (era * 146097) + doe - 719468;
}

struct Civil {
    std::int64_t year = 0;
    std::int64_t month = 0;
    std::int64_t day = 0;
};

Civil civil_from_days(std::int64_t days) {
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - (era * 146097);
    const std::int64_t yoe = (doe - (doe / 1460) + (doe / 36524) - (doe / 146096)) / 365;
    const std::int64_t doy = doe - ((365 * yoe) + (yoe / 4) - (yoe / 100));
    const std::int64_t mp = ((5 * doy) + 2) / 153;
    const std::int64_t day = doy - (((153 * mp) + 2) / 5) + 1;
    const std::int64_t month = mp < 10 ? mp + 3 : mp - 9;
    return {yoe + (era * 400) + (month <= 2 ? 1 : 0), month, day};
}

// Whether a param's name says it holds a secret: one of its parts ("api_key", "accessToken",
// "x-api-key") is a word such as key or token. "keys" and "monkey" are not.
bool is_secret_key(std::string_view key) {
    static constexpr std::array<std::string_view, 9> kWords{
        "key",      "apikey", "token",         "secret",    "secrets",
        "password", "passwd", "authorization", "credential"};
    std::vector<std::string> parts(1);
    for (std::size_t i = 0; i < key.size(); ++i) {
        const auto c = static_cast<unsigned char>(key[i]);
        if (c == '_' || c == '-' || c == '.' || c == ' ') {
            parts.emplace_back();
            continue;
        }
        if (std::isupper(c) != 0 && i > 0 &&
            std::islower(static_cast<unsigned char>(key[i - 1])) != 0) {
            parts.emplace_back();
        }
        parts.back() += static_cast<char>(std::tolower(c));
    }
    return std::ranges::any_of(parts, [](const std::string& part) {
        return std::ranges::find(kWords, part) != kWords.end() || part == "credentials";
    });
}

// A short form of a value: long strings, arrays and objects cut, deep parts elided and anything
// that looks like a secret left out.
json::Value brief(const json::Value& value, int depth = 0) {
    if (value.is_string()) {
        const std::string& text = value.as_string();
        if (text.size() <= kBriefString) {
            return value;
        }
        // Not inside a UTF-8 sequence.
        std::size_t end = kBriefString - 3;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
            --end;
        }
        return text.substr(0, end) + "...";
    }
    if (value.is_array()) {
        if (depth >= kBriefDepth) {
            return "[" + std::to_string(value.as_array().size()) + " items]";
        }
        json::Array out;
        const json::Array& items = value.as_array();
        for (std::size_t i = 0; i < items.size() && i < kBriefArray; ++i) {
            out.push_back(brief(items[i], depth + 1));
        }
        if (items.size() > kBriefArray) {
            out.emplace_back("+" + std::to_string(items.size() - kBriefArray) + " more");
        }
        return out;
    }
    if (value.is_object()) {
        if (depth >= kBriefDepth) {
            return "{" + std::to_string(value.as_object().size()) + " keys}";
        }
        json::Value out = json::Object{};
        std::size_t kept = 0;
        for (const auto& [key, item] : value.as_object()) {
            if (kept == kBriefKeys) {
                out["..."] = "+" + std::to_string(value.as_object().size() - kBriefKeys) + " more";
                break;
            }
            out[key] = is_secret_key(key) ? json::Value("[left out]") : brief(item, depth + 1);
            ++kept;
        }
        return out;
    }
    return value;
}

// Tests and checks keep their results, which say whether the work is good.
bool keeps_result(std::string_view command) {
    return command == "test.run" || command == "build.check" || command == "playtest.run" ||
           command.ends_with(".check");
}

// A result cut to fit a line: its short form, or its numbers, flags and the size of each list.
json::Value kept_result(const json::Value& result) {
    json::Value kept = brief(result);
    if (json::write(kept, json::Style::Compact).size() <= kMaxResultBytes || !result.is_object()) {
        return kept;
    }
    json::Value cut = json::Object{};
    for (const auto& [key, item] : result.as_object()) {
        if (item.is_array()) {
            cut[key] = static_cast<std::int64_t>(item.as_array().size());
        } else if (item.is_object()) {
            cut[key] = "{" + std::to_string(item.as_object().size()) + " keys}";
        } else {
            cut[key] = brief(item);
        }
    }
    cut["truncated"] = true;
    return cut;
}

// diff_scenes' changes by entity, component and field name, without the values.
json::Value summarize_changes(const json::Value& changes) {
    json::Value out = json::Object{};
    if (!changes.is_object()) {
        return out;
    }
    for (const std::string_view part : {"added", "removed"}) {
        const json::Value* ids = changes.find(part);
        if (ids == nullptr || !ids->is_array() || ids->as_array().empty()) {
            continue;
        }
        json::Array kept;
        for (const json::Value& id : ids->as_array()) {
            if (kept.size() == kMaxIds) {
                break;
            }
            kept.push_back(id);
        }
        out[std::string(part)] = std::move(kept);
        if (ids->as_array().size() > kMaxIds) {
            out[std::string(part) + "_more"] =
                static_cast<std::int64_t>(ids->as_array().size() - kMaxIds);
        }
    }
    const json::Value* changed = changes.find("changed");
    if (changed != nullptr && changed->is_object() && !changed->as_object().empty()) {
        json::Value entities = json::Object{};
        std::size_t count = 0;
        const json::Value* added = changes.find("added");
        for (const auto& [id, components] : changed->as_object()) {
            // An added entity's components are its whole self, not a change.
            if (added != nullptr && added->is_array() &&
                std::ranges::find(added->as_array(), json::Value(id)) != added->as_array().end()) {
                continue;
            }
            if (++count > kMaxIds) {
                continue;
            }
            json::Value fields = json::Object{};
            if (components.is_object()) {
                for (const auto& [component, values] : components.as_object()) {
                    if (values.is_object()) {
                        json::Array names;
                        for (const auto& [field, unused] : values.as_object()) {
                            names.emplace_back(field);
                        }
                        fields[component] = std::move(names);
                    } else {
                        fields[component] = nullptr;
                    }
                }
            }
            entities[id] = std::move(fields);
        }
        if (!entities.as_object().empty()) {
            out["changed"] = std::move(entities);
        }
        if (count > kMaxIds) {
            out["changed_more"] = static_cast<std::int64_t>(count - kMaxIds);
        }
    }
    return out;
}

std::string make_id(std::int64_t now) {
    const std::int64_t seconds = now / 1000;
    const Civil date = civil_from_days(seconds / 86400);
    const std::int64_t of_day = seconds % 86400;
    std::random_device device;
    constexpr std::uint32_t kSuffix = 0x10000;
    const std::uint32_t random = device() % kSuffix;
    static constexpr std::string_view kHex = "0123456789abcdef";
    std::string suffix;
    for (int shift = 12; shift >= 0; shift -= 4) {
        suffix += kHex[(random >> static_cast<unsigned>(shift)) & 0xFU];
    }
    return pad(date.year, 4) + pad(date.month, 2) + pad(date.day, 2) + "-" + pad(of_day / 3600, 2) +
           pad((of_day / 60) % 60, 2) + pad(of_day % 60, 2) + "-" + suffix;
}

std::string time_of_day(const json::Value& line) {
    const json::Value* time = line.find("time");
    if (time == nullptr || !time->is_string() || time->as_string().size() < 19) {
        return "--:--:--";
    }
    return time->as_string().substr(11, 8);
}

// A member that is there by construction.
const json::Value& at(const json::Value& value, std::string_view key) {
    static const json::Value kNull;
    const json::Value* found = value.find(key);
    return found != nullptr ? *found : kNull;
}

std::string compact(const json::Value& value) {
    return json::write(value, json::Style::Compact);
}

// The ids of a part of a line's changes, as "+a +b" and the like.
void add_ids(std::string& text, const json::Value& changes, std::string_view part,
             std::string_view mark) {
    const json::Value* ids = changes.find(part);
    if (ids == nullptr || !ids->is_array()) {
        return;
    }
    for (const json::Value& id : ids->as_array()) {
        text += " ";
        text += mark;
        text += id.is_string() ? id.as_string() : compact(id);
    }
    if (const json::Value* more = changes.find(std::string(part) + "_more");
        more != nullptr && more->is_int()) {
        text += " (+" + std::to_string(more->as_int()) + " more)";
    }
}

std::string describe_changes(const json::Value& changes) {
    std::string text;
    add_ids(text, changes, "added", "+");
    add_ids(text, changes, "removed", "-");
    if (const json::Value* changed = changes.find("changed");
        changed != nullptr && changed->is_object()) {
        for (const auto& [id, components] : changed->as_object()) {
            text += " ~" + id;
            if (components.is_object() && !components.as_object().empty()) {
                std::string names;
                for (const auto& [component, unused] : components.as_object()) {
                    names += names.empty() ? "" : ",";
                    names += component;
                }
                text += "(" + names + ")";
            }
        }
        if (const json::Value* more = changes.find("changed_more");
            more != nullptr && more->is_int()) {
            text += " (+" + std::to_string(more->as_int()) + " more)";
        }
    }
    return text;
}

// Whether a kept test or check line passed: no error, and nothing in its result says it failed.
bool passed(const json::Value& line) {
    if (line.find("error") != nullptr) {
        return false;
    }
    const json::Value* result = line.find("result");
    if (result == nullptr || !result->is_object()) {
        return true;
    }
    for (const std::string_view flag : {"ok", "passed", "valid"}) {
        if (const json::Value* value = result->find(flag); value != nullptr && value->is_bool()) {
            return value->as_bool();
        }
    }
    if (const json::Value* failed = result->find("failed"); failed != nullptr) {
        if (failed->is_int()) {
            return failed->as_int() == 0;
        }
        if (failed->is_array()) {
            return failed->as_array().empty();
        }
    }
    static constexpr std::array<std::string_view, 6> kLists{"failures", "errors", "problems",
                                                            "findings", "broken", "drift"};
    return std::ranges::none_of(kLists, [&](std::string_view list) {
        const json::Value* value = result->find(list);
        return value != nullptr && ((value->is_array() && !value->as_array().empty()) ||
                                    (value->is_int() && value->as_int() > 0));
    });
}

// A kept result's gist in a few words: test counts, or its verdict.
std::string result_gist(const json::Value& line) {
    const json::Value* result = line.find("result");
    std::string gist = passed(line) ? "ok" : "failing";
    if (result != nullptr && result->is_object()) {
        const json::Value* count = result->find("passed");
        const json::Value* failed = result->find("failed");
        if (count != nullptr && count->is_int() && failed != nullptr) {
            std::int64_t failures = 0;
            if (failed->is_array()) {
                failures = static_cast<std::int64_t>(failed->as_array().size());
            } else if (failed->is_int()) {
                failures = failed->as_int();
            }
            gist = std::to_string(count->as_int()) + " passed, " + std::to_string(failures) +
                   " failed";
        }
    }
    return gist;
}

} // namespace

// Settings ---------------------------------------------------------------------------------------

Settings read_settings(std::string_view project_root) {
    Settings settings;
    auto text = platform::read_file(join(project_root, "project.json"));
    if (!text) {
        return settings;
    }
    auto parsed = json::parse(text.value());
    if (!parsed || !parsed.value().is_object()) {
        return settings;
    }
    const json::Value* work = parsed.value().find("work");
    if (work == nullptr || !work->is_object()) {
        return settings;
    }
    if (const json::Value* journal = work->find("journal");
        journal != nullptr && journal->is_bool()) {
        settings.journal = journal->as_bool();
    }
    if (const json::Value* serve = work->find("serve"); serve != nullptr && serve->is_bool()) {
        settings.serve = serve->as_bool();
    }
    if (const json::Value* checkpoints = work->find("checkpoints");
        checkpoints != nullptr && checkpoints->is_bool()) {
        settings.checkpoints = checkpoints->as_bool();
    }
    if (const json::Value* size = work->find("max_mb");
        size != nullptr && size->is_int() && size->as_int() >= 1) {
        settings.max_bytes = static_cast<std::uint64_t>(size->as_int()) << 20U;
    }
    return settings;
}

// Times ------------------------------------------------------------------------------------------

std::string format_time(std::int64_t unix_milliseconds) {
    std::int64_t seconds = unix_milliseconds / 1000;
    std::int64_t millis = unix_milliseconds % 1000;
    if (millis < 0) {
        millis += 1000;
        --seconds;
    }
    std::int64_t days = seconds / 86400;
    std::int64_t of_day = seconds % 86400;
    if (of_day < 0) {
        of_day += 86400;
        --days;
    }
    const Civil date = civil_from_days(days);
    return pad(date.year, 4) + "-" + pad(date.month, 2) + "-" + pad(date.day, 2) + "T" +
           pad(of_day / 3600, 2) + ":" + pad((of_day / 60) % 60, 2) + ":" + pad(of_day % 60, 2) +
           "." + pad(millis, 3) + "Z";
}

std::optional<std::int64_t> line_time(const json::Value& line) {
    const json::Value* time = line.find("time");
    if (time == nullptr || !time->is_string()) {
        return std::nullopt;
    }
    const std::string& text = time->as_string();
    // YYYY-MM-DDTHH:MM[:SS[.mmm]][Z]
    if (text.size() < 16 || text[4] != '-' || text[7] != '-' ||
        (text[10] != 'T' && text[10] != ' ') || text[13] != ':') {
        return std::nullopt;
    }
    const auto number = [&](std::size_t at, std::size_t length) -> std::optional<std::int64_t> {
        std::int64_t value = 0;
        for (std::size_t i = at; i < at + length; ++i) {
            if (i >= text.size() || std::isdigit(static_cast<unsigned char>(text[i])) == 0) {
                return std::nullopt;
            }
            value = (value * 10) + (text[i] - '0');
        }
        return value;
    };
    const auto year = number(0, 4);
    const auto month = number(5, 2);
    const auto day = number(8, 2);
    const auto hour = number(11, 2);
    const auto minute = number(14, 2);
    std::size_t at = 16;
    std::optional<std::int64_t> second = 0;
    if (at < text.size() && text[at] == ':') {
        second = number(at + 1, 2);
        at += 3;
    }
    std::int64_t millis = 0;
    if (at < text.size() && text[at] == '.') {
        const auto fraction = number(at + 1, 3);
        if (!fraction) {
            return std::nullopt;
        }
        millis = *fraction;
        at += 4;
    }
    if (at < text.size() && text[at] == 'Z') {
        ++at;
    }
    if (at != text.size() || !year || !month || !day || !hour || !minute || !second || *month < 1 ||
        *month > 12 || *day < 1 || *day > 31 || *hour > 23 || *minute > 59 || *second > 60) {
        return std::nullopt;
    }
    const std::int64_t days = days_from_civil(*year, *month, *day);
    return (((days * 86400) + (*hour * 3600) + (*minute * 60) + *second) * 1000) + millis;
}

Result<std::int64_t> parse_duration(std::string_view text) {
    if (text.size() < 2) {
        return make_error(ErrorCode::InvalidArgument,
                          "a duration is a number and a unit, such as 45s, 90m, 3h or 2d");
    }
    std::int64_t number = 0;
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(text[i])) == 0 || number > 1000000) {
            return make_error(ErrorCode::InvalidArgument,
                              "\"" + std::string(text) +
                                  "\" is not a duration such as 45s, 90m, 3h or 2d");
        }
        number = (number * 10) + (text[i] - '0');
    }
    std::int64_t unit = 0;
    switch (text.back()) {
    case 's':
        unit = 1000;
        break;
    case 'm':
        unit = std::int64_t{60} * 1000;
        break;
    case 'h':
        unit = std::int64_t{3600} * 1000;
        break;
    case 'd':
        unit = std::int64_t{86400} * 1000;
        break;
    default:
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + std::string(text) +
                              "\" is not a duration such as 45s, 90m, 3h or 2d");
    }
    return number * unit;
}

// The journal ------------------------------------------------------------------------------------

Journal::Journal(JournalOptions options) : options_(std::move(options)) {}

Result<std::unique_ptr<Journal>> Journal::open(JournalOptions options) {
    if (!options.clock) {
        options.clock = system_now;
    }
    const std::string folder = join(options.project_root, kFolder);
    if (auto made = platform::create_directories(folder); !made) {
        return made.error();
    }
    // Room for this session: the oldest sessions' journals go until the project's take less than
    // half the most they may.
    const std::vector<JournalFile> journals = list_journals(options.project_root);
    std::uint64_t total = 0;
    for (const JournalFile& file : journals) {
        total += file.bytes;
    }
    const std::vector<SessionEntry> live = live_sessions(options.project_root);
    for (const JournalFile& file : journals) {
        if (total < options.max_bytes / 2) {
            break;
        }
        if (std::ranges::any_of(live,
                                [&](const SessionEntry& entry) { return entry.id == file.id; })) {
            continue;
        }
        for (const std::string& path : file.paths) {
            (void)platform::remove_file(join(options.project_root, path));
        }
        remove_checkpoints(options.project_root, file.id);
        total -= std::min(total, file.bytes);
    }

    auto journal = std::unique_ptr<Journal>(new Journal(std::move(options)));
    const std::int64_t now = journal->now();
    std::string id = journal->options_.id.empty() ? make_id(now) : journal->options_.id;
    if (journal->options_.id.empty()) {
        while (platform::exists(join(folder, id + std::string(kJournalSuffix)))) {
            id = make_id(now);
        }
    }
    journal->id_ = std::move(id);
    journal->full_path_ = join(folder, journal->id_ + std::string(kJournalSuffix));
    if (const auto stamp = platform::file_stamp(journal->full_path_); stamp) {
        journal->bytes_ = stamp->size;
    }
    return journal;
}

Journal::~Journal() {
    flush();
    // A session that did nothing leaves no journal.
    if (started_) {
        json::Value line;
        line["end"] = true;
        write(std::move(line));
    }
}

std::string Journal::path() const {
    return std::string(kFolder) + "/" + id_ + std::string(kJournalSuffix);
}

std::int64_t Journal::now() const {
    return options_.clock ? options_.clock() : system_now();
}

void Journal::write(json::Value line) {
    if (full_path_.empty()) {
        return;
    }
    if (!started_) {
        started_ = true;
        json::Value start;
        start["host"] = options_.host;
        start["version"] = std::string(version());
        start["pid"] = platform::process_id();
        json::Value first;
        first["session"] = id_;
        first["start"] = std::move(start);
        write(std::move(first));
    }
    if (line.find("time") == nullptr) {
        line["time"] = format_time(now());
    }
    line["n"] = static_cast<std::int64_t>(++sequence_);
    std::string text = compact(line);
    text += '\n';
    // A journal past a quarter of the project's share moves to its older part, replacing the
    // part before.
    if (bytes_ > 0 && bytes_ + text.size() > options_.max_bytes / 4) {
        const std::string older =
            join(join(options_.project_root, kFolder), id_ + std::string(kRotatedSuffix));
        if (platform::rename_file(full_path_, older)) {
            bytes_ = 0;
        }
    }
    if (auto appended = platform::append_file(full_path_, text); !appended) {
        log::warn("the work journal can't be written; it stops",
                  {{"path", full_path_}, {"detail", appended.error().message}});
        full_path_.clear();
        return;
    }
    bytes_ += text.size();
}

void Journal::record(const Call& call) {
    const bool keep = keeps_result(call.command);
    if (call.read && call.reply.ok && !keep) {
        if (read_count_ > 0 && reads_client_ != call.client) {
            flush();
        }
        if (read_count_ == 0) {
            reads_from_ = now();
            reads_client_ = std::string(call.client);
        }
        json::Value& count = reads_[std::string(call.command)];
        count = (count.is_int() ? count.as_int() : 0) + 1;
        ++read_count_;
        reads_until_ = now();
        if (read_count_ >= kMaxReadRun) {
            flush();
        }
        return;
    }
    flush();
    json::Value line;
    line["command"] = std::string(call.command);
    if (call.params.is_object() && !call.params.as_object().empty()) {
        line["params"] = brief(call.params);
    }
    if (!call.client.empty()) {
        line["client"] = std::string(call.client);
    }
    // Tenths of a millisecond.
    line["ms"] = static_cast<double>(static_cast<std::int64_t>(call.milliseconds * 10.0)) / 10.0;
    if (call.read) {
        line["read"] = true;
    }
    if (!call.reply.ok) {
        json::Value error;
        error["code"] = static_cast<std::int64_t>(call.reply.code);
        error["message"] = brief(json::Value(call.reply.message));
        line["error"] = std::move(error);
    } else if (keep) {
        line["result"] = kept_result(call.reply.value);
    }
    if (json::Value changes = summarize_changes(call.changes); !changes.as_object().empty()) {
        line["changes"] = std::move(changes);
    }
    if (!call.files.empty()) {
        json::Array files;
        for (const std::string& file : call.files) {
            files.emplace_back(file);
        }
        line["files"] = std::move(files);
    }
    write(std::move(line));
}

void Journal::record_event(std::string_view what, const json::Value& changes) {
    flush();
    json::Value line;
    line["event"] = std::string(what);
    if (json::Value summary = summarize_changes(changes); !summary.as_object().empty()) {
        line["changes"] = std::move(summary);
    }
    write(std::move(line));
}

void Journal::steer(json::Value line) {
    flush();
    write(std::move(line));
}

void Journal::idle() {
    if (read_count_ > 0 && now() - reads_until_ >= kReadQuietMs) {
        flush();
    }
}

void Journal::flush() {
    if (read_count_ == 0) {
        return;
    }
    json::Value line;
    line["time"] = format_time(reads_from_);
    if (reads_until_ != reads_from_) {
        line["until"] = format_time(reads_until_);
    }
    line["reads"] = std::move(reads_);
    if (!reads_client_.empty()) {
        line["client"] = reads_client_;
    }
    reads_ = {};
    read_count_ = 0;
    reads_client_.clear();
    write(std::move(line));
}

// Reading journals -------------------------------------------------------------------------------

std::vector<JournalFile> list_journals(std::string_view project_root) {
    std::vector<JournalFile> journals;
    const std::string folder = join(project_root, kFolder);
    auto files = platform::list_files(folder, [](std::string_view) { return false; });
    if (!files) {
        return journals;
    }
    std::map<std::string, JournalFile, std::less<>> by_id;
    for (const std::string& name : files.value()) {
        if (!name.ends_with(kJournalSuffix) || name.find('/') != std::string::npos) {
            continue;
        }
        const bool rotated = name.ends_with(kRotatedSuffix);
        const std::string id =
            name.substr(0, name.size() - (rotated ? kRotatedSuffix : kJournalSuffix).size());
        JournalFile& file = by_id[id];
        file.id = id;
        const std::string path = std::string(kFolder) + "/" + name;
        if (rotated) {
            file.paths.insert(file.paths.begin(), path);
        } else {
            file.paths.push_back(path);
        }
        if (const auto stamp = platform::file_stamp(join(folder, name)); stamp) {
            file.bytes += stamp->size;
        }
    }
    for (auto& [id, file] : by_id) {
        file.bytes += checkpoint_bytes(project_root, id);
        journals.push_back(std::move(file));
    }
    return journals;
}

Result<std::vector<json::Value>> read_journal(std::string_view project_root, std::string_view id) {
    std::vector<JournalFile> journals = list_journals(project_root);
    const auto found =
        std::ranges::find_if(journals, [&](const JournalFile& file) { return file.id == id; });
    if (found == journals.end()) {
        return make_error(ErrorCode::NotFound, "no journal of a session \"" + std::string(id) +
                                                   "\" in " + std::string(kFolder));
    }
    std::vector<json::Value> lines;
    for (const std::string& path : found->paths) {
        auto text = platform::read_file(join(project_root, path));
        if (!text) {
            continue;
        }
        std::size_t start = 0;
        const std::string& body = text.value();
        while (start < body.size()) {
            std::size_t end = body.find('\n', start);
            if (end == std::string::npos) {
                end = body.size();
            }
            if (end > start) {
                if (auto line = json::parse(std::string_view(body).substr(start, end - start));
                    line && line.value().is_object()) {
                    lines.push_back(std::move(line).value());
                }
            }
            start = end + 1;
        }
    }
    return lines;
}

// sessions.json ----------------------------------------------------------------------------------

json::Value SessionEntry::to_json() const {
    json::Value value;
    value["id"] = id;
    value["host"] = host;
    value["pid"] = pid;
    if (!url.empty()) {
        value["url"] = url;
        value["token"] = token;
    }
    if (!journal.empty()) {
        value["journal"] = journal;
    }
    value["started"] = started;
    return value;
}

std::optional<SessionEntry> SessionEntry::from_json(const json::Value& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }
    const auto text = [&](std::string_view key) {
        const json::Value* item = value.find(key);
        return item != nullptr && item->is_string() ? item->as_string() : std::string();
    };
    SessionEntry entry;
    entry.id = text("id");
    if (entry.id.empty()) {
        return std::nullopt;
    }
    entry.host = text("host");
    if (const json::Value* pid = value.find("pid"); pid != nullptr && pid->is_int()) {
        entry.pid = pid->as_int();
    }
    entry.url = text("url");
    entry.token = text("token");
    entry.journal = text("journal");
    entry.started = text("started");
    return entry;
}

namespace {

std::vector<SessionEntry> listed_sessions(std::string_view project_root) {
    std::vector<SessionEntry> sessions;
    auto text = platform::read_file(join(project_root, kSessionsFile));
    if (!text) {
        return sessions;
    }
    auto parsed = json::parse(text.value());
    if (!parsed) {
        return sessions;
    }
    const json::Value* list = parsed.value().find("sessions");
    if (list == nullptr || !list->is_array()) {
        return sessions;
    }
    for (const json::Value& item : list->as_array()) {
        if (auto entry = SessionEntry::from_json(item); entry) {
            sessions.push_back(std::move(*entry));
        }
    }
    return sessions;
}

Result<void> write_sessions(std::string_view project_root,
                            const std::vector<SessionEntry>& sessions) {
    const std::string path = join(project_root, kSessionsFile);
    if (sessions.empty()) {
        return platform::remove_file(path);
    }
    if (auto made = platform::create_directories(join(project_root, kFolder)); !made) {
        return made;
    }
    json::Array list;
    for (const SessionEntry& entry : sessions) {
        list.push_back(entry.to_json());
    }
    json::Value file;
    file["sessions"] = std::move(list);
    return platform::write_private_file(path, json::write(file) + "\n");
}

} // namespace

std::vector<SessionEntry> live_sessions(std::string_view project_root) {
    std::vector<SessionEntry> sessions = listed_sessions(project_root);
    std::erase_if(sessions,
                  [](const SessionEntry& entry) { return !platform::process_running(entry.pid); });
    return sessions;
}

Result<void> add_session(std::string_view project_root, const SessionEntry& entry) {
    std::vector<SessionEntry> sessions = live_sessions(project_root);
    std::erase_if(sessions, [&](const SessionEntry& other) { return other.id == entry.id; });
    sessions.push_back(entry);
    return write_sessions(project_root, sessions);
}

Result<void> remove_session(std::string_view project_root, std::string_view id) {
    std::vector<SessionEntry> sessions = live_sessions(project_root);
    std::erase_if(sessions, [&](const SessionEntry& other) { return other.id == id; });
    return write_sessions(project_root, sessions);
}

std::string pick_session(std::string_view project_root, std::string_view id) {
    if (!id.empty()) {
        return std::string(id);
    }
    if (const std::vector<SessionEntry> live = live_sessions(project_root); !live.empty()) {
        // Newest first: IDs start with the time they started.
        return std::ranges::max(live, {}, &SessionEntry::id).id;
    }
    if (const std::vector<JournalFile> journals = list_journals(project_root); !journals.empty()) {
        return journals.back().id;
    }
    return {};
}

// Plain words ------------------------------------------------------------------------------------

namespace {

// Text with a full stop, unless it ends with one already.
std::string sentence(std::string text) {
    if (text.empty() || (text.back() != '.' && text.back() != '?' && text.back() != '!')) {
        text += '.';
    }
    return text;
}

} // namespace

std::string plan_gist(const json::Value& plan) {
    std::string text;
    if (const json::Value* task = plan.find("task"); task != nullptr && task->is_string()) {
        text = task->as_string();
    }
    std::size_t done = 0;
    std::size_t steps = 0;
    std::string next;
    if (const json::Value* list = plan.find("steps"); list != nullptr && list->is_array()) {
        for (const json::Value& step : list->as_array()) {
            ++steps;
            const json::Value* ticked = step.find("done");
            if (ticked != nullptr && ticked->is_bool() && ticked->as_bool()) {
                ++done;
            } else if (const json::Value* words = step.find("text");
                       next.empty() && words != nullptr && words->is_string()) {
                next = words->as_string();
            }
        }
    }
    if (!text.empty()) {
        text += ", ";
    }
    text += std::to_string(done) + " of " + std::to_string(steps) + " steps done";
    if (const json::Value* status = plan.find("status");
        status != nullptr && status->is_string() && status->as_string() != "working") {
        text += " (" + status->as_string() + ")";
    }
    if (!next.empty()) {
        text += "; next: " + next;
    }
    return text;
}

std::string describe(const json::Value& line) {
    std::string text = time_of_day(line) + "  ";
    if (const json::Value* start = line.find("start"); start != nullptr && start->is_object()) {
        const json::Value* session = line.find("session");
        const json::Value* host = start->find("host");
        text +=
            "session " +
            (session != nullptr && session->is_string() ? session->as_string() : std::string("?")) +
            " started";
        if (host != nullptr && host->is_string() && !host->as_string().empty()) {
            text += " (" + host->as_string() + ")";
        }
        return text;
    }
    if (line.find("end") != nullptr) {
        return text + "session ended";
    }
    if (const json::Value* n = line.find("checkpoint"); n != nullptr && n->is_int()) {
        text += "checkpoint " + std::to_string(n->as_int());
        const json::Value* label = line.find("label");
        const json::Value* reason = line.find("reason");
        if (label != nullptr && label->is_string()) {
            text += ": " + label->as_string();
        } else if (reason != nullptr && reason->is_string()) {
            text += " (" + reason->as_string() + ")";
        }
        return text;
    }
    if (const json::Value* paused = line.find("paused"); paused != nullptr) {
        const json::Value* why = paused->is_object() ? paused->find("why") : nullptr;
        return text + "paused by a person watching: " +
               (why != nullptr && why->is_string() ? why->as_string() : std::string());
    }
    if (line.find("resumed") != nullptr) {
        return text + "resumed by a person watching";
    }
    if (const json::Value* note = line.find("note"); note != nullptr && note->is_string()) {
        return text + "note from a person watching: " + note->as_string();
    }
    if (const json::Value* client = line.find("client");
        client != nullptr && client->is_string() && !client->as_string().empty()) {
        text += client->as_string() + "  ";
    }
    if (const json::Value* plan = line.find("plan"); plan != nullptr && plan->is_object()) {
        return text + "plan: " + plan_gist(*plan);
    }
    if (const json::Value* reads = line.find("reads"); reads != nullptr && reads->is_object()) {
        text += "read:";
        bool first = true;
        for (const auto& [command, count] : reads->as_object()) {
            text += first ? " " : ", ";
            first = false;
            text += command;
            if (count.is_int() && count.as_int() > 1) {
                text += " x" + std::to_string(count.as_int());
            }
        }
        return text;
    }
    if (const json::Value* event = line.find("event"); event != nullptr && event->is_string()) {
        text += event->as_string();
    } else if (const json::Value* command = line.find("command");
               command != nullptr && command->is_string()) {
        text += command->as_string();
        if (const json::Value* params = line.find("params"); params != nullptr) {
            std::string shown = compact(*params);
            constexpr std::size_t kShown = 80;
            if (shown.size() > kShown) {
                shown = shown.substr(0, kShown - 3) + "...";
            }
            text += " " + shown;
        }
    }
    if (const json::Value* error = line.find("error"); error != nullptr && error->is_object()) {
        const json::Value* message = error->find("message");
        text += "  FAILED: ";
        text += message != nullptr && message->is_string() ? message->as_string() : "error";
        return text;
    }
    if (line.find("result") != nullptr) {
        text += "  -> " + result_gist(line);
    }
    if (const json::Value* changes = line.find("changes"); changes != nullptr) {
        if (const std::string changed = describe_changes(*changes); !changed.empty()) {
            text += " " + changed;
        }
    }
    if (const json::Value* files = line.find("files"); files != nullptr && files->is_array()) {
        text += "  wrote";
        for (const json::Value& file : files->as_array()) {
            text += " " + (file.is_string() ? file.as_string() : compact(file));
        }
    }
    return text;
}

Result<std::int64_t> parse_since(std::string_view text) {
    json::Value line;
    line["time"] = std::string(text);
    if (const std::optional<std::int64_t> time = line_time(line); time) {
        return *time;
    }
    auto stretch = parse_duration(text);
    if (!stretch) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + std::string(text) +
                              "\" is neither a duration such as 45s, 90m, 3h or 2d nor a time "
                              "such as 2026-11-22T13:05:00Z");
    }
    return system_now() - stretch.value();
}

// Digest -----------------------------------------------------------------------------------------

json::Value digest(std::string_view project_root, std::int64_t since, std::string_view session) {
    std::vector<std::string> ids;
    if (!session.empty()) {
        ids.emplace_back(session);
    } else {
        for (const JournalFile& file : list_journals(project_root)) {
            ids.push_back(file.id);
        }
    }
    const std::vector<SessionEntry> live = live_sessions(project_root);
    std::int64_t commands = 0;
    std::int64_t reads = 0;
    std::int64_t failed = 0;
    std::set<std::string> scenes;
    std::set<std::string> files;
    std::set<std::string> added;
    std::set<std::string> removed;
    std::set<std::string> changed;
    json::Value tests = json::Object{};
    json::Value checks = json::Object{};
    json::Array sessions;
    json::Value last_failure;
    std::int64_t first = 0;
    std::int64_t last = 0;
    for (const std::string& id : ids) {
        auto lines = read_journal(project_root, id);
        if (!lines) {
            continue;
        }
        std::string host;
        std::set<std::string> clients;
        std::int64_t session_commands = 0;
        bool active = false;
        // The session's plan and pause as they stand, from any time.
        json::Value plan;
        json::Value paused;
        for (const json::Value& line : lines.value()) {
            if (const json::Value* start = line.find("start"); start != nullptr) {
                if (const json::Value* name = start->find("host");
                    name != nullptr && name->is_string()) {
                    host = name->as_string();
                }
            }
            if (const json::Value* set = line.find("plan"); set != nullptr && set->is_object()) {
                plan = *set;
            }
            if (const json::Value* stop = line.find("paused"); stop != nullptr) {
                paused = *stop;
            } else if (line.find("resumed") != nullptr || line.find("end") != nullptr) {
                paused = json::Value();
            }
            const std::optional<std::int64_t> when = line_time(line);
            if (!when || *when < since) {
                continue;
            }
            first = first == 0 ? *when : std::min(first, *when);
            last = std::max(last, *when);
            if (const json::Value* client = line.find("client");
                client != nullptr && client->is_string()) {
                clients.insert(client->as_string());
            }
            if (const json::Value* run = line.find("reads"); run != nullptr && run->is_object()) {
                for (const auto& [name, count] : run->as_object()) {
                    reads += count.is_int() ? count.as_int() : 0;
                }
                active = true;
                continue;
            }
            const json::Value* command = line.find("command");
            const json::Value* event = line.find("event");
            if (command == nullptr && event == nullptr) {
                continue;
            }
            active = true;
            const std::string name =
                command != nullptr && command->is_string() ? command->as_string() : "";
            if (command != nullptr) {
                if (line.find("read") != nullptr) {
                    ++reads;
                } else {
                    ++commands;
                    ++session_commands;
                }
            }
            if (line.find("error") != nullptr) {
                ++failed;
                last_failure = json::Object{};
                last_failure["command"] = name;
                last_failure["time"] = format_time(*when);
                if (const json::Value* message = line.find("error")->find("message");
                    message != nullptr) {
                    last_failure["message"] = *message;
                }
            }
            if (name.starts_with("scene.")) {
                if (const json::Value* params = line.find("params"); params != nullptr) {
                    if (const json::Value* path = params->find("path");
                        path != nullptr && path->is_string() &&
                        path->as_string().ends_with(".scene.json")) {
                        scenes.insert(path->as_string());
                    }
                }
            }
            if (const json::Value* written = line.find("files");
                written != nullptr && written->is_array()) {
                for (const json::Value& file : written->as_array()) {
                    if (file.is_string()) {
                        files.insert(file.as_string());
                        if (file.as_string().ends_with(".scene.json")) {
                            scenes.insert(file.as_string());
                        }
                    }
                }
            }
            if (const json::Value* changes = line.find("changes"); changes != nullptr) {
                for (const auto& [part, into] :
                     std::array<std::pair<std::string_view, std::set<std::string>*>, 2>{
                         {{"added", &added}, {"removed", &removed}}}) {
                    if (const json::Value* list = changes->find(part);
                        list != nullptr && list->is_array()) {
                        for (const json::Value& item : list->as_array()) {
                            if (item.is_string()) {
                                into->insert(item.as_string());
                            }
                        }
                    }
                }
                if (const json::Value* list = changes->find("changed");
                    list != nullptr && list->is_object()) {
                    for (const auto& [entity, unused] : list->as_object()) {
                        changed.insert(entity);
                    }
                }
            }
            if (command != nullptr && keeps_result(name)) {
                const bool ok = passed(line);
                json::Value state;
                state["time"] = format_time(*when);
                state["ok"] = ok;
                state["gist"] = result_gist(line);
                const bool is_check = name.ends_with(".check") && name != "build.check";
                json::Value& slot = (is_check ? checks : tests)[name];
                const bool was_ok = slot.is_object() &&
                                    (slot.find("was_ok") != nullptr || at(slot, "ok").as_bool());
                const std::int64_t runs = slot.is_object() ? at(slot, "runs").as_int() : 0;
                state["runs"] = runs + 1;
                if (was_ok) {
                    state["was_ok"] = true;
                }
                slot = std::move(state);
            }
        }
        if (!active) {
            continue;
        }
        json::Value entry;
        entry["id"] = id;
        if (!host.empty()) {
            entry["host"] = host;
        }
        json::Array names;
        for (const std::string& client : clients) {
            names.emplace_back(client);
        }
        if (!names.empty()) {
            entry["clients"] = std::move(names);
        }
        entry["commands"] = session_commands;
        entry["live"] =
            std::ranges::any_of(live, [&](const SessionEntry& other) { return other.id == id; });
        if (plan.is_object()) {
            entry["plan"] = std::move(plan);
        }
        if (paused.is_object()) {
            entry["paused"] = std::move(paused);
        }
        sessions.push_back(std::move(entry));
    }
    // A check that passed earlier in the stretch and fails now started failing.
    for (auto& [name, state] : checks.as_object()) {
        const bool was_ok = state.find("was_ok") != nullptr;
        state.as_object().erase("was_ok");
        if (was_ok && !at(state, "ok").as_bool()) {
            state["started_failing"] = true;
        }
    }
    for (auto& [name, state] : tests.as_object()) {
        state.as_object().erase("was_ok");
    }

    const auto list_of = [](const std::set<std::string>& items) {
        json::Array list;
        for (const std::string& item : items) {
            list.emplace_back(item);
        }
        return list;
    };
    json::Value result;
    result["since"] = format_time(since);
    if (last > 0) {
        result["from"] = format_time(first);
        result["until"] = format_time(last);
    }
    result["sessions"] = sessions;
    result["commands"] = commands;
    result["reads"] = reads;
    result["failed"] = failed;
    result["scenes"] = list_of(scenes);
    result["files"] = list_of(files);
    json::Value entities;
    entities["added"] = static_cast<std::int64_t>(added.size());
    entities["removed"] = static_cast<std::int64_t>(removed.size());
    entities["changed"] = static_cast<std::int64_t>(changed.size());
    result["entities"] = entities;
    result["tests"] = tests;
    result["checks"] = checks;
    if (!last_failure.is_null()) {
        result["last_failure"] = last_failure;
    }

    // The same in plain lines.
    json::Array lines;
    if (sessions.empty()) {
        lines.emplace_back("Nothing was done since " + format_time(since).substr(0, 16) + ".");
    } else {
        std::string who;
        for (const json::Value& entry : sessions) {
            who += who.empty() ? "" : "; ";
            who += at(entry, "id").as_string();
            if (const json::Value* host = entry.find("host"); host != nullptr) {
                who += " (" + host->as_string();
                if (const json::Value* clients = entry.find("clients"); clients != nullptr) {
                    for (const json::Value& client : clients->as_array()) {
                        who += ", " + client.as_string();
                    }
                }
                who += at(entry, "live").as_bool() ? ", live)" : ")";
            }
        }
        lines.emplace_back(std::to_string(sessions.size()) +
                           (sessions.size() == 1 ? " session: " : " sessions: ") + who);
        std::string done =
            std::to_string(commands) + (commands == 1 ? " command and " : " commands and ");
        done += std::to_string(reads) + (reads == 1 ? " read from " : " reads from ");
        done += result["from"].as_string().substr(11, 8);
        done += " to ";
        done += result["until"].as_string().substr(11, 8);
        done += " UTC; ";
        done += std::to_string(failed);
        done += " failed.";
        lines.emplace_back(std::move(done));
        for (const json::Value& entry : sessions) {
            const std::string whose =
                sessions.size() == 1 ? "" : " (" + at(entry, "id").as_string() + ")";
            if (const json::Value* plan = entry.find("plan"); plan != nullptr) {
                lines.emplace_back(sentence("Plan" + whose + ": " + plan_gist(*plan)));
            }
            if (const json::Value* paused = entry.find("paused"); paused != nullptr) {
                const json::Value* why = paused->find("why");
                lines.emplace_back(sentence(
                    "Paused" + whose + " by a person watching: " +
                    (why != nullptr && why->is_string() ? why->as_string() : std::string())));
            }
        }
        constexpr std::size_t kNamed = 6;
        const auto named = [&](const std::set<std::string>& items) {
            std::string text;
            std::size_t count = 0;
            for (const std::string& item : items) {
                if (count++ == kNamed) {
                    text += ", +" + std::to_string(items.size() - kNamed) + " more";
                    break;
                }
                text += text.empty() ? "" : ", ";
                text += item;
            }
            return text;
        };
        if (!scenes.empty()) {
            lines.emplace_back("Scenes: " + named(scenes) + ".");
        }
        if (!files.empty()) {
            lines.emplace_back("Files written (" + std::to_string(files.size()) +
                               "): " + named(files) + ".");
        }
        if (!added.empty() || !removed.empty() || !changed.empty()) {
            lines.emplace_back("Entities: " + std::to_string(added.size()) + " added, " +
                               std::to_string(removed.size()) + " removed, " +
                               std::to_string(changed.size()) + " changed.");
        }
        for (const auto& [name, state] : tests.as_object()) {
            lines.emplace_back(name + ": " + at(state, "gist").as_string() + " at " +
                               at(state, "time").as_string().substr(11, 8) + ".");
        }
        std::string failing;
        std::string passing;
        for (const auto& [name, state] : checks.as_object()) {
            if (at(state, "ok").as_bool()) {
                passing += passing.empty() ? "" : ", ";
                passing += name;
            } else {
                failing += failing.empty() ? "" : ", ";
                failing += name;
                if (state.find("started_failing") != nullptr) {
                    failing += " (started failing)";
                }
            }
        }
        if (!failing.empty()) {
            lines.emplace_back("Checks failing now: " + failing + ".");
        }
        if (!passing.empty()) {
            lines.emplace_back("Checks passing: " + passing + ".");
        }
        if (!last_failure.is_null()) {
            const json::Value* message = last_failure.find("message");
            std::string text = "Last failure: " + at(last_failure, "command").as_string() + " at " +
                               at(last_failure, "time").as_string().substr(11, 8);
            if (message != nullptr && message->is_string()) {
                text += ": " + message->as_string();
            }
            if (text.back() != '.' && text.back() != '?' && text.back() != '!') {
                text += '.';
            }
            lines.emplace_back(std::move(text));
        }
    }
    result["lines"] = std::move(lines);
    return result;
}

// Commands ---------------------------------------------------------------------------------------

void register_work_commands(Registry& registry) {
    json::Value session_param;
    session_param["type"] = "string";
    session_param["description"] =
        "The session, as named in .slash/work/sessions.json; default the newest.";

    json::Value feed;
    feed["type"] = "object";
    feed["additionalProperties"] = false;
    feed["properties"]["session"] = session_param;
    feed["properties"]["after"]["type"] = "integer";
    feed["properties"]["after"]["minimum"] = 0;
    feed["properties"]["after"]["description"] =
        "Only lines after this one (a reply's next), to follow the feed; default the last ones.";
    feed["properties"]["limit"]["type"] = "integer";
    feed["properties"]["limit"]["minimum"] = 1;
    feed["properties"]["limit"]["maximum"] = 500;
    feed["properties"]["limit"]["default"] = 20;
    feed["properties"]["limit"]["description"] = "The most lines to return.";
    feed["properties"]["text"]["type"] = "boolean";
    feed["properties"]["text"]["default"] = false;
    feed["properties"]["text"]["description"] = "Lines in plain words rather than as JSON.";
    registry.add({
        .name = "work.feed",
        .description = "What a session's commands did, from the project's work journal: each "
                       "edit with its changes and files, runs of reads, tests and checks with "
                       "their results. For reviewing another agent's work.",
        .capabilities = {Capability::Read},
        .effect = Effect::Read,
        .params = std::move(feed),
        .handler =
            [](Session& session, const json::Value& params) {
                if (session.options().journal != nullptr) {
                    session.options().journal->flush();
                }
                const std::string& root = session.options().project_root;
                const json::Value* named = params.find("session");
                const std::string id = pick_session(
                    root, named != nullptr ? std::string_view(named->as_string()) : "");
                if (id.empty()) {
                    return Reply::failure(code::kNotFound,
                                          "no session has written a work journal in this "
                                          "project yet");
                }
                auto lines = read_journal(root, id);
                if (!lines) {
                    json::Value data;
                    data["path"] = "/session";
                    return Reply::failure(code::kNotFound, lines.error().message, std::move(data));
                }
                const json::Value* after = params.find("after");
                const json::Value* limit = params.find("limit");
                const auto most = static_cast<std::size_t>(limit != nullptr ? limit->as_int() : 20);
                const bool text = params.find("text") != nullptr && at(params, "text").as_bool();
                std::vector<const json::Value*> picked;
                for (const json::Value& line : lines.value()) {
                    const json::Value* n = line.find("n");
                    if (after != nullptr &&
                        (n == nullptr || !n->is_int() || n->as_int() <= after->as_int())) {
                        continue;
                    }
                    picked.push_back(&line);
                }
                // From the start of what follows `after`, else the last ones.
                std::size_t from = 0;
                std::size_t to = picked.size();
                if (after != nullptr) {
                    to = std::min(picked.size(), most);
                } else if (picked.size() > most) {
                    from = picked.size() - most;
                }
                json::Array out;
                std::int64_t next = after != nullptr ? after->as_int() : 0;
                for (std::size_t i = from; i < to; ++i) {
                    out.push_back(text ? json::Value(describe(*picked[i])) : *picked[i]);
                    if (const json::Value* n = picked[i]->find("n"); n != nullptr && n->is_int()) {
                        next = std::max(next, n->as_int());
                    }
                }
                json::Value result;
                result["session"] = id;
                result["lines"] = std::move(out);
                result["next"] = next;
                result["more"] = to < picked.size();
                result["live"] = std::ranges::any_of(
                    live_sessions(root), [&](const SessionEntry& entry) { return entry.id == id; });
                return Reply::success(std::move(result));
            },
    });

    json::Value digest_params;
    digest_params["type"] = "object";
    digest_params["additionalProperties"] = false;
    digest_params["properties"]["since"]["type"] = "string";
    digest_params["properties"]["since"]["default"] = "3h";
    digest_params["properties"]["since"]["description"] =
        "How far back, such as 45m, 3h or 2d, or a UTC time such as 2026-11-22T13:05:00Z.";
    digest_params["properties"]["session"] = session_param;
    digest_params["properties"]["session"]["description"] =
        "Only this session; default every session in the project.";
    registry.add({
        .name = "work.digest",
        .description = "A summary of the project's sessions' work over a stretch, from the work "
                       "journals: commands, failures, scenes, files and entities changed, the "
                       "last test results and the checks failing, with the same as plain lines.",
        .capabilities = {Capability::Read},
        .effect = Effect::Read,
        .params = std::move(digest_params),
        .handler =
            [](Session& session, const json::Value& params) {
                if (session.options().journal != nullptr) {
                    session.options().journal->flush();
                }
                const json::Value* since = params.find("since");
                auto from = parse_since(since != nullptr ? since->as_string() : "3h");
                if (!from) {
                    json::Value data;
                    data["path"] = "/since";
                    return Reply::failure(code::kInvalidParams, "/since: " + from.error().message,
                                          std::move(data));
                }
                const json::Value* named = params.find("session");
                return Reply::success(digest(session.options().project_root, from.value(),
                                             named != nullptr ? named->as_string() : ""));
            },
    });

    json::Value mark;
    mark["type"] = "object";
    mark["additionalProperties"] = false;
    mark["properties"]["label"]["type"] = "string";
    mark["properties"]["label"]["maxLength"] = 200;
    mark["properties"]["label"]["description"] = "What the work has reached, in a few words.";
    registry.add({
        .name = "work.checkpoint",
        .description = "Marks a checkpoint in this session's work, as scene.save and a passing "
                       "test.run do: the files written since the last one, the world and a "
                       "frame, so a person can compare it and go back to it.",
        .capabilities = {Capability::Read},
        .effect = Effect::Read,
        .params = std::move(mark),
        .handler =
            [](Session& session, const json::Value& params) {
                const json::Value* label = params.find("label");
                auto taken = take_checkpoint(session, "work.checkpoint",
                                             label != nullptr ? label->as_string() : "");
                if (!taken) {
                    return Reply::failure(taken.error());
                }
                json::Value result = taken.value().to_json();
                result["session"] = session.options().journal->id();
                return Reply::success(std::move(result));
            },
    });

    // The session named, else the newest; a failure when there is none.
    const auto pick = [](Session& session, const json::Value& params) -> Result<std::string> {
        if (session.options().journal != nullptr) {
            session.options().journal->flush();
        }
        const json::Value* named = params.find("session");
        std::string id = pick_session(session.options().project_root,
                                      named != nullptr ? std::string_view(named->as_string()) : "");
        if (id.empty()) {
            return make_error(ErrorCode::NotFound,
                              "no session has written a work journal in this project yet");
        }
        return id;
    };

    json::Value listing;
    listing["type"] = "object";
    listing["additionalProperties"] = false;
    listing["properties"]["session"] = session_param;
    listing["properties"]["after"]["type"] = "integer";
    listing["properties"]["after"]["minimum"] = 0;
    listing["properties"]["after"]["description"] = "Only checkpoints after this one.";
    listing["properties"]["image"]["type"] = "boolean";
    listing["properties"]["image"]["default"] = false;
    listing["properties"]["image"]["description"] = "Each one's frame as a base64 PNG, as \"png\".";
    registry.add({
        .name = "work.checkpoints",
        .description = "A session's checkpoints, oldest first: when, why, the files each holds "
                       "and, with image, its frame.",
        .capabilities = {Capability::Read},
        .effect = Effect::Read,
        .params = std::move(listing),
        .handler =
            [pick](Session& session, const json::Value& params) {
                auto id = pick(session, params);
                if (!id) {
                    return Reply::failure(id.error());
                }
                const json::Value* after = params.find("after");
                const bool image = params.find("image") != nullptr && at(params, "image").as_bool();
                json::Array list;
                for (const Checkpoint& checkpoint :
                     list_checkpoints(session.options().project_root, id.value())) {
                    if (after != nullptr && checkpoint.n <= after->as_int()) {
                        continue;
                    }
                    json::Value entry = checkpoint.to_json();
                    if (image && checkpoint.frame) {
                        if (auto png = checkpoint_frame(session.options().project_root, id.value(),
                                                        checkpoint.n);
                            png) {
                            entry["png"] = base64_encode(png.value());
                        }
                    }
                    list.push_back(std::move(entry));
                }
                json::Value result;
                result["session"] = id.value();
                result["checkpoints"] = std::move(list);
                return Reply::success(std::move(result));
            },
    });

    json::Value compare;
    compare["type"] = "object";
    compare["additionalProperties"] = false;
    compare["properties"]["session"] = session_param;
    compare["properties"]["from"]["type"] = "integer";
    compare["properties"]["from"]["minimum"] = 1;
    compare["properties"]["from"]["description"] = "A checkpoint; default the one before to.";
    compare["properties"]["to"]["type"] = "integer";
    compare["properties"]["to"]["minimum"] = 1;
    compare["properties"]["to"]["description"] = "A checkpoint; default the last.";
    compare["properties"]["image"]["type"] = "boolean";
    compare["properties"]["image"]["default"] = false;
    compare["properties"]["image"]["description"] =
        "Both frames as base64 PNGs, to set side by side.";
    registry.add({
        .name = "work.compare",
        .description = "What changed between two of a session's checkpoints: files added, "
                       "removed and changed (with lines), and entities as world.diff gives "
                       "them; with image, both frames.",
        .capabilities = {Capability::Read},
        .effect = Effect::Read,
        .params = std::move(compare),
        .handler =
            [pick](Session& session, const json::Value& params) {
                auto id = pick(session, params);
                if (!id) {
                    return Reply::failure(id.error());
                }
                const std::string& root = session.options().project_root;
                const std::vector<Checkpoint> list = list_checkpoints(root, id.value());
                const json::Value* from = params.find("from");
                const json::Value* to = params.find("to");
                if (to == nullptr && list.empty()) {
                    return Reply::failure(code::kNotFound,
                                          "session " + id.value() + " has no checkpoints yet");
                }
                const std::int64_t last = to != nullptr ? to->as_int() : list.back().n;
                std::int64_t first = last - 1;
                if (from != nullptr) {
                    first = from->as_int();
                } else {
                    // The checkpoint before it, or itself when it is the first.
                    first = last;
                    for (const Checkpoint& checkpoint : list) {
                        if (checkpoint.n < last) {
                            first = checkpoint.n;
                        }
                    }
                }
                auto compared =
                    compare_checkpoints(root, id.value(), first, last, session.components());
                if (!compared) {
                    json::Value data;
                    data["path"] = from != nullptr ? "/from" : "/to";
                    return Reply::failure(code::kNotFound, compared.error().message,
                                          std::move(data));
                }
                json::Value result = std::move(compared).value();
                result["session"] = id.value();
                if (params.find("image") != nullptr && at(params, "image").as_bool()) {
                    for (const auto& [key, n] : {std::pair{"from", first}, std::pair{"to", last}}) {
                        if (auto png = checkpoint_frame(root, id.value(), n); png) {
                            result[key]["png"] = base64_encode(png.value());
                        }
                    }
                }
                return Reply::success(std::move(result));
            },
    });

    // Steering ------------------------------------------------------------------------------------

    const auto watchers_only = [](Session& session, std::string_view name) -> std::optional<Reply> {
        if (session.watching()) {
            return std::nullopt;
        }
        return Reply::failure(code::kPermissionDenied,
                              "\"" + std::string(name) +
                                  "\" is for the people watching, through the session's watch "
                                  "server: slash-cli work " +
                                  std::string(name.substr(5)) +
                                  ", the watch page, or from another session with \"session\"");
    };
    const auto steer = [](Session& session, json::Value line) {
        if (session.options().journal != nullptr) {
            session.options().journal->steer(std::move(line));
        }
    };

    json::Value steer_session;
    steer_session["type"] = "string";
    steer_session["description"] =
        "Another running session to steer, as named in .slash/work/sessions.json.";

    json::Value pause;
    pause["type"] = "object";
    pause["additionalProperties"] = false;
    pause["properties"]["session"] = steer_session;
    pause["required"] = json::Array{json::Value("why")};
    pause["properties"]["why"]["type"] = "string";
    pause["properties"]["why"]["minLength"] = 1;
    pause["properties"]["why"]["maxLength"] = 500;
    pause["properties"]["why"]["description"] = "Why, for the agent: its edits fail with it.";
    registry.add({
        .name = "work.pause",
        .description = "Pauses the agent's edits, for a person watching: until work.resume, "
                       "commands that change anything fail with the reason; reads still work.",
        .capabilities = {Capability::Read},
        .effect = Effect::Read,
        .params = std::move(pause),
        .handler =
            [watchers_only, steer](Session& session, const json::Value& params) {
                if (auto forwarded = forward_elsewhere(session, "work.pause", params)) {
                    return *std::move(forwarded);
                }
                if (auto refused = watchers_only(session, "work.pause")) {
                    return *refused;
                }
                json::Value paused;
                paused["why"] = at(params, "why");
                session.steering().paused = paused;
                json::Value line;
                line["paused"] = paused;
                steer(session, std::move(line));
                json::Value result;
                result["paused"] = std::move(paused);
                return Reply::success(std::move(result));
            },
    });

    json::Value resume;
    resume["type"] = "object";
    resume["additionalProperties"] = false;
    resume["properties"]["session"] = steer_session;
    registry.add({
        .name = "work.resume",
        .description = "Lets the agent edit again after work.pause, for a person watching.",
        .capabilities = {Capability::Read},
        .effect = Effect::Read,
        .params = std::move(resume),
        .handler =
            [watchers_only, steer](Session& session, const json::Value& params) {
                if (auto forwarded = forward_elsewhere(session, "work.resume", params)) {
                    return *std::move(forwarded);
                }
                if (auto refused = watchers_only(session, "work.resume")) {
                    return *refused;
                }
                const bool was = session.steering().paused.is_object();
                session.steering().paused = json::Value();
                if (was) {
                    json::Value line;
                    line["resumed"] = true;
                    steer(session, std::move(line));
                }
                json::Value result;
                result["resumed"] = was;
                return Reply::success(std::move(result));
            },
    });

    json::Value note;
    note["type"] = "object";
    note["additionalProperties"] = false;
    note["properties"]["session"] = steer_session;
    note["required"] = json::Array{json::Value("text")};
    note["properties"]["text"]["type"] = "string";
    note["properties"]["text"]["minLength"] = 1;
    note["properties"]["text"]["maxLength"] = 2000;
    note["properties"]["text"]["description"] = "The note.";
    registry.add({
        .name = "work.note",
        .description = "Leaves the agent a note, for a person watching: its next reply carries "
                       "it in \"notes\". Nothing stops.",
        .capabilities = {Capability::Read},
        .effect = Effect::Read,
        .params = std::move(note),
        .handler =
            [watchers_only, steer](Session& session, const json::Value& params) {
                if (auto forwarded = forward_elsewhere(session, "work.note", params)) {
                    return *std::move(forwarded);
                }
                if (auto refused = watchers_only(session, "work.note")) {
                    return *refused;
                }
                json::Value line;
                line["note"] = at(params, "text");
                steer(session, std::move(line));
                json::Value waiting;
                waiting["text"] = at(params, "text");
                session.steering().notes.push_back(std::move(waiting));
                json::Value result;
                result["waiting"] = static_cast<std::int64_t>(session.steering().notes.size());
                return Reply::success(std::move(result));
            },
    });

    json::Value step;
    step["type"] = "object";
    step["additionalProperties"] = false;
    step["required"] = json::Array{json::Value("text")};
    step["properties"]["text"]["type"] = "string";
    step["properties"]["text"]["minLength"] = 1;
    step["properties"]["text"]["maxLength"] = 200;
    step["properties"]["done"]["type"] = "boolean";
    step["properties"]["done"]["default"] = false;
    step["properties"]["story"]["type"] = "string";
    step["properties"]["story"]["description"] =
        "A storyboard task this step finishes, such as \"task/boss_fight\".";
    json::Value step_text;
    step_text["type"] = "string";
    step_text["minLength"] = 1;
    step_text["maxLength"] = 200;
    json::Value plan;
    plan["type"] = "object";
    plan["additionalProperties"] = false;
    plan["properties"]["task"]["type"] = "string";
    plan["properties"]["task"]["maxLength"] = 200;
    plan["properties"]["task"]["description"] = "What the agent is doing, in a few words.";
    plan["properties"]["steps"]["type"] = "array";
    plan["properties"]["steps"]["maxItems"] = 50;
    plan["properties"]["steps"]["items"]["oneOf"] = json::Array{step_text, step};
    plan["properties"]["steps"]["description"] =
        "Every step, replacing the list: text, or {text, done, story}.";
    plan["properties"]["done"]["type"] = "array";
    plan["properties"]["done"]["maxItems"] = 50;
    plan["properties"]["done"]["items"]["type"] = "integer";
    plan["properties"]["done"]["items"]["minimum"] = 1;
    plan["properties"]["done"]["description"] = "Steps to tick, by number from 1.";
    plan["properties"]["status"]["enum"] =
        json::Array{json::Value("working"), json::Value("blocked"), json::Value("done")};
    plan["properties"]["status"]["description"] =
        "How the task stands; \"blocked\" when a person is needed.";
    plan["properties"]["session"] = session_param;
    plan["properties"]["session"]["description"] =
        "Read this session's plan; with nothing else, the plan is only read.";
    registry.add({
        .name = "work.plan",
        .description = "Keeps the agent's plan, which people watching see beside its commands: "
                       "set the task and steps, tick steps done, set the status. With no "
                       "params, returns it.",
        .capabilities = {Capability::Read},
        .effect = Effect::Read,
        .params = std::move(plan),
        .handler =
            [steer](Session& session, const json::Value& params) {
                const bool changes =
                    params.find("task") != nullptr || params.find("steps") != nullptr ||
                    params.find("done") != nullptr || params.find("status") != nullptr;
                const Journal* journal = session.options().journal;
                const std::string own = journal != nullptr ? journal->id() : "";
                const json::Value* named = params.find("session");
                if (!changes) {
                    json::Value found = session.steering().plan;
                    std::string id = own;
                    if (named != nullptr && named->as_string() != own) {
                        id = named->as_string();
                        found = json::Value();
                        auto lines = read_journal(session.options().project_root, id);
                        if (!lines) {
                            json::Value data;
                            data["path"] = "/session";
                            return Reply::failure(code::kNotFound,
                                                  "/session: no session " + id +
                                                      " has a work journal in this project",
                                                  std::move(data));
                        }
                        for (const json::Value& line : lines.value()) {
                            if (const json::Value* set = line.find("plan");
                                set != nullptr && set->is_object()) {
                                found = *set;
                            }
                        }
                    }
                    json::Value result = found.is_object() ? found : json::Value(json::Object{});
                    if (result.find("steps") == nullptr) {
                        result["steps"] = json::Array{};
                    }
                    if (!id.empty()) {
                        result["session"] = id;
                    }
                    return Reply::success(std::move(result));
                }
                if (session.watching()) {
                    return Reply::failure(code::kPermissionDenied,
                                          "only the agent keeps its plan; watchers read it");
                }
                if (named != nullptr) {
                    json::Value data;
                    data["path"] = "/session";
                    return Reply::failure(code::kInvalidArgument,
                                          "/session: an agent sets only its own session's plan; "
                                          "leave session out",
                                          std::move(data));
                }
                json::Value next = session.steering().plan;
                if (!next.is_object()) {
                    next = json::Object{};
                    next["status"] = "working";
                    next["steps"] = json::Array{};
                }
                std::set<std::size_t> done_before;
                for (std::size_t i = 0; i < at(next, "steps").as_array().size(); ++i) {
                    const json::Value* done = at(next, "steps").as_array()[i].find("done");
                    if (done != nullptr && done->as_bool()) {
                        done_before.insert(i);
                    }
                }
                if (const json::Value* task = params.find("task")) {
                    next["task"] = *task;
                }
                if (const json::Value* status = params.find("status")) {
                    next["status"] = *status;
                }
                if (const json::Value* steps = params.find("steps")) {
                    json::Array list;
                    for (const json::Value& item : steps->as_array()) {
                        json::Value entry;
                        entry["text"] = item.is_string() ? item : at(item, "text");
                        const json::Value* done = item.is_object() ? item.find("done") : nullptr;
                        entry["done"] = done != nullptr && done->as_bool();
                        if (const json::Value* story =
                                item.is_object() ? item.find("story") : nullptr) {
                            entry["story"] = *story;
                        }
                        list.push_back(std::move(entry));
                    }
                    next["steps"] = std::move(list);
                }
                json::Array& list = next["steps"].as_array();
                if (const json::Value* done = params.find("done")) {
                    for (std::size_t i = 0; i < done->as_array().size(); ++i) {
                        const std::int64_t n = done->as_array()[i].as_int();
                        if (n > static_cast<std::int64_t>(list.size())) {
                            json::Value data;
                            data["path"] = "/done/" + std::to_string(i);
                            return Reply::failure(code::kInvalidArgument,
                                                  "/done/" + std::to_string(i) + ": the plan has " +
                                                      std::to_string(list.size()) +
                                                      " steps, so no step " + std::to_string(n),
                                                  std::move(data));
                        }
                        list[static_cast<std::size_t>(n - 1)]["done"] = true;
                    }
                }
                // Steps naming storyboard tasks: the task must exist, and a step just done hands
                // back the edit that closes it.
                json::Array story_edits;
                if (std::ranges::any_of(list, [](const json::Value& entry) {
                        return entry.find("story") != nullptr;
                    })) {
                    auto loaded = storyboard::load([&session](std::string_view path) {
                        return session.read_project_file(path);
                    });
                    if (!loaded) {
                        return Reply::failure(loaded.error());
                    }
                    const json::Object& entries = loaded.value().entries;
                    for (std::size_t i = 0; i < list.size(); ++i) {
                        const json::Value* story = list[i].find("story");
                        if (story == nullptr) {
                            continue;
                        }
                        const std::string& id = story->as_string();
                        const auto it = entries.find(id);
                        const storyboard::Kind* kind = storyboard::kind_of(id);
                        if (it == entries.end() || kind == nullptr || kind->name != "task") {
                            json::Value data;
                            data["path"] = "/steps/" + std::to_string(i) + "/story";
                            return Reply::failure(code::kNotFound,
                                                  "/steps/" + std::to_string(i) + "/story: " +
                                                      (it == entries.end()
                                                           ? storyboard::unknown_entry(entries, id)
                                                           : "\"" + id + "\" is no task"),
                                                  std::move(data));
                        }
                        const json::Value* status = it->second.find("status");
                        const bool closed = status != nullptr && status->is_string() &&
                                            status->as_string() == "done";
                        if (at(list[i], "done").as_bool() && !done_before.contains(i) && !closed) {
                            json::Value edit;
                            edit["id"] = id;
                            edit["version"] = storyboard::version(it->second);
                            edit["set"]["status"] = "done";
                            story_edits.push_back(std::move(edit));
                        }
                    }
                }
                session.steering().plan = next;
                json::Value line;
                line["plan"] = next;
                if (!session.client().empty()) {
                    line["client"] = session.client();
                }
                steer(session, std::move(line));
                json::Value result = std::move(next);
                if (!story_edits.empty()) {
                    result["story_edits"] = std::move(story_edits);
                }
                return Reply::success(std::move(result));
            },
    });

    register_view_commands(registry);
}

} // namespace slash::command::work
