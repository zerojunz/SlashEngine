#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/platform.hpp"
#include "slash/runtime/project.hpp"
#include "slash/services/services.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace world = slash::world;
namespace services = slash::services;
namespace command = slash::command;

namespace {

constexpr double kTick = 1.0 / 60.0;

std::string compact(const json::Value& value) {
    return json::write(value, json::Style::Compact);
}

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

constexpr std::string_view kSettings = R"({
    "ads": {"placements": {
        "bonus": {"format": "rewarded", "reward": {"type": "coins", "amount": 50},
                  "ids": {"android": "ca-app-pub-1/1", "ios": "ca-app-pub-1/2"}},
        "between_levels": {"format": "interstitial", "auto_load": false},
        "footer": {"format": "banner"}}},
    "purchases": {"products": {
        "coins_100": {"type": "consumable"},
        "remove_ads": {"type": "non_consumable"},
        "vip": {"type": "subscription"}}},
    "consent": {"analytics": "granted"}
})";

services::Settings test_settings() {
    const json::Value doc = parse(kSettings);
    REQUIRE(services::validate_settings(doc, "/services").empty());
    return services::parse_settings(doc);
}

std::vector<std::string> kinds(const world::World& world) {
    std::vector<std::string> out;
    for (const services::ServiceEvent& event : services::events(world)) {
        out.push_back(event.kind + (event.id.empty() ? "" : ":" + event.id));
    }
    return out;
}

// Steps one tick and returns its events.
std::vector<std::string> step(world::World& world) {
    world.step(kTick);
    return kinds(world);
}

struct Calls {
    std::mutex mutex;
    std::vector<std::string> list;

    void add(std::string call) {
        const std::scoped_lock lock(mutex);
        list.push_back(std::move(call));
    }
};

class FakeAds : public services::AdsBackend {
public:
    explicit FakeAds(std::shared_ptr<Calls> calls) : calls_(std::move(calls)) {}
    [[nodiscard]] std::string_view name() const override { return "fake_ads"; }
    void start(const std::shared_ptr<services::Reports>& reports) override {
        reports_ = reports;
        calls_->add("ads.start");
    }
    void set_consent(const services::ConsentState& consent) override {
        calls_->add("ads.consent " + std::string(services::consent_name(consent.personalized_ads)));
    }
    void load(const services::AdUnit& unit) override {
        calls_->add("ads.load " + unit.placement + " " + unit.id);
        reports_->ad_loaded(unit.placement);
    }
    void show(const services::AdUnit& unit) override {
        calls_->add("ads.show " + unit.placement);
        // As an SDK calls back on its own thread.
        std::thread([reports = reports_, placement = unit.placement] {
            reports->ad_shown(placement);
            reports->ad_rewarded(placement, {.type = "gems", .amount = 2});
            reports->ad_closed(placement);
        }).join();
    }
    void hide(const services::AdUnit& unit) override { calls_->add("ads.hide " + unit.placement); }

private:
    std::shared_ptr<Calls> calls_;
    std::shared_ptr<services::Reports> reports_;
};

class FakeAnalytics : public services::AnalyticsBackend {
public:
    explicit FakeAnalytics(std::shared_ptr<Calls> calls) : calls_(std::move(calls)) {}
    [[nodiscard]] std::string_view name() const override { return "fake_analytics"; }
    void log_event(std::string_view name, const json::Value& params) override {
        calls_->add("analytics.event " + std::string(name) + " " + compact(params));
    }
    void set_user_property(std::string_view name, std::string_view value) override {
        calls_->add("analytics.property " + std::string(name) + "=" + std::string(value));
    }
    void set_user_id(std::string_view id) override {
        calls_->add("analytics.user " + std::string(id));
    }
    void flush() override { calls_->add("analytics.flush"); }

private:
    std::shared_ptr<Calls> calls_;
};

