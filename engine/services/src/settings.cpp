#include "slash/core/text.hpp"
#include "slash/services/services.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace slash::services {
namespace {

constexpr std::array<std::string_view, 6> kPlatforms{"android", "ios",   "macos",
                                                     "windows", "linux", "web"};
constexpr std::array<std::string_view, 3> kFormats{"banner", "interstitial", "rewarded"};
constexpr std::array<std::string_view, 3> kTypes{"consumable", "non_consumable", "subscription"};
constexpr std::array<std::string_view, 3> kConsents{"unknown", "granted", "denied"};

constexpr std::size_t kMaxPlacementName = 64;
constexpr std::size_t kMaxProductId = 100;

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

// Placement names as entity IDs: letters, digits, '_' and '-'.
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

std::string_view consent_name(Consent consent) {
    return kConsents.at(static_cast<std::size_t>(consent));
}

json::Value settings_schema() {
    json::Value reward = object_schema("What a watched rewarded ad gives when the ad network "
                                       "doesn't say, e.g. {\"type\": \"coins\", \"amount\": 50}.");
    reward["required"] = json::Array{"type", "amount"};
    reward["properties"]["type"]["type"] = "string";
    reward["properties"]["type"]["minLength"] = 1;
    reward["properties"]["amount"]["type"] = "number";
    reward["properties"]["amount"]["minimum"] = 0;

    json::Value ids = object_schema("The ad network's ad unit ID on each platform: \"android\", "
                                    "\"ios\", \"macos\", \"windows\", \"linux\" or \"web\".");
    json::Value id;
    id["type"] = "string";
    id["minLength"] = 1;
    ids["additionalProperties"] = std::move(id);

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

    json::Value consent = object_schema("The player's consent until the game sets it.");
    json::Value analytics_consent =
        enum_property(kConsents, "Events are sent only when granted. Default: unknown.");
    json::Value ads_consent =
        enum_property(kConsents, "Ads are personalized only when granted. Default: unknown.");
    consent["properties"]["analytics"] = std::move(analytics_consent);
    consent["properties"]["personalized_ads"] = std::move(ads_consent);

    json::Value schema =
        object_schema("Ads, analytics and in-app purchases, run by SDK packages' backends.");
    schema["properties"]["ads"] = std::move(ads);
    schema["properties"]["purchases"] = std::move(purchases);
    schema["properties"]["analytics"] = std::move(analytics);
    schema["properties"]["consent"] = std::move(consent);
    return schema;
}

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
                for (const auto& [platform, id] : ids->as_object()) {
                    if (std::ranges::find(kPlatforms, platform) != kPlatforms.end()) {
                        continue;
                    }
                    std::string message = "no platform \"" + platform + "\"";
                    if (const std::string_view hint = closest_match(platform, kPlatforms);
                        !hint.empty()) {
                        message += "; did you mean \"" + std::string(hint) + "\"?";
                    }
                    errors.push_back({path + "/ids/" + escape_pointer(platform), message});
                }
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
            if (const json::Value* ids = item.find("ids"); ids != nullptr) {
                for (const auto& [platform, id] : ids->as_object()) {
                    placement.ids.emplace(platform, id.as_string());
                }
            }
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
    }
    if (const json::Value* consent = settings.find("consent"); consent != nullptr) {
        if (const json::Value* analytics = consent->find("analytics"); analytics != nullptr) {
            out.consent.analytics = parse_consent(analytics->as_string());
        }
        if (const json::Value* ads_consent = consent->find("personalized_ads");
            ads_consent != nullptr) {
            out.consent.personalized_ads = parse_consent(ads_consent->as_string());
        }
    }
    return out;
}

} // namespace slash::services
