#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slash::world {
class World;
} // namespace slash::world

// Ads, analytics, in-app purchases, achievements and leaderboards behind one cross-platform API,
// as Unity's Ads, Analytics, In-App Purchasing and Social API. A game loads and shows ad
// placements, logs events, sells products, unlocks achievements and submits scores by the names
// in project.json's "services", and hears back through world events at a tick, as it hears
// input. Backends do the work: an SDK package's glue code implements AdsBackend,
// AnalyticsBackend, PurchasesBackend or GamesBackend over a vendor's SDK or the platform's store
// and games service. Sessions use the test backend, which loads ads, completes purchases and
// signs the player in as its settings say, with no account. With no backend, ads fail to load,
// the store is unavailable, sign-in fails and events are only kept.
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

// An achievement, by a name the game chooses: unlocked at once, or incremental and unlocked when
// its steps are done, as Google Play Games' achievements.
struct Achievement {
    std::string name;
    // Its ID on each platform's games service: Google Play Games' on "android", Game Center's on
    // "ios" and "macos".
    std::map<std::string, std::string, std::less<>> ids;
    int steps = 0; // an incremental achievement's steps; 0 for one unlocked at once

    friend bool operator==(const Achievement&, const Achievement&) = default;
};

// Which scores rank first, as the leaderboard is set up in Play Console and App Store Connect.
enum class SortOrder : std::uint8_t { HighToLow, LowToHigh };

// A leaderboard, by a name the game chooses.
struct Leaderboard {
    std::string name;
    std::map<std::string, std::string, std::less<>> ids; // as an achievement's
    SortOrder sort_order = SortOrder::HighToLow;

    friend bool operator==(const Leaderboard&, const Leaderboard&) = default;
};

// project.json's "services".
struct Settings {
    std::vector<Placement> placements; // by name
    std::vector<Product> products;     // their IDs and types
    bool analytics_enabled = true;
    ConsentState consent;                  // until the game sets it
    std::vector<Achievement> achievements; // by name
    std::vector<Leaderboard> leaderboards; // by name
    // Sign the player in when the services start, as Play Games and Game Center do, if the game
    // has achievements or leaderboards or a games backend.
    bool sign_in_at_start = true;

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
[[nodiscard]] std::string_view sort_order_name(SortOrder order);

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

// A player signed in to the platform's games service.
struct Player {
    std::string id;   // the platform's player ID
    std::string name; // the name the player shows

    friend bool operator==(const Player&, const Player&) = default;
};

// How far a player is with an achievement.
struct AchievementProgress {
    std::string achievement; // its name
    int steps = 0;           // an incremental achievement's steps done
    bool unlocked = false;

    friend bool operator==(const AchievementProgress&, const AchievementProgress&) = default;
};

enum class TimeSpan : std::uint8_t { AllTime, Week, Today };
enum class Collection : std::uint8_t { Global, Friends };

[[nodiscard]] std::string_view span_name(TimeSpan span);
[[nodiscard]] std::string_view collection_name(Collection collection);

// Which scores load_scores loads: the best count over a time span, of every player or of the
// player's friends, as Play Games' loadTopScores and Game Center's loadEntries.
struct ScoreQuery {
    TimeSpan time = TimeSpan::AllTime;
    Collection collection = Collection::Global;
    int count = 10; // 1 to 25, as Play Games allows

    friend bool operator==(const ScoreQuery&, const ScoreQuery&) = default;
};

// One player's score on a leaderboard.
struct Score {
    std::int64_t rank = 0; // 1 for the best
    std::string player;    // the player's ID
    std::string name;      // the name they show
    std::int64_t value = 0;

    friend bool operator==(const Score&, const Score&) = default;
};

// A leaderboard's scores, as load_scores loaded them.
struct Scores {
    ScoreQuery query; // the services fill it in; backends may leave it
    std::vector<Score> entries;
    std::optional<Score> player; // the signed-in player's, if they have one
    std::int64_t total = 0;      // players with a score

