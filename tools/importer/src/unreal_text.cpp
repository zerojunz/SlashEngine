#include "unreal_text.hpp"

#include "slash/core/bits.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::importer::unreal_text {
namespace {

// Where each of text's top-level parts ends: at commas outside quotes and parentheses.
std::vector<std::string_view> top_level_parts(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    int depth = 0;
    bool quoted = false;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : ',';
        if (c == '"' && (i == 0 || text[i - 1] != '\\')) {
            quoted = !quoted;
        } else if (!quoted && c == '(') {
            ++depth;
        } else if (!quoted && c == ')') {
            --depth;
        } else if (!quoted && depth == 0 && c == ',') {
            if (const std::string_view part = trimmed(text.substr(start, i - start));
                !part.empty()) {
                out.push_back(part);
            }
            start = i + 1;
        }
    }
    return out;
}

// Where a part's '=' is, outside quotes and parentheses; npos for an item with no key.
std::size_t key_end(std::string_view part) {
    int depth = 0;
    bool quoted = false;
    for (std::size_t i = 0; i < part.size(); ++i) {
        const char c = part[i];
        if (c == '"') {
            quoted = !quoted;
        } else if (!quoted && c == '(') {
            ++depth;
        } else if (!quoted && c == ')') {
            --depth;
        } else if (!quoted && depth == 0 && c == '=') {
            return i;
        }
    }
    return std::string_view::npos;
}

} // namespace

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

// A value as Unreal writes it as text (ExportText: (Key=Value,...), (Item,Item), "quoted" or a
// bare word) as JSON: structs as objects, lists as arrays, the rest as strings. Keys written with
// an index, Inputs(0)=..., gather into arrays.
json::Value text_tree(std::string_view text) {
    text = trimmed(text);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        return std::string(text.substr(1, text.size() - 2));
    }
    if (text.size() < 2 || text.front() != '(' || text.back() != ')') {
        return std::string(text);
    }
    const std::vector<std::string_view> parts = top_level_parts(text.substr(1, text.size() - 2));
    const bool keyed = std::ranges::any_of(
        parts, [](std::string_view part) { return key_end(part) != std::string_view::npos; });
    if (!keyed) {
        json::Array list;
        for (const std::string_view part : parts) {
            list.push_back(text_tree(part));
        }
        return list;
    }
    json::Object out;
    for (const std::string_view part : parts) {
        const std::size_t equals = key_end(part);
        if (equals == std::string_view::npos) {
            continue;
        }
        std::string key(trimmed(part.substr(0, equals)));
        json::Value value = text_tree(part.substr(equals + 1));
        if (const std::size_t open = key.find('(');
            open != std::string::npos && key.back() == ')') {
            key.resize(open);
            json::Value& list = out[key];
            if (!list.is_array()) {
                list = json::Array{};
            }
            list.as_array().push_back(std::move(value));
            continue;
        }
        out[key] = std::move(value);
    }
    return out;
}

namespace {

// --- T3D ---

// One object of a T3D file: its class (NiagaraNodeFunctionCall), its path (/Game/FX/NS_Fire.
// NS_Fire:Sparks_0.NiagaraScriptSource_0...), its properties as written and its graph pins.
struct Object {
    std::string cls;
    std::string path;
    std::vector<std::pair<std::string_view, std::string_view>> properties;
    std::vector<json::Value> pins;

    [[nodiscard]] std::string_view property(std::string_view key) const {
        for (const auto& [name, value] : properties) {
            if (name == key) {
                return value;
            }
        }
        return {};
    }
};

// A T3D file's objects by path. Unreal writes each object twice: its class and name first, then,
// further down, its properties; both go to one Object.
struct Document {
    std::vector<Object> objects;
    std::map<std::string, std::size_t, std::less<>> by_path;

