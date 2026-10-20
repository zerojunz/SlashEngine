#include "common.hpp"

#include "slash/storyboard/storyboard.hpp"

#include <charconv>
#include <set>
#include <system_error>

namespace slash::importer {
namespace {

bool is_upper(char c) {
    return c >= 'A' && c <= 'Z';
}
bool is_lower(char c) {
    return c >= 'a' && c <= 'z';
}
bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

// An array index in a JSON Pointer, or nullopt.
std::optional<std::size_t> pointer_index(std::string_view part) {
    std::size_t index = 0;
    const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), index);
    if (part.empty() || error != std::errc() || end != part.data() + part.size()) {
        return std::nullopt;
    }
    return index;
}

} // namespace

// --- Paths and names ---

std::string lowercase(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (is_upper(c)) {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::size_t extension_start(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
        return path.size();
    }
    return dot;
}

std::string extension_of(std::string_view path) {
    return lowercase(path.substr(extension_start(path)));
}

std::string without_extension(std::string_view path) {
    return std::string(path.substr(0, extension_start(path)));
}

std::string file_stem(std::string_view path) {
    const std::string bare = without_extension(path);
    const std::size_t slash = bare.find_last_of('/');
    return slash == std::string::npos ? bare : bare.substr(slash + 1);
}

std::string snake_case(std::string_view name, std::size_t limit) {
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (is_upper(c)) {
            const char previous = i > 0 ? name[i - 1] : '\0';
            const char next = i + 1 < name.size() ? name[i + 1] : '\0';
            const bool word = is_lower(previous) || (is_upper(previous) && is_lower(next));
            if (word && !out.empty() && out.back() != '_') {
                out += '_';
            }
            out += static_cast<char>(c - 'A' + 'a');
        } else if (is_lower(c) || is_digit(c) || c == '-') {
            out += c;
        } else if (!out.empty() && out.back() != '_') {
            out += '_';
        }
    }
    if (out.size() > limit) {
        out.resize(limit);
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    return out.empty() ? "node" : out;
}

std::optional<std::int64_t> parse_int(std::string_view text) {
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::string_view> split_path(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find(separator, start), text.size());
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

std::string join(const std::vector<std::string>& items, std::string_view separator) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) {
            out += separator;
        }
        out += item;
    }
    return out;
}

bool is_texture_file(std::string_view extension) {
    return extension == ".png" || extension == ".bmp";
}
bool is_sound_file(std::string_view extension) {
    return extension == ".wav" || extension == ".ogg" || extension == ".mp3";
}
bool is_font_file(std::string_view extension) {
    return extension == ".ttf" || extension == ".otf";
}
bool is_data_file(std::string_view extension) {
    return extension == ".json" || extension == ".txt" || extension == ".csv";
}

std::string unsupported_asset(std::string_view extension) {
    if (extension == ".svg" || extension == ".jpg" || extension == ".jpeg" ||
        extension == ".webp" || extension == ".tga" || extension == ".exr" || extension == ".hdr" ||
        extension == ".ktx" || extension == ".dds" || extension == ".psd" || extension == ".psb" ||
        extension == ".tif" || extension == ".tiff" || extension == ".gif") {
        return "textures are PNG or BMP files; save it as PNG";
    }
    if (extension == ".woff" || extension == ".woff2" || extension == ".fnt" ||
        extension == ".pfb" || extension == ".pfm") {
        return "fonts are TTF or OTF files";
    }
    if (extension == ".glb" || extension == ".gltf" || extension == ".obj" || extension == ".fbx" ||
        extension == ".blend" || extension == ".dae" || extension == ".3ds" ||
        extension == ".max" || extension == ".ma" || extension == ".mb") {
        return "3D comes with milestone 7";
    }
    if (extension == ".ogv" || extension == ".mp4" || extension == ".mov" || extension == ".webm" ||
        extension == ".avi") {
        return "video isn't supported yet";
    }
    if (extension == ".aif" || extension == ".aiff" || extension == ".flac" ||
        extension == ".mod" || extension == ".it" || extension == ".s3m" || extension == ".xm") {
        return "sounds are WAV, Ogg Vorbis or MP3 files";
    }
    if (extension == ".audioclip") {
        return "a sound left in Unity's own format, which doesn't decode (web builds keep sounds "
               "as AAC); sounds are WAV, Ogg Vorbis or MP3 files";
    }
    return {};
}

std::optional<std::pair<int, int>> png_size(std::string_view bytes) {
    constexpr std::size_t kHeader = 24;
    if (bytes.size() < kHeader || bytes.substr(1, 3) != "PNG" || bytes.substr(12, 4) != "IHDR") {
        return std::nullopt;
    }
    const auto read = [&](std::size_t at) {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            value = (value << 8U) | static_cast<unsigned char>(bytes[at + i]);
        }
        return static_cast<int>(value);
    };
    return std::pair{read(16), read(20)};
}