    friend bool operator==(const Scores&, const Scores&) = default;
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
        SignedIn,
        SignInFailed,
        AchievementsLoaded,
        AchievementFailed,
        ScoreSubmitted,
        ScoreFailed,
        ScoresLoaded,
        ScoresFailed,
        GamesUiClosed,
        // Made by the services themselves when progress unlocks an achievement.
        AchievementUnlocked,
    };
    Kind kind = Kind::AdLoaded;
    std::string placement;
    std::string error;
    Reward reward;
    std::vector<Product> products;
    services::Purchase purchase;
    std::string name; // an achievement or leaderboard
    services::Player player;
    std::vector<AchievementProgress> achievements;
    std::int64_t score = 0;
    services::Scores scores;
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
    void signed_in(Player player);
    void sign_in_failed(std::string_view error);
    // The player's progress on the platform after load_achievements, by achievement name; those
    // left out have none.
    void achievements(std::vector<AchievementProgress> progress);
    // The platform refused an achievement's progress, or loading it with no achievement.
    void achievement_failed(std::string_view achievement, std::string_view error);
    void score_submitted(std::string_view leaderboard, std::int64_t score);
    void score_failed(std::string_view leaderboard, std::string_view error);
    void scores(std::string_view leaderboard, Scores scores);
    void scores_failed(std::string_view leaderboard, std::string_view error);
    // The player closed the achievements or leaderboards screen.
    void games_ui_closed();

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

// An achievement as a backend sees it.
struct AchievementUnit {
    std::string achievement;
    std::string id; // its ID on this platform, or "" if it has none
    int steps = 0;  // an incremental achievement's steps; 0 for one unlocked at once
};

// A leaderboard as a backend sees it.
struct LeaderboardUnit {
    std::string leaderboard;
    std::string id; // its ID on this platform, or "" if it has none
    SortOrder sort_order = SortOrder::HighToLow;
};

class GamesBackend : public Backend {
public:
    // Report signed_in or sign_in_failed. interactive is false when the services start, where
    // Play Games and Game Center sign a returning player in without asking, and true when the
    // game asks, where the platform may show its sign-in screen.
    virtual void sign_in(bool interactive) = 0;
    // After signed_in: report achievements with the player's progress, or achievement_failed
    // with no achievement.
    virtual void load_achievements(std::span<const AchievementUnit> units) = 0;
    // Report achievement_failed if the platform refuses.
    virtual void unlock(const AchievementUnit& unit) = 0;
    // An incremental achievement's steps done, which unlocks it at unit.steps: Play Games'
    // setSteps, or Game Center's percentComplete of steps out of unit.steps. Report
    // achievement_failed if the platform refuses.
    virtual void set_steps(const AchievementUnit& unit, int steps) = 0;
    // Report score_submitted or score_failed.
    virtual void submit_score(const LeaderboardUnit& unit, std::int64_t score) = 0;
    // Report scores or scores_failed.
    virtual void load_scores(const LeaderboardUnit& unit, const ScoreQuery& query) = 0;
    // Shows the platform's achievements screen over the game; report games_ui_closed when the
    // player closes it.
    virtual void show_achievements() = 0;
    // Shows a leaderboard's screen, or every leaderboard's for nullptr; report games_ui_closed.
    virtual void show_leaderboards(const LeaderboardUnit* unit) = 0;
};

// The backends a game runs on; any may be missing.
struct Backends {
    std::unique_ptr<AdsBackend> ads;
    std::vector<std::unique_ptr<AnalyticsBackend>> analytics;
    std::unique_ptr<PurchasesBackend> purchases;
    std::unique_ptr<GamesBackend> games;
};

// How the test backend behaves.
struct TestSettings {
    bool fill = true;   // ads load; when false they fail with "no fill"
    int load_ticks = 1; // ticks an ad takes to load
    // Ticks an interstitial or rewarded ad, or the achievements or leaderboards screen, shows
    // before it closes.
    int show_ticks = 1;
    bool watch_to_end = true; // the player watches rewarded ads to the end and earns the reward
    int purchase_ticks = 1;   // ticks a purchase takes
    PurchaseState outcome = PurchaseState::Purchased;  // Purchased, Pending, Cancelled or Failed
    std::string error = "the payment was declined";    // a failed purchase's error
    std::map<std::string, double, std::less<>> prices; // in US dollars; others cost 0.99
    bool accept_sign_in = true;    // the player signs in; when false sign-in fails
    std::string player = "Player"; // the signed-in player's name; their ID is "test-player"
    int games_ticks = 1;           // ticks signing in, submitting a score and loading scores take
    // Other players' scores on each leaderboard, by its name: their names and values.
    std::map<std::string, std::vector<Score>, std::less<>> scores;