class FakeStore : public services::PurchasesBackend {
public:
    explicit FakeStore(std::shared_ptr<Calls> calls) : calls_(std::move(calls)) {}
    [[nodiscard]] std::string_view name() const override { return "fake_store"; }
    void start(const std::shared_ptr<services::Reports>& reports) override { reports_ = reports; }
    void poll() override { ++polls; }
    void fetch_products(std::span<const services::Product> catalog) override {
        calls_->add("store.fetch " + std::to_string(catalog.size()));
        // It sells all but the subscription, and remembers an unfinished purchase.
        std::vector<services::Product> products;
        for (const services::Product& product : catalog) {
            if (product.type != services::ProductType::Subscription) {
                services::Product details = product;
                details.price = "1,99 €";
                details.price_micros = 1'990'000;
                details.currency = "EUR";
                products.push_back(std::move(details));
            }
        }
        reports_->products(std::move(products));
        reports_->purchase({.product = "coins_100",
                            .transaction = "GPA.1",
                            .state = services::PurchaseState::Purchased,
                            .receipt = "token-1",
                            .error = {}});
    }
    void purchase(const services::Product& product) override {
        calls_->add("store.purchase " + product.id);
        reports_->purchase({.product = product.id,
                            .transaction = "GPA.2",
                            .state = services::PurchaseState::Purchased,
                            .receipt = "token-2",
                            .error = {}});
    }
    void finish(const services::Purchase& purchase, const services::Product& product) override {
        calls_->add("store.finish " + purchase.transaction + " " +
                    std::string(services::type_name(product.type)));
    }
    void restore() override {
        calls_->add("store.restore");
        reports_->restore_finished("network down");
    }

    int polls = 0;

private:
    std::shared_ptr<Calls> calls_;
    std::shared_ptr<services::Reports> reports_;
};

} // namespace

TEST_CASE("services settings validate and parse project.json's services") {
    const services::Settings settings = test_settings();
    REQUIRE(settings.placements.size() == 3);
    CHECK(settings.placements[0].name == "between_levels");
    CHECK(settings.placements[0].format == services::AdFormat::Interstitial);
    CHECK_FALSE(settings.placements[0].auto_load);
    CHECK(settings.placements[1].name == "bonus");
    CHECK(settings.placements[1].reward == services::Reward{.type = "coins", .amount = 50});
    CHECK(settings.placements[1].ids.at("ios") == "ca-app-pub-1/2");
    CHECK(settings.placements[2].format == services::AdFormat::Banner);
    REQUIRE(settings.products.size() == 3);
    CHECK(settings.products[1].id == "remove_ads");
    CHECK(settings.products[1].type == services::ProductType::NonConsumable);
    CHECK(settings.products[2].type == services::ProductType::Subscription);
    CHECK(settings.consent.analytics == services::Consent::Granted);
    CHECK(settings.consent.personalized_ads == services::Consent::Unknown);
    CHECK(settings.analytics_enabled);

    const auto errors = services::validate_settings(parse(R"({
        "ads": {"placements": {
            "bad name": {"format": "banner"},
            "pop": {"format": "interstitial", "ids": {"andriod": "x"}, "reward": {"type": "a", "amount": 1}}}},
        "purchases": {"products": {"-coins": {"type": "consumable"}}}
    })"),
                                                    "/services");
    REQUIRE(errors.size() == 4);
    CHECK(errors[0].path == "/services/ads/placements/bad name");
    CHECK(errors[1].path == "/services/ads/placements/pop/ids/andriod");
    CHECK(errors[1].message == "no platform \"andriod\"; did you mean \"android\"?");
    CHECK(errors[2].path == "/services/ads/placements/pop/reward");
    CHECK(errors[3].path == "/services/purchases/products/-coins");
    const auto schema_errors = services::validate_settings(
        parse(R"({"ads": {"placements": {"x": {"format": "video"}}}})"), "/services");
    REQUIRE(schema_errors.size() == 1);
    CHECK(schema_errors[0].path == "/services/ads/placements/x/format");

    // project.json takes them.
    auto project = slash::runtime::parse_project(parse(
        R"({"format": 1, "name": "Shop", "main_scene": "main.scene.json",
            "services": {"purchases": {"products": {"gold": {"type": "consumable"}}}}})"));
    REQUIRE(project.ok());
    REQUIRE(project.value().services.products.size() == 1);
    CHECK(project.value().services.products[0].id == "gold");
    const auto project_errors = slash::runtime::validate_project(
        parse(R"({"format": 1, "name": "Shop", "main_scene": "main.scene.json",
                  "services": {"ads": {"placements": {"a": {"format": "banner",
                                                            "ids": {"iso": "x"}}}}}})"));
    REQUIRE(project_errors.size() == 1);
    CHECK(project_errors[0].path == "/services/ads/placements/a/ids/iso");
}

