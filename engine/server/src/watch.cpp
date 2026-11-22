#include "slash/server/watch.hpp"

#include "slash/command/session.hpp"
#include "slash/core/log.hpp"
#include "slash/platform/process.hpp"
#include "slash/server/client.hpp"

#include <charconv>
#include <chrono>
#include <iostream>
#include <utility>

namespace slash::server {

Watched::Watched(std::string project_root, std::unique_ptr<Server> server,
                 command::work::SessionEntry entry)
    : project_root_(std::move(project_root)), server_(std::move(server)), entry_(std::move(entry)) {
}

Result<std::unique_ptr<Watched>> Watched::start(command::Session& session, std::string id,
                                                std::string host, std::string journal) {
    ServerOptions options;
    options.watch_only = true;
    options.watch_session = id;
    auto server = Server::start(session, options);
    if (!server) {
        return server.error();
    }
    command::work::SessionEntry entry;
    entry.id = std::move(id);
    entry.host = std::move(host);
    entry.pid = platform::process_id();
    entry.url = server.value()->endpoint().url();
    entry.token = server.value()->endpoint().token;
    entry.journal = std::move(journal);
    entry.started =
        command::work::format_time(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
    const std::string& root = session.options().project_root;
    if (auto listed = command::work::add_session(root, entry); !listed) {
        return listed.error();
    }
    return std::unique_ptr<Watched>(new Watched(root, std::move(server).value(), std::move(entry)));
}

Watched::~Watched() {
    server_->stop();
    (void)command::work::remove_session(project_root_, entry_.id);
}

void Watched::poll(std::chrono::milliseconds wait) {
    server_->poll(wait);
}

Work::Work(std::string project_root, std::string host)
    : project_root_(std::move(project_root)), host_(std::move(host)),
      settings_(command::work::read_settings(project_root_)) {
    if (!settings_.journal) {
        return;
    }
    command::work::JournalOptions options;
    options.project_root = project_root_;
    options.host = host_;
    options.max_bytes = settings_.max_bytes;
    options.checkpoints = settings_.checkpoints;
    auto opened = command::work::Journal::open(std::move(options));
    if (!opened) {
        log::warn("no work journal for this session", {{"detail", opened.error().message}});
        return;
    }
    journal_ = std::move(opened).value();
}

Work::~Work() {
    // The server first, as it runs requests on the session the journal belongs to.
    watched_.reset();
    journal_.reset();
}

void Work::watch(command::Session& session) {
    if (!settings_.serve) {
        return;
    }
    std::string id =
        journal_ != nullptr ? journal_->id() : host_ + "-" + std::to_string(platform::process_id());
    auto started = Watched::start(session, std::move(id), host_,
                                  journal_ != nullptr ? journal_->path() : std::string());
    if (!started) {
        log::warn("this session can't be watched", {{"detail", started.error().message}});
        return;
    }
    watched_ = std::move(started).value();
}

void Work::poll() {
    if (watched_ != nullptr) {
        watched_->poll();
    }
    if (journal_ != nullptr) {
        journal_->idle();
    }
}

StdinLines::StdinLines() : shared_(std::make_shared<Shared>()) {
    reader_ = std::thread([shared = shared_] {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const std::scoped_lock lock(shared->mutex);
            shared->lines.push_back(std::move(line));
            shared->arrived.notify_one();
        }
        const std::scoped_lock lock(shared->mutex);
        shared->ended = true;
        shared->arrived.notify_one();
    });
}

StdinLines::~StdinLines() {
    bool ended = false;
    {
        const std::scoped_lock lock(shared_->mutex);
        ended = shared_->ended;
    }
    if (ended) {
        reader_.join();
    } else {
        reader_.detach();
    }
}

std::optional<std::string> StdinLines::next(std::chrono::milliseconds wait) {
    std::unique_lock lock(shared_->mutex);
    shared_->arrived.wait_for(lock, wait,
                              [&] { return !shared_->lines.empty() || shared_->ended; });
    if (shared_->lines.empty()) {
        return std::nullopt;
    }
    std::string line = std::move(shared_->lines.front());
    shared_->lines.pop_front();
    return line;
}

bool StdinLines::ended() const {
    const std::scoped_lock lock(shared_->mutex);
    return shared_->ended && shared_->lines.empty();
}

namespace {

// A watch server's endpoint from its sessions.json entry ("http://127.0.0.1:port").
std::optional<Endpoint> endpoint_of(const command::work::SessionEntry& entry) {
    constexpr std::string_view kScheme = "http://";
    std::string_view rest = entry.url;
    if (!rest.starts_with(kScheme) || entry.token.empty()) {
        return std::nullopt;
    }
    rest.remove_prefix(kScheme.size());
    const std::size_t colon = rest.rfind(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    int port = 0;
    const std::string_view digits = rest.substr(colon + 1);
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), port);
    if (parsed.ec != std::errc{} || port <= 0) {
        return std::nullopt;
    }
    Endpoint endpoint;
    endpoint.host = std::string(rest.substr(0, colon));
    endpoint.port = port;
    endpoint.token = entry.token;
    return endpoint;
}

} // namespace

