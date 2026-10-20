// Backends written in Objective-C (slash/services/bridges.hpp and apple.h), called through the
// Objective-C runtime. Built with ARC.

#import "slash/services/apple.h"

#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/services/bridges.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace services = slash::services;
namespace json = slash::json;

namespace {

NSString* to_ns(std::string_view text) {
    NSString* out = [[NSString alloc] initWithBytes:text.data()
                                             length:text.size()
                                           encoding:NSUTF8StringEncoding];
    return out != nil ? out : @"";
}

std::string to_std(NSString* _Nullable text) {
    if (text == nil) {
        return {};
    }
    const char* chars = [text UTF8String];
    return chars != nullptr ? std::string(chars) : std::string();
}

std::string text_field(NSDictionary* dictionary, NSString* key) {
    id value = dictionary[key];
    if (![value isKindOfClass:[NSString class]]) {
        return {};
    }
    NSString* string = value;
    return to_std(string);
}

// A number in a dictionary, or 0.
int64_t number_field(NSDictionary* dictionary, NSString* key) {
    id value = dictionary[key];
    if (![value isKindOfClass:[NSNumber class]]) {
        return 0;
    }
    NSNumber* number = value;
    return [number longLongValue];
}

services::Score score_of(NSDictionary* dictionary) {
    return {.rank = number_field(dictionary, @"rank"),
            .player = text_field(dictionary, @"player"),
            .name = text_field(dictionary, @"name"),
            .value = number_field(dictionary, @"score")};
}

// A parameter object as Foundation values: strings, numbers and booleans by name.
NSDictionary<NSString*, id>* to_dictionary(const json::Value& params) {
    NSMutableDictionary<NSString*, id>* out = [NSMutableDictionary dictionary];
    if (!params.is_object()) {
        return out;
    }
    for (const auto& [key, value] : params.as_object()) {
        id converted = nil;
        if (value.is_string()) {
            converted = to_ns(value.as_string());
        } else if (value.is_bool()) {
            converted = [NSNumber numberWithBool:value.as_bool()];
        } else if (value.is_int()) {
            converted = [NSNumber numberWithLongLong:value.as_int()];
        } else if (value.is_number()) {
            converted = [NSNumber numberWithDouble:value.as_double()];
        }
        if (converted != nil) {
            out[to_ns(key)] = converted;
        }
    }
    return out;
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
    if (name == "restored") {
        return services::PurchaseState::Restored;
    }
    return services::PurchaseState::Purchased;
}

NSString* consent_text(services::Consent consent) {
    return to_ns(services::consent_name(consent));
}

} // namespace

@interface SlashServiceReports ()
- (instancetype)initWithReports:(const std::shared_ptr<services::Reports>&)reports;
@end

@implementation SlashServiceReports {
    // Weak, so a report made after the backend is gone is dropped.
    std::weak_ptr<services::Reports> _reports;
}

- (instancetype)initWithReports:(const std::shared_ptr<services::Reports>&)reports {
    self = [super init];
    if (self != nil) {
        _reports = reports;
    }
    return self;
}

- (void)adLoaded:(NSString*)placement {
    if (const auto reports = _reports.lock()) {
        reports->ad_loaded(to_std(placement));
    }
}

- (void)adFailed:(NSString*)placement error:(NSString*)error {
    if (const auto reports = _reports.lock()) {
        reports->ad_failed(to_std(placement), to_std(error));
    }
}

- (void)adShown:(NSString*)placement {
    if (const auto reports = _reports.lock()) {
        reports->ad_shown(to_std(placement));
    }
}

- (void)adRewarded:(NSString*)placement type:(NSString*)type amount:(double)amount {
    if (const auto reports = _reports.lock()) {
        reports->ad_rewarded(to_std(placement), {.type = to_std(type), .amount = amount});
    }
}

- (void)adClosed:(NSString*)placement {
    if (const auto reports = _reports.lock()) {
        reports->ad_closed(to_std(placement));
    }
}