    [[nodiscard]] const Object* find(std::string_view path) const {
        const auto found = by_path.find(path);
        return found == by_path.end() ? nullptr : &objects[found->second];
    }
};

// A Begin Object line's attribute: Name="X" as X.
std::string_view attribute(std::string_view line, std::string_view key) {
    std::size_t at = 0;
    while ((at = line.find(key, at)) != std::string_view::npos) {
        const bool starts = at == 0 || line[at - 1] == ' ';
        at += key.size();
        if (starts && at < line.size() && line[at] == '=') {
            std::string_view value = line.substr(at + 1);
            if (value.starts_with('"')) {
                value.remove_prefix(1);
                return value.substr(0, value.find('"'));
            }
            return value.substr(0, value.find(' '));
        }
    }
    return {};
}

// What a reference names, between its quotes: /Game/A/B.B for /Script/Engine.StaticMesh'/Game/A/
// B.B', Sparks_0 for /Script/Niagara.NiagaraEmitter'Sparks_0'.
std::string_view referenced(std::string_view reference) {
    reference = trimmed(reference);
    if (reference.size() >= 2 && reference.front() == '"' && reference.back() == '"') {
        reference = reference.substr(1, reference.size() - 2);
    }
    const std::size_t open = reference.find('\'');
    const std::size_t close = reference.rfind('\'');
    if (open == std::string_view::npos || close <= open) {
        return reference;
    }
    return reference.substr(open + 1, close - open - 1);
}

// An asset's package: /Game/A/B for /Game/A/B.B.
std::string package_of(std::string_view object) {
    const std::size_t slash = object.rfind('/');
    const std::size_t dot = object.find('.', slash == std::string_view::npos ? 0 : slash);
    return std::string(object.substr(0, dot));
}

Document read_t3d(std::string_view text) {
    Document doc;
    std::vector<std::size_t> open; // the objects being read, npos for one that isn't kept
    constexpr auto kNone = static_cast<std::size_t>(-1);
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view line = trimmed(text.substr(start, end - start));
        start = end + 1;
        if (line.starts_with("Begin Object")) {
            const std::string_view exported = attribute(line, "ExportPath");
            const std::string_view path = referenced(exported);
            if (path.empty() || path == exported) {
                open.push_back(kNone);
                continue;
            }
            const auto [found, added] =
                doc.by_path.try_emplace(std::string(path), doc.objects.size());
            if (added) {
                std::string_view cls = exported.substr(0, exported.find('\''));
                cls.remove_prefix(cls.rfind('.') == std::string_view::npos ? 0
                                                                           : cls.rfind('.') + 1);
                doc.objects.push_back(Object{.cls = std::string(cls),
                                             .path = std::string(path),
                                             .properties = {},
                                             .pins = {}});
            }
            open.push_back(found->second);
        } else if (line.starts_with("End Object")) {
            if (!open.empty()) {
                open.pop_back();
            }
        } else if (!open.empty() && open.back() != kNone) {
            Object& object = doc.objects[open.back()];
            constexpr std::string_view kPin = "CustomProperties Pin ";
            if (line.starts_with(kPin)) {
                object.pins.push_back(text_tree(line.substr(kPin.size())));
            } else if (const std::size_t equals = line.find('=');
                       equals != std::string_view::npos && !line.starts_with("CustomProperties")) {
                object.properties.emplace_back(line.substr(0, equals), line.substr(equals + 1));
            }
        }
    }
    return doc;
}

// --- Values ---

std::string word(const json::Value& tree, std::string_view key) {
    const json::Value* found = tree.find(key);
    return found != nullptr && found->is_string() ? found->as_string() : std::string();
}

std::string number_text(float value) {
    return json::write(json::Value(value), json::Style::Compact);
}