TEST_CASE("with no backends, ads fail, the store is unavailable and events are only kept") {
    world::ComponentRegistry components;
    world::World world(components);
    services::install(world, test_settings());
    CHECK(step(world) ==
          std::vector<std::string>{"products_failed", "ad_failed:bonus", "ad_failed:footer"});
    CHECK(services::events(world)[0].error == "no store");
    CHECK(services::events(world)[1].error == "no ads backend");
    CHECK(services::ad_status(world, "bonus") == services::AdStatus::Failed);
    CHECK(services::ad_status(world, "between_levels") == services::AdStatus::Unloaded);

    auto bought = services::buy(world, "coins_100");
    REQUIRE_FALSE(bought.ok());
    CHECK(bought.error().message == "the store is unavailable: no store");
    auto shown = services::show_ad(world, "bonus");
    REQUIRE_FALSE(shown.ok());
    CHECK(shown.error().message == "the ad failed to load (no ads backend); load_ad it again");
    REQUIRE(services::restore_purchases(world).ok());
    CHECK(step(world) == std::vector<std::string>{"restore_finished"});
    CHECK(services::events(world)[0].error == "no store");

    REQUIRE(services::log_event(world, "level_start", parse(R"({"level": 3})")).ok());
    const json::Value state = services::state_json(world, 10);
    CHECK(state.find("backends")->find("ads")->as_string() == "none");
    CHECK(state.find("backends")->find("purchases")->as_string() == "none");
    CHECK(state.find("products_status")->as_string() == "failed");
    CHECK(state.find("products_error")->as_string() == "no store");
    const json::Value& logged = state.find("analytics")->find("events")->as_array()[0];
    CHECK(logged.find("name")->as_string() == "level_start");
    CHECK_FALSE(logged.find("sent")->as_bool());
}

