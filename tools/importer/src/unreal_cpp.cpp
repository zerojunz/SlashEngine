// Reads an Unreal C++ class's declarations, as the skeleton of a script: its header's UCLASS and
// the functions its source file defines.

#include "common.hpp"

#include "slash/importer/scripts.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::importer {
namespace {

constexpr std::size_t kNone = std::string_view::npos;

bool is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// text with its comments and the insides of its string and character literals made spaces, so
// braces and semicolons in them don't count; newlines stay, so offsets and lines match.
std::string code_of(std::string_view text) {
    std::string out(text);
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '/' && i + 1 < out.size() && out[i + 1] == '/') {
            for (; i < out.size() && out[i] != '\n'; ++i) {
                out[i] = ' ';
            }
        } else if (out[i] == '/' && i + 1 < out.size() && out[i + 1] == '*') {
            const std::size_t end = out.find("*/", i + 2);
            const std::size_t stop = end == std::string::npos ? out.size() : end + 2;
            for (; i < stop; ++i) {
                if (out[i] != '\n') {
                    out[i] = ' ';
                }
            }
            --i;
        } else if (out[i] == '"' || out[i] == '\'') {
            const char quote = out[i];
            for (++i; i < out.size() && out[i] != quote && out[i] != '\n'; ++i) {
                if (out[i] == '\\' && i + 1 < out.size()) {
                    out[i++] = ' ';
                }
                out[i] = ' ';
            }
        }
    }
    return out;
}

// The offset of the bracket closing the one at open, or kNone.
std::size_t closing(std::string_view code, std::size_t open) {
    const char opener = code[open];
    char closer = '>';
    switch (opener) {
    case '(':
        closer = ')';
        break;
    case '{':
        closer = '}';
        break;
    case '[':
        closer = ']';
        break;
    default:
        break;
    }
    int depth = 0;
    for (std::size_t i = open; i < code.size(); ++i) {
        if (code[i] == opener) {
            ++depth;
        } else if (code[i] == closer && --depth == 0) {
            return i;
        }
    }
    return kNone;
}

// Whether a word starts at an offset, on its own.
bool word_at(std::string_view code, std::size_t at, std::string_view word) {
    return code.substr(at, word.size()) == word && (at == 0 || !is_word(code[at - 1])) &&
           (at + word.size() >= code.size() || !is_word(code[at + word.size()]));
}

std::size_t find_word(std::string_view code, std::string_view word, std::size_t from = 0,
                      std::size_t end = kNone) {
    for (std::size_t at = code.find(word, from); at != kNone && at < end;
         at = code.find(word, at + 1)) {
        if (word_at(code, at, word)) {
            return at;
        }
    }
    return kNone;
}

// Runs of whitespace as one space.
std::string one_line(std::string_view text) {
    std::string out;
    bool space = false;
    for (const char c : trimmed(text)) {
        if (is_space(c)) {
            space = true;
            continue;
        }
        if (space && !out.empty()) {
            out += ' ';
        }
        space = false;
        out += c;
    }
    return out;
}