    friend bool operator==(const TestSettings&, const TestSettings&) = default;
};

// ---------------------------------------------------------------------------------------------
// Worlds

// Adds the services state and the "services" system (priority -800, after input and the GUI),
// which hands each tick's reports to the game as events, and starts the services: products are
// fetched, auto_load placements loaded and the player signed in. Installing again with other
// settings replaces them, keeps the backends and starts again.
void install(world::World& world, Settings settings = {});
// Adds from's backends to into's, as the backends SDK packages register are joined: analytics
// backends all run, but a service runs on one backend, so a backend for a service into
// already has is kept and from's is dropped with an error logged naming source. Ads, purchases
// and games each run on one backend.
void merge_backends(Backends& into, Backends from, std::string_view source);

// Runs the services on these backends instead, starting them now. Services left without one
// work as with none.
void set_backends(world::World& world, Backends backends);
// Sends what analytics backends batched and lets go of the backends, as when the game quits; the
// services then work as with none.
void stop(world::World& world);
// Runs every service on the test backend instead, as sessions do, with a fresh test store.
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

// Achievements, leaderboards and sign-in, as Unity's Social API over Google Play Games and Game
// Center. The player is signed in when the services start (sign_in_at_start), and sign_in asks
// again, as a Sign In button does. Progress and scores made before the player signs in are kept
// and sent when they do.
enum class SignIn : std::uint8_t { SignedOut, SigningIn, SignedIn, Failed };
[[nodiscard]] std::string_view sign_in_name(SignIn status);
// Signs the player in, showing the platform's sign-in screen if it needs to; does nothing while
// they are signed in or signing in.
[[nodiscard]] Result<void> sign_in(world::World& world);
[[nodiscard]] SignIn sign_in_status(const world::World& world);
// The signed-in player; nullptr until signed_in.
[[nodiscard]] const Player* player(const world::World& world);
// Unlocks an achievement; one already unlocked stays so.
[[nodiscard]] Result<void> unlock_achievement(world::World& world, std::string_view name);
// Adds steps to an incremental achievement, unlocking it when they are all done.
[[nodiscard]] Result<void> increment_achievement(world::World& world, std::string_view name,
                                                 int steps = 1);
// An achievement's progress; nullptr if the game has none by that name.
[[nodiscard]] const AchievementProgress* achievement(const world::World& world,
                                                     std::string_view name);
// Submits a score; the platform keeps each player's best.
[[nodiscard]] Result<void> submit_score(world::World& world, std::string_view leaderboard,
                                        std::int64_t score);
// The best score the game submitted to a leaderboard since the services started, by its sort
// order.
[[nodiscard]] std::optional<std::int64_t> best_score(const world::World& world,
                                                     std::string_view leaderboard);
// Loads a leaderboard's scores (scores_loaded); the player must be signed in.
[[nodiscard]] Result<void> load_scores(world::World& world, std::string_view leaderboard,
                                       ScoreQuery query = {});
// Shows the platform's achievements screen over the game, or a leaderboard's ("" for every
// leaderboard); games_ui_closed when the player closes it. The player must be signed in.
[[nodiscard]] Result<void> show_achievements(world::World& world);
[[nodiscard]] Result<void> show_leaderboards(world::World& world,
                                             std::string_view leaderboard = {});

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
//   signed_in                            the player, by ID: {"name"}
//   sign_in_failed                       {"error"}
//   achievements_loaded                  the player's progress arrived after signing in:
//                                        {"unlocked": [names]}
//   achievement_unlocked                 an achievement, by name, unlocked now
//   achievement_failed                   the platform refused it: {"error"}
//   score_submitted                      a leaderboard, by name: {"score"}
//   score_failed                         {"error"}
//   scores_loaded                        {"time", "collection", "entries": [{"rank", "player",
//                                        "name", "score"}], "player", "total"}
//   scores_failed                        {"error"}
//   games_ui_closed                      the achievements or leaderboards screen closed
struct ServiceEvent {
    std::string kind;
    std::string id;
    Reward reward;
    Purchase purchase;
    std::string error;
    std::vector<std::string> available;
    services::Player player;
    std::vector<std::string> unlocked;
    std::int64_t score = 0;
    services::Scores scores;
};
// The events of the tick the "services" system last ran (inside a system: this tick).
[[nodiscard]] std::span<const ServiceEvent> events(const world::World& world);

// The backends the services run on, by name: {"ads", "analytics": [...], "purchases", "games"},
// with "none" for a service with no backend and "test" for the test backend's.
[[nodiscard]] json::Value backends_json(const world::World& world);

// What services.state returns: the backends, consent, each placement's status, the products,
// what the player owns, purchases unfinished and pending, the last analytics_events events
// logged, and for a game with achievements or leaderboards, sign-in and their progress.
[[nodiscard]] json::Value state_json(const world::World& world, std::size_t analytics_events);

// A copy of the services' state, so a session can put it back when a command fails or was a dry
// run. Backends are not copied.
struct Snapshot {
    std::shared_ptr<const void> data;
};
[[nodiscard]] Snapshot save(const world::World& world);
void restore(world::World& world, const Snapshot& snapshot);

} // namespace slash::services
