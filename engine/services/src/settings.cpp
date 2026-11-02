#include "slash/core/text.hpp"
#include "slash/services/services.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace slash::services {
namespace {

constexpr std::array<std::string_view, 6> kPlatforms{"android", "ios",   "macos",
                                                     "windows", "linux", "web"};
constexpr std::array<std::string_view, 3> kFormats{"banner", "interstitial", "rewarded"};
constexpr std::array<std::string_view, 3> kTypes{"consumable", "non_consumable", "subscription"};
constexpr std::array<std::string_view, 3> kConsents{"unknown", "granted", "denied"};
constexpr std::array<std::string_view, 2> kSortOrders{"high_to_low", "low_to_high"};
constexpr std::array<std::string_view, 2> kEndpointFormats{"json", "posthog"};

constexpr std::size_t kMaxPlacementName = 64;
constexpr std::size_t kMaxProductId = 100;
// Play Games' most steps for an incremental achievement.
constexpr std::int64_t kMaxSteps = 10'000;

json::Value enum_property(std::span<const std::string_view> values, std::string_view description) {
    json::Value schema;
    json::Array items;
    for (const std::string_view value : values) {
        items.emplace_back(std::string(value));
    }
    schema["enum"] = std::move(items);
    schema["description"] = description;
    return schema;
}

json::Value object_schema(std::string_view description) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["description"] = description;
    return schema;
}

bool is_letter_or_digit(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// Placement, achievement and leaderboard names as entity IDs: letters, digits, '_' and '-'.
bool valid_placement_name(std::string_view name) {
    return !name.empty() && name.size() <= kMaxPlacementName &&
           std::ranges::all_of(
               name, [](char c) { return is_letter_or_digit(c) || c == '_' || c == '-'; });
}

// Product IDs as both Google Play and the App Store take them: a letter or digit, then letters,
// digits, '_' and '.'.
bool valid_product_id(std::string_view id) {
    return !id.empty() && id.size() <= kMaxProductId && is_letter_or_digit(id.front()) &&
           std::ranges::all_of(
               id, [](char c) { return is_letter_or_digit(c) || c == '_' || c == '.'; });
}

std::string escape_pointer(std::string_view key) {
    std::string out;
    for (const char c : key) {
        if (c == '~') {
            out += "~0";
        } else if (c == '/') {
            out += "~1";
        } else {
            out += c;
        }
    }
    return out;
}

Consent parse_consent(std::string_view name) {
    if (name == "granted") {
        return Consent::Granted;
    }
    if (name == "denied") {
        return Consent::Denied;
    }
    return Consent::Unknown;
}

AdFormat parse_format(std::string_view name) {
    if (name == "banner") {
        return AdFormat::Banner;
    }
    if (name == "rewarded") {
        return AdFormat::Rewarded;
    }
    return AdFormat::Interstitial;
}

json::Value platform_ids(std::string_view description) {
    json::Value ids = object_schema(description);
    json::Value id;
    id["type"] = "string";
    id["minLength"] = 1;
    ids["additionalProperties"] = std::move(id);
    return ids;
}

// Errors for platforms in an object of IDs by platform that no export has.
void check_platforms(const json::Value& ids, const std::string& path,
                     std::vector<json::ValidationError>& errors) {
    for (const auto& [platform, id] : ids.as_object()) {
        if (std::ranges::find(kPlatforms, platform) != kPlatforms.end()) {
            continue;
        }
        std::string message = "no platform \"" + platform + "\"";
        if (const std::string_view hint = closest_match(platform, kPlatforms); !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        }
        errors.push_back({path + "/ids/" + escape_pointer(platform), message});
    }
}

std::map<std::string, std::string, std::less<>> parse_ids(const json::Value& item) {
    std::map<std::string, std::string, std::less<>> out;
    if (const json::Value* ids = item.find("ids"); ids != nullptr) {
        for (const auto& [platform, id] : ids->as_object()) {
            out.emplace(platform, id.as_string());
        }
    }
    return out;
}

ProductType parse_type(std::string_view name) {
    if (name == "non_consumable") {
        return ProductType::NonConsumable;
    }
    if (name == "subscription") {
        return ProductType::Subscription;
    }
    return ProductType::Consumable;
}

} // namespace

std::string_view format_name(AdFormat format) {
    return kFormats.at(static_cast<std::size_t>(format));
}

