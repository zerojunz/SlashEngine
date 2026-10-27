#include "slash/command/l10n.hpp"

#include "l10n_extract.hpp"

#include "slash/command/session.hpp"
#include "slash/l10n/l10n.hpp"
#include "slash/l10n/locale.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::command {
namespace {

constexpr std::int64_t kMaxMessages = 256;
constexpr std::int64_t kMaxProblems = 50;
constexpr std::int64_t kMaxLocale = 64;

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value params_schema(json::Object properties) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = std::move(properties);
    return schema;
}

json::Value locale_property(std::string_view description) {
    json::Value schema = property("string", description);
    schema["minLength"] = 1;
    schema["maxLength"] = kMaxLocale;
    return schema;
}

json::Value strings(const std::vector<std::string>& values) {
    json::Array out;
    for (const std::string& value : values) {
        out.emplace_back(value);
    }
    return out;
}

// What the game would show for one of l10n.state's "tr" entries.
std::string translate(const world::World& world, const json::Value& entry) {
    if (entry.is_string()) {
        return l10n::tr(world, entry.as_string());
    }
    const std::string& message = entry.find("message")->as_string();
    const json::Value* context = entry.find("context");
    const std::string_view in = context != nullptr ? context->as_string() : std::string_view();
    if (const json::Value* plural = entry.find("plural"); plural != nullptr) {
        const json::Value* n = entry.find("n");
        return l10n::tr_n(world, message, plural->as_string(),
                          n != nullptr ? static_cast<int>(n->as_int()) : 1, in);
    }
    return l10n::tr(world, message, in);
}

Reply l10n_state(Session& session, const json::Value& params) {
    const world::World& world = session.world();
    const l10n::Settings settings = l10n::settings(world);
    json::Value out;
    out["locale"] = l10n::locale(world);
    if (const std::string system = l10n::system_locale(world);
        system != out["locale"].as_string()) {
        out["system_locale"] = system;
    }
    out["fallback"] = settings.fallback;
    out["locales"] = strings(l10n::loaded_locales(world));
    if (settings.pseudolocalization.use_pseudolocalization) {
        out["pseudolocalization"] = true;
    }
    const std::vector<l10n::FileProblem> problems = l10n::problems(world);
    if (!problems.empty()) {
        json::Array list;
        for (const l10n::FileProblem& problem : problems) {
            if (std::cmp_greater_equal(list.size(), kMaxProblems)) {
                break;
            }
            json::Value item;
            item["file"] = problem.file;
            if (problem.line > 0) {
                item["line"] = problem.line;
            }
            item["message"] = problem.message;
            list.push_back(std::move(item));
        }
        out["problems"] = std::move(list);
    }
    if (const json::Value* entries = params.find("tr"); entries != nullptr) {
        json::Array shown;
        for (const json::Value& entry : entries->as_array()) {
            shown.emplace_back(translate(world, entry));
        }
        out["tr"] = std::move(shown);
    }
    return Reply::success(std::move(out));
}

Reply l10n_set_locale(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const std::string& asked = params.find("locale")->as_string();
    const std::string locale = l10n::standardize_locale(asked);
    if (!l10n::is_locale(locale)) {
        json::Value data;
        data["path"] = "/locale";
        return Reply::failure(code::kInvalidParams,
                              R"(/locale: expected a locale such as "fr" or "pt_BR")",
                              std::move(data));
    }
    const std::string before = l10n::locale(world);
    l10n::set_locale(world, locale);
    json::Value out;
    out["locale"] = locale;
    if (before != locale) {
        out["was"] = before;
    }
    // A locale no translation answers shows the fallback's text, or the source's.
    const std::vector<std::string> loaded = l10n::loaded_locales(world);
    bool answered = false;
    for (const std::string& each : loaded) {
        answered = answered || l10n::compare_locales(locale, each) > 0;
    }
    if (!answered && !loaded.empty()) {
        out["warning"] = "no translation is in " + locale + "; texts show in " +
                         l10n::settings(world).fallback + " or as written";
    }
    return Reply::success(std::move(out));
}

} // namespace

void register_l10n_commands(Registry& registry) {
    using enum Capability;
    json::Value entry = params_schema({
        {"message", property("string", "The source text.")},
        {"context", property("string", "Tells apart messages written the same.")},
        {"plural", property("string", "The source's plural, as tr_n's.")},
        {"n", property("integer", "The number the plural form is for. Default 1.")},
    });
    entry["required"] = json::Array{"message"};
    json::Value one;
    one["oneOf"] = json::Array{property("string", "A message, as tr's."), std::move(entry)};
    json::Value tr = property("array", "Messages to show as the game would, in its locale.");
    tr["items"] = std::move(one);
    tr["maxItems"] = kMaxMessages;
    registry.add({
        .name = "l10n.state",
        .description = "Returns the game's locale, its fallback, the locales of the translations "
                       "loaded and what in the project's translation files didn't load; tr "
                       "gives messages as the game shows them.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({{"tr", std::move(tr)}}),
        .handler = l10n_state,
    });
    json::Value set =
        params_schema({{"locale", locale_property(R"(Such as "fr", "pt_BR" or "zh-Hant".)")}});
    set["required"] = json::Array{"locale"};
    registry.add({
        .name = "l10n.set_locale",
        .description = "Switches the game's language, as its options menu would; controls "
                       "show it at once and the game gets a translation_changed event.",
        .capabilities = {Run},
        .effect = Effect::Read,
        .params = std::move(set),
        .handler = l10n_set_locale,
    });
    register_l10n_extract(registry);
}

} // namespace slash::command
