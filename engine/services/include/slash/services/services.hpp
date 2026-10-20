#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slash::world {
class World;
} // namespace slash::world

// Ads, analytics and in-app purchases behind one cross-platform API, as Unity's Ads, Analytics
// and In-App Purchasing. A game loads and shows ad placements, logs events and sells products by
// the names in project.json's "services", and hears back through world events at a tick, as it
// hears input. Backends do the work: an SDK package's glue code implements AdsBackend,
// AnalyticsBackend or PurchasesBackend over a vendor's SDK or the platform's store. Sessions use
// the test backend, which loads ads and completes purchases as its settings say, with no store
// account. With no backend, ads fail to load, the store is unavailable and events are only kept.
namespace slash::services {

// ---------------------------------------------------------------------------------------------
// Settings

enum class AdFormat : std::uint8_t { Banner, Interstitial, Rewarded };

// What a watched rewarded ad gives, as AdMob's RewardItem.
struct Reward {
    std::string type; // e.g. "coins"
    double amount = 0.0;

    friend bool operator==(const Reward&, const Reward&) = default;
};

// An ad placement: where in the game ads show, by a name the game chooses, as Unity's placement
// IDs.
struct Placement {
    std::string name;
    AdFormat format = AdFormat::Interstitial;
    // The ad network's ad unit ID on each platform ("android", "ios" and so on), which the ads
    // backend loads.
    std::map<std::string, std::string, std::less<>> ids;
    // What a watched rewarded ad gives when the network doesn't say.
    Reward reward;
    // Load at start and again after each time the ad closes, so it is ready when the game wants
    // it.
    bool auto_load = true;

    friend bool operator==(const Placement&, const Placement&) = default;
};

enum class ProductType : std::uint8_t { Consumable, NonConsumable, Subscription };

// A product the game sells, by the store's product ID, and what the store says about it once
// products are fetched.
struct Product {
    std::string id;
    ProductType type = ProductType::Consumable;
    std::string title;
    std::string description;
    std::string price;             // as the store shows it, in the player's currency: "$0.99"
    std::int64_t price_micros = 0; // the price in millionths of the currency
    std::string currency;          // its ISO 4217 code: "USD"
    bool available = false;        // the store sells it

    friend bool operator==(const Product&, const Product&) = default;
};

enum class Consent : std::uint8_t { Unknown, Granted, Denied };

// What the player agreed to, as one state that gates analytics and ads.
struct ConsentState {
    Consent analytics = Consent::Unknown;        // events are sent only when Granted
    Consent personalized_ads = Consent::Unknown; // ads are personalized only when Granted

    friend bool operator==(const ConsentState&, const ConsentState&) = default;
};

// project.json's "services".
struct Settings {
    std::vector<Placement> placements; // by name
    std::vector<Product> products;     // their IDs and types
    bool analytics_enabled = true;
    ConsentState consent; // until the game sets it

    friend bool operator==(const Settings&, const Settings&) = default;
};

[[nodiscard]] json::Value settings_schema();
// Every error in project.json's "services", each with its JSON Pointer under at, such as
// "/services".
[[nodiscard]] std::vector<json::ValidationError> validate_settings(const json::Value& settings,
                                                                   std::string_view at);
// The settings in a valid "services" object.
[[nodiscard]] Settings parse_settings(const json::Value& settings);

[[nodiscard]] std::string_view format_name(AdFormat format);
[[nodiscard]] std::string_view type_name(ProductType type);
[[nodiscard]] std::string_view consent_name(Consent consent);

// ---------------------------------------------------------------------------------------------
// Backends

enum class PurchaseState : std::uint8_t { Purchased, Pending, Cancelled, Failed, Restored };

[[nodiscard]] std::string_view state_name(PurchaseState state);

struct Purchase {
    std::string product;
    std::string transaction; // the store's ID for it; empty for cancelled and failed ones
    PurchaseState state = PurchaseState::Purchased;
    // What the game's server checks with the store: StoreKit's signed transaction or Google
    // Play's purchase token.
    std::string receipt;
    std::string error; // why it failed

    friend bool operator==(const Purchase&, const Purchase&) = default;
};

// One thing a backend reported.
struct Report {
    enum class Kind : std::uint8_t {
        AdLoaded,
        AdFailed,
        AdShown,
        AdRewarded,
        AdClosed,
        Products,
        ProductsFailed,
        Purchase,
        RestoreFinished,
    };
    Kind kind = Kind::AdLoaded;
    std::string placement;
    std::string error;
    Reward reward;
    std::vector<Product> products;
    services::Purchase purchase;
};

// Where backends report what happened, from any thread. The "services" system takes the reports
// at the start of the next tick, so the game hears of them on its own thread, in order.
class Reports {
public:
    void ad_loaded(std::string_view placement);
    // Loading or showing the ad failed.
    void ad_failed(std::string_view placement, std::string_view error);
    void ad_shown(std::string_view placement);
    // The player watched a rewarded ad far enough to earn its reward; one with no type gives
    // the placement's.
    void ad_rewarded(std::string_view placement, Reward reward = {});
    void ad_closed(std::string_view placement);
    // What the store says about the products it sells, by ID.
    void products(std::vector<Product> products);
    void products_failed(std::string_view error);
    // A purchase's outcome, or one the store delivers unasked: approved later, restored, or
    // left unfinished when the game last ran.
    void purchase(Purchase purchase);
    void restore_finished(std::string_view error = {});