std::string_view type_name(ProductType type) {
    return kTypes.at(static_cast<std::size_t>(type));
}

std::string_view tracking_name(TrackingStatus status) {
    static constexpr std::array<std::string_view, 5> kNames{"not_determined", "restricted",
                                                            "denied", "authorized", "unavailable"};
    return kNames.at(static_cast<std::size_t>(status));
}

std::string_view consent_name(Consent consent) {
    return kConsents.at(static_cast<std::size_t>(consent));
}

std::string_view sort_order_name(SortOrder order) {
    return kSortOrders.at(static_cast<std::size_t>(order));
}

json::Value settings_schema() {
    json::Value reward = object_schema("What a watched rewarded ad gives when the ad network "
                                       "doesn't say, e.g. {\"type\": \"coins\", \"amount\": 50}.");
    reward["required"] = json::Array{"type", "amount"};
    reward["properties"]["type"]["type"] = "string";
    reward["properties"]["type"]["minLength"] = 1;
    reward["properties"]["amount"]["type"] = "number";
    reward["properties"]["amount"]["minimum"] = 0;

    json::Value ids = platform_ids("The ad network's ad unit ID on each platform: \"android\", "
                                   "\"ios\", \"macos\", \"windows\", \"linux\" or \"web\".");

    json::Value placement = object_schema("An ad placement.");
    placement["required"] = json::Array{"format"};
    placement["properties"]["format"] = enum_property(kFormats, "The kind of ad.");
    placement["properties"]["ids"] = std::move(ids);
    placement["properties"]["reward"] = std::move(reward);
    placement["properties"]["auto_load"]["type"] = "boolean";
    placement["properties"]["auto_load"]["default"] = true;
    placement["properties"]["auto_load"]["description"] =
        "Load at start and again after each time the ad closes.";

    json::Value placements;
    placements["type"] = "object";
    placements["description"] =
        "Ad placements by the name the game shows them by: letters, digits, '_' and '-'.";
    placements["additionalProperties"] = std::move(placement);

    json::Value ads = object_schema("Ads, as Unity's Advertisement.");
    ads["properties"]["placements"] = std::move(placements);

    json::Value product = object_schema("A product.");
    product["required"] = json::Array{"type"};
    product["properties"]["type"] =
        enum_property(kTypes, "Consumables can be bought again once finished; non-consumables "
                              "and subscriptions are owned.");

    json::Value products;
    products["type"] = "object";
    products["description"] = "Products by the store's product ID, the same on every store: a "
                              "letter or digit, then letters, digits, '_' and '.'.";
    products["additionalProperties"] = std::move(product);

    json::Value purchases = object_schema("In-app purchases, as Unity IAP.");
    purchases["properties"]["products"] = std::move(products);

    json::Value analytics = object_schema("Analytics events.");
    analytics["properties"]["enabled"]["type"] = "boolean";
    analytics["properties"]["enabled"]["default"] = true;
    analytics["properties"]["enabled"]["description"] =
        "Send events to the analytics backends while the player consents.";

    json::Value endpoint = object_schema(
        "Send events, batched as JSON, to a server you run, when no SDK package sends them.");
    endpoint["required"] = json::Array{"url"};
    endpoint["properties"]["url"]["type"] = "string";
    endpoint["properties"]["url"]["description"] = "An https:// URL, or http:// on this machine.";
    endpoint["properties"]["format"] =
        enum_property(kEndpointFormats,
                      "json (the engine's own) or posthog (PostHog's batch API). Default: json.");
    endpoint["properties"]["api_key"]["type"] = "string";
    endpoint["properties"]["api_key"]["description"] =
        "PostHog's project API key, which only writes events.";
    json::Value header;
    header["type"] = "string";
    endpoint["properties"]["headers"] = object_schema("More HTTP headers. Not for secrets.");
    endpoint["properties"]["headers"]["additionalProperties"] = std::move(header);
    const auto number = [&](std::string_view name, std::string_view type, double minimum,
                            double maximum, double fallback, std::string_view description) {
        json::Value& property = endpoint["properties"][name];
        property["type"] = type;
        property["minimum"] = minimum;
        property["maximum"] = maximum;
        property["default"] = fallback;
        property["description"] = description;
    };
    number("batch_size", "integer", 1, 1000, 20, "Events sent together.");
    number("flush_seconds", "number", 1, 3600, 30, "The longest an event waits for its batch.");
    number("session_minutes", "number", 1, 1440, 30,
           "A new session starts after this long without events.");
    number("max_queued", "integer", 1, 100'000, 1000, "Unsent events kept; the oldest go first.");
    analytics["properties"]["endpoint"] = std::move(endpoint);

    json::Value step;
    step["type"] = "string";
    json::Value funnel;
    funnel["type"] = "array";
    funnel["minItems"] = 2;
    funnel["items"] = std::move(step);
    json::Value funnels;
    funnels["type"] = "object";
    funnels["description"] = "Funnels by name: the events a player goes through, in order.";
    funnels["additionalProperties"] = std::move(funnel);
    analytics["properties"]["funnels"] = std::move(funnels);

    json::Value consent = object_schema("The player's consent until the game sets it.");
    json::Value analytics_consent =
        enum_property(kConsents, "Events are sent only when granted. Default: unknown.");
    json::Value ads_consent =
        enum_property(kConsents, "Ads are personalized only when granted. Default: unknown.");
    consent["properties"]["analytics"] = std::move(analytics_consent);
    consent["properties"]["personalized_ads"] = std::move(ads_consent);
    consent["properties"]["tracking_usage"]["type"] = "string";
    consent["properties"]["tracking_usage"]["description"] =
        "Why the game asks to track the player, shown in Apple's tracking prompt.";

    json::Value achievement = object_schema("An achievement.");
    achievement["properties"]["ids"] =
        platform_ids("Its ID in Google Play Games (\"android\") and Game Center (\"ios\", "
                     "\"macos\").");
    achievement["properties"]["steps"]["type"] = "integer";
    achievement["properties"]["steps"]["minimum"] = 2;
    achievement["properties"]["steps"]["maximum"] = kMaxSteps;
    achievement["properties"]["steps"]["description"] =
        "Steps that unlock an incremental achievement; leave it out for one unlocked at once.";

    json::Value achievements;
    achievements["type"] = "object";
    achievements["description"] =
        "Achievements by the name the game unlocks them by: letters, digits, '_' and '-'.";
    achievements["additionalProperties"] = std::move(achievement);

    json::Value leaderboard = object_schema("A leaderboard.");
    leaderboard["properties"]["ids"] =
        platform_ids("Its ID in Google Play Games (\"android\") and Game Center (\"ios\", "
                     "\"macos\").");
    leaderboard["properties"]["sort_order"] =
        enum_property(kSortOrders, "Which scores rank first, as set up in Play Console and App "
                                   "Store Connect. Default: high_to_low.");

    json::Value leaderboards;
    leaderboards["type"] = "object";
    leaderboards["description"] =
        "Leaderboards by the name the game submits scores by: letters, digits, '_' and '-'.";
    leaderboards["additionalProperties"] = std::move(leaderboard);

    json::Value games = object_schema("Achievements, leaderboards and sign-in, as Unity's Social "
                                      "API over Google Play Games and Game Center.");
    games["properties"]["achievements"] = std::move(achievements);
    games["properties"]["leaderboards"] = std::move(leaderboards);
    games["properties"]["sign_in_at_start"]["type"] = "boolean";
    games["properties"]["sign_in_at_start"]["default"] = true;
    games["properties"]["sign_in_at_start"]["description"] =
        "Sign the player in when the game starts, as the platforms do.";

    json::Value schema = object_schema("Ads, analytics, in-app purchases, achievements and "
                                       "leaderboards, run by SDK packages' backends.");
    schema["properties"]["ads"] = std::move(ads);
    schema["properties"]["purchases"] = std::move(purchases);
    schema["properties"]["analytics"] = std::move(analytics);
    schema["properties"]["consent"] = std::move(consent);
    schema["properties"]["games"] = std::move(games);
    return schema;
}

namespace {

// The endpoint's URL and key, and the funnels' names and steps.
void check_analytics(const json::Value& analytics, const std::string& path,
                     std::vector<json::ValidationError>& errors) {
    if (const json::Value* endpoint = analytics.find("endpoint"); endpoint != nullptr) {
        const std::string& url = endpoint->find("url")->as_string();
        if (!url.starts_with("https://") && !url.starts_with("http://")) {
            errors.push_back({path + "/endpoint/url", "expected an https:// or http:// URL"});
        }
        const json::Value* format = endpoint->find("format");
        const json::Value* key = endpoint->find("api_key");
        if (format != nullptr && format->as_string() == "posthog" &&
            (key == nullptr || key->as_string().empty())) {
            errors.push_back({path + "/endpoint/api_key", "PostHog needs the project's API key"});
        }
    }
    if (const json::Value* funnels = analytics.find("funnels"); funnels != nullptr) {
        for (const auto& [name, steps] : funnels->as_object()) {
            const std::string at = path + "/funnels/" + escape_pointer(name);
            if (!valid_placement_name(name)) {
                errors.push_back(
                    {at, "expected a funnel name of letters, digits, '_' and '-', at most 64 "
                         "of them"});
            }
            for (std::size_t i = 0; i < steps.size(); ++i) {
                if (!valid_event_name(steps.as_array()[i].as_string())) {
                    errors.push_back({at + "/" + std::to_string(i),
                                      "expected an event name: a letter, then letters, digits "
                                      "and '_', at most 40 of them"});
                }
            }
        }
    }
}

} // namespace

std::vector<json::ValidationError> validate_settings(const json::Value& settings,
                                                     std::string_view at) {
    std::vector<json::ValidationError> errors = json::validate(settings, settings_schema());
    for (json::ValidationError& error : errors) {
        error.path = std::string(at) + error.path;
    }
    if (!errors.empty()) {
        return errors;
    }
    const json::Value* ads = settings.find("ads");
    const json::Value* placements = ads != nullptr ? ads->find("placements") : nullptr;
    if (placements != nullptr) {
        for (const auto& [name, placement] : placements->as_object()) {
            const std::string path = std::string(at) + "/ads/placements/" + escape_pointer(name);
            if (!valid_placement_name(name)) {
                errors.push_back({path, "expected a placement name of letters, digits, '_' and "
                                        "'-', at most 64 of them"});
            }
            if (const json::Value* ids = placement.find("ids"); ids != nullptr) {
                check_platforms(*ids, path, errors);
            }
            if (placement.find("reward") != nullptr &&
                placement.find("format")->as_string() != "rewarded") {
                errors.push_back({path + "/reward", "only rewarded placements give a reward"});
            }
        }
    }
    const json::Value* purchases = settings.find("purchases");
    const json::Value* products = purchases != nullptr ? purchases->find("products") : nullptr;
    if (products != nullptr) {
        for (const auto& [id, product] : products->as_object()) {
            if (!valid_product_id(id)) {
                errors.push_back(
                    {std::string(at) + "/purchases/products/" + escape_pointer(id),
                     "expected a product ID of a letter or digit, then letters, digits, '_' and "
                     "'.', at most 100 of them"});
            }
        }
    }
    if (const json::Value* analytics = settings.find("analytics"); analytics != nullptr) {
        check_analytics(*analytics, std::string(at) + "/analytics", errors);
    }
    if (const json::Value* games = settings.find("games"); games != nullptr) {
        for (const std::string_view kind : {"achievements", "leaderboards"}) {
            const json::Value* named = games->find(kind);
            if (named == nullptr) {
                continue;
            }
            for (const auto& [name, item] : named->as_object()) {
                const std::string path =
                    std::string(at) + "/games/" + std::string(kind) + "/" + escape_pointer(name);
                if (!valid_placement_name(name)) {
                    errors.push_back(
                        {path, "expected a name of letters, digits, '_' and '-', at most 64 of "
                               "them"});
                }
                if (const json::Value* ids = item.find("ids"); ids != nullptr) {
                    check_platforms(*ids, path, errors);
                }
            }
        }
    }
    return errors;
}

Settings parse_settings(const json::Value& settings) {
    Settings out;
    if (!settings.is_object()) {
        return out;
    }
    const json::Value* ads = settings.find("ads");
    if (const json::Value* placements = ads != nullptr ? ads->find("placements") : nullptr;
        placements != nullptr) {
        for (const auto& [name, item] : placements->as_object()) {
            Placement placement;
            placement.name = name;
            const std::string& format = item.find("format")->as_string();
            placement.format = parse_format(format);
            placement.ids = parse_ids(item);
            if (const json::Value* reward = item.find("reward"); reward != nullptr) {
                placement.reward = {.type = reward->find("type")->as_string(),
                                    .amount = reward->find("amount")->as_double()};
            }
            if (const json::Value* auto_load = item.find("auto_load"); auto_load != nullptr) {
                placement.auto_load = auto_load->as_bool();
            }
            out.placements.push_back(std::move(placement));
        }
    }
    const json::Value* purchases = settings.find("purchases");
    if (const json::Value* products = purchases != nullptr ? purchases->find("products") : nullptr;
        products != nullptr) {
        for (const auto& [id, item] : products->as_object()) {
            Product product;
            product.id = id;
            const std::string& type = item.find("type")->as_string();
            product.type = parse_type(type);
            out.products.push_back(std::move(product));
        }
    }
    if (const json::Value* analytics = settings.find("analytics"); analytics != nullptr) {
        if (const json::Value* enabled = analytics->find("enabled"); enabled != nullptr) {
            out.analytics_enabled = enabled->as_bool();
        }
        if (const json::Value* endpoint = analytics->find("endpoint"); endpoint != nullptr) {
            AnalyticsEndpoint parsed;
            parsed.url = endpoint->find("url")->as_string();
            if (const json::Value* format = endpoint->find("format");
                format != nullptr && format->as_string() == "posthog") {
                parsed.format = AnalyticsEndpoint::Format::PostHog;
            }
            if (const json::Value* key = endpoint->find("api_key"); key != nullptr) {
                parsed.api_key = key->as_string();
            }
            if (const json::Value* headers = endpoint->find("headers"); headers != nullptr) {
                for (const auto& [name, value] : headers->as_object()) {
                    parsed.headers.emplace_back(name, value.as_string());
                }
            }
            if (const json::Value* size = endpoint->find("batch_size"); size != nullptr) {
                parsed.batch_size = static_cast<int>(size->as_int());
            }
            if (const json::Value* seconds = endpoint->find("flush_seconds"); seconds != nullptr) {
                parsed.flush_seconds = seconds->as_double();
            }
            if (const json::Value* minutes = endpoint->find("session_minutes");
                minutes != nullptr) {
                parsed.session_minutes = minutes->as_double();
            }
            if (const json::Value* kept = endpoint->find("max_queued"); kept != nullptr) {
                parsed.max_queued = static_cast<int>(kept->as_int());
            }
            out.analytics_endpoint = std::move(parsed);
        }
        if (const json::Value* funnels = analytics->find("funnels"); funnels != nullptr) {
            for (const auto& [name, steps] : funnels->as_object()) {
                Funnel funnel{.name = name, .steps = {}};
                for (const json::Value& step : steps.as_array()) {
                    funnel.steps.push_back(step.as_string());
                }
                out.funnels.push_back(std::move(funnel));
            }
        }
    }
    if (const json::Value* consent = settings.find("consent"); consent != nullptr) {
        if (const json::Value* analytics = consent->find("analytics"); analytics != nullptr) {
            out.consent.analytics = parse_consent(analytics->as_string());
        }
        if (const json::Value* ads_consent = consent->find("personalized_ads");
            ads_consent != nullptr) {
            out.consent.personalized_ads = parse_consent(ads_consent->as_string());
        }
        if (const json::Value* usage = consent->find("tracking_usage"); usage != nullptr) {
            out.tracking_usage = usage->as_string();
        }
    }
    if (const json::Value* games = settings.find("games"); games != nullptr) {
        if (const json::Value* achievements = games->find("achievements");
            achievements != nullptr) {
            for (const auto& [name, item] : achievements->as_object()) {
                Achievement achievement;
                achievement.name = name;
                achievement.ids = parse_ids(item);
                if (const json::Value* steps = item.find("steps"); steps != nullptr) {
                    achievement.steps = static_cast<int>(steps->as_int());
                }
                out.achievements.push_back(std::move(achievement));
            }
        }
        if (const json::Value* leaderboards = games->find("leaderboards");
            leaderboards != nullptr) {
            for (const auto& [name, item] : leaderboards->as_object()) {
                Leaderboard leaderboard;
                leaderboard.name = name;
                leaderboard.ids = parse_ids(item);
                if (const json::Value* order = item.find("sort_order");
                    order != nullptr && order->as_string() == "low_to_high") {
                    leaderboard.sort_order = SortOrder::LowToHigh;
                }
                out.leaderboards.push_back(std::move(leaderboard));
            }
        }
        if (const json::Value* at_start = games->find("sign_in_at_start"); at_start != nullptr) {
            out.sign_in_at_start = at_start->as_bool();
        }
    }
    return out;
}

} // namespace slash::services
