// Backends written in Objective-C, for SDKs whose API is Objective-C or Swift: an SDK package's
// glue code (.m or .mm files in sdk.json's "sources") defines a class conforming to one of these
// protocols and names it in "backends", and exports make it with [[Class alloc] init] when the
// game starts. The game calls it on the main thread. format is "banner", "interstitial" or
// "rewarded"; type is "consumable", "non_consumable" or "subscription"; consent answers are
// "unknown", "granted" or "denied". Achievements and leaderboards come by the game's names for
// them and their Game Center IDs ("" if they have none).
#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Where a backend reports what happened, from any thread. The game hears of each report at its
// next tick, in order. Reports made after the game stopped the backend are dropped.
@interface SlashServiceReports : NSObject
// The ad is ready to show.
- (void)adLoaded:(NSString*)placement;
// Loading or showing the ad failed, e.g. with "no fill".
- (void)adFailed:(NSString*)placement error:(NSString*)error;
// The ad is on screen.
- (void)adShown:(NSString*)placement;
// The player watched a rewarded ad far enough to earn its reward; a nil type gives the
// placement's.
- (void)adRewarded:(NSString*)placement type:(nullable NSString*)type amount:(double)amount;
// The ad is gone.
- (void)adClosed:(NSString*)placement;
// What the store says about the products it sells: dictionaries with "id", "title",
// "description", "price" (as the store shows it), "price_micros" (an NSNumber), "currency" (ISO
// 4217) and "available" (an NSNumber boolean, default YES).
- (void)products:(NSArray<NSDictionary<NSString*, id>*>*)products;
// The store is unavailable.
- (void)productsFailed:(NSString*)error;
// A purchase's outcome, or one the store delivers unasked. state is "purchased", "pending",
// "cancelled", "failed" or "restored"; receipt is what the game's server checks with the store,
// such as StoreKit's signed transaction.
- (void)purchase:(NSString*)product
     transaction:(nullable NSString*)transaction
           state:(NSString*)state
         receipt:(nullable NSString*)receipt
           error:(nullable NSString*)error;
// restore is done; error is nil unless it failed.
- (void)restoreFinished:(nullable NSString*)error;
// The player signed in: their ID on the games service and the name they show.
- (void)signedIn:(NSString*)playerID name:(NSString*)name;
// The player isn't signed in.
- (void)signInFailed:(NSString*)error;
// The player's progress after loadAchievements: dictionaries with "achievement" (the game's name
// for it), "steps" (an NSNumber) and "unlocked" (an NSNumber boolean). Those left out have none.
- (void)achievements:(NSArray<NSDictionary<NSString*, id>*>*)progress;
// The platform refused an achievement's progress, or loading it with a nil achievement.
- (void)achievementFailed:(nullable NSString*)achievement error:(NSString*)error;
// The score is on the leaderboard.
- (void)scoreSubmitted:(NSString*)leaderboard score:(int64_t)score;
// The leaderboard refused the score.
- (void)scoreFailed:(NSString*)leaderboard error:(NSString*)error;
// The scores loadScores asked for: entries best first and the signed-in player's, each a
// dictionary with "rank" (an NSNumber, 1 for the best), "player" (their ID), "name" and "score"
// (an NSNumber), and total, the players with a score.
- (void)scores:(NSString*)leaderboard
       entries:(NSArray<NSDictionary<NSString*, id>*>*)entries
        player:(nullable NSDictionary<NSString*, id>*)player
         total:(int64_t)total;
// Loading the scores failed.
- (void)scoresFailed:(NSString*)leaderboard error:(NSString*)error;
// The player closed the achievements or leaderboards screen.
- (void)gamesUIClosed;
@end

@protocol SlashAdsBackend <NSObject>
// A short name for services.state and the log, e.g. "admob".
- (NSString*)name;
// Called once before anything else; keep reports for the SDK's callbacks.
- (void)startWithReports:(SlashServiceReports*)reports;
// Report adLoaded or adFailed.
- (void)load:(NSString*)placement format:(NSString*)format unitID:(NSString*)unitID;
// Shows a loaded ad. Report adShown, then for an interstitial or rewarded ad adRewarded if the
// player earned it and adClosed; or adFailed.
- (void)show:(NSString*)placement format:(NSString*)format unitID:(NSString*)unitID;
// Hides a banner.
- (void)hide:(NSString*)placement format:(NSString*)format unitID:(NSString*)unitID;
@optional
- (void)setConsentAnalytics:(NSString*)analytics personalizedAds:(NSString*)personalizedAds;
// At the start of each tick, for SDKs that poll.
- (void)poll;
@end