- (void)products:(NSArray<NSDictionary<NSString*, id>*>*)products {
    const auto reports = _reports.lock();
    if (reports == nullptr) {
        return;
    }
    std::vector<services::Product> out;
    for (id item in products) {
        if (![item isKindOfClass:[NSDictionary class]]) {
            continue;
        }
        NSDictionary* dictionary = item;
        services::Product product;
        product.id = text_field(dictionary, @"id");
        product.title = text_field(dictionary, @"title");
        product.description = text_field(dictionary, @"description");
        product.price = text_field(dictionary, @"price");
        product.currency = text_field(dictionary, @"currency");
        id micros = dictionary[@"price_micros"];
        if ([micros isKindOfClass:[NSNumber class]]) {
            NSNumber* number = micros;
            product.price_micros = [number longLongValue];
        }
        id available = dictionary[@"available"];
        product.available = true;
        if ([available isKindOfClass:[NSNumber class]]) {
            NSNumber* flag = available;
            product.available = [flag boolValue];
        }
        out.push_back(std::move(product));
    }
    reports->products(std::move(out));
}

- (void)productsFailed:(NSString*)error {
    if (const auto reports = _reports.lock()) {
        reports->products_failed(to_std(error));
    }
}

- (void)purchase:(NSString*)product
     transaction:(NSString*)transaction
           state:(NSString*)state
         receipt:(NSString*)receipt
           error:(NSString*)error {
    if (const auto reports = _reports.lock()) {
        reports->purchase({.product = to_std(product),
                           .transaction = to_std(transaction),
                           .state = parse_state(to_std(state)),
                           .receipt = to_std(receipt),
                           .error = to_std(error)});
    }
}

- (void)restoreFinished:(NSString*)error {
    if (const auto reports = _reports.lock()) {
        reports->restore_finished(to_std(error));
    }
}

- (void)signedIn:(NSString*)playerID name:(NSString*)name {
    if (const auto reports = _reports.lock()) {
        reports->signed_in({.id = to_std(playerID), .name = to_std(name)});
    }
}

- (void)signInFailed:(NSString*)error {
    if (const auto reports = _reports.lock()) {
        reports->sign_in_failed(to_std(error));
    }
}

- (void)achievements:(NSArray<NSDictionary<NSString*, id>*>*)progress {
    const auto reports = _reports.lock();
    if (reports == nullptr) {
        return;
    }
    std::vector<services::AchievementProgress> out;
    for (id item in progress) {
        if (![item isKindOfClass:[NSDictionary class]]) {
            continue;
        }
        NSDictionary* dictionary = item;
        out.push_back({.achievement = text_field(dictionary, @"achievement"),
                       .steps = static_cast<int>(number_field(dictionary, @"steps")),
                       .unlocked = number_field(dictionary, @"unlocked") != 0});
    }
    reports->achievements(std::move(out));
}

- (void)achievementFailed:(NSString*)achievement error:(NSString*)error {
    if (const auto reports = _reports.lock()) {
        reports->achievement_failed(to_std(achievement), to_std(error));
    }
}

- (void)scoreSubmitted:(NSString*)leaderboard score:(int64_t)score {
    if (const auto reports = _reports.lock()) {
        reports->score_submitted(to_std(leaderboard), score);
    }
}

- (void)scoreFailed:(NSString*)leaderboard error:(NSString*)error {
    if (const auto reports = _reports.lock()) {
        reports->score_failed(to_std(leaderboard), to_std(error));
    }
}

- (void)scores:(NSString*)leaderboard
       entries:(NSArray<NSDictionary<NSString*, id>*>*)entries
        player:(NSDictionary<NSString*, id>*)player
         total:(int64_t)total {
    const auto reports = _reports.lock();
    if (reports == nullptr) {
        return;
    }
    services::Scores scores;
    for (id item in entries) {
        if ([item isKindOfClass:[NSDictionary class]]) {
            scores.entries.push_back(score_of(item));
        }
    }
    if (player != nil) {
        scores.player = score_of(player);
    }
    scores.total = total;
    reports->scores(to_std(leaderboard), std::move(scores));
}