// The values of a parameter store (rapid iteration parameters, a system's user parameters) by
// name, as Unreal writes such values as text: a number, (X=..,Y=..,Z=..) or (R=..,G=..,B=..,A=..).
// Its bytes don't say their types, so sizes do: four bytes are a float unless they read as a
// small whole number or -1 (true), as ints, bools and enums are kept.
std::map<std::string, std::string> parameter_values(const json::Value& store) {
    struct Entry {
        std::size_t offset = 0;
        std::string name;
    };
    std::vector<Entry> entries;
    if (const json::Value* offsets = store.find("SortedParameterOffsets");
        offsets != nullptr && offsets->is_array()) {
        for (const json::Value& item : offsets->as_array()) {
            const std::string offset = word(item, "Offset");
            std::size_t at = 0;
            bool digits = !offset.empty();
            for (const char c : offset) {
                digits = digits && std::isdigit(static_cast<unsigned char>(c)) != 0;
                at = (at * 10) + static_cast<std::size_t>(c - '0');
            }
            if (digits && !word(item, "Name").empty()) {
                entries.push_back({at, word(item, "Name")});
            }
        }
    }
    std::vector<std::uint8_t> bytes;
    if (const json::Value* data = store.find("ParameterData");
        data != nullptr && data->is_array()) {
        for (const json::Value& item : data->as_array()) {
            const std::string text = item.is_string() ? item.as_string() : std::string();
            int value = 0;
            for (const char c : text) {
                value = (value * 10) + (c - '0');
            }
            bytes.push_back(static_cast<std::uint8_t>(value));
        }
    }
    std::ranges::sort(entries, {}, &Entry::offset);
    const auto float_at = [&](std::size_t at) {
        std::uint32_t bits = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            bits |= static_cast<std::uint32_t>(bytes[at + i]) << (8 * i);
        }
        return bits;
    };
    std::map<std::string, std::string> out;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::size_t at = entries[i].offset;
        const std::size_t end = i + 1 < entries.size() ? entries[i + 1].offset : bytes.size();
        if (end <= at || end > bytes.size() || (end - at) % 4 != 0) {
            continue;
        }
        const std::size_t count = (end - at) / 4;
        const std::string& name = entries[i].name;
        if (count == 1) {
            const std::uint32_t bits = float_at(at);
            const auto value = slash::bit_cast<float>(bits);
            const auto whole = slash::bit_cast<std::int32_t>(bits);
            if (whole == -1) {
                out[name] = "True";
            } else if (std::isnan(value) || (bits != 0 && std::fabs(value) < 1e-30F)) {
                out[name] = std::to_string(whole);
            } else {
                out[name] = number_text(value);
            }
            continue;
        }
        std::string lower;
        for (const char c : name) {
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        const bool color = count == 4 && lower.find("color") != std::string::npos;
        static constexpr std::array<std::string_view, 4> kVector{"X", "Y", "Z", "W"};
        static constexpr std::array<std::string_view, 4> kColor{"R", "G", "B", "A"};
        if (count > 4) {
            continue;
        }
        std::string text = "(";
        for (std::size_t c = 0; c < count; ++c) {
            if (c > 0) {
                text += ',';
            }
            text += color ? kColor.at(c) : kVector.at(c);
            text += '=';
            text += number_text(slash::bit_cast<float>(float_at(at + (4 * c))));
        }
        out[name] = text + ")";
    }
    return out;
}

std::string quote(std::string_view text) {
    std::string out = "\"";
    out += text;
    out += '"';
    return out;
}

// --- Graphs ---

// An emitter's graph: its nodes by name, the override pins that set its functions' inputs by
// "Function.Input", and its functions' names.
class Graph {
public:
    Graph(const Document& doc, const Object& graph, std::map<std::string, std::string> constants,
          std::string emitter, const json::Value& enums)
        : doc_(doc), constants_(std::move(constants)), emitter_(std::move(emitter)), enums_(enums) {
        const std::string prefix = graph.path + ".";
        for (const Object& object : doc.objects) {
            if (object.path.starts_with(prefix) &&
                object.path.find('.', prefix.size()) == std::string::npos) {
                nodes_[object.path.substr(prefix.size())] = &object;
                if (object.cls == "NiagaraNodeFunctionCall") {
                    functions_.insert(function_name(object));
                }
            }
        }
        for (const auto& [name, node] : nodes_) {
            if (node->cls != "NiagaraNodeParameterMapSet") {
                continue;
            }
            for (const json::Value& pin : node->pins) {
                const std::string pin_name = word(pin, "PinName");
                const std::size_t dot = pin_name.find('.');
                if (!output(pin) && dot != std::string::npos &&
                    functions_.contains(pin_name.substr(0, dot))) {
                    overrides_[pin_name] = &pin;
                }
            }
        }
    }

