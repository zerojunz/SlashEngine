#include "slash/platform/file.hpp"

#include "slash/core/text.hpp"

#include <SDL3/SDL.h>

#include <algorithm>

namespace slash::platform {
namespace {

SDL_EnumerationResult SDLCALL collect_name(void* userdata, const char* /*dirname*/,
                                           const char* name) {
    static_cast<std::vector<std::string>*>(userdata)->emplace_back(name);
    return SDL_ENUM_CONTINUE;
}

} // namespace

Result<std::string> read_file(std::string_view path) {
    const std::string name(path);
    std::size_t size = 0;
    void* data = SDL_LoadFile(name.c_str(), &size);
    if (data == nullptr) {
        return make_error(ErrorCode::NotFound,
                          "could not read \"" + name + "\": " + SDL_GetError());
    }
    std::string contents(static_cast<const char*>(data), size);
    SDL_free(data);
    return contents;
}

Result<std::string> read_file_in(std::string_view folder, std::string_view path) {
    if (!reserved_device_name(path).empty()) {
        return reserved_device_error(path);
    }
    std::string full(folder);
    if (full.empty() || full == ".") {
        full = path;
    } else {
        if (full.back() != '/' && full.back() != '\\') {
            full += '/';
        }
        full += path;
    }
    auto text = read_file(full);
    if (text || exists(full)) {
        return text;
    }
    std::string message = "no such file";
    // Files of the same type are the likely meant ones: "con.prefab.json" for "coin.prefab.json".
    const std::size_t name = path.rfind('/') == std::string_view::npos ? 0 : path.rfind('/') + 1;
    const std::size_t dot = path.find('.', name);
    const std::string_view suffix = dot == std::string_view::npos ? "" : path.substr(dot);
    auto files = list_files(folder.empty() ? "." : folder,
                            [](std::string_view subfolder) { return subfolder != "build/"; });
    if (files) {
        std::vector<std::string_view> candidates;
        for (const std::string& file : files.value()) {
            if (file.ends_with(suffix)) {
                candidates.emplace_back(file);
            }
        }
        if (const std::string_view hint = closest_match(path, candidates); !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        }
    }
    return make_error(ErrorCode::NotFound, std::move(message));
}

std::string_view reserved_device_name(std::string_view path) {
    std::size_t start = 0;
    while (start < path.size()) {
        std::size_t end = path.find_first_of("/\\", start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        const std::string_view part = path.substr(start, end - start);
        const std::string_view stem = part.substr(0, part.find('.'));
        std::string lower(stem);
        std::ranges::transform(lower, lower.begin(), [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        });
        const bool numbered = lower.size() == 4 &&
                              (lower.starts_with("com") || lower.starts_with("lpt")) &&
                              lower[3] >= '1' && lower[3] <= '9';
        if (numbered || lower == "con" || lower == "prn" || lower == "aux" || lower == "nul") {
            return stem;
        }
        start = end + 1;
    }
    return {};
}

Error reserved_device_error(std::string_view path) {
    return make_error(ErrorCode::InvalidArgument,
                      "path \"" + std::string(path) + "\" uses \"" +
                          std::string(reserved_device_name(path)) +
                          "\", a name Windows keeps for a device; choose another name");
}

Result<void> write_file(std::string_view path, std::string_view contents) {
    const std::string name(path);
    if (!SDL_SaveFile(name.c_str(), contents.data(), contents.size())) {
        return make_error(ErrorCode::PlatformFailure,
                          "could not write \"" + name + "\": " + SDL_GetError());
    }
    return {};
}

Result<void> create_directories(std::string_view path) {
    const std::string name(path);
    if (!SDL_CreateDirectory(name.c_str())) {
        return make_error(ErrorCode::PlatformFailure,
                          "could not create the directory \"" + name + "\": " + SDL_GetError());
    }
    return {};
}

bool exists(std::string_view path) {
    const std::string name(path);
    return SDL_GetPathInfo(name.c_str(), nullptr);
}

std::optional<FileStamp> file_stamp(std::string_view path) {
    const std::string name(path);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(name.c_str(), &info) || info.type != SDL_PATHTYPE_FILE) {
        return std::nullopt;
    }
    return FileStamp{.modified = info.modify_time, .size = info.size};
}

Result<std::vector<std::string>> list_files(std::string_view folder,
                                            const std::function<bool(std::string_view)>& descend) {
    std::string base(folder);
    if (!base.empty() && base.back() != '/' && base.back() != '\\') {
        base += '/';
    }
    std::vector<std::string> files;
    std::vector<std::string> pending = {""};
    while (!pending.empty()) {
        const std::string relative = std::move(pending.back());
        pending.pop_back();
        const std::string directory = base + relative;
        std::vector<std::string> names;
        if (!SDL_EnumerateDirectory(directory.empty() ? "." : directory.c_str(), collect_name,
                                    &names)) {
            return make_error(ErrorCode::NotFound,
                              "could not list \"" + directory + "\": " + SDL_GetError());
        }
        for (const std::string& name : names) {
            if (name.starts_with('.')) {
                continue;
            }
            const std::string path = relative + name;
            SDL_PathInfo info;
            if (!SDL_GetPathInfo((base + path).c_str(), &info)) {
                continue;
            }
            if (info.type == SDL_PATHTYPE_DIRECTORY) {
                if (std::string sub = path + "/"; descend == nullptr || descend(sub)) {
                    pending.push_back(std::move(sub));
                }
            } else if (info.type == SDL_PATHTYPE_FILE) {
                files.push_back(path);
            }
        }
    }
    std::ranges::sort(files);
    return files;
}

std::string base_path() {
    const char* path = SDL_GetBasePath();
    return path == nullptr ? std::string() : std::string(path);
}

} // namespace slash::platform