Follower::Follower(std::string project_root, std::string id, bool world)
    : project_root_(std::move(project_root)), id_(std::move(id)), world_(world) {}

Follower::~Follower() {
    if (!fetcher_.joinable()) {
        return;
    }
    bool done = false;
    {
        const std::scoped_lock lock(fetch_->mutex);
        done = fetch_->done;
    }
    if (done) {
        fetcher_.join();
    } else {
        fetcher_.detach();
    }
}

Follower::Update Follower::poll(std::chrono::milliseconds refresh) {
    Update update;
    if (ended_) {
        update.ended = true;
        return update;
    }
    // The journal, read again when its files grow.
    std::uint64_t bytes = 0;
    for (const command::work::JournalFile& file : command::work::list_journals(project_root_)) {
        if (file.id == id_) {
            bytes += file.bytes;
        }
    }
    if (bytes != bytes_ || !read_once_) {
        bytes_ = bytes;
        read_once_ = true;
        if (auto read = command::work::read_journal(project_root_, id_); read) {
            for (json::Value& line : read.value()) {
                const json::Value* number = line.find("n");
                if (number == nullptr || !number->is_int() || number->as_int() <= seen_) {
                    continue;
                }
                seen_ = number->as_int();
                ended_ = ended_ || line.find("end") != nullptr;
                update.lines.push_back(std::move(line));
            }
        }
        wanted_ = wanted_ || !update.lines.empty();
    }
    entry_.reset();
    for (command::work::SessionEntry& live : command::work::live_sessions(project_root_)) {
        if (live.id == id_) {
            entry_ = std::move(live);
        }
    }
    if (!entry_ && update.lines.empty()) {
        // The process ended without its last line, or the session never served.
        ended_ = true;
    }

    // The world: the answer to the last request, then another if one is due.
    if (fetcher_.joinable()) {
        std::optional<std::string> body;
        bool done = false;
        {
            const std::scoped_lock lock(fetch_->mutex);
            done = fetch_->done;
            body = std::move(fetch_->body);
        }
        if (!done) {
            update.ended = ended_;
            return update;
        }
        fetcher_.join();
        if (body && *body != scene_text_) {
            scene_text_ = std::move(*body);
            if (auto parsed = json::parse(scene_text_); parsed) {
                if (const json::Value* scene = parsed.value().find("scene");
                    scene != nullptr && scene->is_object()) {
                    update.scene = *scene;
                }
            }
        }
    }
    const auto now = std::chrono::steady_clock::now();
    const bool due = wanted_ || (refresh.count() > 0 && now - fetched_ >= refresh);
    const std::optional<Endpoint> endpoint = entry_ ? endpoint_of(*entry_) : std::nullopt;
    if (world_ && due && endpoint && !ended_) {
        wanted_ = false;
        fetched_ = now;
        fetch_ = std::make_shared<Fetch>();
        fetcher_ = std::thread([fetch = fetch_, endpoint = *endpoint] {
            HttpRequest request;
            request.path = "/v1/commands/scene.get";
            request.body = "{}";
            auto response = send(endpoint, request);
            const std::scoped_lock lock(fetch->mutex);
            if (response && response.value().status == 200) {
                fetch->body = std::move(response.value().body);
            }
            fetch->done = true;
        });
    }
    update.ended = ended_;
    return update;
}

} // namespace slash::server