- (void)scoresFailed:(NSString*)leaderboard error:(NSString*)error {
    if (const auto reports = _reports.lock()) {
        reports->scores_failed(to_std(leaderboard), to_std(error));
    }
}

- (void)gamesUIClosed {
    if (const auto reports = _reports.lock()) {
        reports->games_ui_closed();
    }
}

@end

namespace {

// An object of the class a backend names, made at start, and its name.
template <typename Object>
class ObjcObject {
public:
    ObjcObject(std::string_view class_name, Protocol* protocol)
        : class_name_(class_name), name_(class_name), protocol_(protocol) {}

    // Makes the object; false if the class isn't linked or doesn't conform.
    bool make() {
        Class found = NSClassFromString(to_ns(class_name_));
        if (found == nil) {
            slash::log::error("an Objective-C services backend's class is not linked",
                              {{"class", class_name_}});
            return false;
        }
        if (![found conformsToProtocol:protocol_]) {
            slash::log::error(
                "an Objective-C services backend's class does not conform to its "
                "protocol",
                {{"class", class_name_}, {"protocol", to_std(NSStringFromProtocol(protocol_))}});
            return false;
        }
        object_ = [[found alloc] init];
        if (object_ == nil) {
            return false;
        }
        if (std::string name = to_std([object_ name]); !name.empty()) {
            name_ = std::move(name);
        }
        return true;
    }

    [[nodiscard]] Object get() const { return object_; }
    [[nodiscard]] std::string_view name() const { return name_; }

    void set_consent(const services::ConsentState& consent) {
        if ([object_ respondsToSelector:@selector(setConsentAnalytics:personalizedAds:)]) {
            [object_ setConsentAnalytics:consent_text(consent.analytics)
                         personalizedAds:consent_text(consent.personalized_ads)];
        }
    }

    void poll() {
        if ([object_ respondsToSelector:@selector(poll)]) {
            [object_ poll];
        }
    }

private:
    std::string class_name_;
    std::string name_;
    Protocol* protocol_;
    Object object_ = nil;
};

SlashServiceReports* reports_object(const std::shared_ptr<services::Reports>& reports) {
    return [[SlashServiceReports alloc] initWithReports:reports];
}

class ObjcAds final : public services::AdsBackend {
public:
    explicit ObjcAds(std::string_view class_name)
        : object_(class_name, @protocol(SlashAdsBackend)) {}
    [[nodiscard]] std::string_view name() const override { return object_.name(); }
    void start(const std::shared_ptr<services::Reports>& reports) override {
        reports_ = reports;
        if (object_.make()) {
            [object_.get() startWithReports:reports_object(reports)];
        }
    }
    void set_consent(const services::ConsentState& consent) override {
        if (object_.get() != nil) {
            object_.set_consent(consent);
        }
    }
    void poll() override {
        if (object_.get() != nil) {
            object_.poll();
        }
    }
    void load(const services::AdUnit& unit) override {
        if (object_.get() == nil) {
            fail(unit);
            return;
        }
        [object_.get() load:to_ns(unit.placement)
                     format:to_ns(services::format_name(unit.format))
                     unitID:to_ns(unit.id)];
    }
    void show(const services::AdUnit& unit) override {
        if (object_.get() == nil) {
            fail(unit);
            return;
        }
        [object_.get() show:to_ns(unit.placement)
                     format:to_ns(services::format_name(unit.format))
                     unitID:to_ns(unit.id)];
    }
    void hide(const services::AdUnit& unit) override {
        if (object_.get() != nil) {
            [object_.get() hide:to_ns(unit.placement)
                         format:to_ns(services::format_name(unit.format))
                         unitID:to_ns(unit.id)];
        }
    }

private:
    void fail(const services::AdUnit& unit) {
        if (const auto reports = reports_.lock()) {
            reports->ad_failed(unit.placement, "the ads Objective-C backend could not start");
        }
    }

