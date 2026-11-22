#include "work_view.hpp"

#include "slash/canvas/canvas.hpp"
#include "slash/command/session.hpp"
#include "slash/command/work.hpp"
#include "slash/core/text.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/ui/ui.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Remote viewing: what a person far from the machine sees of a session through an agent they ask,
// with no server opened to the internet. work.view is the moment, work.timeline the way there and
// work.report a page of it all an agent can hand on. work.pause and work.note reach another
// session through the forwarder (forward_elsewhere), as slash-mcp --watch does for every tool.
namespace slash::command::work {
namespace {

constexpr std::uint32_t kViewWidth = 640;
constexpr std::int64_t kViewLines = 8;
// Frames a report holds; the latest ones.
constexpr std::size_t kReportFrames = 48;
constexpr std::uint32_t kReportFrameWidth = 480;
// Feed lines a report holds; the latest ones.
constexpr std::size_t kReportLines = 400;
constexpr std::uint32_t kTileGap = 4;
constexpr std::uint8_t kSheetGray = 40;
constexpr std::uint8_t kOpaque = 255;

std::string quoted(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

std::string join(std::string_view root, std::string_view path) {
    if (root.empty() || root == ".") {
        return std::string(path);
    }
    std::string out(root);
    if (out.back() != '/') {
        out += '/';
    }
    out += path;
    return out;
}

Forwarder& the_forwarder() {
    static Forwarder forwarder;
    return forwarder;
}

const json::Value& at(const json::Value& value, std::string_view key) {
    static const json::Value null;
    const json::Value* found = value.find(key);
    return found != nullptr ? *found : null;
}

std::optional<SessionEntry> live_entry(std::string_view root, std::string_view id) {
    for (SessionEntry& entry : live_sessions(root)) {
        if (entry.id == id) {
            return std::move(entry);
        }
    }
    return std::nullopt;
}

std::string own_id(const Session& session) {
    return session.options().journal != nullptr ? session.options().journal->id() : "";
}

Reply session_error(std::string message) {
    json::Value data;
    data["path"] = "/session";
    return Reply::failure(code::kNotFound, "/session: " + std::move(message), std::move(data));
}

// The session params name, or this session's, or the newest.
Result<std::string> pick(Session& session, const json::Value& params) {
    if (session.options().journal != nullptr) {
        session.options().journal->flush();
    }
    const json::Value* named = params.find("session");
    if (named == nullptr && session.options().journal != nullptr) {
        return session.options().journal->id();
    }
    std::string id = pick_session(session.options().project_root,
                                  named != nullptr ? std::string_view(named->as_string()) : "");
    if (id.empty()) {
        return make_error(ErrorCode::NotFound,
                          "no session has written a work journal in this project yet");
    }
    return id;
}

// The last plan line in a journal's lines, or null.
json::Value last_plan(const std::vector<json::Value>& lines) {
    json::Value plan;
    for (const json::Value& line : lines) {
        if (const json::Value* found = line.find("plan"); found != nullptr) {
            plan = *found;
        }
    }
    return plan;
}

// Whether the journal's lines leave the session paused, and why.
json::Value last_pause(const std::vector<json::Value>& lines) {
    json::Value paused;
    for (const json::Value& line : lines) {
        if (const json::Value* found = line.find("paused"); found != nullptr) {
            paused = *found;
        } else if (line.find("resumed") != nullptr || line.find("end") != nullptr) {
            paused = json::Value();
        }
    }
    return paused;
}

json::Array recent(const std::vector<json::Value>& lines, std::int64_t count) {
    json::Array out;
    const std::size_t most = static_cast<std::size_t>(std::max<std::int64_t>(count, 0));
    const std::size_t from = lines.size() > most ? lines.size() - most : 0;
    for (std::size_t i = from; i < lines.size(); ++i) {
        out.emplace_back(describe(lines[i]));
    }
    return out;
}

// An image fitted into width by height, centered, over the sheet's gray.
void place(render::Image& sheet, const render::Image& frame, std::uint32_t x, std::uint32_t y,
           std::uint32_t width, std::uint32_t height) {
    if (frame.width == 0 || frame.height == 0) {
        return;
    }
    std::uint32_t fit_width = width;
    auto fit_height = static_cast<std::uint32_t>(std::uint64_t{frame.height} * width / frame.width);
    if (fit_height > height) {
        fit_height = height;
        fit_width = static_cast<std::uint32_t>(std::uint64_t{frame.width} * height / frame.height);
    }
    const render::Image fitted =
        render::resize_image(frame, std::max(fit_width, 1U), std::max(fit_height, 1U));
    const std::uint32_t left = x + ((width - fitted.width) / 2);
    const std::uint32_t top = y + ((height - fitted.height) / 2);
    for (std::uint32_t row = 0; row < fitted.height; ++row) {
        for (std::uint32_t column = 0; column < fitted.width; ++column) {
            const std::size_t from = ((std::size_t{row} * fitted.width) + column) * 4;
            const std::size_t to =
                ((std::size_t{top + row} * sheet.width) + left + column) * std::size_t{4};
            const std::uint32_t alpha = fitted.pixels[from + 3];
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const std::uint32_t mixed =
                    ((fitted.pixels[from + channel] * alpha) + (kSheetGray * (kOpaque - alpha))) /
                    kOpaque;
                sheet.pixels[to + channel] = static_cast<std::uint8_t>(mixed);
            }
            sheet.pixels[to + 3] = kOpaque;
        }
    }
}

std::string escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

std::string join_ids(const json::Value& ids) {
    std::string out;
    if (ids.is_array()) {
        for (const json::Value& id : ids.as_array()) {
            out += out.empty() ? "" : ", ";
            out += id.is_string() ? id.as_string() : json::write(id, json::Style::Compact);
        }
    } else if (ids.is_object()) {
        for (const auto& [id, unused] : ids.as_object()) {
            out += out.empty() ? "" : ", ";
            out += id;
        }
    }
    return out;
}

// What changed since the checkpoint before, as list items.
std::string changes_html(const json::Value& compared) {
    std::string items;
    const auto item = [&items](std::string_view what, const std::string& list) {
        if (!list.empty()) {
            items += "<li>";
            items += what;
            items += ": ";
            items += escape(list);
            items += "</li>";
        }
    };
    const json::Value& files = at(compared, "files");
    item("Files added", join_ids(at(files, "added")));
    item("Files removed", join_ids(at(files, "removed")));
    std::string changed;
    for (const json::Value& file :
         at(files, "changed").is_array() ? at(files, "changed").as_array() : json::Array{}) {
        changed += changed.empty() ? "" : ", ";
        changed += at(file, "path").as_string();
        changed += " (";
        changed += at(file, "lines").as_string();
        changed += ")";
    }
    item("Files changed", changed);
    const json::Value& entities = at(compared, "entities");
    item("Entities added", join_ids(at(entities, "added")));
    item("Entities removed", join_ids(at(entities, "removed")));
    // Entities added show their fields as changes too; name them once.
    std::string changed_entities;
    const json::Value& added = at(entities, "added");
    if (at(entities, "changed").is_object()) {
        for (const auto& [id, unused] : at(entities, "changed").as_object()) {
            if (added.is_array() &&
                std::ranges::find(added.as_array(), json::Value(id)) != added.as_array().end()) {
                continue;
            }
            changed_entities += changed_entities.empty() ? "" : ", ";
            changed_entities += id;
        }
    }
    item("Entities changed", changed_entities);
    return items.empty() ? "<p class=\"muted\">Nothing changed.</p>" : "<ul>" + items + "</ul>";
}

constexpr std::string_view kReportStyle = R"CSS(
:root { color-scheme: light dark; --bg: #f6f7f9; --card: #ffffff; --ink: #1d2430; --muted: #5f6b7a;
  --line: #dfe3e8; --ok: #18794e; --bad: #c4321c; }
@media (prefers-color-scheme: dark) { :root { --bg: #14171c; --card: #1c2027; --ink: #e6e9ee;
  --muted: #9aa4b1; --line: #2d333c; --ok: #4cc38a; --bad: #f2735f; } }
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--ink);
  font: 15px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif; }
main { max-width: 960px; margin: 0 auto; padding: 24px 16px 48px; }
h1 { font-size: 22px; margin: 0 0 4px; overflow-wrap: anywhere; }
h2 { font-size: 13px; text-transform: uppercase; letter-spacing: .06em; color: var(--muted);
  margin: 0 0 10px; }
section { background: var(--card); border: 1px solid var(--line); border-radius: 10px;
  padding: 16px; margin-top: 16px; }
.muted { color: var(--muted); }
.ok { color: var(--ok); } .bad { color: var(--bad); }
ul { margin: 0; padding-left: 20px; }
ul.plan { list-style: none; padding-left: 0; }
figure { margin: 0 0 20px; }
figure img { display: block; width: 100%; max-width: 480px; height: auto; border-radius: 6px;
  border: 1px solid var(--line); }
figcaption { margin: 6px 0; font-weight: 600; }
pre { margin: 0; white-space: pre-wrap; overflow-wrap: anywhere; font: 13px/1.5 ui-monospace,
  SFMono-Regular, Menlo, monospace; }
)CSS";

// Uses forward_elsewhere's rules for work.view, which falls back to the journal's files.
json::Value view_from_files(std::string_view root, const std::string& id, bool image,
                            std::int64_t lines) {
    json::Value result;
    result["session"] = id;
    result["live"] = live_entry(root, id).has_value();
    auto journal = read_journal(root, id);
    if (journal) {
        if (json::Value plan = last_plan(journal.value()); !plan.is_null()) {
            result["plan"] = std::move(plan);
        }
        if (json::Value paused = last_pause(journal.value()); !paused.is_null()) {
            result["paused"] = std::move(paused);
        }
        result["recent"] = recent(journal.value(), lines);
    }
    const std::vector<Checkpoint> checkpoints = list_checkpoints(root, id);
    for (const Checkpoint& last : std::ranges::reverse_view(checkpoints)) {
        if (!last.frame) {
            continue;
        }
        json::Value frame;
        frame["checkpoint"] = last.n;
        frame["time"] = last.time;
        result["frame"] = std::move(frame);
        if (image) {
            if (auto png = checkpoint_frame(root, id, last.n); png) {
                result["png"] = base64_encode(png.value());
            }
        }
        break;
    }
    return result;
}

void add_results(std::string_view root, const std::string& id, json::Value& result) {
    const json::Value summed = digest(root, 0, id);
    if (const json::Value* tests = summed.find("tests");
        tests != nullptr && tests->is_object() && !tests->as_object().empty()) {
        result["tests"] = *tests;
    }
    if (const json::Value* checks = summed.find("checks");
        checks != nullptr && checks->is_object() && !checks->as_object().empty()) {
        result["checks"] = *checks;
    }
}

} // namespace

void set_forwarder(Forwarder forwarder) {
    the_forwarder() = std::move(forwarder);
}

std::optional<Reply> forward_elsewhere(Session& session, std::string_view command,
                                       const json::Value& params) {
    const json::Value* named = params.find("session");
    if (named == nullptr || named->as_string() == own_id(session)) {
        return std::nullopt;
    }
    const std::string& id = named->as_string();
    const std::optional<SessionEntry> entry = live_entry(session.options().project_root, id);
    if (!entry) {
        std::string message = "session " + quoted(id) + " isn't running";
        std::vector<std::string> live;
        for (const SessionEntry& one : live_sessions(session.options().project_root)) {
            live.push_back(one.id);
        }
        const std::vector<std::string_view> names(live.begin(), live.end());
        if (const std::string_view hint = closest_match(id, names); !hint.empty()) {
            message += "; did you mean " + quoted(hint) + "?";
        }
        return session_error(std::move(message));
    }
    if (entry->url.empty()) {
        return session_error("session " + quoted(id) +
                             " doesn't serve watchers (project.json's work.serve is off)");
    }
    if (!the_forwarder()) {
        return Reply::failure(code::kPlatformFailure,
                              "this program can't reach other sessions; use slash-cli work or "
                              "slash-mcp --watch");
    }
    json::Value rest = params;
    rest.as_object().erase("session");
    auto reply = the_forwarder()(*entry, command, rest);
    if (!reply) {
        return Reply::failure(code::kPlatformFailure, "could not reach session " + quoted(id) +
                                                          ": " + reply.error().message);
    }
    Reply out = std::move(reply).value();
    if (out.ok && out.value.is_object()) {
        out.value["session"] = id;
    }
    return out;
}

Result<std::string> timeline_strip(std::string_view project_root, std::string_view session,
                                   const std::vector<Checkpoint>& checkpoints, std::uint32_t width,
                                   std::uint32_t columns) {
    if (checkpoints.empty() || width == 0 || columns == 0) {
        return make_error(ErrorCode::InvalidArgument, "no checkpoints to lay out");
    }
    std::vector<std::optional<render::Image>> frames;
    std::uint32_t tile_height = 0;
    for (const Checkpoint& checkpoint : checkpoints) {
        std::optional<render::Image> frame;
        if (checkpoint.frame) {
            if (auto png = checkpoint_frame(project_root, session, checkpoint.n); png) {
                if (auto decoded = render::decode_image(png.value()); decoded) {
                    frame = std::move(decoded).value();
                }
            }
        }
        if (tile_height == 0 && frame && frame->width > 0) {
            tile_height =
                static_cast<std::uint32_t>(std::uint64_t{frame->height} * width / frame->width);
        }
        frames.push_back(std::move(frame));
    }
    if (tile_height == 0) {
        tile_height = width * 9 / 16;
    }
    tile_height = std::max(tile_height, 1U);
    const auto count = static_cast<std::uint32_t>(frames.size());
    const std::uint32_t across = std::min(columns, count);
    const std::uint32_t down = (count + across - 1) / across;
    render::Image sheet;
    sheet.width = (across * width) + ((across + 1) * kTileGap);
    sheet.height = (down * tile_height) + ((down + 1) * kTileGap);
    sheet.pixels.assign(std::size_t{sheet.width} * sheet.height * 4, kSheetGray);
    for (std::size_t i = 3; i < sheet.pixels.size(); i += 4) {
        sheet.pixels[i] = kOpaque;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::optional<render::Image>& frame = frames[i];
        if (!frame.has_value()) {
            continue;
        }
        const std::uint32_t x = kTileGap + ((i % across) * (width + kTileGap));
        const std::uint32_t y = kTileGap + ((i / across) * (tile_height + kTileGap));
        place(sheet, frame.value(), x, y, width, tile_height);
    }
    return render::encode_png(sheet);
}

Result<std::string> write_report(std::string_view project_root, std::string_view session,
                                 std::int64_t since, const world::ComponentRegistry& components) {
    auto lines = read_journal(project_root, session);
    if (!lines) {
        return lines.error();
    }
    const std::string id(session);
    const json::Value summed = digest(project_root, since, id);
    std::string html = "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
                       "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                       "<title>Work report</title><style>";
    html += kReportStyle;
    html += "</style></head><body><main><h1>Work report: ";
    html += escape(id);
    html += "</h1><p class=\"muted\">";
    const auto when = [&summed](std::string_view key) {
        const json::Value& time = at(summed, key);
        if (!time.is_string() || time.as_string().size() < 19) {
            return std::string();
        }
        return time.as_string().substr(0, 10) + " " + time.as_string().substr(11, 8);
    };
    html += escape(when("from"));
    html += " to ";
    html += escape(when("until"));
    html += " UTC";
    html += live_entry(project_root, id) ? ", still running" : "";
    html += "</p>";

    if (const json::Value plan = last_plan(lines.value()); plan.is_object()) {
        html += "<section><h2>The agent's plan</h2><p>";
        html += escape(plan_gist(plan));
        html += "</p><ul class=\"plan\">";
        for (const json::Value& step :
             at(plan, "steps").is_array() ? at(plan, "steps").as_array() : json::Array{}) {
            const bool done = at(step, "done").is_bool() && at(step, "done").as_bool();
            html += done ? "<li class=\"ok\">&#10003; " : "<li>&#9675; ";
            html += escape(at(step, "text").as_string());
            html += "</li>";
        }
        html += "</ul></section>";
    }

    html += "<section><h2>Summary</h2><ul>";
    for (const json::Value& line : at(summed, "lines").as_array()) {
        html += "<li>" + escape(line.as_string()) + "</li>";
    }
    html += "</ul></section>";

    // Checkpoints in the stretch, the latest kReportFrames, each with what changed since the one
    // before it.
    std::vector<Checkpoint> checkpoints;
    for (Checkpoint& checkpoint : list_checkpoints(project_root, id)) {
        const auto time = line_time(json::Object{{"time", json::Value(checkpoint.time)}});
        if (!time || *time >= since) {
            checkpoints.push_back(std::move(checkpoint));
        }
    }
    if (!checkpoints.empty()) {
        html += "<section><h2>Checkpoints</h2>";
        const std::size_t skipped =
            checkpoints.size() > kReportFrames ? checkpoints.size() - kReportFrames : 0;
        if (skipped > 0) {
            html += "<p class=\"muted\">" + std::to_string(skipped) + " earlier " +
                    (skipped == 1 ? "checkpoint" : "checkpoints") + " left out.</p>";
        }
        std::optional<std::int64_t> before;
        if (skipped > 0) {
            before = checkpoints[skipped - 1].n;
        }
        for (std::size_t i = skipped; i < checkpoints.size(); ++i) {
            const Checkpoint& checkpoint = checkpoints[i];
            html += "<figure><figcaption>" + std::to_string(checkpoint.n) + ". " +
                    escape(checkpoint.reason);
            if (!checkpoint.label.empty()) {
                html += ": " + escape(checkpoint.label);
            }
            html += " <span class=\"muted\">" + escape(checkpoint.time.substr(11, 8)) +
                    " UTC</span></figcaption>";
            if (checkpoint.frame) {
                if (auto png = checkpoint_frame(project_root, id, checkpoint.n); png) {
                    std::string bytes = std::move(png).value();
                    if (auto decoded = render::decode_image(bytes);
                        decoded && decoded.value().width > kReportFrameWidth) {
                        const render::Image& frame = decoded.value();
                        const auto height = static_cast<std::uint32_t>(
                            std::uint64_t{frame.height} * kReportFrameWidth / frame.width);
                        if (auto smaller = render::encode_png(
                                render::resize_image(frame, kReportFrameWidth, height));
                            smaller) {
                            bytes = std::move(smaller).value();
                        }
                    }
                    html += R"(<img alt="The world at checkpoint )" + std::to_string(checkpoint.n) +
                            R"(" src="data:image/png;base64,)" + base64_encode(bytes) + "\">";
                }
            }
            if (before) {
                if (auto compared =
                        compare_checkpoints(project_root, id, *before, checkpoint.n, components);
                    compared) {
                    html += changes_html(compared.value());
                }
            } else {
                html += "<p class=\"muted\">The first checkpoint.</p>";
            }
            before = checkpoint.n;
            html += "</figure>";
        }
        html += "</section>";
    }