    // Each stack's modules, by the script it runs in, in order: walked back from each output
    // node along the parameter map.
    json::Object scripts() {
        json::Object out;
        for (const auto& [name, node] : nodes_) {
            if (node->cls != "NiagaraNodeOutput") {
                continue;
            }
            std::string script(trimmed(node->property("ScriptType")));
            if (script.empty()) {
                script = "ParticleSpawnScript";
            }
            json::Array modules;
            std::set<const Object*> seen;
            const Object* at = nullptr;
            for (const json::Value& pin : node->pins) {
                if (!output(pin) && linked(&pin) != nullptr) {
                    at = linked(&pin);
                }
            }
            while (at != nullptr && seen.insert(at).second) {
                if (at->cls == "NiagaraNodeFunctionCall") {
                    modules.push_back(module_of(*at));
                    at = linked(pin_named(*at, "InputMap"));
                } else if (at->cls == "NiagaraNodeParameterMapSet") {
                    if (!overrides_only(*at)) {
                        modules.push_back(set_parameters(*at));
                    }
                    at = linked(pin_named(*at, "Source"));
                } else {
                    break;
                }
            }
            std::ranges::reverse(modules);
            out[script] = std::move(modules);
        }
        return out;
    }

private:
    static bool output(const json::Value& pin) { return word(pin, "Direction") == "EGPD_Output"; }

    static std::string function_name(const Object& node) {
        const std::string_view name = node.property("FunctionDisplayName");
        if (!name.empty()) {
            return std::string(referenced(name));
        }
        return node.path.substr(node.path.rfind('.') + 1);
    }

    static const json::Value* pin_named(const Object& node, std::string_view name) {
        for (const json::Value& pin : node.pins) {
            if (word(pin, "PinName") == name) {
                return &pin;
            }
        }
        return nullptr;
    }

    // The node a pin's first link reaches, and that link's pin ID.
    [[nodiscard]] std::pair<const Object*, std::string> link(const json::Value* pin) const {
        if (pin == nullptr) {
            return {};
        }
        const json::Value* links = pin->find("LinkedTo");
        if (links == nullptr || !links->is_array()) {
            return {};
        }
        for (const json::Value& item : links->as_array()) {
            const std::string text = item.is_string() ? item.as_string() : std::string();
            const std::size_t space = text.find(' ');
            if (space == std::string::npos) {
                continue;
            }
            const auto found = nodes_.find(text.substr(0, space));
            if (found != nodes_.end()) {
                return {found->second, text.substr(space + 1)};
            }
        }
        return {};
    }

    [[nodiscard]] const Object* linked(const json::Value* pin) const { return link(pin).first; }

    // Whether a parameter map set node only holds the inputs of the functions after it, as the
    // stack's override nodes do, rather than being a Set Parameters module.
    [[nodiscard]] bool overrides_only(const Object& node) const {
        return std::ranges::all_of(node.pins, [&](const json::Value& pin) {
            const std::string name = word(pin, "PinName");
            if (output(pin) || name == "Source" || name == "Add") {
                return true;
            }
            const std::size_t dot = name.find('.');
            return dot != std::string::npos && functions_.contains(name.substr(0, dot));
        });
    }

    json::Value module_of(const Object& node) {
        json::Object out;
        out["module"] = function_name(node);
        out["enabled"] = trimmed(node.property("EnabledState")) != "Disabled";
        out["script"] = package_of(referenced(node.property("FunctionScript")));
        out["inputs"] = inputs_of(node, 0);
        return out;
    }

    json::Value set_parameters(const Object& node) {
        json::Object inputs;
        for (const json::Value& pin : node.pins) {
            const std::string name = word(pin, "PinName");
            if (output(pin) || name == "Source" || name == "Add") {
                continue;
            }
            if (auto value = pin_value(pin, name, 0)) {
                inputs[name] = std::move(*value);
            }
        }
        json::Object out;
        out["module"] = "Set Parameters";
        out["enabled"] = trimmed(node.property("EnabledState")) != "Disabled";
        out["set_parameters"] = true;
        out["inputs"] = std::move(inputs);
        return out;
    }

    // A function's inputs: its static switches, then what the script's rapid iteration parameters
    // hold, then what override pins set.
    json::Object inputs_of(const Object& node, int depth) {
        json::Object out;
        const std::string name = function_name(node);
        for (const json::Value& pin : node.pins) {
            const std::string pin_name = word(pin, "PinName");
            if (output(pin) || pin_name == "InputMap" || pin.find("DefaultValue") == nullptr) {
                continue;
            }
            out[pin_name] = json::Object{{"value", "(Value=" + quote(switch_value(pin)) + ")"}};
        }
        const std::string constant = "Constants." + emitter_ + "." + name + ".";
        for (auto at = constants_.lower_bound(constant);
             at != constants_.end() && at->first.starts_with(constant); ++at) {
            out[at->first.substr(constant.size())] =
                json::Object{{"value", "(Value=(Value=" + at->second + "))"}};
        }
        const std::string prefix = name + ".";
        for (auto at = overrides_.lower_bound(prefix);
             at != overrides_.end() && at->first.starts_with(prefix); ++at) {
            const std::string input = at->first.substr(prefix.size());
            if (auto value = pin_value(*at->second, input, depth)) {
                out[input] = std::move(*value);
            }
        }
        return out;
    }