    ObjcObject<id<SlashAdsBackend>> object_;
    std::weak_ptr<services::Reports> reports_;
};

class ObjcAnalytics final : public services::AnalyticsBackend {
public:
    explicit ObjcAnalytics(std::string_view class_name)
        : object_(class_name, @protocol(SlashAnalyticsBackend)) {}
    [[nodiscard]] std::string_view name() const override { return object_.name(); }
    void start(const std::shared_ptr<services::Reports>& reports) override {
        if (object_.make() && [object_.get() respondsToSelector:@selector(startWithReports:)]) {
            [object_.get() startWithReports:reports_object(reports)];
        }
    }
    void set_consent(const services::ConsentState& consent) override {
        if (object_.get() != nil) {
            object_.set_consent(consent);
        }
    }
    void poll() override {
        if (object_.get() != nil) {
            object_.poll();
        }
    }
    void log_event(std::string_view event, const json::Value& params) override {
        if (object_.get() != nil) {
            [object_.get() logEvent:to_ns(event) parameters:to_dictionary(params)];
        }
    }
    void set_user_property(std::string_view property, std::string_view value) override {
        if (object_.get() != nil &&
            [object_.get() respondsToSelector:@selector(setUserProperty:value:)]) {
            [object_.get() setUserProperty:to_ns(property) value:to_ns(value)];
        }
    }
    // Not "id", which Objective-C++ reads as a type.
    void set_user_id(std::string_view user) override {
        if (object_.get() != nil && [object_.get() respondsToSelector:@selector(setUserID:)]) {
            [object_.get() setUserID:to_ns(user)];
        }
    }
    void flush() override {
        if (object_.get() != nil && [object_.get() respondsToSelector:@selector(flush)]) {
            [object_.get() flush];
        }
    }

private:
    ObjcObject<id<SlashAnalyticsBackend>> object_;
};

class ObjcPurchases final : public services::PurchasesBackend {
public:
    explicit ObjcPurchases(std::string_view class_name)
        : object_(class_name, @protocol(SlashPurchasesBackend)) {}
    [[nodiscard]] std::string_view name() const override { return object_.name(); }
    void start(const std::shared_ptr<services::Reports>& reports) override {
        reports_ = reports;
        if (object_.make()) {
            [object_.get() startWithReports:reports_object(reports)];
        }
    }
    void set_consent(const services::ConsentState& consent) override {
        if (object_.get() != nil) {
            object_.set_consent(consent);
        }
    }
    void poll() override {
        if (object_.get() != nil) {
            object_.poll();
        }
    }
    void fetch_products(std::span<const services::Product> catalog) override {
        if (object_.get() == nil) {
            if (const auto reports = reports_.lock()) {
                reports->products_failed("the store's Objective-C backend could not start");
            }
            return;
        }
        NSMutableArray<NSDictionary<NSString*, NSString*>*>* products = [NSMutableArray array];
        for (const services::Product& product : catalog) {
            [products addObject:@{
                @"id" : to_ns(product.id),
                @"type" : to_ns(services::type_name(product.type))
            }];
        }
        [object_.get() fetchProducts:products];
    }
    void purchase(const services::Product& product) override {
        if (object_.get() != nil) {
            [object_.get() purchase:to_ns(product.id)
                               type:to_ns(services::type_name(product.type))];
        }
    }
    void finish(const services::Purchase& purchase, const services::Product& product) override {
        if (object_.get() != nil) {
            [object_.get() finish:to_ns(purchase.product)
                             type:to_ns(services::type_name(product.type))
                      transaction:to_ns(purchase.transaction)
                          receipt:to_ns(purchase.receipt)];
        }
    }
    void restore() override {
        if (object_.get() != nil) {
            [object_.get() restore];
        } else if (const auto reports = reports_.lock()) {
            reports->restore_finished("the store's Objective-C backend could not start");
        }
    }

private:
    ObjcObject<id<SlashPurchasesBackend>> object_;
    std::weak_ptr<services::Reports> reports_;
};

class ObjcGames final : public services::GamesBackend {
public:
    explicit ObjcGames(std::string_view class_name)
        : object_(class_name, @protocol(SlashGamesBackend)) {}
    [[nodiscard]] std::string_view name() const override { return object_.name(); }
    void start(const std::shared_ptr<services::Reports>& reports) override {
        reports_ = reports;
        if (object_.make()) {
            [object_.get() startWithReports:reports_object(reports)];
        }
    }
    void set_consent(const services::ConsentState& consent) override {
        if (object_.get() != nil) {
            object_.set_consent(consent);
        }
    }
    void poll() override {
        if (object_.get() != nil) {
            object_.poll();
        }
    }
    void sign_in(bool interactive) override {
        if (object_.get() != nil) {
            [object_.get() signIn:static_cast<BOOL>(interactive)];
        } else if (const auto reports = reports_.lock()) {
            reports->sign_in_failed("the games Objective-C backend could not start");
        }
    }
    void load_achievements(std::span<const services::AchievementUnit> units) override {
        if (object_.get() == nil) {
            return;
        }
        NSMutableArray<NSDictionary<NSString*, id>*>* achievements = [NSMutableArray array];
        for (const services::AchievementUnit& unit : units) {
            [achievements addObject:@{
                @"achievement" : to_ns(unit.achievement),
                @"id" : to_ns(unit.id),
                @"steps" : [NSNumber numberWithLongLong:unit.steps]
            }];
        }
        [object_.get() loadAchievements:achievements];
    }
    void unlock(const services::AchievementUnit& unit) override {
        if (object_.get() != nil) {
            [object_.get() unlock:to_ns(unit.achievement) achievementID:to_ns(unit.id)];
        }
    }
    void set_steps(const services::AchievementUnit& unit, int steps) override {
        if (object_.get() != nil) {
            [object_.get() setSteps:steps
                                 of:unit.steps
                        achievement:to_ns(unit.achievement)
                      achievementID:to_ns(unit.id)];
        }
    }
    void submit_score(const services::LeaderboardUnit& unit, std::int64_t score) override {
        if (object_.get() != nil) {
            [object_.get() submitScore:score
                           leaderboard:to_ns(unit.leaderboard)
                         leaderboardID:to_ns(unit.id)];
        }
    }
    void load_scores(const services::LeaderboardUnit& unit,
                     const services::ScoreQuery& query) override {
        if (object_.get() == nil) {
            if (const auto reports = reports_.lock()) {
                reports->scores_failed(unit.leaderboard,
                                       "the games Objective-C backend could not start");
            }
            return;
        }
        [object_.get() loadScores:to_ns(unit.leaderboard)
                    leaderboardID:to_ns(unit.id)
                             time:to_ns(services::span_name(query.time))
                       collection:to_ns(services::collection_name(query.collection))
                            count:query.count];
    }
    void show_achievements() override {
        if (object_.get() != nil) {
            [object_.get() showAchievements];
        } else if (const auto reports = reports_.lock()) {
            reports->games_ui_closed();
        }
    }
    void show_leaderboards(const services::LeaderboardUnit* unit) override {
        if (object_.get() == nil) {
            if (const auto reports = reports_.lock()) {
                reports->games_ui_closed();
            }
        } else if (unit != nullptr) {
            [object_.get() showLeaderboard:to_ns(unit->leaderboard)
                             leaderboardID:to_ns(unit->id)];
        } else {
            [object_.get() showLeaderboard:nil leaderboardID:nil];
        }
    }

private:
    ObjcObject<id<SlashGamesBackend>> object_;
    std::weak_ptr<services::Reports> reports_;
};

} // namespace

namespace slash::services {

std::unique_ptr<AdsBackend> objc_ads_backend(std::string_view class_name) {
    return std::make_unique<ObjcAds>(class_name);
}

std::unique_ptr<AnalyticsBackend> objc_analytics_backend(std::string_view class_name) {
    return std::make_unique<ObjcAnalytics>(class_name);
}

std::unique_ptr<PurchasesBackend> objc_purchases_backend(std::string_view class_name) {
    return std::make_unique<ObjcPurchases>(class_name);
}

std::unique_ptr<GamesBackend> objc_games_backend(std::string_view class_name) {
    return std::make_unique<ObjcGames>(class_name);
}

} // namespace slash::services
