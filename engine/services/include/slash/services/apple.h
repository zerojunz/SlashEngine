// Backends written in Objective-C, for SDKs whose API is Objective-C or Swift: an SDK package's
// glue code (.m or .mm files in sdk.json's "sources") defines a class conforming to one of these
// protocols and names it in "backends", and exports make it with [[Class alloc] init] when the
// game starts. The game calls it on the main thread. format is "banner", "interstitial" or
// "rewarded"; type is "consumable", "non_consumable" or "subscription"; consent answers are
// "unknown", "granted" or "denied".
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

NS_ASSUME_NONNULL_END