    // A static switch's value: an enum's by its display name.
    [[nodiscard]] std::string switch_value(const json::Value& pin) const {
        std::string value = word(pin, "DefaultValue");
        const std::string type(referenced(word(pin, "PinType.PinSubCategoryObject")));
        if (const json::Value* names = enums_.find(type)) {
            if (const json::Value* shown = names->find(value);
                shown != nullptr && shown->is_string()) {
                return shown->as_string();
            }
        }
        return value;
    }

    // What an override pin gives its input: the parameter or dynamic input linked to it, else
    // the value typed into it; nullopt for an unlinked pin with none, which leaves the input's
    // rapid iteration parameter.
    std::optional<json::Value> pin_value(const json::Value& pin, std::string_view input,
                                         int depth) {
        const auto [source, source_pin] = link(&pin);
        if (source == nullptr) {
            const std::string value = word(pin, "DefaultValue");
            if (value.empty()) {
                return std::nullopt;
            }
            return json::Object{{"value", "(Value=(Value=" + typed_value(value, input) + "))"}};
        }
        if (source->cls == "NiagaraNodeParameterMapGet") {
            std::string parameter;
            for (const json::Value& out : source->pins) {
                if (word(out, "PinId") == source_pin) {
                    parameter = word(out, "PinName");
                }
            }
            return json::Object{{"value", "(LinkedParameter=" + quote(parameter) + ")"}};
        }
        if (source->cls == "NiagaraNodeFunctionCall") {
            const std::string script = package_of(referenced(source->property("FunctionScript")));
            json::Object out{{"value", "(DynamicInput=" + quote(script) + ")"}};
            if (depth < 8) {
                std::string chain = "(Inputs=(";
                bool first = true;
                for (const auto& [name, value] : inputs_of(*source, depth + 1)) {
                    chain += first ? "" : ",";
                    chain += "(Name=" + quote(name) + ",Value=" + word(value, "value") + ")";
                    first = false;
                }
                out["chain"] = chain + "))";
            }
            return out;
        }
        if (source->cls == "NiagaraNodeInput") {
            // A data interface (a curve, say) is an object of its own under the node.
            const std::string inside = source->path + ".";
            for (const Object& object : doc_.objects) {
                if (object.path.starts_with(inside) &&
                    object.cls.starts_with("NiagaraDataInterface")) {
                    return json::Object{{"value", "(DataInterface=" + quote(object.cls) + ")"}};
                }
            }
            const std::string name = word(text_tree(source->property("Input")), "Name");
            return json::Object{{"value", "(LinkedParameter=" + quote(name) + ")"}};
        }
        return json::Object{{"value", "(Node=" + quote(source->cls) + ")"}};
    }

