#include "slash/services/services.hpp"

#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/platform/platform.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <set>
#include <utility>

namespace slash::services {
namespace {

// How many logged analytics events services.state can show.
constexpr std::size_t kMaxLogged = 256;
constexpr std::size_t kMaxEventName = 40;
constexpr std::size_t kMaxParams = 25;
constexpr std::size_t kMaxParamText = 100;
constexpr double kTestPrice = 0.99;
constexpr double kMicros = 1'000'000.0;

constexpr std::array<std::string_view, 5> kStatuses{"unloaded", "loading", "loaded", "showing",
                                                    "failed"};
constexpr std::array<std::string_view, 5> kStates{"purchased", "pending", "cancelled", "failed",
                                                  "restored"};

enum class ProductsStatus : std::uint8_t { None, Fetching, Fetched, Failed };

struct AdState {
    AdStatus status = AdStatus::Unloaded;
    std::string error;
};

// A report the test backend, or a missing backend, makes at a tick.
struct Scheduled {
    std::uint64_t due = 0;
    Report report;
};

struct Logged {
    std::uint64_t tick = 0;
    std::string name;
    json::Value params;
    bool sent = false;
};

// Everything snapshots copy: the game's view of the services and the test store.
struct State {
    Settings settings;
    bool test = false;
    TestSettings test_settings;
    ConsentState consent;
    std::uint64_t tick = 0;
    std::map<std::string, AdState, std::less<>> ads;
    std::vector<Product> products;
    ProductsStatus products_status = ProductsStatus::None;
    std::string products_error;
    std::set<std::string, std::less<>> owned;
    std::vector<Purchase> unfinished;
    std::vector<Purchase> pending;
    std::set<std::string, std::less<>> buying;
    bool restoring = false;
    std::deque<Logged> logged;
    std::uint64_t logged_count = 0;
    std::map<std::string, std::string, std::less<>> user_properties;
    std::string user_id;
    std::vector<Scheduled> scheduled;
    std::vector<ServiceEvent> events;
    // The test store's own records.
    std::set<std::string, std::less<>> store_owned;
    std::uint64_t next_transaction = 1;
};

using Handle = std::shared_ptr<State>;

// Kept apart from State so snapshots leave the backends alone.
struct BackendHolder {
    Backends backends;
    std::shared_ptr<Reports> reports = std::make_shared<Reports>();
};

State* state_of(world::World& world) {
    auto* found = world.ecs().ctx().find<Handle>();
    return found != nullptr ? found->get() : nullptr;
}

const State* state_of(const world::World& world) {
    const auto* found = world.ecs().ctx().find<Handle>();
    return found != nullptr ? found->get() : nullptr;
}

BackendHolder& holder_of(world::World& world) {
    return world.ecs().ctx().get<BackendHolder>();
}

const BackendHolder& holder_of(const world::World& world) {
    return world.ecs().ctx().get<BackendHolder>();
}

std::string quoted(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

Error not_installed() {
    return make_error(ErrorCode::InvalidArgument, "the services are not installed");
}

// The first tick that reports made now can reach: this one before the system runs, the next one
// from inside a system.
std::uint64_t next_tick(const world::World& world) {
    return world.stepping() ? world.tick() + 1 : world.tick();
}

void schedule(const world::World& world, State& state, int ticks, Report report) {
    const std::uint64_t due = next_tick(world) + static_cast<std::uint64_t>(std::max(ticks, 1)) - 1;
    state.scheduled.push_back({due, std::move(report)});
}

Report ad_report(Report::Kind kind, std::string_view placement, std::string_view error = {}) {
    Report report;
    report.kind = kind;
    report.placement = placement;
    report.error = error;
    return report;
}

Report purchase_report(Purchase purchase) {
    Report report;
    report.kind = Report::Kind::Purchase;
    report.purchase = std::move(purchase);
    return report;
}

const Placement* find_placement(const State& state, std::string_view name) {
    const auto it = std::ranges::find(state.settings.placements, name, &Placement::name);
    return it != state.settings.placements.end() ? &*it : nullptr;
}

Product* find_product(State& state, std::string_view id) {
    const auto it = std::ranges::find(state.products, id, &Product::id);
    return it != state.products.end() ? &*it : nullptr;
}

const Product* find_product(const State& state, std::string_view id) {
    const auto it = std::ranges::find(state.products, id, &Product::id);
    return it != state.products.end() ? &*it : nullptr;
}

bool is_owned_type(ProductType type) {
    return type == ProductType::NonConsumable || type == ProductType::Subscription;
}

Error unknown_placement(const State& state, std::string_view name) {
    std::string message = "no ad placement " + quoted(name);
    std::vector<std::string_view> names;
    names.reserve(state.settings.placements.size());
    for (const Placement& placement : state.settings.placements) {
        names.emplace_back(placement.name);
    }
    if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
        message += "; did you mean " + quoted(hint) + "?";
    } else if (names.empty()) {
        message += "; project.json's services.ads.placements has none";
    }
    return make_error(ErrorCode::NotFound, std::move(message));
}

Error unknown_product(const State& state, std::string_view id) {
    std::string message = "no product " + quoted(id);
    std::vector<std::string_view> ids;
    ids.reserve(state.products.size());
    for (const Product& product : state.products) {
        ids.emplace_back(product.id);
    }
    if (const std::string_view hint = closest_match(id, ids); !hint.empty()) {
        message += "; did you mean " + quoted(hint) + "?";
    } else if (ids.empty()) {
        message += "; project.json's services.purchases.products has none";
    }
    return make_error(ErrorCode::NotFound, std::move(message));
}

AdUnit unit_of(const Placement& placement) {
    AdUnit unit{.placement = placement.name, .format = placement.format, .id = {}};
    if (const auto it = placement.ids.find(platform::os_name()); it != placement.ids.end()) {
        unit.id = it->second;
    }
    return unit;
}

// "$1.99" for 1.99.
std::string dollars(double amount) {
    std::array<char, 32> text{};
    (void)std::snprintf(text.data(), text.size(), "$%.2f", amount);
    return text.data();
}

void request_load(world::World& world, State& state, const Placement& placement) {
    AdState& ad = state.ads[placement.name];
    ad.status = AdStatus::Loading;
    ad.error.clear();
    if (state.test) {
        const TestSettings& test = state.test_settings;
        schedule(world, state, test.load_ticks,
                 test.fill ? ad_report(Report::Kind::AdLoaded, placement.name)
                           : ad_report(Report::Kind::AdFailed, placement.name, "no fill"));
    } else if (AdsBackend* ads = holder_of(world).backends.ads.get(); ads != nullptr) {
        ads->load(unit_of(placement));
    } else {
        schedule(world, state, 1,
                 ad_report(Report::Kind::AdFailed, placement.name, "no ads backend"));
    }
}

void request_products(world::World& world, State& state) {
    if (state.products.empty()) {
        return;
    }
    state.products_status = ProductsStatus::Fetching;
    state.products_error.clear();
    if (state.test) {
        Report report;
        report.kind = Report::Kind::Products;
        for (const Product& product : state.products) {
            Product details = product;
            const auto price = state.test_settings.prices.find(product.id);
            const double amount =
                price != state.test_settings.prices.end() ? price->second : kTestPrice;
            details.title = product.id;
            details.price = dollars(amount);
            details.price_micros = std::llround(amount * kMicros);
            details.currency = "USD";
            details.available = true;
            report.products.push_back(std::move(details));
        }
        schedule(world, state, 1, std::move(report));
    } else if (PurchasesBackend* store = holder_of(world).backends.purchases.get();
               store != nullptr) {
        store->fetch_products(state.products);
    } else {
        Report report;
        report.kind = Report::Kind::ProductsFailed;
        report.error = "no store";
        schedule(world, state, 1, std::move(report));
    }
}

// Starts the services over: forgets ads and the store's answers, then fetches products and
// loads auto_load placements. What the player owns and hasn't finished stays.
void start(world::World& world, State& state) {
    state.ads.clear();
    for (const Placement& placement : state.settings.placements) {
        state.ads[placement.name] = {};
    }
    state.products = state.settings.products;
    state.products_status = ProductsStatus::None;
    state.products_error.clear();
    state.buying.clear();
    state.restoring = false;
    state.scheduled.clear();
    request_products(world, state);
    for (const Placement& placement : state.settings.placements) {
        if (placement.auto_load) {
            request_load(world, state, placement);
        }
    }
}

void tell_consent(world::World& world, const ConsentState& consent) {
    BackendHolder& holder = holder_of(world);
    if (holder.backends.ads) {
        holder.backends.ads->set_consent(consent);
    }
    for (const auto& analytics : holder.backends.analytics) {
        analytics->set_consent(consent);
    }
    if (holder.backends.purchases) {
        holder.backends.purchases->set_consent(consent);
    }
}

bool sends_analytics(const world::World& world, const State& state) {
    return state.settings.analytics_enabled && state.consent.analytics == Consent::Granted &&
           (state.test || !holder_of(world).backends.analytics.empty());
}

// Sends the user ID and properties the game set while nothing was sent.
void send_user(world::World& world, const State& state) {
    for (const auto& analytics : holder_of(world).backends.analytics) {
        if (!state.user_id.empty()) {
            analytics->set_user_id(state.user_id);
        }
        for (const auto& [name, value] : state.user_properties) {
            analytics->set_user_property(name, value);
        }
    }
}

void emit(world::World& world, State& state, ServiceEvent event, json::Value data = {}) {
    world.emit(event.kind, event.id, std::move(data));
    state.events.push_back(std::move(event));
}

void erase_purchases(std::vector<Purchase>& purchases, const Purchase& purchase) {
    std::erase_if(purchases, [&](const Purchase& each) {
        return each.product == purchase.product &&
               (purchase.transaction.empty() || each.transaction.empty() ||
                each.transaction == purchase.transaction);
    });
}

void apply_purchase(world::World& world, State& state, const Purchase& purchase) {
    const Product* product = find_product(state, purchase.product);
    ServiceEvent event{.kind = {},
                       .id = purchase.product,
                       .reward = {},
                       .purchase = purchase,
                       .error = {},
                       .available = {}};
    json::Value data = json::Object{};
    switch (purchase.state) {
    case PurchaseState::Purchased:
    case PurchaseState::Restored: {
        // A store delivering a purchase again, as at each start until it is finished.
        if (!purchase.transaction.empty() &&
            std::ranges::find(state.unfinished, purchase.transaction, &Purchase::transaction) !=
                state.unfinished.end()) {
            return;
        }
        state.buying.erase(purchase.product);
        erase_purchases(state.pending, purchase);
        state.unfinished.push_back(purchase);
        if (product != nullptr && is_owned_type(product->type)) {
            state.owned.insert(purchase.product);
        }
        event.kind = "purchase_completed";
        data["transaction"] = purchase.transaction;
        data["state"] = state_name(purchase.state);
        if (!purchase.receipt.empty()) {
            data["receipt"] = purchase.receipt;
        }
        break;
    }
    case PurchaseState::Pending:
        state.buying.erase(purchase.product);
        if (std::ranges::find(state.pending, purchase.transaction, &Purchase::transaction) ==
                state.pending.end() ||
            purchase.transaction.empty()) {
            state.pending.push_back(purchase);
        }
        event.kind = "purchase_pending";
        data["transaction"] = purchase.transaction;
        break;
    case PurchaseState::Cancelled:
        state.buying.erase(purchase.product);
        erase_purchases(state.pending, purchase);
        event.kind = "purchase_cancelled";
        break;
    case PurchaseState::Failed:
        state.buying.erase(purchase.product);
        erase_purchases(state.pending, purchase);
        event.kind = "purchase_failed";
        event.error = purchase.error;
        data["error"] = purchase.error;
        break;
    }
    if (data.size() == 0) {
        data = json::Value();
    }
    emit(world, state, std::move(event), std::move(data));
}

void apply(world::World& world, State& state, Report& report) {
    ServiceEvent event{.kind = {},
                       .id = report.placement,
                       .reward = {},
                       .purchase = {},
                       .error = {},
                       .available = {}};
    json::Value data;
    switch (report.kind) {
    case Report::Kind::AdLoaded:
    case Report::Kind::AdFailed:
    case Report::Kind::AdShown:
    case Report::Kind::AdRewarded:
    case Report::Kind::AdClosed: {
        const Placement* placement = find_placement(state, report.placement);
        if (placement == nullptr) {
            log::warn("an ads backend reported an unknown placement",
                      {{"placement", report.placement}});
            return;
        }
        AdState& ad = state.ads[placement->name];
        if (report.kind == Report::Kind::AdLoaded) {
            if (ad.status != AdStatus::Showing) {
                ad.status = AdStatus::Loaded;
            }
            event.kind = "ad_loaded";
        } else if (report.kind == Report::Kind::AdFailed) {
            ad.status = AdStatus::Failed;
            ad.error = report.error;
            event.kind = "ad_failed";
            event.error = report.error;
            data["error"] = report.error;
        } else if (report.kind == Report::Kind::AdShown) {
            ad.status = AdStatus::Showing;
            event.kind = "ad_shown";
        } else if (report.kind == Report::Kind::AdRewarded) {
            event.kind = "ad_rewarded";
            event.reward = report.reward.type.empty() ? placement->reward : report.reward;
            data["type"] = event.reward.type;
            data["amount"] = event.reward.amount;
        } else {
            ad.status = AdStatus::Unloaded;
            event.kind = "ad_closed";
        }
        emit(world, state, std::move(event), std::move(data));
        if (report.kind == Report::Kind::AdClosed && placement->auto_load) {
            request_load(world, state, *placement);
        }
        return;
    }
    case Report::Kind::Products: {
        for (Product& product : state.products) {
            const auto found = std::ranges::find(report.products, product.id, &Product::id);
            if (found == report.products.end()) {
                Product unsold;
                unsold.id = product.id;
                unsold.type = product.type;
                product = std::move(unsold);
                continue;
            }
            product.title = found->title;
            product.description = found->description;
            product.price = found->price;
            product.price_micros = found->price_micros;
            product.currency = found->currency;
            product.available = true;
            event.available.push_back(product.id);
        }
        state.products_status = ProductsStatus::Fetched;
        state.products_error.clear();
        event.kind = "products_fetched";
        json::Array available;
        for (const std::string& id : event.available) {
            available.emplace_back(id);
        }
        data["available"] = std::move(available);
        emit(world, state, std::move(event), std::move(data));
        return;
    }
    case Report::Kind::ProductsFailed:
        state.products_status = ProductsStatus::Failed;
        state.products_error = report.error;
        event.kind = "products_failed";
        event.error = report.error;
        data["error"] = report.error;
        emit(world, state, std::move(event), std::move(data));
        return;
    case Report::Kind::Purchase:
        apply_purchase(world, state, report.purchase);
        return;
    case Report::Kind::RestoreFinished:
        state.restoring = false;
        event.kind = "restore_finished";
        if (!report.error.empty()) {
            event.error = report.error;
            data["error"] = report.error;
        }
        emit(world, state, std::move(event), std::move(data));
        return;
    }
}

void run(world::World& world) {
    State* state = state_of(world);
    if (state == nullptr) {
        return;
    }
    BackendHolder& holder = holder_of(world);
    state->tick = world.tick();
    state->events.clear();
    if (!state->test) {
        if (holder.backends.ads) {
            holder.backends.ads->poll();
        }
        for (const auto& analytics : holder.backends.analytics) {
            analytics->poll();
        }
        if (holder.backends.purchases) {
            holder.backends.purchases->poll();
        }
    }
    // Reports due by now, in the order they were made, then what the backends reported.
    std::vector<Report> reports;
    const auto due = std::ranges::stable_partition(
        state->scheduled, [&](const Scheduled& each) { return each.due > state->tick; });
    std::ranges::stable_sort(due, {}, &Scheduled::due);
    for (Scheduled& each : due) {
        reports.push_back(std::move(each.report));
    }
    state->scheduled.erase(due.begin(), due.end());
    for (Report& report : holder.reports->take()) {
        reports.push_back(std::move(report));
    }
    for (Report& report : reports) {
        apply(world, *state, report);
    }
}

// Names as the strictest analytics services take them.
bool valid_event_name(std::string_view name) {
    return !name.empty() && name.size() <= kMaxEventName &&
           ((name.front() >= 'a' && name.front() <= 'z') ||
            (name.front() >= 'A' && name.front() <= 'Z')) &&
           std::ranges::all_of(name, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_';
           });
}

Error bad_name(std::string_view what, std::string_view name) {
    return make_error(ErrorCode::InvalidArgument,
                      std::string(what) + " " + quoted(name) +
                          " must start with a letter and hold only letters, digits and '_', at "
                          "most 40 of them");
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Reports

void Reports::add(Report report) {
    const std::scoped_lock lock(mutex_);
    queue_.push_back(std::move(report));
}

void Reports::ad_loaded(std::string_view placement) {
    add(ad_report(Report::Kind::AdLoaded, placement));
}

void Reports::ad_failed(std::string_view placement, std::string_view error) {
    add(ad_report(Report::Kind::AdFailed, placement, error));
}

void Reports::ad_shown(std::string_view placement) {
    add(ad_report(Report::Kind::AdShown, placement));
}

void Reports::ad_rewarded(std::string_view placement, Reward reward) {
    Report report = ad_report(Report::Kind::AdRewarded, placement);
    report.reward = std::move(reward);
    add(std::move(report));
}

void Reports::ad_closed(std::string_view placement) {
    add(ad_report(Report::Kind::AdClosed, placement));
}

void Reports::products(std::vector<Product> products) {
    Report report;
    report.kind = Report::Kind::Products;
    report.products = std::move(products);
    add(std::move(report));
}

void Reports::products_failed(std::string_view error) {
    Report report;
    report.kind = Report::Kind::ProductsFailed;
    report.error = error;
    add(std::move(report));
}

void Reports::purchase(Purchase purchase) {
    add(purchase_report(std::move(purchase)));
}

void Reports::restore_finished(std::string_view error) {
    Report report;
    report.kind = Report::Kind::RestoreFinished;
    report.error = error;
    add(std::move(report));
}

std::vector<Report> Reports::take() {
    const std::scoped_lock lock(mutex_);
    std::vector<Report> out = std::move(queue_);
    queue_.clear();
    return out;
}

// ---------------------------------------------------------------------------------------------
// Names

std::string_view state_name(PurchaseState state) {
    return kStates.at(static_cast<std::size_t>(state));
}

std::string_view status_name(AdStatus status) {
    return kStatuses.at(static_cast<std::size_t>(status));
}

// ---------------------------------------------------------------------------------------------
// Worlds

void install(world::World& world, Settings settings) {
    if (State* state = state_of(world); state != nullptr) {
        if (settings == state->settings) {
            return;
        }
        // A project.json edit that changes the consent written there is the new consent; one
        // that doesn't keeps what the game set.
        if (settings.consent != state->settings.consent) {
            state->consent = settings.consent;
            tell_consent(world, state->consent);
        }
        state->settings = std::move(settings);
        start(world, *state);
        return;
    }
    auto state = std::make_shared<State>();
    state->settings = std::move(settings);
    state->consent = state->settings.consent;
    State& current = *state;
    world.ecs().ctx().emplace<Handle>(std::move(state));
    world.ecs().ctx().emplace<BackendHolder>();
    world.add_reset([](world::World& in_world) {
        State* reset = state_of(in_world);
        if (reset == nullptr) {
            return;
        }
        State fresh;
        fresh.settings = std::move(reset->settings);
        fresh.test = reset->test;
        fresh.test_settings = std::move(reset->test_settings);
        fresh.consent = fresh.settings.consent;
        *reset = std::move(fresh);
        tell_consent(in_world, reset->consent);
        start(in_world, *reset);
    });
    world.add_system(
        "services", [](world::World& in_world, double /*tick_seconds*/) { run(in_world); },
        {.doc = "Hands what the ads, analytics and purchases backends reported to the game as "
                "events.",
         .priority = -800});
    start(world, current);
}

void merge_backends(Backends& into, Backends from, std::string_view source) {
    const auto keep = [&](auto& kept, auto& added, std::string_view service) {
        if (!added) {
            return;
        }
        if (kept) {
            log::error("two backends for one service; the first is kept",
                       {{"service", std::string(service)},
                        {"kept", std::string(kept->name())},
                        {"dropped", std::string(added->name())},
                        {"from", std::string(source)}});
            return;
        }
        kept = std::move(added);
    };
    keep(into.ads, from.ads, "ads");
    keep(into.purchases, from.purchases, "purchases");
    for (auto& analytics : from.analytics) {
        if (analytics) {
            into.analytics.push_back(std::move(analytics));
        }
    }
}

void set_backends(world::World& world, Backends backends) {
    State* state = state_of(world);
    if (state == nullptr) {
        return;
    }
    if (state->test) {
        // What the test store sold is not the real store's.
        state->test = false;
        state->owned.clear();
        state->unfinished.clear();
        state->pending.clear();
        state->store_owned.clear();
    }
    BackendHolder& holder = holder_of(world);
    holder.backends = std::move(backends);
    holder.reports = std::make_shared<Reports>();
    auto begin = [&](Backend& backend) {
        backend.start(holder.reports);
        backend.set_consent(state->consent);
        log::info("services backend started", {{"backend", std::string(backend.name())}});
    };
    if (holder.backends.ads) {
        begin(*holder.backends.ads);
    }
    for (const auto& analytics : holder.backends.analytics) {
        begin(*analytics);
    }
    if (holder.backends.purchases) {
        begin(*holder.backends.purchases);
    }
    if (sends_analytics(world, *state)) {
        send_user(world, *state);
    }
    start(world, *state);
}

void stop(world::World& world) {
    if (state_of(world) == nullptr) {
        return;
    }
    flush_analytics(world);
    BackendHolder& holder = holder_of(world);
    holder.backends = {};
    holder.reports = std::make_shared<Reports>();
}

void use_test_backend(world::World& world, TestSettings settings) {
    State* state = state_of(world);
    if (state == nullptr) {
        return;
    }
    BackendHolder& holder = holder_of(world);
    holder.backends = {};
    holder.reports = std::make_shared<Reports>();
    state->test = true;
    state->test_settings = std::move(settings);
    state->owned.clear();
    state->unfinished.clear();
    state->pending.clear();
    state->store_owned.clear();
    state->next_transaction = 1;
    start(world, *state);
}

bool testing(const world::World& world) {
    const State* state = state_of(world);
    return state != nullptr && state->test;
}

const TestSettings* test_settings(const world::World& world) {
    const State* state = state_of(world);
    return state != nullptr && state->test ? &state->test_settings : nullptr;
}

Result<void> set_test_settings(world::World& world, TestSettings settings) {
    State* state = state_of(world);
    if (state == nullptr || !state->test) {
        return make_error(ErrorCode::InvalidArgument,
                          "the services run on the game's backends, not the test backend");
    }
    const bool repriced = settings.prices != state->test_settings.prices;
    state->test_settings = std::move(settings);
    if (repriced) {
        request_products(world, *state);
    }
    return {};
}

Result<void> complete_pending(world::World& world, PurchaseState outcome) {
    State* state = state_of(world);
    if (state == nullptr || !state->test) {
        return make_error(ErrorCode::InvalidArgument,
                          "the services run on the game's backends, not the test backend");
    }
    for (const Purchase& waiting : state->pending) {
        Purchase done = waiting;
        done.state = outcome;
        if (outcome == PurchaseState::Purchased) {
            if (const Product* product = find_product(*state, waiting.product);
                product != nullptr && is_owned_type(product->type)) {
                state->store_owned.insert(waiting.product);
            }
        } else {
            done.transaction.clear();
            done.receipt.clear();
            if (outcome == PurchaseState::Failed) {
                done.error = state->test_settings.error;
            }
        }
        schedule(world, *state, 1, purchase_report(std::move(done)));
    }
    return {};
}

const Settings* settings(const world::World& world) {
    const State* state = state_of(world);
    return state != nullptr ? &state->settings : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Ads

Result<void> load_ad(world::World& world, std::string_view name) {
    State* state = state_of(world);
    if (state == nullptr) {
        return not_installed();
    }
    const Placement* placement = find_placement(*state, name);
    if (placement == nullptr) {
        return unknown_placement(*state, name);
    }
    const AdStatus status = state->ads[placement->name].status;
    if (status == AdStatus::Showing) {
        return make_error(ErrorCode::InvalidArgument, "the ad shows now");
    }
    if (status == AdStatus::Unloaded || status == AdStatus::Failed) {
        request_load(world, *state, *placement);
    }
    return {};
}

Result<void> show_ad(world::World& world, std::string_view name) {
    State* state = state_of(world);
    if (state == nullptr) {
        return not_installed();
    }
    const Placement* placement = find_placement(*state, name);
    if (placement == nullptr) {
        return unknown_placement(*state, name);
    }
    AdState& ad = state->ads[placement->name];
    switch (ad.status) {
    case AdStatus::Loaded:
        break;
    case AdStatus::Loading:
        return make_error(ErrorCode::InvalidArgument,
                          "the ad is still loading; show it after ad_loaded");
    case AdStatus::Showing:
        return make_error(ErrorCode::InvalidArgument, "the ad shows already");
    case AdStatus::Failed:
        return make_error(ErrorCode::InvalidArgument,
                          "the ad failed to load (" + ad.error + "); load_ad it again");
    case AdStatus::Unloaded:
        return make_error(ErrorCode::InvalidArgument,
                          "no ad is loaded; load_ad it and show it after ad_loaded");
    }
    ad.status = AdStatus::Showing;
    if (state->test) {
        const TestSettings& test = state->test_settings;
        schedule(world, *state, 1, ad_report(Report::Kind::AdShown, placement->name));
        if (placement->format != AdFormat::Banner) {
            const int closes = 1 + std::max(test.show_ticks, 1);
            if (placement->format == AdFormat::Rewarded && test.watch_to_end) {
                schedule(world, *state, closes,
                         ad_report(Report::Kind::AdRewarded, placement->name));
            }
            schedule(world, *state, closes, ad_report(Report::Kind::AdClosed, placement->name));
        }
    } else if (AdsBackend* ads = holder_of(world).backends.ads.get(); ads != nullptr) {
        ads->show(unit_of(*placement));
    }
    return {};
}

Result<void> hide_ad(world::World& world, std::string_view name) {
    State* state = state_of(world);
    if (state == nullptr) {
        return not_installed();
    }
    const Placement* placement = find_placement(*state, name);
    if (placement == nullptr) {
        return unknown_placement(*state, name);
    }
    if (placement->format != AdFormat::Banner) {
        return make_error(ErrorCode::InvalidArgument,
                          "only banners hide; " + std::string(format_name(placement->format)) +
                              " ads close on their own");
    }
    AdState& ad = state->ads[placement->name];
    if (ad.status != AdStatus::Showing) {
        return make_error(ErrorCode::InvalidArgument, "the banner doesn't show");
    }
    ad.status = AdStatus::Loaded;
    if (!state->test) {
        if (AdsBackend* ads = holder_of(world).backends.ads.get(); ads != nullptr) {
            ads->hide(unit_of(*placement));
        }
    }
    return {};
}

AdStatus ad_status(const world::World& world, std::string_view placement) {
    const State* state = state_of(world);
    if (state == nullptr) {
        return AdStatus::Unloaded;
    }
    const auto it = state->ads.find(placement);
    return it != state->ads.end() ? it->second.status : AdStatus::Unloaded;
}

bool is_ad_loaded(const world::World& world, std::string_view placement) {
    return ad_status(world, placement) == AdStatus::Loaded;
}

// ---------------------------------------------------------------------------------------------
// Analytics

Result<void> log_event(world::World& world, std::string_view name, const json::Value& params) {
    State* state = state_of(world);
    if (state == nullptr) {
        return not_installed();
    }
    if (!valid_event_name(name)) {
        return bad_name("the event name", name);
    }
    if (!params.is_null() && !params.is_object()) {
        return make_error(ErrorCode::InvalidArgument, "params must be an object or null");
    }
    if (params.is_object()) {
        if (params.size() > kMaxParams) {
            return make_error(ErrorCode::InvalidArgument, "an event takes at most 25 params");
        }
        for (const auto& [key, value] : params.as_object()) {
            if (!valid_event_name(key)) {
                return bad_name("the param name", key);
            }
            if (value.is_string() ? value.as_string().size() > kMaxParamText
                                  : !value.is_number() && !value.is_bool()) {
                return make_error(ErrorCode::InvalidArgument,
                                  "param " + quoted(key) +
                                      " must be a number, a boolean or a string of at most "
                                      "100 bytes");
            }
        }
    }
    const bool sent = sends_analytics(world, *state);
    if (sent) {
        for (const auto& analytics : holder_of(world).backends.analytics) {
            analytics->log_event(name, params);
        }
    }
    state->logged.push_back({.tick = world.tick(),
                             .name = std::string(name),
                             .params = params.is_object() ? params : json::Value(),
                             .sent = sent});
    if (state->logged.size() > kMaxLogged) {
        state->logged.pop_front();
    }
    ++state->logged_count;
    return {};
}

Result<void> set_user_property(world::World& world, std::string_view name, std::string_view value) {
    State* state = state_of(world);
    if (state == nullptr) {
        return not_installed();
    }
    if (!valid_event_name(name)) {
        return bad_name("the user property name", name);
    }
    if (value.size() > kMaxParamText) {
        return make_error(ErrorCode::InvalidArgument,
                          "a user property's value is at most 100 bytes");
    }
    state->user_properties.insert_or_assign(std::string(name), std::string(value));
    if (sends_analytics(world, *state)) {
        for (const auto& analytics : holder_of(world).backends.analytics) {
            analytics->set_user_property(name, value);
        }
    }
    return {};
}

void set_user_id(world::World& world, std::string_view id) {
    State* state = state_of(world);
    if (state == nullptr) {
        return;
    }
    state->user_id = id;
    if (sends_analytics(world, *state)) {
        for (const auto& analytics : holder_of(world).backends.analytics) {
            analytics->set_user_id(id);
        }
    }
}

void flush_analytics(world::World& world) {
    if (state_of(world) == nullptr) {
        return;
    }
    for (const auto& analytics : holder_of(world).backends.analytics) {
        analytics->flush();
    }
}

// ---------------------------------------------------------------------------------------------
// Purchases

Result<void> buy(world::World& world, std::string_view id) {
    State* state = state_of(world);
    if (state == nullptr) {
        return not_installed();
    }
    Product* product = find_product(*state, id);
    if (product == nullptr) {
        return unknown_product(*state, id);
    }
    switch (state->products_status) {
    case ProductsStatus::None:
    case ProductsStatus::Fetching:
        return make_error(ErrorCode::InvalidArgument,
                          "the store's products are not fetched yet; buy after products_fetched");
    case ProductsStatus::Failed:
        return make_error(ErrorCode::InvalidArgument,
                          "the store is unavailable: " + state->products_error);
    case ProductsStatus::Fetched:
        break;
    }
    if (!product->available) {
        return make_error(ErrorCode::InvalidArgument, "the store doesn't sell " + quoted(id));
    }
    if (state->buying.contains(id) ||
        std::ranges::find(state->pending, id, &Purchase::product) != state->pending.end()) {
        return make_error(ErrorCode::InvalidArgument,
                          "a purchase of " + quoted(id) + " is in progress");
    }
    if (const auto it = std::ranges::find(state->unfinished, id, &Purchase::product);
        it != state->unfinished.end()) {
        return make_error(ErrorCode::InvalidArgument, quoted(id) + " has an unfinished purchase (" +
                                                          it->transaction +
                                                          "); grant it and finish it first");
    }
    if (is_owned_type(product->type) && state->owned.contains(id)) {
        return make_error(ErrorCode::InvalidArgument, "the player owns " + quoted(id) + " already");
    }
    state->buying.insert(product->id);
    if (state->test) {
        const TestSettings& test = state->test_settings;
        Purchase purchase{.product = product->id,
                          .transaction = {},
                          .state = test.outcome,
                          .receipt = {},
                          .error = {}};
        if (test.outcome == PurchaseState::Purchased || test.outcome == PurchaseState::Pending ||
            test.outcome == PurchaseState::Restored) {
            purchase.state = test.outcome == PurchaseState::Pending ? PurchaseState::Pending
                                                                    : PurchaseState::Purchased;
            const std::string number = std::to_string(state->next_transaction++);
            purchase.transaction = "test-" + number;
            purchase.receipt = "test-receipt-" + number;
            if (purchase.state == PurchaseState::Purchased && is_owned_type(product->type)) {
                state->store_owned.insert(product->id);
            }
        } else if (test.outcome == PurchaseState::Failed) {
            purchase.error = test.error;
        }
        schedule(world, *state, test.purchase_ticks, purchase_report(std::move(purchase)));
    } else if (PurchasesBackend* store = holder_of(world).backends.purchases.get();
               store != nullptr) {
        store->purchase(*product);
    }
    return {};
}

Result<void> finish(world::World& world, std::string_view transaction) {
    State* state = state_of(world);
    if (state == nullptr) {
        return not_installed();
    }
    const auto it = std::ranges::find(state->unfinished, transaction, &Purchase::transaction);
    if (transaction.empty() || it == state->unfinished.end()) {
        std::string message = "no unfinished purchase has the transaction " + quoted(transaction);
        if (state->unfinished.empty()) {
            message += "; none is unfinished";
        }
        return make_error(ErrorCode::NotFound, std::move(message));
    }
    const Purchase purchase = *it;
    state->unfinished.erase(it);
    if (!state->test) {
        if (PurchasesBackend* store = holder_of(world).backends.purchases.get(); store != nullptr) {
            const Product* found = find_product(*state, purchase.product);
            Product unknown;
            unknown.id = purchase.product;
            store->finish(purchase, found != nullptr ? *found : unknown);
        }
    }
    return {};
}

Result<void> restore_purchases(world::World& world) {
    State* state = state_of(world);
    if (state == nullptr) {
        return not_installed();
    }
    if (state->restoring) {
        return make_error(ErrorCode::InvalidArgument, "purchases are being restored already");
    }
    state->restoring = true;
    if (state->test) {
        const int ticks = state->test_settings.purchase_ticks;
        for (const std::string& id : state->store_owned) {
            const std::string number = std::to_string(state->next_transaction++);
            schedule(world, *state, ticks,
                     purchase_report({.product = id,
                                      .transaction = "test-" + number,
                                      .state = PurchaseState::Restored,
                                      .receipt = "test-receipt-" + number,
                                      .error = {}}));
        }
        Report finished;
        finished.kind = Report::Kind::RestoreFinished;
        schedule(world, *state, ticks, std::move(finished));
    } else if (PurchasesBackend* store = holder_of(world).backends.purchases.get();
               store != nullptr) {
        store->restore();
    } else {
        Report finished;
        finished.kind = Report::Kind::RestoreFinished;
        finished.error = "no store";
        schedule(world, *state, 1, std::move(finished));
    }
    return {};
}

const Product* product(const world::World& world, std::string_view id) {
    const State* state = state_of(world);
    return state != nullptr ? find_product(*state, id) : nullptr;
}

bool owns(const world::World& world, std::string_view product) {
    const State* state = state_of(world);
    return state != nullptr && state->owned.contains(product);
}

std::vector<Purchase> unfinished(const world::World& world) {
    const State* state = state_of(world);
    return state != nullptr ? state->unfinished : std::vector<Purchase>{};
}

// ---------------------------------------------------------------------------------------------
// Consent

void set_consent(world::World& world, ConsentState consent) {
    State* state = state_of(world);
    if (state == nullptr || consent == state->consent) {
        return;
    }
    const bool was_sending = sends_analytics(world, *state);
    state->consent = consent;
    tell_consent(world, consent);
    // What the game set before the player agreed goes out now.
    if (!was_sending && sends_analytics(world, *state)) {
        send_user(world, *state);
    }
}

ConsentState consent(const world::World& world) {
    const State* state = state_of(world);
    return state != nullptr ? state->consent : ConsentState{};
}

// ---------------------------------------------------------------------------------------------
// Events, state and snapshots

std::span<const ServiceEvent> events(const world::World& world) {
    const State* state = state_of(world);
    return state != nullptr ? std::span<const ServiceEvent>(state->events)
                            : std::span<const ServiceEvent>();
}

namespace {

json::Value purchase_json(const Purchase& purchase) {
    json::Value out;
    out["product"] = purchase.product;
    out["transaction"] = purchase.transaction;
    out["state"] = state_name(purchase.state);
    return out;
}

std::string_view products_status_name(ProductsStatus status) {
    switch (status) {
    case ProductsStatus::None:
        return "none";
    case ProductsStatus::Fetching:
        return "fetching";
    case ProductsStatus::Fetched:
        return "fetched";
    case ProductsStatus::Failed:
        return "failed";
    }
    return "none";
}

} // namespace

json::Value backends_json(const world::World& world) {
    const State* state = state_of(world);
    json::Value backends;
    if (state != nullptr && state->test) {
        backends["ads"] = "test";
        backends["analytics"] = json::Array{"test"};
        backends["purchases"] = "test";
        return backends;
    }
    const BackendHolder* holder = state != nullptr ? &holder_of(world) : nullptr;
    const auto name = [](const auto& backend) {
        return backend ? std::string(backend->name()) : std::string("none");
    };
    json::Array analytics;
    if (holder != nullptr) {
        for (const auto& backend : holder->backends.analytics) {
            analytics.emplace_back(std::string(backend->name()));
        }
    }
    backends["ads"] = holder != nullptr ? name(holder->backends.ads) : "none";
    backends["analytics"] = std::move(analytics);
    backends["purchases"] = holder != nullptr ? name(holder->backends.purchases) : "none";
    return backends;
}

json::Value state_json(const world::World& world, std::size_t analytics_events) {
    const State* state = state_of(world);
    if (state == nullptr) {
        return {};
    }
    json::Value out;
    out["tick"] = state->tick;
    out["backends"] = backends_json(world);
    out["consent"]["analytics"] = consent_name(state->consent.analytics);
    out["consent"]["personalized_ads"] = consent_name(state->consent.personalized_ads);
    json::Value ads = json::Object{};
    for (const Placement& placement : state->settings.placements) {
        json::Value ad;
        ad["format"] = format_name(placement.format);
        const auto found = state->ads.find(placement.name);
        const AdState current = found != state->ads.end() ? found->second : AdState{};
        ad["status"] = status_name(current.status);
        if (!current.error.empty() && current.status == AdStatus::Failed) {
            ad["error"] = current.error;
        }
        ads[placement.name] = std::move(ad);
    }
    out["ads"] = std::move(ads);
    out["products_status"] = products_status_name(state->products_status);
    if (state->products_status == ProductsStatus::Failed) {
        out["products_error"] = state->products_error;
    }
    json::Value products = json::Object{};
    for (const Product& item : state->products) {
        json::Value entry;
        entry["type"] = type_name(item.type);
        entry["available"] = item.available;
        if (item.available) {
            entry["price"] = item.price;
        }
        products[item.id] = std::move(entry);
    }
    out["products"] = std::move(products);
    json::Array owned;
    for (const std::string& id : state->owned) {
        owned.emplace_back(id);
    }
    out["owned"] = std::move(owned);
    json::Array unfinished_list;
    for (const Purchase& purchase : state->unfinished) {
        unfinished_list.push_back(purchase_json(purchase));
    }
    out["unfinished"] = std::move(unfinished_list);
    json::Array pending;
    for (const Purchase& purchase : state->pending) {
        pending.push_back(purchase_json(purchase));
    }
    out["pending"] = std::move(pending);
    json::Value analytics;
    analytics["enabled"] = state->settings.analytics_enabled;
    analytics["logged"] = state->logged_count;
    json::Array logged;
    const std::size_t first =
        state->logged.size() > analytics_events ? state->logged.size() - analytics_events : 0;
    for (std::size_t i = first; i < state->logged.size(); ++i) {
        const Logged& event = state->logged[i];
        json::Value entry;
        entry["tick"] = event.tick;
        entry["name"] = event.name;
        if (!event.params.is_null()) {
            entry["params"] = event.params;
        }
        entry["sent"] = event.sent;
        logged.push_back(std::move(entry));
    }
    analytics["events"] = std::move(logged);
    out["analytics"] = std::move(analytics);
    return out;
}

Snapshot save(const world::World& world) {
    const State* state = state_of(world);
    if (state == nullptr) {
        return {};
    }
    return {std::make_shared<const State>(*state)};
}

void restore(world::World& world, const Snapshot& snapshot) {
    State* state = state_of(world);
    if (state == nullptr || snapshot.data == nullptr) {
        return;
    }
    *state = *static_cast<const State*>(snapshot.data.get());
}

} // namespace slash::services