bool remove_pointer(json::Value& root, std::string_view pointer) {
    if (pointer.empty() || pointer.front() != '/') {
        return false;
    }
    std::vector<std::string> parts;
    std::size_t start = 1;
    while (start <= pointer.size()) {
        const std::size_t end = std::min(pointer.find('/', start), pointer.size());
        std::string part(pointer.substr(start, end - start));
        for (std::size_t at = part.find('~'); at != std::string::npos;
             at = part.find('~', at + 1)) {
            part.replace(at, 2, part.compare(at, 2, "~1") == 0 ? "/" : "~");
        }
        parts.push_back(std::move(part));
        start = end + 1;
    }
    json::Value* parent = &root;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (parent->is_object()) {
            parent = parent->find(parts[i]);
        } else if (const auto index = pointer_index(parts[i]); parent->is_array() && index) {
            parent = *index < parent->as_array().size() ? &parent->as_array()[*index] : nullptr;
        } else {
            parent = nullptr;
        }
        if (parent == nullptr) {
            return false;
        }
    }
    const std::string& last = parts.back();
    if (parent->is_object()) {
        return parent->as_object().erase(last) > 0;
    }
    if (const auto index = pointer_index(last); parent->is_array() && index) {
        auto& items = parent->as_array();
        if (*index < items.size()) {
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(*index));
            return true;
        }
    }
    return false;
}

// --- The destination ---

std::string gameplay_folder(const std::optional<std::string>& project) {
    if (project) {
        if (auto parsed = json::parse(*project); parsed) {
            const json::Value* sources = parsed.value().find("gameplay");
            sources = sources != nullptr ? sources->find("sources") : nullptr;
            if (sources != nullptr && sources->is_array()) {
                for (const json::Value& source : sources->as_array()) {
                    if (!source.is_string() || !extension_of(source.as_string()).empty()) {
                        continue;
                    }
                    std::string folder = source.as_string();
                    while (folder.size() > 1 && folder.back() == '/') {
                        folder.pop_back();
                    }
                    if (!folder.empty() && folder != "." && !folder.starts_with("..")) {
                        return folder;
                    }
                }
            }
        }
    }
    return "src";
}

// --- Reports ---

void Problems::add(const std::string& what, const std::string& where, const std::string& why) {
    Entry& entry = entries_[what];
    if (entry.count++ == 0) {
        entry.where = where;
        entry.why = why;
    }
}

json::Array Problems::to_json() const {
    std::vector<std::pair<std::string, const Entry*>> sorted;
    sorted.reserve(entries_.size());
    for (const auto& [what, entry] : entries_) {
        sorted.emplace_back(what, &entry);
    }
    std::ranges::stable_sort(
        sorted, [](const auto& a, const auto& b) { return a.second->count > b.second->count; });
    json::Array out;
    for (const auto& [what, entry] : sorted) {
        json::Value item;
        item["what"] = what;
        item["count"] = entry->count;
        item["where"] = entry->where;
        if (!entry->why.empty()) {
            item["why"] = entry->why;
        }
        out.push_back(std::move(item));
    }
    return out;
}

// --- Storyboard tasks ---

PortTasks write_port_tasks(const std::optional<std::string>& existing,
                           const std::vector<PortTask>& tasks) {
    json::Object entries;
    if (existing) {
        if (auto parsed = json::parse(*existing); parsed && parsed.value().is_object()) {
            if (const json::Value* list = parsed.value().find("entries");
                list != nullptr && list->is_object()) {
                entries = list->as_object();
            }
        }
    }
    std::set<std::string> taken;
    for (const auto& [id, entry] : entries) {
        taken.insert(id);
    }
    PortTasks out;
    for (const PortTask& port : tasks) {
        std::string id;
        for (const auto& [existing_id, entry] : entries) {
            const json::Value* links = entry.find("links");
            if (links != nullptr && links->is_array() && !links->as_array().empty() &&
                links->as_array().front().is_string() &&
                links->as_array().front().as_string() == port.link) {
                id = existing_id; // imported before: update it, keeping its status
            }
        }
        if (id.empty()) {
            std::string base = "task/port-" + snake_case(file_stem(port.link), 56);
            std::ranges::replace(base, '_', '-');
            id = base;
            for (int n = 2; taken.contains(id); ++n) {
                id = base + "-" + std::to_string(n);
            }
            taken.insert(id);
        }
        out.ids.push_back(id);
        json::Value task = entries.contains(id) ? entries[id] : json::Value(json::Object{});
        task["name"] = port.name;
        task["summary"] = port.summary;
        task["text"] = port.text;
        json::Array links{port.link};
        for (const std::string& link : port.links) {
            if (!contains(links, json::Value(link))) {
                links.emplace_back(link);
            }
        }
        task["links"] = std::move(links);
        task["tags"] = json::Array{port.engine, "import"};
        if (task.find("status") == nullptr) {
            task["status"] = "open";
        }
        entries[id] = std::move(task);
    }
    if (!tasks.empty()) {
        out.file = storyboard::format_file(entries);
    }
    return out;
}

} // namespace slash::importer
