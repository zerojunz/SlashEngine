#include "slash/server/watch.hpp"

#include "slash/command/session.hpp"
#include "slash/core/log.hpp"
#include "slash/platform/process.hpp"

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

} // namespace slash::server
