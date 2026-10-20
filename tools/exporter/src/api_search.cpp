// api.search: finds C++ functions, types and macros by name or by what they do, in the engine's
// public headers and the project's gameplay sources, read as text with outline_cpp. Each result
// carries its signature, its doc comment, where it is declared, and the lines of the engine's
// examples (built and run by its tests) or of the project's other files that use it, so an agent
// writes calls that compile instead of guessing names.
#include "slash/command/cpp_outline.hpp"
#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/exporter/commands.hpp"
#include "slash/exporter/export.hpp"
#include "slash/runtime/project.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace slash::exporter {
namespace {

namespace fs = std::filesystem;
using command::CppOutline;
using command::CppSymbol;
using command::Reply;

constexpr std::size_t kMaxCode = 160; // characters of an example line

struct SourceFile {
    std::string path;    // as results show it: from the engine folder, or from the project
    std::string include; // how code includes it ("slash/world/world.hpp"), or ""
    std::vector<std::string> lines;
    // The lines with comments and what string literals hold turned into spaces, for finding
    // names in code.
    std::vector<std::string> code;
    CppOutline outline;
};

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        lines.push_back(std::move(line));
    }
    return lines;
}

// Text with comments and the insides of string and character literals (raw ones included)
// turned into spaces, keeping line breaks and the quotes.
std::string blank_comments_and_strings(std::string text) {
    const auto blank = [&](std::size_t from, std::size_t to) {
        for (std::size_t k = from; k < to && k < text.size(); ++k) {
            if (text[k] != '\n') {
                text[k] = ' ';
            }
        }
    };
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        const char next = i + 1 < text.size() ? text[i + 1] : '\0';
        if (c == '/' && next == '/') {
            const std::size_t end = std::min(text.find('\n', i), text.size());
            blank(i, end);
            i = end;
        } else if (c == '/' && next == '*') {
            const std::size_t close = text.find("*/", i + 2);
            const std::size_t end = close == std::string::npos ? text.size() : close + 2;
            blank(i, end);
            i = end;
        } else if (c == 'R' && next == '"' &&
                   (i == 0 || std::isalnum(static_cast<unsigned char>(text[i - 1])) == 0 ||
                    text[i - 1] == '8' || text[i - 1] == 'u' || text[i - 1] == 'U' ||
                    text[i - 1] == 'L')) {
            const std::size_t open = text.find('(', i + 2);
            if (open == std::string::npos) {
                ++i;
                continue;
            }
            const std::string close = ")" + text.substr(i + 2, open - i - 2) + "\"";
            const std::size_t found = text.find(close, open + 1);
            const std::size_t end = found == std::string::npos ? text.size() : found;
            blank(open, end);
            i = found == std::string::npos ? text.size() : found + close.size();
        } else if (c == '"' ||
                   (c == '\'' &&
                    (i == 0 || std::isalnum(static_cast<unsigned char>(text[i - 1])) == 0))) {
            std::size_t k = i + 1;
            while (k < text.size() && text[k] != c && text[k] != '\n') {
                k += text[k] == '\\' ? std::size_t{2} : std::size_t{1};
            }
            blank(i + 1, k);
            i = k + 1;
        } else {
            ++i;
        }
    }
    return text;
}

// Files read before, kept while they are unchanged, so a session's searches after the first
// read only what changed.
class FileCache {
public:
    std::shared_ptr<const SourceFile> get(const fs::path& file, std::string path,
                                          std::string include) {
        std::error_code error;
        const auto time = fs::last_write_time(file, error);
        const auto size = fs::file_size(file, error);
        if (error) {
            return nullptr;
        }
        const std::string key = file.string();
        {
            const std::scoped_lock lock(mutex_);
            if (const auto found = files_.find(key);
                found != files_.end() && found->second.time == time && found->second.size == size &&
                found->second.file->path == path) {
                return found->second.file;
            }
        }
        const std::ifstream in(file, std::ios::binary);
        if (!in) {
            return nullptr;
        }
        std::ostringstream text;
        text << in.rdbuf();
        auto read = std::make_shared<SourceFile>();
        read->path = std::move(path);
        read->include = std::move(include);
        read->outline = command::outline_cpp(text.str());
        read->lines = split_lines(text.str());
        read->code = split_lines(blank_comments_and_strings(text.str()));
        const std::scoped_lock lock(mutex_);
        files_[key] = {time, size, read};
        return read;
    }

private:
    struct Cached {
        fs::file_time_type time;
        std::uintmax_t size = 0;
        std::shared_ptr<const SourceFile> file;
    };
    std::mutex mutex_;
    std::map<std::string, Cached> files_;
};