TEST_CASE("the test backend loads, shows and closes ads, and rewards watched ones") {
    world::ComponentRegistry components;
    world::World world(components);
    services::install(world, test_settings());
    services::use_test_backend(world);
    CHECK(step(world) ==
          std::vector<std::string>{"products_fetched", "ad_loaded:bonus", "ad_loaded:footer"});
    CHECK(services::is_ad_loaded(world, "bonus"));

    auto unknown = services::show_ad(world, "bonsu");
    REQUIRE_FALSE(unknown.ok());
    CHECK(unknown.error().code == slash::ErrorCode::NotFound);
    CHECK(unknown.error().message == "no ad placement \"bonsu\"; did you mean \"bonus\"?");
    auto unloaded = services::show_ad(world, "between_levels");
    REQUIRE_FALSE(unloaded.ok());
    CHECK(unloaded.error().message == "no ad is loaded; load_ad it and show it after ad_loaded");

    // A rewarded ad: shown at the next tick, closed after show_ticks with its reward, then
    // loaded again since it auto-loads.
    REQUIRE(services::show_ad(world, "bonus").ok());
    CHECK(services::ad_status(world, "bonus") == services::AdStatus::Showing);
    CHECK_FALSE(services::show_ad(world, "bonus").ok());
    CHECK(step(world) == std::vector<std::string>{"ad_shown:bonus"});
    CHECK(step(world) == std::vector<std::string>{"ad_rewarded:bonus", "ad_closed:bonus"});
    CHECK(services::events(world)[0].reward == services::Reward{.type = "coins", .amount = 50});
    CHECK(services::ad_status(world, "bonus") == services::AdStatus::Loading);
    CHECK(step(world) == std::vector<std::string>{"ad_loaded:bonus"});

    // An interstitial the game loads itself, closing with no reward and not loading again.
    REQUIRE(services::load_ad(world, "between_levels").ok());
    auto loading = services::show_ad(world, "between_levels");
    REQUIRE_FALSE(loading.ok());
    CHECK(loading.error().message == "the ad is still loading; show it after ad_loaded");
    CHECK(step(world) == std::vector<std::string>{"ad_loaded:between_levels"});
    REQUIRE(services::show_ad(world, "between_levels").ok());
    CHECK(step(world) == std::vector<std::string>{"ad_shown:between_levels"});
    CHECK(step(world) == std::vector<std::string>{"ad_closed:between_levels"});
    CHECK(step(world).empty());
    CHECK(services::ad_status(world, "between_levels") == services::AdStatus::Unloaded);

    // A banner shows until hidden.
    auto hide_interstitial = services::hide_ad(world, "between_levels");
    REQUIRE_FALSE(hide_interstitial.ok());
    CHECK(hide_interstitial.error().message ==
          "only banners hide; interstitial ads close on their own");
    REQUIRE(services::show_ad(world, "footer").ok());
    CHECK(step(world) == std::vector<std::string>{"ad_shown:footer"});
    CHECK(step(world).empty());
    REQUIRE(services::hide_ad(world, "footer").ok());
    CHECK(services::ad_status(world, "footer") == services::AdStatus::Loaded);
    CHECK_FALSE(services::hide_ad(world, "footer").ok());

    // Skipped ads give nothing; unfilled ones fail.
    services::TestSettings test = *services::test_settings(world);
    test.watch_to_end = false;
    test.show_ticks = 3;
    REQUIRE(services::set_test_settings(world, test).ok());
    REQUIRE(services::show_ad(world, "bonus").ok());
    CHECK(step(world) == std::vector<std::string>{"ad_shown:bonus"});
    CHECK(step(world).empty());
    CHECK(step(world).empty());
    CHECK(step(world) == std::vector<std::string>{"ad_closed:bonus"});
    CHECK(step(world) == std::vector<std::string>{"ad_loaded:bonus"});
    test.fill = false;
    REQUIRE(services::set_test_settings(world, test).ok());
    REQUIRE(services::load_ad(world, "between_levels").ok());
    CHECK(step(world) == std::vector<std::string>{"ad_failed:between_levels"});
    CHECK(services::events(world)[0].error == "no fill");
    CHECK(services::state_json(world, 0)
              .find("ads")
              ->find("between_levels")
              ->find("error")
              ->as_string() == "no fill");

    // The world event carries the same.
    std::vector<world::Event> seen;
    world.set_event_listener([&](const world::Event& event) { seen.push_back(event); });
    test.fill = true;
    test.watch_to_end = true;
    test.show_ticks = 1;
    REQUIRE(services::set_test_settings(world, test).ok());
    REQUIRE(services::show_ad(world, "bonus").ok());
    (void)step(world);
    (void)step(world);
    REQUIRE(seen.size() == 3);
    CHECK(seen[1].kind == "ad_rewarded");
    CHECK(seen[1].id == "bonus");
    CHECK(compact(seen[1].data) == R"({"amount":50.0,"type":"coins"})");
}