// The pieces of a list split at its top-level commas.
std::vector<std::string_view> split_top(std::string_view text) {
    std::vector<std::string_view> out;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '(' || c == '<' || c == '{' || c == '[') {
            ++depth;
        } else if (c == ')' || c == '>' || c == '}' || c == ']') {
            --depth;
        } else if (c == ',' && depth == 0) {
            out.push_back(trimmed(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (!trimmed(text.substr(start)).empty()) {
        out.push_back(trimmed(text.substr(start)));
    }
    return out;
}

std::size_t line_of(std::string_view text, std::size_t offset) {
    return static_cast<std::size_t>(std::count(text.begin(), text.begin() + offset, '\n')) + 1;
}

// The comment right above a declaration, without its markers: /** ... */ blocks and // lines.
std::string doc_before(std::string_view text, std::size_t at) {
    std::size_t end = at;
    while (end > 0 && is_space(text[end - 1])) {
        --end;
    }
    std::vector<std::string> lines;
    if (end >= 2 && text.substr(end - 2, 2) == "*/") {
        const std::size_t open = text.rfind("/*", end - 2);
        if (open == kNone) {
            return {};
        }
        std::string_view inside = text.substr(open + 2, end - 2 - open - 2);
        while (!inside.empty() && inside.front() == '*') {
            inside.remove_prefix(1);
        }
        std::size_t start = 0;
        while (start <= inside.size()) {
            std::size_t stop = inside.find('\n', start);
            if (stop == kNone) {
                stop = inside.size();
            }
            std::string_view line = trimmed(inside.substr(start, stop - start));
            while (!line.empty() && line.front() == '*') {
                line.remove_prefix(1);
            }
            if (!trimmed(line).empty()) {
                lines.emplace_back(trimmed(line));
            }
            start = stop + 1;
        }
    } else {
        // // lines, each on its own line right above.
        std::size_t stop = end;
        while (stop > 0) {
            const std::size_t start = text.rfind('\n', stop - 1);
            const std::size_t from = start == kNone ? 0 : start + 1;
            const std::string_view line = trimmed(text.substr(from, stop - from));
            if (!line.starts_with("//")) {
                break;
            }
            std::string_view words = line.substr(2);
            while (!words.empty() && words.front() == '/') {
                words.remove_prefix(1);
            }
            lines.insert(lines.begin(), std::string(trimmed(words)));
            if (start == kNone) {
                break;
            }
            stop = start;
        }
    }
    return join(lines, " ");
}

// --- Types ---

struct Mapped {
    std::string type; // in C++
    FieldKind kind = FieldKind::Value;
    std::string comment{}; // NOLINT(readability-redundant-member-init): what C++ doesn't say
};

// "TObjectPtr<AActor>" -> "AActor*", "const FString&" -> "FString", and no "class " marks.
std::string plain_type(std::string_view type) {
    std::string out;
    std::string text = one_line(type);
    for (const std::string_view drop : {"const ", "class ", "struct ", "volatile "}) {
        for (std::size_t at = text.find(drop); at != std::string::npos; at = text.find(drop)) {
            if (at > 0 && is_word(text[at - 1])) {
                break;
            }
            text.erase(at, drop.size());
        }
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '&' || (text[i] == ' ' && (i + 1 == text.size() || text[i + 1] == '*' ||
                                                  text[i + 1] == '&'))) {
            continue;
        }
        out += text[i];
    }
    while (out.ends_with(" const")) {
        out.resize(out.size() - 6);
    }
    for (const std::string_view wrapper : {"TObjectPtr<", "TWeakObjectPtr<", "TEnumAsByte<"}) {
        if (out.starts_with(wrapper) && out.ends_with('>')) {
            const std::string inner = out.substr(wrapper.size(), out.size() - wrapper.size() - 1);
            out = wrapper == "TEnumAsByte<" ? inner : inner + "*";
        }
    }
    return out;
}

// The types inside a template's angle brackets.
std::vector<std::string> template_args(std::string_view type) {
    const std::size_t open = type.find('<');
    if (open == kNone || !type.ends_with('>')) {
        return {};
    }
    std::vector<std::string> out;
    for (const std::string_view arg : split_top(type.substr(open + 1, type.size() - open - 2))) {
        out.push_back(plain_type(arg));
    }
    return out;
}

// Unreal's classes of assets, which a property holds the path of.
bool is_asset_class(std::string_view name) {
    static constexpr std::array<std::string_view, 24> kAssets{
        "UStaticMesh",        "USkeletalMesh",
        "UMaterialInterface", "UMaterial",
        "UMaterialInstance",  "UTexture",
        "UTexture2D",         "USoundBase",
        "USoundWave",         "USoundCue",
        "UAnimSequence",      "UAnimMontage",
        "UAnimationAsset",    "UBlendSpace",
        "UParticleSystem",    "UNiagaraSystem",
        "UCurveFloat",        "UDataTable",
        "UDataAsset",         "UPrimaryDataAsset",
        "UInputAction",       "UInputMappingContext",
        "UUserWidget",        "UPhysicalMaterial"};
    return std::ranges::find(kAssets, name) != kAssets.end();
}

class TypeMapper {
public:
    std::set<std::string> enums;
    std::set<std::string> delegates;

    [[nodiscard]] Mapped map(std::string_view written) const {
        const std::string type = plain_type(written);
        if (type == "bool") {
            return {.type = "bool"};
        }
        if (type == "int32" || type == "int" || type == "uint8" || type == "int8" ||
            type == "int16" || type == "uint16" || type == "uint32") {
            return {.type = "int"};
        }
        if (type == "int64" || type == "uint64") {
            return {.type = "std::int64_t"};
        }
        if (type == "float") {
            return {.type = "float"};
        }
        if (type == "double") {
            return {.type = "double"};
        }
        if (type == "FString" || type == "FName" || type == "FText") {
            return {.type = "std::string", .comment = type == "FString" ? "" : type};
        }
        if (type == "FVector" || type == "FVector3f" || type == "FVector3d") {
            return {.type = "slash::Vec3", .comment = "FVector, in Unreal's axes and units"};
        }
        if (type == "FIntVector") {
            return {.type = "slash::Vec3", .comment = "FIntVector"};
        }
        if (type == "FVector2D" || type == "FVector2f") {
            return {.type = "slash::Vec2"};
        }
        if (type == "FRotator") {
            return {.type = "slash::Vec3",
                    .comment = "FRotator: pitch, yaw and roll in degrees, as Unreal turns"};
        }
        if (type == "FQuat") {
            return {.type = "slash::Quat", .comment = "FQuat, in Unreal's axes"};
        }
        if (type == "FLinearColor") {
            return {.type = "slash::Vec4"};
        }
        if (type == "FColor") {
            return {.type = "slash::Vec4", .comment = "FColor, as 0 to 1"};
        }
        if (type.starts_with('E') || enums.contains(type)) {
            return {.type = "int", .comment = type};
        }
        if (type.starts_with("TArray<") || type.starts_with("TSet<")) {
            const std::vector<std::string> args = template_args(type);
            if (args.empty()) {
                return json_of(type);
            }
            const Mapped item = map(args.front());
            if (item.kind == FieldKind::Entity) {
                return {.type = "std::vector<entt::entity>",
                        .kind = FieldKind::Entities,
                        .comment = type + ": their entities"};
            }
            if (item.type == "slash::json::Value") {
                return json_of(type);
            }
            return {.type = "std::vector<" + item.type + ">",
                    .comment = item.comment.empty() ? "" : type};
        }
        if (type.starts_with("TMap<")) {
            const std::vector<std::string> args = template_args(type);
            if (args.size() == 2) {
                const Mapped value = map(args[1]);
                if (map(args[0]).type == "std::string" && value.kind == FieldKind::Value &&
                    value.type != "slash::json::Value") {
                    return {.type = "std::map<std::string, " + value.type + ">",
                            .comment = value.comment.empty() ? "" : type};
                }
            }
            return json_of(type);
        }
        if (type.starts_with("TSubclassOf<") || type.starts_with("TSoftClassPtr<")) {
            return {.type = "std::string", .comment = type + ": the class's path"};
        }
        if (type.starts_with("TSoftObjectPtr<")) {
            return {.type = "std::string", .comment = type + ": its path"};
        }
        if (type.ends_with('*')) {
            const std::string name = type.substr(0, type.size() - 1);
            if (is_asset_class(name)) {
                return {.type = "std::string", .comment = type + ": its path"};
            }
            if (name.starts_with('A')) {
                return {.type = "entt::entity", .kind = FieldKind::Entity, .comment = type};
            }
            if (name.starts_with('U') && name.ends_with("Component")) {
                return {.type = "entt::entity",
                        .kind = FieldKind::Entity,
                        .comment = type + ": the entity it became"};
            }
        }
        return json_of(type);
    }

    [[nodiscard]] static Mapped json_of(const std::string& type) {
        return {.type = "slash::json::Value", .comment = type};
    }
};

// A parameter's C++ type: strings, lists and JSON by const reference.
std::string parameter_type(const std::string& type) {
    if (type == "std::string" || type.starts_with("std::vector<") ||
        type.starts_with("std::map<") || type == "slash::json::Value") {
        return "const " + type + "&";
    }
    return type;
}

// A member's name in C++: bIsDead -> is_dead, MaxHP -> max_hp.
std::string member_name(std::string_view name) {
    if (name.size() > 1 && name[0] == 'b' && name[1] >= 'A' && name[1] <= 'Z') {
        name.remove_prefix(1);
    }
    return cpp_identifier(snake_case(name));
}

// A type and a name, split: "const FHitResult& Hit" -> ("const FHitResult&", "Hit").
std::pair<std::string, std::string> type_and_name(std::string_view text) {
    if (const std::size_t equals = text.find('='); equals != kNone) {
        text = trimmed(text.substr(0, equals));
    }
    std::size_t end = text.size();
    while (end > 0 && text[end - 1] == ']') { // C arrays
        end = text.rfind('[', end - 1);
        if (end == kNone) {
            return {std::string(text), {}};
        }
        text = trimmed(text.substr(0, end));
        end = text.size();
    }
    std::size_t start = end;
    while (start > 0 && is_word(text[start - 1])) {
        --start;
    }
    return {std::string(trimmed(text.substr(0, start))),
            std::string(text.substr(start, end - start))};
}

// A C++ default value as the field's initializer, when it is a plain one: numbers, booleans,
// strings and the file's enums' values.
std::string initializer(std::string_view value, const std::string& type,
                        const std::set<std::string>& enums) {
    std::string text = one_line(value);
    if (text.starts_with("TEXT(") && text.ends_with(')')) {
        text = trimmed(std::string_view(text).substr(5, text.size() - 6));
    }
    if (type == "bool" && (text == "true" || text == "false")) {
        return " = " + text;
    }
    if (type == "std::string" && text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        return "{" + text + "}";
    }
    if (const std::size_t scope = text.find("::");
        type == "int" && scope != std::string::npos && enums.contains(text.substr(0, scope))) {
        return " = " + text.substr(0, scope) + "::k" + pascal_case(text.substr(scope + 2));
    }
    if (type == "int" || type == "std::int64_t" || type == "float" || type == "double") {
        std::string number = text;
        bool negative = false;
        if (number.starts_with('-')) {
            negative = true;
            number.erase(0, 1);
        }
        while (!number.empty() && (number.back() == 'f' || number.back() == 'F')) {
            number.pop_back();
        }
        if (number.empty() || !std::ranges::all_of(number, [](char c) {
                return (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '-' ||
                       c == '+';
            })) {
            return {};
        }
        if (type == "int" || type == "std::int64_t") {
            if (number.find_first_of(".eE") != std::string::npos) {
                return {};
            }
            return " = " + std::string(negative ? "-" : "") + number;
        }
        if (number.ends_with('.')) {
            number += '0';
        }
        if (number.find_first_of(".eE") == std::string::npos) {
            number += ".0";
        }
        if (number.starts_with('.')) {
            number.insert(0, "0");
        }
        return " = " + std::string(negative ? "-" : "") + number + (type == "float" ? "F" : "");
    }
    return {};
}

// What Unreal calls on an actor or a component, whose parameters the system can't pass but the
// other actor's.
bool keeps_other_actor_only(std::string_view name) {
    return name == "NotifyActorBeginOverlap" || name == "NotifyActorEndOverlap" ||
           name == "NotifyHit";
}

class UnrealReader {
public:
    UnrealReader(std::string_view path, std::string_view header, std::string_view source,
                 std::string_view class_name)
        : header_(header), header_code_(code_of(header)), source_(source),
          source_code_(code_of(source)), class_name_(class_name) {
        script_.source = ScriptSource::UnrealCpp;
        script_.path = std::string(path);
        script_.class_name = std::string(class_name);
    }

    Script read() {
        read_enums();
        read_delegates();
        const std::size_t body = find_class();
        if (body == kNone) {
            name_script();
            return std::move(script_);
        }
        read_members(body + 1, closing(header_code_, body));
        name_script();
        return std::move(script_);
    }

private:
    void name_script() {
        std::string name = class_name_;
        if (name.size() > 1 && (name[0] == 'A' || name[0] == 'U') && name[1] >= 'A' &&
            name[1] <= 'Z') {
            name.erase(0, 1);
        }
        script_.component = cpp_identifier(name);
        script_.ns = cpp_identifier(snake_case(name));
        script_.system = script_.ns;
        const std::string& parent = script_.extends;
        script_.node = class_name_.starts_with('A') ||
                       (class_name_.starts_with('U') && parent.ends_with("Component"));
    }

    void read_enums() {
        for (std::size_t at = header_code_.find("UENUM("); at != kNone;
             at = header_code_.find("UENUM(", at + 1)) {
            const std::size_t keyword = find_word(header_code_, "enum", at);
            const std::size_t open = header_code_.find('{', keyword);
            if (keyword == kNone || open == kNone) {
                break;
            }
            const std::size_t close = closing(header_code_, open);
            if (close == kNone) {
                break;
            }
            std::string_view head =
                trimmed(std::string_view(header_code_).substr(keyword + 4, open - keyword - 4));
            if (head.starts_with("class ")) {
                head.remove_prefix(6);
            }
            ScriptEnum declared;
            declared.name = std::string(trimmed(head.substr(0, head.find(':'))));
            std::vector<std::string> written;
            for (std::string_view item :
                 split_top(std::string_view(header_code_).substr(open + 1, close - open - 1))) {
                if (const std::size_t meta = item.find("UMETA("); meta != kNone) {
                    item = trimmed(item.substr(0, meta));
                }
                if (item.empty()) {
                    continue;
                }
                const std::size_t equals = item.find('=');
                const std::string_view name = trimmed(item.substr(0, equals));
                std::string value = "k" + pascal_case(name);
                if (equals != kNone) {
                    value += " = " + std::string(trimmed(item.substr(equals + 1)));
                }
                declared.values.push_back(std::move(value));
                written.emplace_back(trimmed(item));
            }
            declared.declaration =
                "enum class " + declared.name + " { " + join(written, ", ") + " }";
            enums_.insert(declared.name);
            script_.enums.push_back(std::move(declared));
            at = close;
        }
    }

    void read_delegates() {
        for (std::size_t at = header_code_.find("DECLARE_"); at != kNone;
             at = header_code_.find("DECLARE_", at + 1)) {
            const std::size_t open = header_code_.find('(', at);
            const std::string_view macro =
                std::string_view(header_code_).substr(at, open == kNone ? 0 : open - at);
            if (open == kNone || macro.find("DELEGATE") == kNone ||
                !word_at(header_code_, at, macro)) {
                continue;
            }
            const std::size_t close = closing(header_code_, open);
            if (close == kNone) {
                break;
            }
            const std::vector<std::string_view> args =
                split_top(std::string_view(header_code_).substr(open + 1, close - open - 1));
            if (args.empty()) {
                continue;
            }
            Delegate delegate;
            delegate.declaration = one_line(std::string_view(header_).substr(at, close + 1 - at));
            const bool dynamic = macro.find("DYNAMIC") != kNone;
            const bool returns = macro.find("RetVal") != kNone;
            std::size_t first = returns ? 2 : 1;
            if (!dynamic && returns) {
                first = 1; // DECLARE_DELEGATE_RetVal(Type, Name, ...)
            }
            delegate.name = std::string(args[returns ? 1 : 0]);
            if (dynamic) {
                for (std::size_t i = first; i + 1 < args.size(); i += 2) {
                    delegate.params.push_back(
                        param(std::string(args[i]) + " " + std::string(args[i + 1])));
                }
            } else {
                for (std::size_t i = first; i < args.size(); ++i) {
                    delegate.params.push_back(
                        param(std::string(args[i]) + " Param" + std::to_string(i - first + 1)));
                }
            }
            types_.delegates.insert(delegate.name);
            delegates_.push_back(std::move(delegate));
            at = close;
        }
    }

    // The class's body's opening brace, or kNone.
    std::size_t find_class() {
        for (std::size_t at = header_code_.find("UCLASS("); at != kNone;
             at = header_code_.find("UCLASS(", at + 1)) {
            const std::size_t keyword = find_word(header_code_, "class", at);
            const std::size_t open = header_code_.find('{', keyword);
            if (keyword == kNone || open == kNone) {
                return kNone;
            }
            const std::string_view head =
                std::string_view(header_code_).substr(keyword + 5, open - keyword - 5);
            if (find_word(head, class_name_) == kNone) {
                continue;
            }
            if (const std::size_t colon = head.find(':'); colon != kNone) {
                for (const std::string_view base : split_top(head.substr(colon + 1))) {
                    std::string_view name = base;
                    for (const std::string_view access : {"public ", "protected ", "private "}) {
                        if (name.starts_with(access)) {
                            name = trimmed(name.substr(access.size()));
                        }
                    }
                    script_.extends = std::string(name);
                    break;
                }
            }
            script_.doc = doc_before(header_, at);
            return open;
        }
        return kNone;
    }

    struct Delegate {
        std::string name;
        std::vector<ScriptParam> params;
        std::string declaration;
    };

    [[nodiscard]] ScriptParam param(const std::string& written) const {
        auto [type, name] = type_and_name(written);
        ScriptParam out;
        out.name = member_name(name);
        out.source = one_line(written);
        out.type = parameter_type(types().map(type).type);
        return out;
    }

    [[nodiscard]] const TypeMapper& types() const {
        types_.enums = enums_;
        return types_;
    }

    // The class's members, from after its opening brace to its closing one.
    void read_members(std::size_t from, std::size_t end) {
        const std::string_view code = header_code_;
        std::string macro;            // the UPROPERTY or UFUNCTION before the declaration
        std::size_t macro_at = kNone; // where it starts
        std::size_t at = from;
        while (at < end) {
            while (at < end && is_space(code[at])) {
                ++at;
            }
            if (at >= end) {
                break;
            }
            // Access specifiers and the generated body.
            bool skipped = false;
            for (const std::string_view access : {"public:", "protected:", "private:"}) {
                if (code.substr(at, access.size()) == access) {
                    at += access.size();
                    skipped = true;
                }
            }
            if (skipped) {
                continue;
            }
            if (code.substr(at, 9) == "UPROPERTY" || code.substr(at, 9) == "UFUNCTION" ||
                code.substr(at, 13).starts_with("GENERATED_") ||
                code.substr(at, 13) == "UE_DEPRECATED") {
                const std::size_t open = code.find('(', at);
                const std::size_t close = open == kNone ? kNone : closing(code, open);
                if (close == kNone) {
                    return;
                }
                if (!code.substr(at).starts_with("GENERATED_")) {
                    macro = std::string(header_.substr(at, close + 1 - at)); // with its strings
                    macro_at = at;
                }
                at = close + 1;
                continue;
            }
            // A declaration: to its ';', or a function's body.
            std::size_t stop = at;
            std::size_t body = kNone;
            for (; stop < end; ++stop) {
                const char c = code[stop];
                if (c == '(' || c == '[') {
                    stop = closing(code, stop);
                    if (stop == kNone) {
                        return;
                    }
                } else if (c == '{') {
                    const std::string_view before = code.substr(at, stop - at);
                    const bool function = before.find('(') != kNone && before.find('=') == kNone;
                    const bool nested = word_at(code, at, "struct") || word_at(code, at, "enum") ||
                                        word_at(code, at, "class") || word_at(code, at, "union");
                    const std::size_t close = closing(code, stop);
                    if (close == kNone) {
                        return;
                    }
                    if (function || nested) {
                        body = stop;
                        stop = close;
                        break;
                    }
                    stop = close; // a brace initializer
                } else if (c == ';') {
                    break;
                }
            }
            const std::size_t start = macro_at == kNone ? at : macro_at;
            const std::string_view statement =
                trimmed(code.substr(at, (body == kNone ? stop : body) - at));
            declaration(statement, macro, start, at, body);
            macro.clear();
            macro_at = kNone;
            at = stop + 1;
            while (at < end && (code[at] == ';' || is_space(code[at]))) {
                ++at;
            }
        }
    }

    void declaration(std::string_view statement, const std::string& macro, std::size_t start,
                     std::size_t at, std::size_t body) {
        // An elaborated type, "class USphereComponent* Trigger", names the type that follows.
        for (const std::string_view keyword : {"class ", "struct ", "enum ", "union "}) {
            if (body == kNone && statement.starts_with(keyword) &&
                trimmed(statement.substr(keyword.size())).find_first_of(" *&") != kNone) {
                statement = trimmed(statement.substr(keyword.size()));
                break;
            }
        }
        for (const std::string_view skip : {"friend", "using", "typedef", "struct", "enum", "class",
                                            "union", "template", "static_assert", "DECLARE_"}) {
            if (statement.starts_with(skip)) {
                return;
            }
        }
        if (statement.starts_with('~') || statement.find("operator") != kNone) {
            return;
        }
        const bool function = find_paren(statement) != kNone;
        if (function) {
            read_function(statement, macro, at, body);
        } else {
            read_variable(statement, macro, start);
        }
    }

    // The '(' of a function's parameters, past any macro's parentheses: kNone for a variable.
    static std::size_t find_paren(std::string_view statement) {
        const std::size_t equals = statement.find('=');
        const std::size_t brace = statement.find('{');
        const std::size_t open = statement.find('(');
        if (open == kNone || (equals != kNone && equals < open) ||
            (brace != kNone && brace < open)) {
            return kNone;
        }
        return open;
    }

    void read_function(std::string_view statement, const std::string& macro, std::size_t at,
                       std::size_t body) {
        const std::size_t open = find_paren(statement);
        const std::size_t close = closing(statement, open);
        if (close == kNone) {
            return;
        }
        std::string_view head = trimmed(statement.substr(0, open));
        bool is_static = false;
        for (bool again = true; again;) {
            again = false;
            for (const std::string_view word : {"virtual ", "static ", "inline ", "FORCEINLINE ",
                                                "explicit ", "constexpr ", "FORCENOINLINE "}) {
                if (head.starts_with(word)) {
                    is_static = is_static || word == "static ";
                    head = trimmed(head.substr(word.size()));
                    again = true;
                }
            }
        }
        auto [returns, name] = type_and_name(head);
        if (name.empty() || name == "GENERATED_BODY") {
            return;
        }
        ScriptFunction function;
        function.source_name = name;
        function.name = cpp_identifier(snake_case(name));
        function.is_static = is_static;
        function.line = static_cast<int>(line_of(header_, at));
        function.declaration = one_line(statement);
        if (!macro.empty()) {
            function.declaration = one_line(macro) + " " + function.declaration;
        }
        if (name == class_name_) {
            function.name = "construct";
            function.returns = "void";
        } else if (plain_type(returns) != "void") {
            function.returns = types().map(returns).type;
        }
        const std::vector<std::string_view> params =
            split_top(statement.substr(open + 1, close - open - 1));
        for (const std::string_view written : params) {
            if (written == "void") {
                continue;
            }
            ScriptParam made = param(std::string(written));
            if (keeps_other_actor_only(name) &&
                (made.type != "entt::entity" || written.find("AActor") == kNone)) {
                continue;
            }
            function.params.push_back(std::move(made));
        }
        if (name == "TickComponent" && function.params.size() > 1) {
            function.params.resize(1); // the tick's seconds, which the system passes
        }
        function.body =
            body != kNone ? lines(header_, header_code_, body) : defined_body(name, params.size());
        if (function.body.empty() && body == kNone) {
            function.body.emplace_back("(defined in no source file the import read)");
        }
        if (!contains_name(name)) {
            script_.functions.push_back(std::move(function));
        }
    }

    [[nodiscard]] bool contains_name(const std::string& name) const {
        return std::ranges::any_of(script_.functions, [&](const ScriptFunction& function) {
            return function.source_name == name;
        });
    }

    // The lines between a body's braces, one indent less, tabs as four spaces.
    static std::vector<std::string> lines(std::string_view text, std::string_view code,
                                          std::size_t open) {
        const std::size_t close = closing(code, open);
        if (close == kNone) {
            return {};
        }
        std::vector<std::string> out;
        const std::string_view inside = text.substr(open + 1, close - open - 1);
        std::size_t start = 0;
        while (start <= inside.size()) {
            std::size_t stop = inside.find('\n', start);
            if (stop == kNone) {
                stop = inside.size();
            }
            std::string line;
            for (const char c : inside.substr(start, stop - start)) {
                if (c == '\t') {
                    line.append(4 - (line.size() % 4), ' ');
                } else if (c != '\r') {
                    line += c;
                }
            }
            while (!line.empty() && line.back() == ' ') {
                line.pop_back();
            }
            out.push_back(std::move(line));
            start = stop + 1;
        }
        while (!out.empty() && out.front().empty()) {
            out.erase(out.begin());
        }
        while (!out.empty() && out.back().empty()) {
            out.pop_back();
        }
        std::size_t indent = std::string::npos;
        for (const std::string& line : out) {
            if (!line.empty()) {
                indent = std::min(indent, line.find_first_not_of(' '));
            }
        }
        if (indent != std::string::npos) {
            for (std::string& line : out) {
                line.erase(0, std::min(indent, line.size()));
            }
        }
        return out;
    }

    // The body the source file defines for a function of the class, the first with as many
    // parameters when it is overloaded.
    [[nodiscard]] std::vector<std::string> defined_body(const std::string& name,
                                                        std::size_t count) const {
        const std::string qualified = class_name_ + "::" + name;
        std::vector<std::string> first;
        for (std::size_t at = source_code_.find(qualified); at != kNone;
             at = source_code_.find(qualified, at + 1)) {
            if (at > 0 && is_word(source_code_[at - 1])) {
                continue;
            }
            std::size_t open = at + qualified.size();
            while (open < source_code_.size() && is_space(source_code_[open])) {
                ++open;
            }
            if (open >= source_code_.size() || source_code_[open] != '(') {
                continue;
            }
            const std::size_t close = closing(source_code_, open);
            if (close == kNone) {
                continue;
            }
            const std::size_t params =
                split_top(std::string_view(source_code_).substr(open + 1, close - open - 1)).size();
            // The body, past a constructor's initializers.
            std::size_t brace = close + 1;
            while (brace < source_code_.size() && source_code_[brace] != '{' &&
                   source_code_[brace] != ';') {
                if (source_code_[brace] == '(') {
                    brace = closing(source_code_, brace);
                    if (brace == kNone) {
                        return first;
                    }
                }
                ++brace;
            }
            if (brace >= source_code_.size() || source_code_[brace] != '{') {
                continue;
            }
            std::vector<std::string> body = lines(source_, source_code_, brace);
            if (params == count) {
                return body;
            }
            if (first.empty()) {
                first = std::move(body);
            }
        }
        return first;
    }

    void read_variable(std::string_view statement, const std::string& macro, std::size_t start) {
        std::string_view text = statement;
        bool is_static = false;
        bool is_constant = false;
        for (bool again = true; again;) {
            again = false;
            for (const std::string_view word :
                 {"static ", "constexpr ", "inline ", "mutable ", "const "}) {
                if (text.starts_with(word)) {
                    is_static = is_static || word == "static ";
                    is_constant = is_constant || word == "constexpr " || word == "const ";
                    text = trimmed(text.substr(word.size()));
                    again = true;
                }
            }
        }
        // Its value: after '=', or in braces.
        std::string_view value;
        std::string_view declared = text;
        if (const std::size_t equals = text.find('='); equals != kNone) {
            value = trimmed(text.substr(equals + 1));
            declared = trimmed(text.substr(0, equals));
        } else if (const std::size_t brace = text.find('{');
                   brace != kNone && text.ends_with('}')) {
            value = trimmed(text.substr(brace + 1, text.size() - brace - 2));
            declared = trimmed(text.substr(0, brace));
        }
        // A bit field: "uint8 bDead : 1".
        if (const std::size_t colon = declared.find(" :"); colon != kNone) {
            declared = trimmed(declared.substr(0, colon));
        }
        auto [type, name] = type_and_name(declared);
        if (name.empty() || type.empty()) {
            return;
        }
        if (is_static) {
            if (is_constant && !value.empty()) {
                ScriptConstant constant;
                constant.name = "k" + pascal_case(name);
                constant.declaration = one_line(statement) + ";";
                const Mapped mapped = types().map(type);
                const std::string init = initializer(value, mapped.type, enums_);
                if (init.starts_with(" = ")) {
                    constant.type = mapped.type;
                    constant.value = init.substr(3);
                }
                script_.constants.push_back(std::move(constant));
            }
            return;
        }
        const std::string plain = plain_type(type);
        // A delegate the class broadcasts is a signal.
        if (types_.delegates.contains(plain)) {
            const auto found = std::ranges::find_if(
                delegates_, [&](const Delegate& delegate) { return delegate.name == plain; });
            ScriptSignal signal;
            signal.name = name;
            signal.params = found->params;
            signal.declaration = found->declaration;
            script_.signals.push_back(std::move(signal));
            return;
        }
        const Mapped mapped = types().map(type);
        ScriptField field;
        field.name = member_name(name);
        field.source_name = name;
        field.type = mapped.type;
        field.kind = mapped.kind;
        field.comment = mapped.comment;
        field.doc = doc_before(header_, start);
        if (!value.empty()) {
            field.init = initializer(value, mapped.type, enums_);
            if (field.init.empty() && value != "nullptr" && value != "NULL") {
                field.comment +=
                    std::string(field.comment.empty() ? "" : "; ") + "Unreal's " + one_line(value);
            }
        }
        if (mapped.kind == FieldKind::Entity) {
            field.init = " = entt::null";
        }
        field.exported = macro.find("EditAnywhere") != std::string::npos ||
                         macro.find("EditInstanceOnly") != std::string::npos;
        field.min = meta_number(macro, "ClampMin");
        field.max = meta_number(macro, "ClampMax");
        // Names fields take apart from the script's, as two Unreal names can snake alike.
        while (std::ranges::any_of(
            script_.fields, [&](const ScriptField& other) { return other.name == field.name; })) {
            field.name += "_";
        }
        script_.fields.push_back(std::move(field));
    }

    // A number a UPROPERTY's meta gives: ClampMin = "0" or ClampMin = 0.
    static std::optional<double> meta_number(std::string_view macro, std::string_view key) {
        const std::size_t at = macro.find(key);
        if (at == kNone) {
            return std::nullopt;
        }
        std::size_t i = macro.find('=', at);
        if (i == kNone) {
            return std::nullopt;
        }
        ++i;
        while (i < macro.size() && (is_space(macro[i]) || macro[i] == '"')) {
            ++i;
        }
        std::size_t end = i;
        while (end < macro.size() &&
               ((macro[end] >= '0' && macro[end] <= '9') || macro[end] == '.' ||
                macro[end] == '-' || macro[end] == 'e' || macro[end] == 'E' || macro[end] == '+')) {
            ++end;
        }
        if (end == i) {
            return std::nullopt;
        }
        const std::string number(macro.substr(i, end - i));
        char* last = nullptr;
        const double parsed = std::strtod(number.c_str(), &last);
        if (last != number.c_str() + number.size()) {
            return std::nullopt;
        }
        return parsed;
    }

    std::string_view header_;
    std::string header_code_;
    std::string_view source_;
    std::string source_code_;
    std::string class_name_;
    Script script_;
    std::set<std::string> enums_;
    mutable TypeMapper types_;
    std::vector<Delegate> delegates_;
};

} // namespace

Script read_unreal_class(std::string_view path, std::string_view header, std::string_view source,
                         std::string_view class_name) {
    return UnrealReader(path, header, source, class_name).read();
}

} // namespace slash::importer
