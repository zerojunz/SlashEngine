#include "slash/command/services.hpp"

#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/services/services.hpp"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace slash::command {
namespace {

constexpr std::int64_t kDefaultEvents = 10;
constexpr std::int64_t kMaxEvents = 256;
constexpr std::int64_t kMaxTicks = 3600;
constexpr std::int64_t kMaxScores = 25;
constexpr std::int64_t kMaxSteps = 10'000;
constexpr std::int64_t kMaxRivals = 100;

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

json::Value object_property(json::Object properties, json::Array required,
                            std::string_view description) {
    json::Value schema = params_schema(std::move(properties));
    schema["required"] = std::move(required);
    schema["description"] = description;
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

services::TimeSpan parse_span(std::string_view name) {
    if (name == "week") {
        return services::TimeSpan::Week;
    }
    if (name == "today") {
        return services::TimeSpan::Today;
    }
    return services::TimeSpan::AllTime;
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
    out["accept_sign_in"] = test.accept_sign_in;
    out["player"] = test.player;
    out["games_ticks"] = test.games_ticks;
    json::Value scores = json::Object{};
    for (const auto& [leaderboard, entries] : test.scores) {
        json::Array list;
        for (const services::Score& entry : entries) {
            json::Value item;
            item["name"] = entry.name;
            item["score"] = entry.value;
            list.push_back(std::move(item));
        }
        scores[leaderboard] = std::move(list);
    }
    out["scores"] = std::move(scores);
    return out;
}

// A name in the game's list of achievements or leaderboards, or the error for a misspelled one
// at path.
template <typename Item>
std::optional<Reply> check_name(const std::vector<Item>& items, const std::string& name,
                                std::string_view what, const std::string& path) {
    std::vector<std::string_view> names;
    names.reserve(items.size());
    for (const Item& item : items) {
        if (item.name == name) {
            return std::nullopt;
        }
        names.emplace_back(item.name);
    }
    std::string message = "no " + std::string(what) + " \"" + name + "\"";
    if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
        message += "; did you mean \"" + std::string(hint) + "\"?";
    }
    return param_error(code::kNotFound, path, message);
}

// The games calls services.test makes as the game's code would, in this order.
std::optional<Reply> call_games(world::World& world, const json::Value& params) {
    if (const json::Value* sign = params.find("sign_in"); sign != nullptr && sign->as_bool()) {
        if (auto done = services::sign_in(world); !done) {
            return param_error(done.error(), "/sign_in");
        }
    }
    if (const json::Value* name = params.find("unlock"); name != nullptr) {
        if (auto done = services::unlock_achievement(world, name->as_string()); !done) {
            return param_error(done.error(), "/unlock");
        }
    }
    if (const json::Value* increment = params.find("increment"); increment != nullptr) {
        const json::Value* steps = increment->find("steps");
        if (auto done = services::increment_achievement(
                world, increment->find("achievement")->as_string(),
                steps != nullptr ? static_cast<int>(steps->as_int()) : 1);
            !done) {
            return param_error(done.error(), "/increment");
        }
    }
    if (const json::Value* submit = params.find("submit"); submit != nullptr) {
        if (auto done = services::submit_score(world, submit->find("leaderboard")->as_string(),
                                               submit->find("score")->as_int());
            !done) {
            return param_error(done.error(), "/submit");
        }
    }
    if (const json::Value* load = params.find("load_scores"); load != nullptr) {
        services::ScoreQuery query;
        if (const json::Value* time = load->find("time"); time != nullptr) {
            query.time = parse_span(time->as_string());
        }
        if (const json::Value* collection = load->find("collection");
            collection != nullptr && collection->as_string() == "friends") {
            query.collection = services::Collection::Friends;
        }
        if (const json::Value* count = load->find("count"); count != nullptr) {
            query.count = static_cast<int>(count->as_int());
        }
        if (auto done = services::load_scores(world, load->find("leaderboard")->as_string(), query);
            !done) {
            return param_error(done.error(), "/load_scores");
        }
    }
    if (const json::Value* show = params.find("show_achievements");
        show != nullptr && show->as_bool()) {
        if (auto done = services::show_achievements(world); !done) {
            return param_error(done.error(), "/show_achievements");
        }
    }
    if (const json::Value* name = params.find("show_leaderboards"); name != nullptr) {
        if (auto done = services::show_leaderboards(world, name->as_string()); !done) {
            return param_error(done.error(), "/show_leaderboards");
        }
    }
    return std::nullopt;
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
         std::array<std::pair<const char*, int services::TestSettings::*>, 4>{
             {{"load_ticks", &services::TestSettings::load_ticks},
              {"show_ticks", &services::TestSettings::show_ticks},
              {"purchase_ticks", &services::TestSettings::purchase_ticks},
              {"games_ticks", &services::TestSettings::games_ticks}}}) {
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
    if (const json::Value* accept = params.find("accept_sign_in"); accept != nullptr) {
        test.accept_sign_in = accept->as_bool();
    }
    if (const json::Value* player = params.find("player"); player != nullptr) {
        test.player = player->as_string();
    }
    if (const json::Value* scores = params.find("scores"); scores != nullptr) {
        const services::Settings* settings = services::settings(world);
        for (const auto& [leaderboard, entries] : scores->as_object()) {
            if (auto bad = check_name(settings->leaderboards, leaderboard, "leaderboard",
                                      "/scores/" + escape_pointer(leaderboard))) {
                return std::move(*bad);
            }
            std::vector<services::Score> list;
            for (const json::Value& entry : entries.as_array()) {
                list.push_back({.rank = 0,
                                .player = {},
                                .name = entry.find("name")->as_string(),
                                .value = entry.find("score")->as_int()});
            }
            test.scores.insert_or_assign(leaderboard, std::move(list));
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
    if (auto failed = call_games(world, params)) {
        return std::move(*failed);
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
            "Returns the services: backends, consent, each ad placement's status, products and "
            "prices, products owned, purchases unfinished (granted but not finished) and "
            "pending, the latest analytics events with whether each was sent, and sign-in, "
            "achievement progress and best scores.",
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
    json::Value score_entry = object_property({{"name", name_property("The player's name.")},
                                               {"score", property("integer", "Their best score.")}},
                                              {"name", "score"}, "Another player's score.");
    json::Value scores = property("object", "Other players' scores by leaderboard.");
    scores["additionalProperties"]["type"] = "array";
    scores["additionalProperties"]["maxItems"] = kMaxRivals;
    scores["additionalProperties"]["items"] = std::move(score_entry);
    json::Value increment =
        object_property({{"achievement", name_property("An incremental achievement.")},
                         {"steps", integer_property("Steps to add. Default: 1.", 1, kMaxSteps)}},
                        {"achievement"}, "Adds steps to an achievement.");
    json::Value submit = object_property({{"leaderboard", name_property("A leaderboard.")},
                                          {"score", property("integer", "The score.")}},
                                         {"leaderboard", "score"}, "Submits a score.");
    json::Value load_scores = object_property(
        {{"leaderboard", name_property("A leaderboard.")},
         {"time", enum_property({"all_time", "week", "today"}, "Default: all_time.")},
         {"collection", enum_property({"global", "friends"}, "Default: global.")},
         {"count", integer_property("Default: 10.", 1, kMaxScores)}},
        {"leaderboard"}, "Loads a leaderboard's best scores.");
    json::Value show_leaderboards = property("string", "A leaderboard to show, or \"\" for all.");
    registry.add({
        .name = "services.test",
        .description =
            "Sets how the test backend loads ads, ends purchases, signs in and ranks scores, and "
            "calls the services as the game's code would (load, show, buy, finish, restore, "
            "sign_in, unlock, submit and the rest). Events arrive at the next tick, which it "
            "returns, with all the settings if any changed.",
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
            {"accept_sign_in", property("boolean", "The player signs in; false fails sign-in.")},
            {"player", name_property("The test player's name.")},
            {"games_ticks",
             integer_property("Ticks signing in, a score and loading scores take.", 1, kMaxTicks)},
            {"scores", std::move(scores)},
            {"sign_in", property("boolean", "Signs the player in.")},
            {"unlock", name_property("An achievement to unlock.")},
            {"increment", std::move(increment)},
            {"submit", std::move(submit)},
            {"load_scores", std::move(load_scores)},
            {"show_achievements", property("boolean", "Shows the achievements screen.")},
            {"show_leaderboards", std::move(show_leaderboards)},
        }),
        .handler = services_test,
    });
}

} // namespace slash::command