FileCache& cache() {
    static FileCache files;
    return files;
}

bool source_extension(const fs::path& file, bool headers_only) {
    const std::string extension = file.extension().string();
    if (extension == ".hpp" || extension == ".h" || extension == ".hh") {
        return true;
    }
    return !headers_only && (extension == ".cpp" || extension == ".cc" || extension == ".cxx");
}

// Every C++ file under a folder, sorted.
std::vector<fs::path> files_under(const fs::path& folder, bool headers_only) {
    std::vector<fs::path> files;
    std::error_code error;
    for (auto it = fs::recursive_directory_iterator(folder, error);
         !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (it->is_regular_file(error) && source_extension(it->path(), headers_only)) {
            files.push_back(it->path());
        }
    }
    std::ranges::sort(files);
    return files;
}

std::string path_from(const fs::path& file, const fs::path& root) {
    return file.lexically_relative(root).generic_string();
}

// The engine's public headers (engine/<module>/include/...) and its examples' code.
void read_engine(const fs::path& engine, std::vector<std::shared_ptr<const SourceFile>>& headers,
                 std::vector<std::shared_ptr<const SourceFile>>& examples) {
    std::error_code error;
    std::vector<fs::path> modules;
    for (const auto& entry : fs::directory_iterator(engine / "engine", error)) {
        modules.push_back(entry.path());
    }
    std::ranges::sort(modules);
    for (const fs::path& module : modules) {
        const fs::path include = module / "include";
        for (const fs::path& file : files_under(include, true)) {
            if (auto read = cache().get(file, path_from(file, engine), path_from(file, include))) {
                headers.push_back(std::move(read));
            }
        }
    }
    for (const fs::path& file : files_under(engine / "examples", false)) {
        if (auto read = cache().get(file, path_from(file, engine), {})) {
            examples.push_back(std::move(read));
        }
    }
}