TEST_CASE("the test backend sells products, and finishing lets consumables be bought again") {
    world::ComponentRegistry components;
    world::World world(components);
    services::install(world, test_settings());
    services::TestSettings test;
    test.prices["remove_ads"] = 2.49;
    services::use_test_backend(world, test);

    auto early = services::buy(world, "coins_100");
    REQUIRE_FALSE(early.ok());
    CHECK(early.error().message ==
          "the store's products are not fetched yet; buy after products_fetched");
    (void)step(world);
    const services::ServiceEvent& fetched = services::events(world)[0];
    CHECK(fetched.available == std::vector<std::string>{"coins_100", "remove_ads", "vip"});
    const services::Product* coins = services::product(world, "coins_100");
    REQUIRE(coins != nullptr);
    CHECK(coins->price == "$0.99");
    CHECK(coins->price_micros == 990'000);
    CHECK(coins->currency == "USD");
    CHECK(services::product(world, "remove_ads")->price == "$2.49");

    auto misspelled = services::buy(world, "coins_10");
    REQUIRE_FALSE(misspelled.ok());
    CHECK(misspelled.error().message == "no product \"coins_10\"; did you mean \"coins_100\"?");

    // A consumable: completed, then refused until finished.
    REQUIRE(services::buy(world, "coins_100").ok());
    auto twice = services::buy(world, "coins_100");
    REQUIRE_FALSE(twice.ok());
    CHECK(twice.error().message == "a purchase of \"coins_100\" is in progress");
    CHECK(step(world) == std::vector<std::string>{"purchase_completed:coins_100"});
    const services::Purchase& bought = services::events(world)[0].purchase;
    CHECK(bought.transaction == "test-1");
    CHECK(bought.receipt == "test-receipt-1");
    CHECK(bought.state == services::PurchaseState::Purchased);
    CHECK_FALSE(services::owns(world, "coins_100"));
    REQUIRE(services::unfinished(world).size() == 1);
    auto unfinished = services::buy(world, "coins_100");
    REQUIRE_FALSE(unfinished.ok());
    CHECK(unfinished.error().message ==
          "\"coins_100\" has an unfinished purchase (test-1); grant it and finish it first");
    auto wrong = services::finish(world, "test-9");
    REQUIRE_FALSE(wrong.ok());
    CHECK(wrong.error().code == slash::ErrorCode::NotFound);
    REQUIRE(services::finish(world, "test-1").ok());
    CHECK(services::unfinished(world).empty());
    REQUIRE(services::buy(world, "coins_100").ok());
    (void)step(world);
    REQUIRE(services::finish(world, "test-2").ok());

    // A non-consumable is owned once bought, and restoring delivers it again.
    REQUIRE(services::buy(world, "remove_ads").ok());
    (void)step(world);
    CHECK(services::owns(world, "remove_ads"));
    REQUIRE(services::finish(world, "test-3").ok());
    auto owned = services::buy(world, "remove_ads");
    REQUIRE_FALSE(owned.ok());
    CHECK(owned.error().message == "the player owns \"remove_ads\" already");
    REQUIRE(services::restore_purchases(world).ok());
    CHECK_FALSE(services::restore_purchases(world).ok());
    CHECK(step(world) ==
          std::vector<std::string>{"purchase_completed:remove_ads", "restore_finished"});
    CHECK(services::events(world)[0].purchase.state == services::PurchaseState::Restored);
    CHECK(services::events(world)[1].error.empty());

    // Pending purchases wait for a parent; cancelled and failed ones end with nothing.
    test.outcome = services::PurchaseState::Pending;
    REQUIRE(services::set_test_settings(world, test).ok());
    REQUIRE(services::buy(world, "vip").ok());
    CHECK(step(world) == std::vector<std::string>{"purchase_pending:vip"});
    CHECK_FALSE(services::owns(world, "vip"));
    CHECK_FALSE(services::buy(world, "vip").ok());
    REQUIRE(services::complete_pending(world, services::PurchaseState::Purchased).ok());
    CHECK(step(world) == std::vector<std::string>{"purchase_completed:vip"});
    CHECK(services::owns(world, "vip"));

    test.outcome = services::PurchaseState::Cancelled;
    REQUIRE(services::set_test_settings(world, test).ok());
    REQUIRE(services::buy(world, "coins_100").ok());
    CHECK(step(world) == std::vector<std::string>{"purchase_cancelled:coins_100"});
    test.outcome = services::PurchaseState::Failed;
    test.purchase_ticks = 2;
    REQUIRE(services::set_test_settings(world, test).ok());
    REQUIRE(services::buy(world, "coins_100").ok());
    CHECK(step(world).empty());
    CHECK(step(world) == std::vector<std::string>{"purchase_failed:coins_100"});
    CHECK(services::events(world)[0].error == "the payment was declined");

    const json::Value state = services::state_json(world, 0);
    CHECK(compact(*state.find("owned")) == R"(["remove_ads","vip"])");
    CHECK(state.find("unfinished")->as_array().size() == 2);
    CHECK(state.find("products")->find("remove_ads")->find("price")->as_string() == "$2.49");

    // Snapshots put it all back; a reset is a fresh test store.
    const services::Snapshot before = services::save(world);
    REQUIRE(services::finish(world, services::unfinished(world).front().transaction).ok());
    services::restore(world, before);
    CHECK(services::unfinished(world).size() == 2);
    world.reset_state();
    CHECK_FALSE(services::owns(world, "remove_ads"));
    CHECK(services::unfinished(world).empty());
    CHECK(services::testing(world));
}

TEST_CASE("analytics events are checked, kept, and sent only with consent") {
    world::ComponentRegistry components;
    world::World world(components);
    services::Settings settings = test_settings();
    settings.consent = {};
    services::install(world, settings);
    services::use_test_backend(world);

    CHECK(services::log_event(world, "2fast").error().message ==
          "the event name \"2fast\" must start with a letter and hold only letters, digits and "
          "'_', at most 40 of them");
    CHECK_FALSE(services::log_event(world, std::string(41, 'a')).ok());
    CHECK(services::log_event(world, "shop", parse("[1]")).error().message ==
          "params must be an object or null");
    CHECK(services::log_event(world, "shop", parse(R"({"item": {"id": 1}})")).error().message ==
          "param \"item\" must be a number, a boolean or a string of at most 100 bytes");
    CHECK_FALSE(services::log_event(world, "shop", parse(R"({"bad-key": 1})")).ok());

    REQUIRE(services::log_event(world, "level_start", parse(R"({"level": 1})")).ok());
    services::set_consent(world, {.analytics = services::Consent::Granted,
                                  .personalized_ads = services::Consent::Denied});
    REQUIRE(services::log_event(world, "level_end", parse(R"({"won": true})")).ok());
    REQUIRE(services::log_event(world, "tutorial_done").ok());
    const json::Value state = services::state_json(world, 2);
    CHECK(state.find("consent")->find("analytics")->as_string() == "granted");
    CHECK(state.find("consent")->find("personalized_ads")->as_string() == "denied");
    const json::Value& analytics = *state.find("analytics");
    CHECK(analytics.find("logged")->as_int() == 3);
    CHECK(compact(*analytics.find("events")) ==
          R"([{"name":"level_end","params":{"won":true},"sent":true,"tick":0},)"
          R"({"name":"tutorial_done","sent":true,"tick":0}])");
    CHECK_FALSE(services::state_json(world, 3)
                    .find("analytics")
                    ->find("events")
                    ->as_array()[0]
                    .find("sent")
                    ->as_bool());

    settings.analytics_enabled = false;
    settings.consent.analytics = services::Consent::Granted;
    services::install(world, settings);
    REQUIRE(services::log_event(world, "menu").ok());
    CHECK_FALSE(services::state_json(world, 1)
                    .find("analytics")
                    ->find("events")
                    ->as_array()[0]
                    .find("sent")
                    ->as_bool());
}

TEST_CASE("backends get requests and report from any thread, at the next tick") {
    world::ComponentRegistry components;
    world::World world(components);
    services::Settings settings = test_settings();
    settings.consent = {};
    services::install(world, settings);
    auto calls = std::make_shared<Calls>();
    services::Backends backends;
    backends.ads = std::make_unique<FakeAds>(calls);
    backends.analytics.push_back(std::make_unique<FakeAnalytics>(calls));
    auto store = std::make_unique<FakeStore>(calls);
    FakeStore* store_view = store.get();
    backends.purchases = std::move(store);
    services::set_user_id(world, "player-7");
    services::set_backends(world, std::move(backends));
    const std::string os(slash::platform::os_name());
    const std::string bonus_id = os == "android" ? "ca-app-pub-1/1"
                                 : os == "ios"   ? "ca-app-pub-1/2"
                                                 : "";
    CHECK(calls->list == std::vector<std::string>{"ads.start", "ads.consent unknown",
                                                  "store.fetch 3", "ads.load bonus " + bonus_id,
                                                  "ads.load footer "});
    CHECK(step(world) == std::vector<std::string>{"products_fetched",
                                                  "purchase_completed:coins_100", "ad_loaded:bonus",
                                                  "ad_loaded:footer"});
    CHECK(store_view->polls == 1);
    CHECK(services::product(world, "coins_100")->price == "1,99 €");
    CHECK_FALSE(services::product(world, "vip")->available);
    auto unsold = services::buy(world, "vip");
    REQUIRE_FALSE(unsold.ok());
    CHECK(unsold.error().message == "the store doesn't sell \"vip\"");
    // The purchase the store delivered again waits to be finished.
    REQUIRE(services::unfinished(world).size() == 1);
    calls->list.clear();
    REQUIRE(services::finish(world, "GPA.1").ok());
    REQUIRE(services::buy(world, "remove_ads").ok());
    REQUIRE(services::show_ad(world, "bonus").ok());
    REQUIRE(services::restore_purchases(world).ok());
    CHECK(step(world) == std::vector<std::string>{"purchase_completed:remove_ads", "ad_shown:bonus",
                                                  "ad_rewarded:bonus", "ad_closed:bonus",
                                                  "restore_finished"});
    CHECK(services::events(world)[2].reward == services::Reward{.type = "gems", .amount = 2});
    CHECK(services::events(world)[4].error == "network down");
    CHECK(services::owns(world, "remove_ads"));
    REQUIRE(services::finish(world, "GPA.2").ok());
    CHECK(calls->list == std::vector<std::string>{"store.finish GPA.1 consumable",
                                                  "store.purchase remove_ads", "ads.show bonus",
                                                  "store.restore", "ads.load bonus " + bonus_id,
                                                  "store.finish GPA.2 non_consumable"});

    // Analytics waits for consent, then gets what the game set before it.
    calls->list.clear();
    REQUIRE(services::log_event(world, "before").ok());
    services::set_consent(world, {.analytics = services::Consent::Granted,
                                  .personalized_ads = services::Consent::Granted});
    REQUIRE(services::set_user_property(world, "class", "mage").ok());
    REQUIRE(services::log_event(world, "after", parse(R"({"n": 1})")).ok());
    services::flush_analytics(world);
    CHECK(calls->list == std::vector<std::string>{"ads.consent granted", "analytics.user player-7",
                                                  "analytics.property class=mage",
                                                  R"(analytics.event after {"n":1})",
                                                  "analytics.flush"});
    const json::Value state = services::state_json(world, 0);
    CHECK(compact(*state.find("backends")) ==
          R"({"ads":"fake_ads","analytics":["fake_analytics"],"purchases":"fake_store"})");
    CHECK_FALSE(services::set_test_settings(world, {}).ok());

    // Reports about placements the game doesn't have are dropped.
    auto reports = std::make_shared<services::Reports>();
    reports->ad_loaded("nowhere");
    CHECK(reports->take().size() == 1);
    CHECK(reports->take().empty());
}

TEST_CASE("services.state and services.test drive the test backend in a session") {
    const std::filesystem::path root = "services_test_project";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    REQUIRE(slash::platform::write_file(
                (root / "project.json").string(),
                R"({"format": 1, "name": "Shop", "main_scene": "main.scene.json", "services": )" +
                    std::string(kSettings) + "}")
                .ok());
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::Session session(components, commands, {.project_root = root.string()});
    auto call = [&](std::string_view name, std::string_view params) {
        return session.call(name, parse(params));
    };
    REQUIRE(call("scene.new", "{}").ok);

    command::Reply reply = call("services.test", R"({"outcome": "pending", "load_ticks": 2})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(reply.value.find("test")->find("outcome")->as_string() == "pending");
    REQUIRE(call("step", R"({"ticks": 2})").ok);
    reply = call("services.state", R"({"events": 0})");
    REQUIRE(reply.ok);
    CHECK(reply.value.find("products_status")->as_string() == "fetched");
    CHECK(reply.value.find("ads")->find("bonus")->find("status")->as_string() == "loaded");

    // A failed call changes nothing: the ad it showed first is not showing.
    reply = call("services.test", R"({"show": "bonus", "buy": "gold"})");
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.code == command::code::kNotFound);
    CHECK(reply.data.find("path")->as_string() == "/buy");
    CHECK(services::ad_status(session.world(), "bonus") == services::AdStatus::Loaded);

    reply = call("services.test", R"({"buy": "vip", "show": "bonus"})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    REQUIRE(call("step", R"({"ticks": 1})").ok);
    reply = call("services.state", "{}");
    CHECK(reply.value.find("pending")->as_array().size() == 1);
    REQUIRE(call("services.test", R"({"complete_pending": "purchased"})").ok);
    REQUIRE(call("step", R"({"ticks": 2})").ok);
    reply = call("services.state", "{}");
    CHECK(compact(*reply.value.find("owned")) == R"(["vip"])");
    CHECK(reply.value.find("unfinished")->as_array()[0].find("transaction")->as_string() ==
          "test-1");
    REQUIRE(call("services.test", R"({"finish": "test-1"})").ok);

    reply = call("services.test", R"({"prices": {"coins_10": 1}})");
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.message ==
          "/prices/coins_10: no product \"coins_10\"; did you mean \"coins_100\"?");
    reply = call("services.test", R"({"hide": "bonus"})");
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.message == "/hide: only banners hide; rewarded ads close on their own");

    std::filesystem::remove_all(root);
}