    // The reports so far, oldest first, leaving none.
    [[nodiscard]] std::vector<Report> take();

private:
    void add(Report report);

    std::mutex mutex_;
    std::vector<Report> queue_;
};

// An ad placement as a backend sees it.
struct AdUnit {
    std::string placement;
    AdFormat format = AdFormat::Interstitial;
    std::string id; // the placement's ad unit ID on this platform, or "" if it has none
};

class Backend {
public:
    Backend() = default;
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    Backend(Backend&&) = delete;
    Backend& operator=(Backend&&) = delete;
    virtual ~Backend() = default;

    // A short name for services.state and the log: "admob", "storekit".
    [[nodiscard]] virtual std::string_view name() const = 0;
    // Called once before anything else, with where to report; keep it for callbacks.
    virtual void start(const std::shared_ptr<Reports>& reports) { (void)reports; }
    // The player's consent, after start and whenever it changes.
    virtual void set_consent(const ConsentState& consent) { (void)consent; }
    // At the start of each tick, on the game's thread, for backends that poll.
    virtual void poll() {}
};

class AdsBackend : public Backend {
public:
    // Report ad_loaded or ad_failed.
    virtual void load(const AdUnit& unit) = 0;
    // Shows a loaded ad. Report ad_shown, then for an interstitial or rewarded ad ad_rewarded if
    // the player earned it and ad_closed; or ad_failed.
    virtual void show(const AdUnit& unit) = 0;
    // Hides a banner.
    virtual void hide(const AdUnit& unit) = 0;
};

class AnalyticsBackend : public Backend {
public:
    // An event with its parameters (an object of strings, numbers and booleans, or null), which
    // the game logged with the player's consent.
    virtual void log_event(std::string_view name, const json::Value& params) = 0;
    virtual void set_user_property(std::string_view name, std::string_view value) {
        (void)name;
        (void)value;
    }
    virtual void set_user_id(std::string_view id) { (void)id; }
    // Sends what is batched now, as when the game goes to the background.
    virtual void flush() {}
};

class PurchasesBackend : public Backend {
public:
    // Asks the store about the products: report products or products_failed, then any
    // purchases the store has not had finished, as Unity IAP delivers them again at start.
    virtual void fetch_products(std::span<const Product> catalog) = 0;
    // Report the purchase's outcome.
    virtual void purchase(const Product& product) = 0;
    // The game granted the purchase: acknowledge it, consuming a consumable so it can be bought
    // again.
    virtual void finish(const Purchase& purchase, const Product& product) = 0;
    // Report each owned non-consumable and subscription as Restored, then restore_finished.
    virtual void restore() = 0;
};

// The backends a game runs on; any may be missing.
struct Backends {
    std::unique_ptr<AdsBackend> ads;
    std::vector<std::unique_ptr<AnalyticsBackend>> analytics;
    std::unique_ptr<PurchasesBackend> purchases;
};

// How the test backend behaves.
struct TestSettings {
    bool fill = true;         // ads load; when false they fail with "no fill"
    int load_ticks = 1;       // ticks an ad takes to load
    int show_ticks = 1;       // ticks an interstitial or rewarded ad shows before it closes
    bool watch_to_end = true; // the player watches rewarded ads to the end and earns the reward
    int purchase_ticks = 1;   // ticks a purchase takes
    PurchaseState outcome = PurchaseState::Purchased;  // Purchased, Pending, Cancelled or Failed
    std::string error = "the payment was declined";    // a failed purchase's error
    std::map<std::string, double, std::less<>> prices; // in US dollars; others cost 0.99