// The folders project.json's gameplay.sources names, read even when the rest of the file has
// problems.
std::vector<std::string> source_folders(const std::string& root) {
    std::vector<std::string> folders;
    const std::ifstream in(fs::path(root) / runtime::kProjectFile, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    auto project = json::parse(text.str());
    if (!project || !project.value().is_object()) {
        return folders;
    }
    const json::Value* gameplay = project.value().find("gameplay");
    const json::Value* sources =
        gameplay != nullptr && gameplay->is_object() ? gameplay->find("sources") : nullptr;
    if (sources == nullptr || !sources->is_array()) {
        return folders;
    }
    for (const json::Value& source : sources->as_array()) {
        if (source.is_string() && is_project_path(source.as_string())) {
            folders.push_back(source.as_string());
        }
    }
    return folders;
}

// The project's gameplay sources.
std::vector<std::shared_ptr<const SourceFile>> read_project(const std::string& root) {
    std::vector<std::shared_ptr<const SourceFile>> files;
    std::set<std::string> seen;
    for (const std::string& source : source_folders(root)) {
        for (const fs::path& file : files_under(fs::path(root) / source, false)) {
            const std::string path = path_from(file, root);
            if (!seen.insert(path).second) {
                continue;
            }
            if (auto read = cache().get(file, path, path)) {
                files.push_back(std::move(read));
            }
        }
    }
    return files;
}

// --- Words -------------------------------------------------------------------------------------

bool is_stop_word(std::string_view word) {
    static const std::set<std::string_view> kStopWords = {
        "a",    "an",   "and",  "are",   "as",    "at",   "be",   "by",   "can",  "do",
        "does", "for",  "from", "how",   "i",     "in",   "into", "it",   "its",  "me",
        "my",   "of",   "on",   "or",    "that",  "the",  "this", "to",   "use",  "using",
        "we",   "what", "when", "where", "which", "with", "you",  "your", "want", "need"};
    return kStopWords.contains(word);
}

// A word's stem, so "plays", "playing" and "played" all match "play".
std::string stem(std::string word) {
    const auto ends = [&](std::string_view suffix) { return word.ends_with(suffix); };
    if (word.size() > 5 && ends("ing")) {
        word.resize(word.size() - 3);
    } else if (word.size() > 4 && ends("ies")) {
        word.resize(word.size() - 3);
        word += 'y';
    } else if (word.size() > 4 &&
               (ends("ses") || ends("xes") || ends("ches") || ends("shes") || ends("ed"))) {
        word.resize(word.size() - 2);
    } else if (word.size() > 3 && ends("s") && !ends("ss")) {
        word.resize(word.size() - 1);
    }
    if (word.size() > 3 && word.back() == 'e') {
        word.pop_back();
    }
    return word;
}

// The words in text, lower case and stemmed unless asked not to be: identifiers split at
// underscores and changes of case ("is_action_pressed", "AnimatedSprite2D"), stop words left
// out.
std::vector<std::string> words_of(std::string_view text, bool stemmed = true) {
    std::vector<std::string> words;
    std::string word;
    const auto flush = [&] {
        if (!word.empty()) {
            if (!is_stop_word(word)) {
                words.push_back(stemmed ? stem(std::move(word)) : std::move(word));
            }
            word.clear();
        }
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (std::isalnum(c) == 0) {
            flush();
            continue;
        }
        // A capital starts a word after a small letter, or before one ("HTTPServer").
        if (std::isupper(c) != 0 && !word.empty() &&
            (std::islower(static_cast<unsigned char>(text[i - 1])) != 0 ||
             (i + 1 < text.size() && std::islower(static_cast<unsigned char>(text[i + 1])) != 0 &&
              std::isupper(static_cast<unsigned char>(text[i - 1])) != 0))) {
            flush();
        }
        word += static_cast<char>(std::tolower(c));
    }
    flush();
    return words;
}

// Words joined by underscores: "add_system" for "add a system" or "addSystem".
std::string snake(std::string_view text) {
    std::string out;
    for (const std::string& word : words_of(text, false)) {
        out += out.empty() ? "" : "_";
        out += word;
    }
    return out;
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// --- Entries -----------------------------------------------------------------------------------

// One function (its overloads together), type or macro.
struct Entry {
    std::string name;  // qualified: "slash::world::World::find"
    std::string local; // the last part: "find"
    std::string kind;  // "function", "struct", "class", "union", "enum", "alias" or "macro"
    std::string member_of;
    std::vector<std::string> signatures;
    std::string doc;
    std::string at;      // where it is declared: "path:line"
    std::string defined; // a project function's definition elsewhere: "path:line"
    const SourceFile* file = nullptr;
    bool project = false;
    bool definition = false; // a function's body, which a declaration elsewhere stands for
    std::vector<std::string> fields;
    std::vector<std::string> members;
    // What matching weighs, from the name down to the signature.
    std::set<std::string> name_words;
    std::set<std::string> class_words;
    std::set<std::string> scope_words;
    std::set<std::string> doc_words;
    std::set<std::string> signature_words;
};

std::string at_line(const SourceFile& file, int line) {
    return file.path + ":" + std::to_string(line);
}

std::string last_part(std::string_view name) {
    const std::size_t colon = name.rfind("::");
    return std::string(colon == std::string_view::npos ? name : name.substr(colon + 2));
}

// A doc's first sentence, which is enough beside a field.
std::string first_sentence(std::string_view doc) {
    const std::size_t end = doc.find(". ");
    return std::string(end == std::string_view::npos ? doc : doc.substr(0, end + 1));
}

bool is_type(std::string_view kind) {
    return kind == "struct" || kind == "class" || kind == "union" || kind == "enum" ||
           kind == "alias";
}

class Index {
public:
    void add(const SourceFile& file, bool project) {
        for (const CppSymbol& symbol : file.outline.symbols) {
            add_symbol(file, symbol, project);
        }
        for (const command::CppDefine& define : file.outline.defines) {
            if (define.name.ends_with("_H") || define.name.ends_with("_HPP") ||
                define.name.ends_with('_')) {
                continue; // include guards and helpers
            }
            Entry entry;
            entry.name = define.name;
            entry.local = define.name;
            entry.kind = "macro";
            entry.signatures.push_back(define.name + define.params);
            entry.doc = define.doc;
            entry.at = at_line(file, define.line);
            entry.file = &file;
            entry.project = project;
            insert(std::move(entry));
        }
    }

    std::vector<Entry>& entries() { return entries_; }

private:
    void add_symbol(const SourceFile& file, const CppSymbol& symbol, bool project) {
        const bool out_of_class = symbol.name.find("::") != std::string::npos;
        if (!project) {
            // The engine's API is what its headers declare public, outside detail namespaces.
            if ((symbol.access != "public" && !symbol.member_of.empty()) || out_of_class ||
                symbol.scope.find("detail") != std::string::npos ||
                symbol.scope.find("internal") != std::string::npos) {
                return;
            }
        }
        if (symbol.name.starts_with('~') || symbol.name.find("operator") != std::string::npos ||
            symbol.signature.find("= delete") != std::string::npos) {
            return;
        }
        Entry entry;
        entry.name = symbol.scope.empty() ? symbol.name : symbol.scope + "::" + symbol.name;
        entry.local = last_part(symbol.name);
        entry.kind = symbol.kind;
        entry.member_of = symbol.member_of;
        if (entry.member_of.empty() && out_of_class) {
            entry.member_of = last_part(symbol.name.substr(0, symbol.name.rfind("::")));
        }
        entry.signatures.push_back(symbol.signature);
        entry.doc = symbol.doc;
        entry.at = at_line(file, symbol.line);
        entry.file = &file;
        entry.project = project;
        entry.definition = symbol.kind == "function" && symbol.definition;
        for (const command::CppField& field : symbol.fields) {
            if (field.access == "public" || project) {
                const std::string doc = first_sentence(field.doc);
                entry.fields.push_back(doc.empty() ? field.declaration
                                                   : field.declaration + " // " + doc);
            }
        }
        if (const auto found = by_name_.find(entry.name + " " + entry.kind);
            found != by_name_.end()) {
            merge(entries_[found->second], std::move(entry));
            return;
        }
        insert(std::move(entry));
    }

    // Another declaration of an entry's name: an overload, or the definition of a project
    // function declared elsewhere (a header, or its class), which the declaration stands for.
    static void merge(Entry& entry, Entry other) {
        if (entry.definition && !other.definition) {
            std::swap(entry, other);
        }
        if (other.definition && !entry.definition) {
            if (entry.defined.empty()) {
                entry.defined = std::move(other.at);
            }
            if (entry.doc.empty()) {
                entry.doc = std::move(other.doc);
            }
            return;
        }
        if (std::ranges::find(entry.signatures, other.signatures.front()) ==
            entry.signatures.end()) {
            entry.signatures.push_back(std::move(other.signatures.front()));
        }
        if (entry.doc.empty()) {
            entry.doc = std::move(other.doc);
        }
    }

    void insert(Entry entry) {
        by_name_.emplace(entry.name + " " + entry.kind, entries_.size());
        entries_.push_back(std::move(entry));
    }

    std::vector<Entry> entries_;
    std::map<std::string, std::size_t> by_name_;
};

// Fills in what matching needs and each type's members.
void prepare(std::vector<Entry>& entries) {
    std::map<std::string, std::vector<std::string>> members; // by qualified class name
    for (const Entry& entry : entries) {
        if (entry.kind == "function" && !entry.member_of.empty() &&
            entry.local != entry.member_of) {
            const std::string owner = entry.name.substr(0, entry.name.rfind("::"));
            std::vector<std::string>& names = members[owner];
            if (std::ranges::find(names, entry.local) == names.end()) {
                names.push_back(entry.local);
            }
        }
    }
    for (Entry& entry : entries) {
        if (is_type(entry.kind)) {
            if (const auto found = members.find(entry.name); found != members.end()) {
                entry.members = found->second;
            }
        }
        for (std::string& word : words_of(entry.local)) {
            entry.name_words.insert(std::move(word));
        }
        for (std::string& word : words_of(entry.member_of)) {
            entry.class_words.insert(std::move(word));
        }
        const std::string scope = entry.name.substr(0, entry.name.size() - entry.local.size());
        for (std::string& word : words_of(scope)) {
            entry.scope_words.insert(std::move(word));
        }
        if (!entry.project) {
            // The header's name says what part of the engine it is: "audio", "tilemap".
            for (std::string& word : words_of(fs::path(entry.file->include).stem().string())) {
                entry.scope_words.insert(std::move(word));
            }
        }
        for (std::string& word : words_of(entry.doc)) {
            entry.doc_words.insert(std::move(word));
        }
        for (const std::string& signature : entry.signatures) {
            for (std::string& word : words_of(signature)) {
                entry.signature_words.insert(std::move(word));
            }
        }
    }
}

// How well an entry matches the query: its words found in the name count most, then in the
// class, the namespace and header, the doc, and last the signature; rarer words count more, and
// the share of the query's words found scales the sum. A query that is a name matches it first.
class Scorer {
public:
    Scorer(std::string_view query, const std::vector<Entry>& entries)
        : query_(lower(query)), entries_(static_cast<double>(entries.size())) {
        std::string trimmed = query_;
        std::erase_if(trimmed, [](char c) { return std::isspace(static_cast<unsigned char>(c)); });
        name_query_ = trimmed.size() == query_.size() ? trimmed : std::string();
        snake_query_ = snake(query);
        for (std::string& word : words_of(query)) {
            if (std::ranges::find(terms_, word) == terms_.end()) {
                terms_.push_back(std::move(word));
            }
        }
        for (const std::string& term : terms_) {
            double count = 0;
            for (const Entry& entry : entries) {
                if (entry.name_words.contains(term) || entry.class_words.contains(term) ||
                    entry.scope_words.contains(term) || entry.doc_words.contains(term) ||
                    entry.signature_words.contains(term)) {
                    ++count;
                }
            }
            weights_.push_back(std::log(1.0 + entries_ / (1.0 + count)));
        }
    }

    [[nodiscard]] double score(const Entry& entry) const {
        double total = 0;
        std::size_t matched = 0;
        for (std::size_t t = 0; t < terms_.size(); ++t) {
            const std::string& term = terms_[t];
            double best = 0;
            if (entry.name_words.contains(term)) {
                best = 4;
            } else if (term.size() >= 3 &&
                       std::ranges::any_of(entry.name_words, [&](const std::string& word) {
                           return word.starts_with(term) ||
                                  (word.size() >= 4 && term.starts_with(word));
                       })) {
                best = 2.5;
            }
            if (entry.class_words.contains(term)) {
                best = std::max(best, 2.0);
            }
            if (entry.scope_words.contains(term)) {
                best = std::max(best, 1.5);
            }
            if (entry.doc_words.contains(term)) {
                best = std::max(best, 1.0);
            }
            if (entry.signature_words.contains(term)) {
                best = std::max(best, 0.5);
            }
            if (best > 0) {
                ++matched;
                total += best * weights_[t];
            }
        }
        if (!terms_.empty()) {
            total *= static_cast<double>(matched) / static_cast<double>(terms_.size());
        }
        if (!name_query_.empty()) {
            const std::string name = lower(entry.name);
            const std::string local = lower(entry.local);
            if (name_query_.find("::") != std::string::npos &&
                (name == name_query_ || name.ends_with("::" + name_query_))) {
                total += 150;
            } else if (local == name_query_) {
                total += 100;
            } else if (name_query_.size() >= 3 && local.find(name_query_) != std::string::npos) {
                total += 10 + (10.0 * static_cast<double>(name_query_.size()) /
                               static_cast<double>(local.size()));
            }
        } else if (!snake_query_.empty() && snake(entry.local) == snake_query_) {
            total += 50; // "is action pressed" names is_action_pressed
        }
        return total;
    }

private:
    std::string query_;
    std::string name_query_;  // the query when it is one word or name, else ""
    std::string snake_query_; // its words as a name: "add_system"
    std::vector<std::string> terms_;
    std::vector<double> weights_;
    double entries_;
};

// --- Examples ----------------------------------------------------------------------------------

// The engine headers each file includes, directly or through other files, by include path.
class Includes {
public:
    Includes(const std::vector<std::shared_ptr<const SourceFile>>& headers,
             const std::vector<std::shared_ptr<const SourceFile>>& others) {
        for (const auto& file : headers) {
            by_include_.emplace(file->include, file.get());
            by_path_.emplace(file->path, file.get());
        }
        for (const auto& file : others) {
            by_path_.emplace(file->path, file.get());
        }
    }

    // Whether file includes the engine header with this include path.
    bool includes(const SourceFile& file, const std::string& include) {
        return closure(file).contains(include);
    }

private:
    const std::set<std::string>& closure(const SourceFile& file) {
        if (const auto found = closures_.find(&file); found != closures_.end()) {
            return found->second;
        }
        std::set<std::string>& out = closures_[&file]; // empty while it is being made
        std::set<std::string> found;
        const fs::path folder = fs::path(file.path).parent_path();
        for (const std::string& include : file.outline.includes) {
            const SourceFile* next = nullptr;
            if (const auto beside =
                    by_path_.find((folder / include).lexically_normal().generic_string());
                beside != by_path_.end()) {
                next = beside->second;
            } else if (const auto header = by_include_.find(include); header != by_include_.end()) {
                next = header->second;
                found.insert(include);
            }
            if (next != nullptr) {
                const std::set<std::string>& more = closure(*next);
                found.insert(more.begin(), more.end());
            }
        }
        out = std::move(found);
        return closures_[&file];
    }

    std::map<std::string, const SourceFile*> by_include_;
    std::map<std::string, const SourceFile*> by_path_;
    std::map<const SourceFile*, std::set<std::string>> closures_;
};

bool identifier_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// The name of what code[0, end) ends with, past a call's or a subscript's brackets:
// "world" in "session.world()" or "game_world".
std::string_view receiver(std::string_view code, std::size_t end) {
    while (end > 0 && code[end - 1] == ' ') {
        --end;
    }
    if (end > 0 && (code[end - 1] == ')' || code[end - 1] == ']')) {
        const char close = code[end - 1];
        const char open = close == ')' ? '(' : '[';
        int depth = 0;
        while (end > 0) {
            --end;
            if (code[end] == close) {
                ++depth;
            } else if (code[end] == open) {
                --depth;
            }
            if (depth == 0) {
                break;
            }
        }
    }
    std::size_t begin = end;
    while (begin > 0 && identifier_char(code[begin - 1])) {
        --begin;
    }
    return code.substr(begin, end - begin);
}

// Whether a line of code (comments and strings blanked) uses an entry: its name as a whole word,
// qualified, if at all, by one of its namespaces or classes, and called if it is a function not
// named that way.
// A member function must be called through its class ("World::find") or on something whose
// name shares a word with it ("game_world.find"), since other classes have members of the same
// name.
bool uses(std::string_view code, const Entry& entry) {
    const std::size_t first = code.find_first_not_of(" \t");
    if (first == std::string_view::npos || code[first] == '#') {
        return false;
    }
    const std::string& name = entry.local;
    const bool function = entry.kind == "function";
    const bool member = function && !entry.member_of.empty() && entry.local != entry.member_of;
    const std::string_view scope =
        std::string_view(entry.name).substr(0, entry.name.size() - entry.local.size());
    for (std::size_t at = code.find(name); at != std::string_view::npos;
         at = code.find(name, at + 1)) {
        const std::size_t end = at + name.size();
        if ((at > 0 && identifier_char(code[at - 1])) ||
            (end < code.size() && identifier_char(code[end]))) {
            continue;
        }
        std::size_t before = at;
        while (before > 0 && code[before - 1] == ' ') {
            --before;
        }
        const bool dot = before > 0 && code[before - 1] == '.';
        const bool arrow = before > 1 && code[before - 1] == '>' && code[before - 2] == '-';
        const bool qualified = before > 1 && code[before - 1] == ':' && code[before - 2] == ':';
        if (qualified) {
            const std::string_view by = receiver(code, before - 2);
            if (member
                    ? by != entry.member_of
                    : !by.empty() && scope.find(std::string(by) + "::") == std::string_view::npos) {
                continue;
            }
        }
        if (function) {
            std::size_t next = end;
            while (next < code.size() && code[next] == ' ') {
                ++next;
            }
            if (next < code.size() && code[next] == '<') {
                int depth = 0;
                for (; next < code.size(); ++next) {
                    if (code[next] == '<') {
                        ++depth;
                    } else if (code[next] == '>') {
                        --depth;
                    }
                    if (depth == 0) {
                        ++next;
                        break;
                    }
                }
            }
            // Called, or named through its namespace or class, as when passed to add_system.
            const bool called = next < code.size() && (code[next] == '(' || code[next] == '{');
            if (!called && !qualified) {
                continue;
            }
            if (!member && (dot || arrow)) {
                continue;
            }
            if (member && !qualified) {
                if (!dot && !arrow) {
                    continue;
                }
                const std::vector<std::string> words =
                    words_of(receiver(code, before - (dot ? 1 : 2)));
                if (std::ranges::none_of(words, [&](const std::string& word) {
                        return entry.class_words.contains(word);
                    })) {
                    continue;
                }
            }
        }
        return true;
    }
    return false;
}

std::string code_line(std::string_view line) {
    const std::size_t first = line.find_first_not_of(" \t");
    const std::size_t last = line.find_last_not_of(" \t\r");
    std::string code(first == std::string_view::npos ? std::string_view()
                                                     : line.substr(first, last - first + 1));
    if (code.size() > kMaxCode) {
        code.resize(kMaxCode);
        code += "...";
    }
    return code;
}

// The first line of each file that uses the entry, up to count of them.
json::Array examples_of(const Entry& entry, const std::vector<const SourceFile*>& files,
                        Includes& includes, std::size_t count) {
    json::Array out;
    for (const SourceFile* file : files) {
        if (out.size() >= count) {
            break;
        }
        if (!entry.project && !includes.includes(*file, entry.file->include)) {
            continue;
        }
        for (std::size_t n = 0; n < file->lines.size() && n < file->code.size(); ++n) {
            const std::string at = at_line(*file, static_cast<int>(n + 1));
            if (at == entry.at || at == entry.defined || !uses(file->code[n], entry)) {
                continue;
            }
            json::Value example;
            example["at"] = at;
            example["code"] = code_line(file->lines[n]);
            out.push_back(std::move(example));
            break;
        }
    }
    return out;
}

// --- The command -------------------------------------------------------------------------------

json::Value result_of(const Entry& entry, json::Array examples) {
    json::Value out;
    out["name"] = entry.name;
    out["kind"] = entry.kind;
    out["signature"] = entry.signatures.front();
    if (entry.signatures.size() > 1) {
        json::Array overloads;
        for (std::size_t k = 1; k < entry.signatures.size(); ++k) {
            overloads.emplace_back(entry.signatures[k]);
        }
        out["overloads"] = std::move(overloads);
    }
    if (!entry.doc.empty()) {
        out["doc"] = entry.doc;
    }
    out["at"] = entry.at;
    if (!entry.defined.empty()) {
        out["defined"] = entry.defined;
    }
    if (!entry.project) {
        out["include"] = entry.file->include;
    }
    if (!entry.fields.empty()) {
        json::Array fields;
        for (const std::string& field : entry.fields) {
            fields.emplace_back(field);
        }
        out["fields"] = std::move(fields);
    }
    if (!entry.members.empty()) {
        json::Array members;
        for (const std::string& member : entry.members) {
            members.emplace_back(member);
        }
        out["members"] = std::move(members);
    }
    if (!examples.empty()) {
        out["examples"] = std::move(examples);
    }
    return out;
}

bool wanted(const Entry& entry, std::string_view kind) {
    if (kind.empty()) {
        return true;
    }
    if (kind == "type") {
        return is_type(entry.kind);
    }
    return entry.kind == kind;
}

Reply api_search(command::Session& session, const json::Value& params) {
    const std::string& query = params.find("query")->as_string();
    bool engine_wanted = true;
    bool project_wanted = true;
    if (const json::Value* in = params.find("in"); in != nullptr) {
        engine_wanted = project_wanted = false;
        for (const json::Value& source : in->as_array()) {
            engine_wanted = engine_wanted || source.as_string() == "engine";
            project_wanted = project_wanted || source.as_string() == "project";
        }
    }
    const json::Value* kind_param = params.find("kind");
    const std::string kind = kind_param != nullptr ? kind_param->as_string() : std::string();
    const json::Value* limit_param = params.find("limit");
    const auto limit = static_cast<std::size_t>(limit_param != nullptr ? limit_param->as_int() : 8);
    const json::Value* examples_param = params.find("examples");
    const auto example_count =
        static_cast<std::size_t>(examples_param != nullptr ? examples_param->as_int() : 2);

    std::vector<std::shared_ptr<const SourceFile>> headers;
    std::vector<std::shared_ptr<const SourceFile>> examples;
    const std::optional<std::string> engine = find_engine();
    if (engine && engine_wanted) {
        read_engine(*engine, headers, examples);
    }
    const std::vector<std::shared_ptr<const SourceFile>> project =
        read_project(session.options().project_root);

    Index index;
    for (const auto& file : headers) {
        index.add(*file, false);
    }
    if (project_wanted) {
        for (const auto& file : project) {
            index.add(*file, true);
        }
    }
    std::vector<Entry>& entries = index.entries();
    prepare(entries);

    const Scorer scorer(query, entries);
    std::vector<std::pair<double, const Entry*>> ranked;
    for (const Entry& entry : entries) {
        if (!wanted(entry, kind)) {
            continue;
        }
        if (const double score = scorer.score(entry); score > 0) {
            ranked.emplace_back(score, &entry);
        }
    }
    std::ranges::sort(ranked, [](const auto& a, const auto& b) {
        if (a.first != b.first) {
            return a.first > b.first;
        }
        if (a.second->name.size() != b.second->name.size()) {
            return a.second->name.size() < b.second->name.size();
        }
        return a.second->name < b.second->name;
    });
    if (ranked.size() > limit) {
        ranked.resize(limit);
    }

    std::vector<const SourceFile*> engine_examples;
    engine_examples.reserve(examples.size());
    for (const auto& file : examples) {
        engine_examples.push_back(file.get());
    }
    std::vector<const SourceFile*> project_files;
    project_files.reserve(project.size());
    for (const auto& file : project) {
        project_files.push_back(file.get());
    }
    std::vector<std::shared_ptr<const SourceFile>> others = examples;
    others.insert(others.end(), project.begin(), project.end());
    Includes includes(headers, others);

    json::Array results;
    for (const auto& [score, entry] : ranked) {
        results.push_back(result_of(
            *entry, example_count == 0
                        ? json::Array{}
                        : examples_of(*entry, entry->project ? project_files : engine_examples,
                                      includes, example_count)));
    }
    json::Value out;
    if (results.empty()) {
        std::set<std::string> names;
        for (const Entry& entry : entries) {
            if (wanted(entry, kind)) {
                names.insert(entry.local);
            }
        }
        if (const std::string_view close = closest_match(query, names); !close.empty()) {
            out["did_you_mean"] = std::string(close);
        }
    }
    out["results"] = std::move(results);
    if (engine_wanted && !engine) {
        out["warning"] = "The engine's sources were not found, so only the project was searched. "
                         "Set SLASH_ENGINE_DIR to an engine checkout.";
    }
    return Reply::success(std::move(out));
}

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

} // namespace

void register_api_commands(command::Registry& registry) {
    json::Value params;
    params["type"] = "object";
    params["additionalProperties"] = false;
    params["required"] = json::Array{"query"};
    params["properties"]["query"] =
        property("string", "A name (World::find, is_action_pressed) or words for what it does "
                           "(play a sound).");
    params["properties"]["query"]["minLength"] = 1;
    params["properties"]["in"] =
        property("array", "Search only these: engine (its public headers), project (the "
                          "gameplay sources). Default both.");
    params["properties"]["in"]["items"]["enum"] = json::Array{"engine", "project"};
    params["properties"]["kind"] = property("string", "Only functions, types or macros.");
    params["properties"]["kind"]["enum"] = json::Array{"function", "type", "macro"};
    params["properties"]["limit"] = property("integer", "Most results. Default 8.");
    params["properties"]["limit"]["minimum"] = 1;
    params["properties"]["limit"]["maximum"] = 50;
    params["properties"]["examples"] =
        property("integer", "Most example lines per result. Default 2.");
    params["properties"]["examples"]["minimum"] = 0;
    params["properties"]["examples"]["maximum"] = 10;
    registry.add({
        .name = "api.search",
        .description = "Finds C++ functions, types and macros in the engine and the game's code "
                       "by name or purpose. Returns signatures, docs, the header to include and "
                       "lines of verified examples that use each one.",
        .capabilities = {command::Capability::Read},
        .effect = command::Effect::Read,
        .params = std::move(params),
        .handler = api_search,
    });
}

} // namespace slash::exporter
