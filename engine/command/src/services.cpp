#include "slash/command/services.hpp"

#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/services/services.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace slash::command {
namespace {

constexpr std::int64_t kDefaultEvents = 10;
constexpr std::int64_t kMaxEvents = 256;
constexpr std::int64_t kMaxTicks = 3600;

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value integer_property(std::string_view description, std::int64_t minimum,
                             std::int64_t maximum) {
    json::Value schema = property("integer", description);
    schema["minimum"] = minimum;
    schema["maximum"] = maximum;
    return schema;
}

json::Value enum_property(json::Array values, std::string_view description) {
    json::Value schema;
    schema["enum"] = std::move(values);
    schema["description"] = description;
    return schema;
}

json::Value name_property(std::string_view description) {
    json::Value schema = property("string", description);
    schema["minLength"] = 1;
    return schema;
}

json::Value params_schema(json::Object properties) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = std::move(properties);
    return schema;
}

Reply param_error(int error_code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(error_code, std::string(path) + ": " + message, std::move(data));
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

Reply param_error(const Error& error, std::string_view path) {
    return param_error(Reply::failure(error).code, path, error.message);
}

services::PurchaseState parse_state(std::string_view name) {
    if (name == "pending") {
        return services::PurchaseState::Pending;
    }
    if (name == "cancelled") {
        return services::PurchaseState::Cancelled;
    }
    if (name == "failed") {
        return services::PurchaseState::Failed;
    }
    return services::PurchaseState::Purchased;
}

services::Consent parse_consent(std::string_view name) {
    if (name == "granted") {
        return services::Consent::Granted;
    }
    if (name == "denied") {
        return services::Consent::Denied;
    }
    return services::Consent::Unknown;
}

json::Value test_json(const services::TestSettings& test) {
    json::Value out;
    out["fill"] = test.fill;
    out["load_ticks"] = test.load_ticks;
    out["show_ticks"] = test.show_ticks;
    out["watch_to_end"] = test.watch_to_end;
    out["purchase_ticks"] = test.purchase_ticks;
    out["outcome"] = services::state_name(test.outcome);
    out["error"] = test.error;
    json::Value prices = json::Object{};
    for (const auto& [id, price] : test.prices) {
        prices[id] = price;
    }
    out["prices"] = std::move(prices);
    return out;
}

Reply services_state(Session& session, const json::Value& params) {
    const json::Value* events = params.find("events");
    const auto count =
        static_cast<std::size_t>(events != nullptr ? events->as_int() : kDefaultEvents);
    return Reply::success(services::state_json(session.world(), count));
}

Reply test_services(world::World& world, const json::Value& params) {
    const services::TestSettings* current = services::test_settings(world);
    if (current == nullptr) {
        return Reply::failure(code::kInvalidArgument,
                              "the services run on the game's backends, not the test backend");
    }
    const services::TestSettings before = *current;
    services::TestSettings test = before;
    if (const json::Value* fill = params.find("fill"); fill != nullptr) {
        test.fill = fill->as_bool();
    }
    for (const auto& [key, field] :
         std::array<std::pair<const char*, int services::TestSettings::*>, 3>{
             {{"load_ticks", &services::TestSettings::load_ticks},
              {"show_ticks", &services::TestSettings::show_ticks},
              {"purchase_ticks", &services::TestSettings::purchase_ticks}}}) {
        if (const json::Value* ticks = params.find(key); ticks != nullptr) {
            test.*field = static_cast<int>(ticks->as_int());
        }
    }
    if (const json::Value* watch = params.find("watch_to_end"); watch != nullptr) {
        test.watch_to_end = watch->as_bool();
    }
    if (const json::Value* outcome = params.find("outcome"); outcome != nullptr) {
        test.outcome = parse_state(outcome->as_string());
    }
    if (const json::Value* error = params.find("error"); error != nullptr) {
        test.error = error->as_string();
    }
    if (const json::Value* prices = params.find("prices"); prices != nullptr) {
        const services::Settings* settings = services::settings(world);
        std::vector<std::string_view> ids;
        ids.reserve(settings->products.size());
        for (const services::Product& product : settings->products) {
            ids.emplace_back(product.id);
        }
        for (const auto& [id, price] : prices->as_object()) {
            if (services::product(world, id) == nullptr) {
                std::string message = "no product \"" + id + "\"";
                if (const std::string_view hint = closest_match(id, ids); !hint.empty()) {
                    message += "; did you mean \"" + std::string(hint) + "\"?";
                }
                return param_error(code::kNotFound, "/prices/" + escape_pointer(id), message);
            }
            test.prices.insert_or_assign(id, price.as_double());
        }
    }
    (void)services::set_test_settings(world, test);
    if (const json::Value* consent = params.find("consent"); consent != nullptr) {
        services::ConsentState state = services::consent(world);
        if (const json::Value* analytics = consent->find("analytics"); analytics != nullptr) {
            state.analytics = parse_consent(analytics->as_string());
        }
        if (const json::Value* ads = consent->find("personalized_ads"); ads != nullptr) {
            state.personalized_ads = parse_consent(ads->as_string());
        }
        services::set_consent(world, state);
    }
    if (const json::Value* outcome = params.find("complete_pending"); outcome != nullptr) {
        (void)services::complete_pending(world, parse_state(outcome->as_string()));
    }
    // As the game would call them, in this order.
    using Call = Result<void> (*)(world::World&, std::string_view);
    for (const auto& [key, call] :
         std::array<std::pair<const char*, Call>, 5>{{{"load", &services::load_ad},
                                                      {"show", &services::show_ad},
                                                      {"hide", &services::hide_ad},
                                                      {"buy", &services::buy},
                                                      {"finish", &services::finish}}}) {
        if (const json::Value* name = params.find(key); name != nullptr) {
            if (auto done = call(world, name->as_string()); !done) {
                return param_error(done.error(), "/" + std::string(key));
            }
        }
    }
    if (const json::Value* restore = params.find("restore");
        restore != nullptr && restore->as_bool()) {
        if (auto done = services::restore_purchases(world); !done) {
            return param_error(done.error(), "/restore");
        }
    }
    json::Value out;
    out["tick"] = world.tick();
    if (test != before) {
        out["test"] = test_json(test);
    }
    return Reply::success(std::move(out));
}

Reply services_test(Session& session, const json::Value& params) {
    // The session snapshots only commands that change entities, so this one puts the services
    // back itself when part of it fails.
    world::World& world = session.world();
    const services::Snapshot before = services::save(world);
    Reply reply = test_services(world, params);
    if (!reply.ok) {
        services::restore(world, before);
    }
    return reply;
}

} // namespace