    // The feed, the latest kReportLines lines.
    std::vector<std::string> feed;
    for (const json::Value& line : lines.value()) {
        if (const auto time = line_time(line); !time || *time >= since) {
            feed.push_back(describe(line));
        }
    }
    html += "<section><h2>What it did</h2>";
    const std::size_t left_out = feed.size() > kReportLines ? feed.size() - kReportLines : 0;
    if (left_out > 0) {
        html += "<p class=\"muted\">" + std::to_string(left_out) + " earlier lines left out.</p>";
    }
    html += "<pre>";
    for (std::size_t i = left_out; i < feed.size(); ++i) {
        html += escape(feed[i]);
        html += '\n';
    }
    html += "</pre></section></main></body></html>\n";

    const std::string folder = join(project_root, kReportsFolder);
    if (auto made = platform::create_directories(folder); !made) {
        return made.error();
    }
    const std::string path = std::string(kReportsFolder) + "/" + id + ".html";
    if (auto written = platform::write_file(join(project_root, path), html); !written) {
        return written.error();
    }
    return path;
}

void register_view_commands(Registry& registry) {
    json::Value session_param;
    session_param["type"] = "string";
    session_param["description"] =
        "The session, as named in .slash/work/sessions.json; default this one, else the newest.";

    json::Value view;
    view["type"] = "object";
    view["additionalProperties"] = false;
    view["properties"]["session"] = session_param;
    view["properties"]["image"]["type"] = "boolean";
    view["properties"]["image"]["default"] = true;
    view["properties"]["image"]["description"] = "The frame as an image.";
    view["properties"]["width"]["type"] = "integer";
    view["properties"]["width"]["minimum"] = 160;
    view["properties"]["width"]["maximum"] = 1280;
    view["properties"]["width"]["default"] = kViewWidth;
    view["properties"]["width"]["description"] = "The frame's width at most, in pixels.";
    view["properties"]["lines"]["type"] = "integer";
    view["properties"]["lines"]["minimum"] = 0;
    view["properties"]["lines"]["maximum"] = 50;
    view["properties"]["lines"]["default"] = kViewLines;
    view["properties"]["lines"]["description"] = "The last lines of the feed to include.";
    registry.add({
        .name = "work.view",
        .description = "A session as it is now, sized for a chat reply: its world drawn as an "
                       "image, its plan, its last commands in plain words, whether it is paused "
                       "and its latest tests and checks. A session that ended shows its last "
                       "checkpoint.",
        .capabilities = {Capability::Read},
        .effect = Effect::Read,
        .params = std::move(view),
        .handler =
            [](Session& session, const json::Value& params) {
                const std::string& root = session.options().project_root;
                const json::Value* named = params.find("session");
                const bool image = params.find("image") == nullptr || at(params, "image").as_bool();
                const std::int64_t lines =
                    params.find("lines") != nullptr ? at(params, "lines").as_int() : kViewLines;
                if (named != nullptr && named->as_string() != own_id(session)) {
                    // Another session: its own world while it runs, else its files.
                    if (const std::optional<SessionEntry> entry =
                            live_entry(root, named->as_string());
                        entry && !entry->url.empty()) {
                        if (std::optional<Reply> forwarded =
                                forward_elsewhere(session, "work.view", params);
                            forwarded && forwarded->ok) {
                            return *std::move(forwarded);
                        }
                    }
                    auto id = pick(session, params);
                    if (!id) {
                        return Reply::failure(id.error());
                    }
                    if (!read_journal(root, id.value())) {
                        return session_error("no session " + quoted(id.value()) +
                                             " has a work journal here");
                    }
                    json::Value result = view_from_files(root, id.value(), image, lines);
                    add_results(root, id.value(), result);
                    return Reply::success(std::move(result));
                }
                // This session: its world as it is now.
                json::Value result;
                const std::string id = own_id(session);
                if (!id.empty()) {
                    session.options().journal->flush();
                    result["session"] = id;
                }
                result["live"] = true;
                if (image) {
                    const auto width = static_cast<double>(params.find("width") != nullptr
                                                               ? at(params, "width").as_int()
                                                               : kViewWidth);
                    const Vec2 viewport = ui::settings(session.world()).viewport;
                    const double scale = viewport.x > width ? width / viewport.x : 1.0;
                    if (auto captured = canvas::capture(session.world(), session.assets(),
                                                        {.scale = static_cast<float>(scale)});
                        captured) {
                        json::Value frame;
                        frame["width"] = static_cast<std::int64_t>(captured.value().image.width);
                        frame["height"] = static_cast<std::int64_t>(captured.value().image.height);
                        result["frame"] = std::move(frame);
                        if (auto encoded = render::encode_png(captured.value().image); encoded) {
                            result["png"] = base64_encode(encoded.value());
                        }
                    }
                }
                if (session.steering().plan.is_object()) {
                    result["plan"] = session.steering().plan;
                }
                if (session.steering().paused.is_object()) {
                    result["paused"] = session.steering().paused;
                }
                if (!id.empty()) {
                    if (auto journal = read_journal(root, id); journal) {
                        result["recent"] = recent(journal.value(), lines);
                    }
                    add_results(root, id, result);
                }
                return Reply::success(std::move(result));
            },
    });

    json::Value timeline;
    timeline["type"] = "object";
    timeline["additionalProperties"] = false;
    timeline["properties"]["session"] = session_param;
    timeline["properties"]["after"]["type"] = "integer";
    timeline["properties"]["after"]["minimum"] = 0;
    timeline["properties"]["after"]["description"] = "Only checkpoints after this one.";
    timeline["properties"]["limit"]["type"] = "integer";
    timeline["properties"]["limit"]["minimum"] = 1;
    timeline["properties"]["limit"]["maximum"] = 48;
    timeline["properties"]["limit"]["default"] = 12;
    timeline["properties"]["limit"]["description"] =
        "The most checkpoints; the first after `after`, else the last ones.";
    timeline["properties"]["width"]["type"] = "integer";
    timeline["properties"]["width"]["minimum"] = 64;
    timeline["properties"]["width"]["maximum"] = 480;
    timeline["properties"]["width"]["default"] = 192;
    timeline["properties"]["width"]["description"] = "Each frame's width, in pixels.";
    timeline["properties"]["columns"]["type"] = "integer";
    timeline["properties"]["columns"]["minimum"] = 1;
    timeline["properties"]["columns"]["maximum"] = 8;
    timeline["properties"]["columns"]["default"] = 4;
    timeline["properties"]["columns"]["description"] = "Frames in a row.";
    registry.add({
        .name = "work.timeline",
        .description = "A session's checkpoints as one image, their frames in rows oldest first, "
                       "with what each was (its number, why it was taken and when).",
        .capabilities = {Capability::Read},
        .effect = Effect::Read,
        .params = std::move(timeline),
        .handler =
            [](Session& session, const json::Value& params) {
                auto id = pick(session, params);
                if (!id) {
                    return Reply::failure(id.error());
                }
                const std::string& root = session.options().project_root;
                const json::Value* after = params.find("after");
                const json::Value* limit = params.find("limit");
                const auto most = static_cast<std::size_t>(limit != nullptr ? limit->as_int() : 12);
                std::vector<Checkpoint> all;
                for (Checkpoint& checkpoint : list_checkpoints(root, id.value())) {
                    if (after == nullptr || checkpoint.n > after->as_int()) {
                        all.push_back(std::move(checkpoint));
                    }
                }
                if (all.empty()) {
                    return Reply::failure(code::kNotFound,
                                          "session " + id.value() + " has no checkpoints" +
                                              (after != nullptr ? " after that one" : " yet"));
                }
                std::size_t from = 0;
                std::size_t to = all.size();
                if (after != nullptr) {
                    to = std::min(all.size(), most);
                } else if (all.size() > most) {
                    from = all.size() - most;
                }
                const std::vector<Checkpoint> picked(all.begin() +
                                                         static_cast<std::ptrdiff_t>(from),
                                                     all.begin() + static_cast<std::ptrdiff_t>(to));
                const json::Value* width = params.find("width");
                const json::Value* columns = params.find("columns");
                auto strip = timeline_strip(
                    root, id.value(), picked,
                    static_cast<std::uint32_t>(width != nullptr ? width->as_int() : 192),
                    static_cast<std::uint32_t>(columns != nullptr ? columns->as_int() : 4));
                if (!strip) {
                    return Reply::failure(strip.error());
                }
                json::Array listed;
                for (const Checkpoint& checkpoint : picked) {
                    json::Value entry;
                    entry["n"] = checkpoint.n;
                    entry["reason"] = checkpoint.reason;
                    if (!checkpoint.label.empty()) {
                        entry["label"] = checkpoint.label;
                    }
                    entry["time"] = checkpoint.time;
                    entry["files"] = static_cast<std::int64_t>(checkpoint.files.size() +
                                                               checkpoint.removed.size());
                    listed.push_back(std::move(entry));
                }
                json::Value result;
                result["session"] = id.value();
                result["checkpoints"] = std::move(listed);
                result["next"] = picked.back().n;
                result["more"] = to < all.size();
                result["png"] = base64_encode(strip.value());
                return Reply::success(std::move(result));
            },
    });

    json::Value report;
    report["type"] = "object";
    report["additionalProperties"] = false;
    report["properties"]["session"] = session_param;
    report["properties"]["since"]["type"] = "string";
    report["properties"]["since"]["description"] =
        "How far back, such as 3h or a UTC time; default the whole session.";
    registry.add({
        .name = "work.report",
        .description = "Writes one self-contained HTML page of a session's work to hand on, "
                       "such as an artifact or a file in a chat: its plan, a summary, tests and "
                       "checks, each checkpoint's frame and changes, and the feed.",
        .capabilities = {Capability::Read},
        .effect = Effect::Read,
        .params = std::move(report),
        .handler =
            [](Session& session, const json::Value& params) {
                auto id = pick(session, params);
                if (!id) {
                    return Reply::failure(id.error());
                }
                std::int64_t since = 0;
                if (const json::Value* text = params.find("since"); text != nullptr) {
                    auto parsed = parse_since(text->as_string());
                    if (!parsed) {
                        json::Value data;
                        data["path"] = "/since";
                        return Reply::failure(code::kInvalidParams,
                                              "/since: " + parsed.error().message, std::move(data));
                    }
                    since = parsed.value();
                }
                const std::string& root = session.options().project_root;
                auto written = write_report(root, id.value(), since, session.components());
                if (!written) {
                    if (written.error().code == ErrorCode::NotFound) {
                        return session_error(written.error().message);
                    }
                    return Reply::failure(written.error());
                }
                json::Value result;
                result["session"] = id.value();
                result["path"] = written.value();
                if (const auto stamp = platform::file_stamp(join(root, written.value()))) {
                    result["bytes"] = static_cast<std::int64_t>(stamp->size);
                }
                return Reply::success(std::move(result));
            },
    });
}

} // namespace slash::command::work