    friend bool operator==(const TestSettings&, const TestSettings&) = default;
};

// ---------------------------------------------------------------------------------------------
// Worlds

// Adds the services state and the "services" system (priority -800, after input and the GUI),
// which hands each tick's reports to the game as events, and starts the services: products are
// fetched and auto_load placements loaded. Installing again with other settings replaces them,
// keeps the backends and starts again.
void install(world::World& world, Settings settings = {});
// Adds from's backends to into's, as the backends SDK packages register are joined: analytics
// backends all run, but a service runs on one backend, so an ads or purchases backend into
// already has is kept and from's is dropped with an error logged naming source.
void merge_backends(Backends& into, Backends from, std::string_view source);

// Runs the services on these backends instead, starting them now. Services left without one
// work as with none.
void set_backends(world::World& world, Backends backends);
// Sends what analytics backends batched and lets go of the backends, as when the game quits; the
// services then work as with none.
void stop(world::World& world);
// Runs all three services on the test backend instead, as sessions do, with a fresh test store.
void use_test_backend(world::World& world, TestSettings settings = {});
// Whether the test backend runs the services, and its settings.
[[nodiscard]] bool testing(const world::World& world);
[[nodiscard]] const TestSettings* test_settings(const world::World& world);
// Changes the test backend's settings, fetching products again if prices changed; fails if it
// isn't in use.
[[nodiscard]] Result<void> set_test_settings(world::World& world, TestSettings settings);
// Ends every pending test purchase at the next tick as Purchased, Cancelled or Failed, as when
// a parent approves or declines one; fails if the test backend isn't in use.
[[nodiscard]] Result<void> complete_pending(world::World& world, PurchaseState outcome);

[[nodiscard]] const Settings* settings(const world::World& world);

// Ads, as Unity's Advertisement: load an ad, wait for ad_loaded, show it. An ad that closed must
// load again, which auto_load placements do on their own.
enum class AdStatus : std::uint8_t { Unloaded, Loading, Loaded, Showing, Failed };
[[nodiscard]] std::string_view status_name(AdStatus status);
[[nodiscard]] Result<void> load_ad(world::World& world, std::string_view name);
[[nodiscard]] Result<void> show_ad(world::World& world, std::string_view name);
// Hides a banner that shows.
[[nodiscard]] Result<void> hide_ad(world::World& world, std::string_view name);
[[nodiscard]] AdStatus ad_status(const world::World& world, std::string_view placement);
[[nodiscard]] bool is_ad_loaded(const world::World& world, std::string_view placement);

// Analytics. Names start with a letter and hold letters, digits and '_', at most 40 of them;
// params is null or an object of at most 25 such names, each a string of at most 100 bytes, a
// number or a boolean: the strictest of the common services' rules, so an event any backend
// takes. Events are kept for services.state and sent to the analytics backends only while
// analytics is enabled and the player granted consent.
[[nodiscard]] Result<void> log_event(world::World& world, std::string_view name,
                                     const json::Value& params = {});
[[nodiscard]] Result<void> set_user_property(world::World& world, std::string_view name,
                                             std::string_view value);
void set_user_id(world::World& world, std::string_view id);
void flush_analytics(world::World& world);

// Purchases, as Unity IAP: products are fetched at start (products_fetched), buy reports
// purchase_completed, and the game grants the product and calls finish with its transaction,
// which a store requires before it lets a consumable be bought again. Until then the purchase
// is unfinished, and the store delivers it again at the next start.
[[nodiscard]] Result<void> buy(world::World& world, std::string_view id);
[[nodiscard]] Result<void> finish(world::World& world, std::string_view transaction);
[[nodiscard]] Result<void> restore_purchases(world::World& world);
// The product with what the store says about it; nullptr if the game doesn't sell it.
[[nodiscard]] const Product* product(const world::World& world, std::string_view id);
// Whether the player owns a non-consumable or subscription.
[[nodiscard]] bool owns(const world::World& world, std::string_view product);
// Purchases completed but not finished, oldest first.
[[nodiscard]] std::vector<Purchase> unfinished(const world::World& world);

// The player's consent, which the game asks for and keeps; it starts as project.json's.
void set_consent(world::World& world, ConsentState consent);
[[nodiscard]] ConsentState consent(const world::World& world);

// What happened at a tick, also emitted as a world event of the same kind with the placement or
// product as its ID:
//   ad_loaded, ad_shown, ad_closed       an ad placement's ad
//   ad_failed                            loading or showing it failed: {"error"}
//   ad_rewarded                          the player earned the reward: {"type", "amount"}
//   products_fetched                     the store's products: {"available": [IDs]}
//   products_failed                      {"error"}
//   purchase_completed                   {"transaction", "state" ("purchased" or "restored"),
//                                        "receipt"}: grant it, then finish
//   purchase_pending                     waiting on the player or a parent: {"transaction"}
//   purchase_cancelled                   the player cancelled it
//   purchase_failed                      {"error"}
//   restore_finished                     restore_purchases is done: {"error"} if it failed
struct ServiceEvent {
    std::string kind;
    std::string id;
    Reward reward;
    Purchase purchase;
    std::string error;
    std::vector<std::string> available;
};
// The events of the tick the "services" system last ran (inside a system: this tick).
[[nodiscard]] std::span<const ServiceEvent> events(const world::World& world);

// The backends the services run on, by name: {"ads", "analytics": [...], "purchases"}, with
// "none" for a service with no backend and "test" for the test backend's.
[[nodiscard]] json::Value backends_json(const world::World& world);

// What services.state returns: the backends, consent, each placement's status, the products,
// what the player owns, purchases unfinished and pending, and the last analytics_events events
// logged.
[[nodiscard]] json::Value state_json(const world::World& world, std::size_t analytics_events);

// A copy of the services' state, so a session can put it back when a command fails or was a dry
// run. Backends are not copied.
struct Snapshot {
    std::shared_ptr<const void> data;
};
[[nodiscard]] Snapshot save(const world::World& world);
void restore(world::World& world, const Snapshot& snapshot);

} // namespace slash::services