TEST_CASE("packages' backends merge, and stopping flushes analytics and lets go of them") {
    auto calls = std::make_shared<Calls>();
    services::Backends backends;
    services::Backends first;
    first.ads = std::make_unique<FakeAds>(calls);
    first.analytics.push_back(std::make_unique<FakeAnalytics>(calls));
    services::merge_backends(backends, std::move(first), "first");
    // Analytics backends all run; a second ads backend is dropped.
    services::Backends second;
    second.ads = std::make_unique<FakeAds>(calls);
    second.analytics.push_back(std::make_unique<FakeAnalytics>(calls));
    second.purchases = std::make_unique<FakeStore>(calls);
    services::merge_backends(backends, std::move(second), "second");
    CHECK(backends.ads != nullptr);
    CHECK(backends.analytics.size() == 2);
    CHECK(backends.purchases != nullptr);

    world::ComponentRegistry components;
    world::World world(components);
    constexpr std::string_view kNone = R"({"ads":"none","analytics":[],"purchases":"none"})";
    CHECK(compact(services::backends_json(world)) == kNone);
    services::stop(world); // does nothing before install
    services::install(world, test_settings());
    services::set_backends(world, std::move(backends));
    CHECK(compact(services::backends_json(world)) ==
          R"({"ads":"fake_ads","analytics":["fake_analytics","fake_analytics"],)"
          R"("purchases":"fake_store"})");
    calls->list.clear();
    services::stop(world);
    CHECK(calls->list == std::vector<std::string>{"analytics.flush", "analytics.flush"});
    CHECK(compact(services::backends_json(world)) == kNone);
    services::use_test_backend(world);
    CHECK(compact(services::backends_json(world)) ==
          R"({"ads":"test","analytics":["test"],"purchases":"test"})");
}
