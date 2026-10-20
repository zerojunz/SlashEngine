#pragma once

#include "slash/services/services.hpp"

#include <memory>
#include <string_view>

// Backends written in the platform's own language, for SDKs that only have a Java or
// Objective-C API. Each wraps an object of a class the game ships, made when the backend starts
// and called through JNI or the Objective-C runtime; what the object reports reaches the
// backend's Reports from any thread. SDK packages name such classes in sdk.json's "backends",
// and exports register them; the interfaces the classes implement are in
// engine/services/android (org.slashengine.services) and slash/services/apple.h.
namespace slash::services {

#ifdef __ANDROID__
// A class implementing org.slashengine.services.AdsBackend, AnalyticsBackend, PurchasesBackend
// or GamesBackend, by its Java name: "com.example.hello.HelloAds". It needs a public
// constructor with no parameters; a class that can't be made logs an error and makes the
// backend do nothing.
[[nodiscard]] std::unique_ptr<AdsBackend> java_ads_backend(std::string_view class_name);
[[nodiscard]] std::unique_ptr<AnalyticsBackend> java_analytics_backend(std::string_view class_name);
[[nodiscard]] std::unique_ptr<PurchasesBackend> java_purchases_backend(std::string_view class_name);
[[nodiscard]] std::unique_ptr<GamesBackend> java_games_backend(std::string_view class_name);
#endif

#ifdef __APPLE__
// A class conforming to SlashAdsBackend, SlashAnalyticsBackend, SlashPurchasesBackend or
// SlashGamesBackend, by its Objective-C name: "HelloAds". It is made with [[Class alloc] init];
// a class that isn't linked logs an error and makes the backend do nothing.
[[nodiscard]] std::unique_ptr<AdsBackend> objc_ads_backend(std::string_view class_name);
[[nodiscard]] std::unique_ptr<AnalyticsBackend> objc_analytics_backend(std::string_view class_name);
[[nodiscard]] std::unique_ptr<PurchasesBackend> objc_purchases_backend(std::string_view class_name);
[[nodiscard]] std::unique_ptr<GamesBackend> objc_games_backend(std::string_view class_name);
#endif

} // namespace slash::services
