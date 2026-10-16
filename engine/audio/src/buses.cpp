// .bus_layout.json files, as Godot's AudioBusLayout resources.

#include "slash/audio/audio.hpp"
#include "slash/audio/effects.hpp"
#include "slash/core/text.hpp"

#include <algorithm>
#include <string>

namespace slash::audio {

namespace {

constexpr std::size_t kMaxBuses = 64;
constexpr std::size_t kMaxEffects = 16;

json::Value flag(std::string_view doc) {
    json::Value value;
    value["type"] = "boolean";
    value["default"] = false;
    value["description"] = doc;
    return value;
}

std::string unknown_bus(std::string_view name, const std::vector<std::string>& candidates,
                        std::string_view where) {
    std::string message =
        "no bus " + std::string(where) + " is named \"" + std::string(name) + "\"";
    if (const std::string_view hint = closest_match(name, candidates); !hint.empty()) {
        message += "; did you mean \"" + std::string(hint) + "\"?";
    }
    return message;
}

} // namespace

BusLayout default_bus_layout() {
    BusLayout layout;
    BusSettings master;
    master.name = "Master";
    layout.buses.push_back(std::move(master));
    return layout;
}

json::Value bus_layout_schema() {
    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine bus layout";
    schema["description"] =
        "Audio buses, as Godot's AudioBusLayout: players mix into a bus, each bus runs its "
        "effects, applies its volume and mixes into the bus it sends to, and Master is what "
        "plays.";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "buses"};
    json::Value& properties = schema["properties"];
    properties["format"]["const"] = 1;
    properties["format"]["description"] = "Format version. Must be 1.";
    json::Value& buses = properties["buses"];
    buses["type"] = "array";
    buses["minItems"] = 1;
    buses["maxItems"] = kMaxBuses;
    buses["description"] = "The buses, Master first. A bus sends to one before it.";
    json::Value& bus = buses["items"];
    bus["type"] = "object";
    bus["additionalProperties"] = false;
    bus["required"] = json::Array{"name"};
    json::Value& fields = bus["properties"];
    fields["name"]["type"] = "string";
    fields["name"]["minLength"] = 1;
    fields["name"]["maxLength"] = 64;
    fields["name"]["description"] = "What players and sends call it; the first is \"Master\".";
    fields["volume_db"]["type"] = "number";
    fields["volume_db"]["minimum"] = -80;
    fields["volume_db"]["maximum"] = 24;
    fields["volume_db"]["default"] = 0;
    fields["volume_db"]["description"] = "Gain in dB, after its effects.";
    fields["solo"] = flag("While any bus is solo, only solo buses and those they send to play.");
    fields["mute"] = flag("Silences it.");
    fields["bypass_effects"] = flag("Skips its effects.");
    fields["send"]["type"] = "string";
    fields["send"]["minLength"] = 1;
    fields["send"]["default"] = "Master";
    fields["send"]["description"] = "The bus it mixes into, which must come before it; Master "
                                    "sends nowhere.";
    json::Value& effects = fields["effects"];
    effects["type"] = "array";
    effects["maxItems"] = kMaxEffects;
    effects["description"] = "Effects it runs in order on what was mixed into it.";
    effects["items"] = effect_schema();
    return schema;
}

std::vector<json::ValidationError> validate_bus_layout(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, bus_layout_schema());
    if (!errors.empty()) {
        return errors;
    }
    const json::Array& buses = doc.find("buses")->as_array();
    std::vector<std::string> names;
    for (std::size_t i = 0; i < buses.size(); ++i) {
        const std::string at = "/buses/" + std::to_string(i);
        const std::string& name = buses[i].find("name")->as_string();
        if (i == 0 && name != "Master") {
            errors.push_back({at + "/name", "the first bus is \"Master\""});
        } else if (i > 0 && std::ranges::find(names, name) != names.end()) {
            errors.push_back({at + "/name", "another bus is named \"" + name + "\""});
        }
        if (const json::Value* send = buses[i].find("send"); send != nullptr) {
            if (i == 0) {
                errors.push_back({at + "/send", "Master sends nowhere"});
            } else if (std::ranges::find(names, send->as_string()) == names.end()) {
                errors.push_back(
                    {at + "/send", unknown_bus(send->as_string(), names, "before this one")});
            }
        }
        names.push_back(name);
    }
    // A compressor's sidechain may name any bus.
    for (std::size_t i = 0; i < buses.size(); ++i) {
        const json::Value* effects = buses[i].find("effects");
        if (effects == nullptr) {
            continue;
        }
        const json::Array& list = effects->as_array();
        for (std::size_t j = 0; j < list.size(); ++j) {
            const json::Value* sidechain = list[j].find("sidechain");
            if (sidechain != nullptr && !sidechain->as_string().empty() &&
                std::ranges::find(names, sidechain->as_string()) == names.end()) {
                errors.push_back(
                    {"/buses/" + std::to_string(i) + "/effects/" + std::to_string(j) + "/sidechain",
                     unknown_bus(sidechain->as_string(), names, "in the layout")});
            }
        }
    }
    return errors;
}

Result<BusLayout> parse_bus_layout(const json::Value& doc) {
    if (const auto errors = validate_bus_layout(doc); !errors.empty()) {
        const json::ValidationError& first = errors.front();
        return make_error(ErrorCode::InvalidArgument,
                          (first.path.empty() ? std::string("root") : first.path) + ": " +
                              first.message);
    }
    BusLayout layout;
    for (const json::Value& entry : doc.find("buses")->as_array()) {
        BusSettings bus;
        bus.name = entry.find("name")->as_string();
        if (const json::Value* volume = entry.find("volume_db"); volume != nullptr) {
            bus.volume_db = static_cast<float>(volume->as_double());
        }
        if (const json::Value* solo = entry.find("solo"); solo != nullptr) {
            bus.solo = solo->as_bool();
        }
        if (const json::Value* mute = entry.find("mute"); mute != nullptr) {
            bus.mute = mute->as_bool();
        }
        if (const json::Value* bypass = entry.find("bypass_effects"); bypass != nullptr) {
            bus.bypass_effects = bypass->as_bool();
        }
        if (const json::Value* send = entry.find("send"); send != nullptr) {
            bus.send = send->as_string();
        }
        if (const json::Value* effects = entry.find("effects"); effects != nullptr) {
            for (const json::Value& item : effects->as_array()) {
                EffectSlot slot;
                json::Object params;
                for (const auto& [key, value] : item.as_object()) {
                    if (key == "type") {
                        slot.type = value.as_string();
                    } else if (key == "enabled") {
                        slot.enabled = value.as_bool();
                    } else {
                        params.emplace(key, value);
                    }
                }
                slot.params = json::Value(std::move(params));
                bus.effects.push_back(std::move(slot));
            }
        }
        layout.buses.push_back(std::move(bus));
    }
    return layout;
}

} // namespace slash::audio