    // A value typed into a pin as Unreal writes such values: "1,2,3" as a vector or color.
    static std::string typed_value(const std::string& value, std::string_view input) {
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (start <= value.size()) {
            std::size_t comma = value.find(',', start);
            if (comma == std::string::npos) {
                comma = value.size();
            }
            parts.emplace_back(trimmed(std::string_view(value).substr(start, comma - start)));
            start = comma + 1;
        }
        if (parts.size() < 2 || parts.size() > 4) {
            return value;
        }
        std::string lower(input);
        for (char& c : lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        const bool color = lower.find("color") != std::string::npos;
        static constexpr std::array<std::string_view, 4> kVector{"X", "Y", "Z", "W"};
        static constexpr std::array<std::string_view, 4> kColor{"R", "G", "B", "A"};
        std::string out = "(";
        for (std::size_t i = 0; i < parts.size(); ++i) {
            out += i > 0 ? "," : "";
            out += color ? kColor.at(i) : kVector.at(i);
            out += "=" + parts[i];
        }
        return out + ")";
    }

    const Document& doc_;
    std::map<std::string, const Object*, std::less<>> nodes_;
    std::map<std::string, const json::Value*, std::less<>> overrides_;
    std::set<std::string, std::less<>> functions_;
    std::map<std::string, std::string> constants_;
    std::string emitter_;
    const json::Value& enums_;
};

json::Value emitter_of(const Document& doc, const Object& emitter, const json::Value& handle,
                       const json::Value& enums) {
    json::Object out;
    std::string name(referenced(emitter.property("UniqueEmitterName")));
    if (name.empty()) {
        name = word(handle, "Name");
    }
    out["name"] = name;
    out["enabled"] = word(handle, "bIsEnabled") != "False";
    const json::Value version = text_tree(emitter.property("VersionData(0)"));
    out["sim_target"] =
        word(version, "SimTarget") == "GPUComputeSim" ? "gpu_compute_sim" : "cpu_sim";
    json::Object data;
    if (word(version, "bLocalSpace") == "True") {
        data["bLocalSpace"] = true;
    }
    out["data"] = std::move(data);

    // The values every script of the emitter keeps for its modules' inputs.
    std::map<std::string, std::string> constants;
    const std::string prefix = emitter.path + ".";
    const Object* graph = nullptr;
    for (const Object& object : doc.objects) {
        if (!object.path.starts_with(prefix)) {
            continue;
        }
        if (object.cls == "NiagaraScript") {
            constants.merge(
                parameter_values(text_tree(object.property("RapidIterationParameters"))));
        } else if (object.cls == "NiagaraGraph" && graph == nullptr) {
            graph = &object;
        }
    }
    if (graph != nullptr) {
        out["scripts"] = Graph(doc, *graph, std::move(constants), name, enums).scripts();
    }

    json::Array renderers;
    if (const json::Value* list = version.find("RendererProperties"); list != nullptr) {
        const json::Array none;
        for (const json::Value& item : list->is_array() ? list->as_array() : none) {
            const Object* renderer =
                item.is_string() ? doc.find(prefix + std::string(referenced(item.as_string())))
                                 : nullptr;
            if (renderer == nullptr || trimmed(renderer->property("bIsEnabled")) == "False") {
                continue;
            }
            json::Object entry{{"class", renderer->cls}};
            json::Array meshes;
            for (const auto& [key, value] : renderer->properties) {
                if (key.starts_with("Meshes(")) {
                    const std::string mesh = word(text_tree(value), "Mesh");
                    if (!mesh.empty()) {
                        meshes.emplace_back(package_of(referenced(mesh)));
                    }
                }
            }
            if (!meshes.empty()) {
                entry["meshes"] = std::move(meshes);
            }
            renderers.emplace_back(std::move(entry));
        }
    }
    out["renderers"] = std::move(renderers);
    return out;
}

} // namespace

json::Value niagara_from_text(std::string_view text, const json::Value& enums) {
    const Document doc = read_t3d(text);
    json::Object out;
    const Object* system = nullptr;
    for (const Object& object : doc.objects) {
        if (object.cls == "NiagaraSystem") {
            system = &object;
            break;
        }
    }
    if (system == nullptr) {
        return out;
    }
    json::Array user;
    for (const auto& [name, value] :
         parameter_values(text_tree(system->property("ExposedParameters")))) {
        if (name.starts_with("User.")) {
            user.emplace_back("(Name=" + quote(name) + ",Value=" + value + ")");
        }
    }
    out["user_variables"] = std::move(user);
    json::Array emitters;
    for (const auto& [key, value] : system->properties) {
        if (!key.starts_with("EmitterHandles(")) {
            continue;
        }
        const json::Value handle = text_tree(value);
        const json::Value* instance = handle.find("VersionedInstance");
        const std::string reference =
            instance != nullptr ? std::string(referenced(word(*instance, "Emitter"))) : "";
        const Object* emitter = reference.find(':') != std::string::npos
                                    ? doc.find(reference)
                                    : doc.find(system->path + ":" + reference);
        if (emitter != nullptr) {
            emitters.push_back(emitter_of(doc, *emitter, handle, enums));
        }
    }
    out["emitters"] = std::move(emitters);
    return out;
}

} // namespace slash::importer::unreal_text