void register_services_commands(Registry& registry) {
    using enum Capability;
    registry.add({
        .name = "services.state",
        .description =
            "Returns the ads, analytics and purchases services: backends, consent, each ad "
            "placement's status, products and prices, products owned, purchases unfinished "
            "(granted but not finished) and pending, and the latest analytics events with "
            "whether each was sent.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({{"events", integer_property("How many of the latest analytics "
                                                             "events. Default: 10.",
                                                             0, kMaxEvents)}}),
        .handler = services_state,
    });

    json::Value prices = property("object", "Prices in US dollars by product ID. Default: 0.99.");
    prices["additionalProperties"]["type"] = "number";
    prices["additionalProperties"]["minimum"] = 0;
    json::Value consent = property("object", "Sets the player's consent, as the game's "
                                             "set_consent would.");
    consent["additionalProperties"] = false;
    consent["properties"]["analytics"] =
        enum_property({"unknown", "granted", "denied"}, "Analytics events are sent if granted.");
    consent["properties"]["personalized_ads"] =
        enum_property({"unknown", "granted", "denied"}, "Ads are personalized if granted.");
    registry.add({
        .name = "services.test",
        .description =
            "Sets how the test backend loads ads and ends purchases, and loads, shows, hides, "
            "buys, finishes and restores as the game's code would. Events arrive at the next "
            "tick, which it returns, with all the settings if any changed.",
        .capabilities = {Run},
        .effect = Effect::Read,
        .params = params_schema({
            {"fill", property("boolean", "Ads load; false fails them with \"no fill\".")},
            {"load_ticks", integer_property("Ticks an ad takes to load.", 1, kMaxTicks)},
            {"show_ticks",
             integer_property("Ticks an interstitial or rewarded ad shows.", 1, kMaxTicks)},
            {"watch_to_end", property("boolean", "The player watches rewarded ads to the end.")},
            {"purchase_ticks", integer_property("Ticks a purchase takes.", 1, kMaxTicks)},
            {"outcome",
             enum_property({"purchased", "pending", "cancelled", "failed"}, "How purchases end.")},
            {"error", property("string", "A failed purchase's error.")},
            {"prices", std::move(prices)},
            {"consent", std::move(consent)},
            {"complete_pending",
             enum_property({"purchased", "cancelled", "failed"}, "Ends each pending purchase so.")},
            {"load", name_property("An ad placement to load.")},
            {"show", name_property("An ad placement to show.")},
            {"hide", name_property("A banner placement to hide.")},
            {"buy", name_property("A product to buy.")},
            {"finish", name_property("A purchase's transaction to finish, once granted.")},
            {"restore", property("boolean", "Restores purchases.")},
        }),
        .handler = services_test,
    });
}

} // namespace slash::command