@protocol SlashAnalyticsBackend <NSObject>
- (NSString*)name;
// An event with its parameters: strings, NSNumbers and NSNumber booleans by name.
- (void)logEvent:(NSString*)name parameters:(NSDictionary<NSString*, id>*)parameters;
@optional
- (void)startWithReports:(SlashServiceReports*)reports;
- (void)setConsentAnalytics:(NSString*)analytics personalizedAds:(NSString*)personalizedAds;
- (void)setUserProperty:(NSString*)name value:(NSString*)value;
- (void)setUserID:(NSString*)userID;
// Sends what is batched now, as when the game goes to the background.
- (void)flush;
- (void)poll;
@end

@protocol SlashPurchasesBackend <NSObject>
- (NSString*)name;
- (void)startWithReports:(SlashServiceReports*)reports;
// Asks the store about the products, dictionaries with "id" and "type": report products or
// productsFailed, then any purchases the store has not had finished.
- (void)fetchProducts:(NSArray<NSDictionary<NSString*, NSString*>*>*)catalog;
// Report the purchase's outcome.
- (void)purchase:(NSString*)product type:(NSString*)type;
// The game granted the purchase: finish the transaction.
- (void)finish:(NSString*)product
           type:(NSString*)type
    transaction:(NSString*)transaction
        receipt:(NSString*)receipt;
// Report each owned non-consumable and subscription as "restored", then restoreFinished.
- (void)restore;
@optional
- (void)setConsentAnalytics:(NSString*)analytics personalizedAds:(NSString*)personalizedAds;
- (void)poll;
@end

@protocol SlashGamesBackend <NSObject>
// A short name for services.state and the log, e.g. "game_center".
- (NSString*)name;
- (void)startWithReports:(SlashServiceReports*)reports;
// Report signedIn or signInFailed. interactive is NO when the game starts, where Game Center
// signs a returning player in without asking, and YES when the game asks, where it may show its
// sign-in screen.
- (void)signIn:(BOOL)interactive;
// After signedIn: report achievements with the player's progress, or achievementFailed with a
// nil achievement. Dictionaries with "achievement", "id" and "steps" (an NSNumber: an
// incremental achievement's steps, or 0).
- (void)loadAchievements:(NSArray<NSDictionary<NSString*, id>*>*)achievements;
// Report achievementFailed if the platform refuses.
- (void)unlock:(NSString*)achievement achievementID:(NSString*)achievementID;
// An incremental achievement's steps done out of total, as GKAchievement's percentComplete of
// steps / total. Report achievementFailed if the platform refuses.
- (void)setSteps:(int)steps
              of:(int)total
     achievement:(NSString*)achievement
   achievementID:(NSString*)achievementID;
// Report scoreSubmitted or scoreFailed.
- (void)submitScore:(int64_t)score
        leaderboard:(NSString*)leaderboard
      leaderboardID:(NSString*)leaderboardID;
// Report scores or scoresFailed for the best count scores. time is "all_time", "week" or
// "today", and collection "global" or "friends".
- (void)loadScores:(NSString*)leaderboard
     leaderboardID:(NSString*)leaderboardID
              time:(NSString*)time
        collection:(NSString*)collection
             count:(int)count;
// Shows the achievements screen; report gamesUIClosed when the player closes it.
- (void)showAchievements;
// Shows a leaderboard's screen, or every leaderboard's for nil; report gamesUIClosed.
- (void)showLeaderboard:(nullable NSString*)leaderboard
          leaderboardID:(nullable NSString*)leaderboardID;
@optional
- (void)setConsentAnalytics:(NSString*)analytics personalizedAds:(NSString*)personalizedAds;
- (void)poll;
@end

NS_ASSUME_NONNULL_END
