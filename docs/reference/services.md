# Services: ads, analytics, purchases, achievements and leaderboards

A game shows ads, logs analytics events, sells products, signs the player in, unlocks
achievements and submits scores through one C++ API, `slash/services/services.hpp`, as Unity's
Ads, Analytics, In-App Purchasing and Social API. The game names its ad placements, products,
achievements and leaderboards in [`project.json`](project-format.md#services) and asks for them
by name; what happens comes back as events at a later tick, as input does. Backends do the work:

| Backend | Runs | Used by |
| --- | --- | --- |
| Test | Ads load, purchases end and the player signs in as [`services.test`](commands.md#servicestest) says, with no account. Deterministic, so runs and replays repeat | Sessions: whatever `slash-cli` and `slash-mcp` run, `test.run` included |
| An SDK package's | Glue code over a vendor's SDK (an ad network, an analytics service) or the platform's store or games service implements `AdsBackend`, `AnalyticsBackend`, `PurchasesBackend` or `GamesBackend` | Exported games whose [SDK packages](sdk-format.md) register one |
| None | Ads fail to load with `no ads backend`, the store is unavailable (`no store`), sign-in fails with `no games backend`, and analytics events, achievement progress and scores are only kept | Exported games with no backend for that service |

## Ads

```cpp
#include "slash/services/services.hpp"
namespace services = slash::services;

// An ad that auto-loads is loaded at start and after each time it closes.
if (services::is_ad_loaded(world, "bonus")) {
    (void)services::show_ad(world, "bonus");
}
for (const services::ServiceEvent& event : services::events(world)) {
    if (event.kind == "ad_rewarded") {
        coins += static_cast<int>(event.reward.amount);
    }
}
```

A placement is `banner`, `interstitial` or `rewarded`. Its status is `unloaded`, `loading`,
`loaded`, `showing` or `failed`. `load_ad` starts loading (nothing happens if it is loading or
loaded already), `show_ad` shows a loaded ad and fails with a reason otherwise, and `hide_ad`
hides a banner. Interstitial and rewarded ads close on their own; then they must load again,
which placements with `auto_load` (the default) do by themselves. A banner shows until hidden.

Events, each also a world event with the placement as its ID:

| Event | Data | When |
| --- | --- | --- |
| `ad_loaded` | | The ad is ready to show |
| `ad_failed` | `error` | Loading or showing failed, e.g. `no fill` |
| `ad_shown` | | The ad is on screen |
| `ad_rewarded` | `type`, `amount` | The player watched a rewarded ad far enough to earn its reward. The network's reward, or the placement's `reward` when it gives none |
| `ad_closed` | | The ad is gone. A skipped rewarded ad closes with no `ad_rewarded` |

`personalized_ads` consent is passed to the ads backend, which shows non-personalized ads unless
it is `granted`.

## Analytics

```cpp
json::Value params;
params["level"] = 3;
params["won"] = true;
(void)services::log_event(world, "level_end", params);
(void)services::set_user_property(world, "class", "mage");
services::set_user_id(world, "player-7");
```

Event, param and user property names start with a letter and hold only letters, digits and
`_`, at most 40 of them. An event has at most 25 params, each a number, a boolean or a string of
at most 100 bytes. These are the strictest of the common analytics services' rules, so an event
that passes them is one any backend takes; one that breaks them is an error.

Every event is kept (the last 256) for [`services.state`](commands.md#servicesstate), which
says whether each was sent. Events go to the analytics backends only while
`services.analytics.enabled` is on and the player's `analytics` consent is `granted`. The user ID
and properties set before the player agrees are sent when they do. `flush_analytics` sends what
a backend has batched, as when the game goes to the background.

## Purchases

```cpp
(void)services::buy(world, "coins_100");
for (const services::ServiceEvent& event : services::events(world)) {
    if (event.kind == "purchase_completed") {
        grant(event.id);                                      // give the player what they paid for
        (void)services::finish(world, event.purchase.transaction); // then tell the store
    }
}
```

Products are `consumable` (bought again and again, such as coins), `non_consumable` (owned for
good, such as removing ads) or `subscription`. The store's details (`title`, `description`, the
localized `price`, `price_micros` and `currency`) arrive at start with `products_fetched`, and
`product(world, id)` returns them. `buy` starts a purchase and fails with a reason if it can't:
the products aren't fetched yet, the store doesn't sell it, a purchase of it is in progress or
pending, it has an unfinished purchase, or the player owns it already.

A completed purchase is unfinished until the game grants it and calls `finish` with its
transaction, as Unity IAP's `ConfirmPendingPurchase`. Stores require this: Google Play refunds a
purchase that isn't acknowledged within three days, and a consumable can't be bought again until
it is consumed. Unfinished purchases are delivered again at each start, so a game that quits
between paying and granting grants at the next start. `restore_purchases` delivers each owned
non-consumable and subscription again with the state `restored`, then `restore_finished`, as
an iOS game's Restore Purchases button needs. `owns(world, id)` says whether the player owns a
non-consumable or subscription.

| Event | Data | When |
| --- | --- | --- |
| `products_fetched` | `available`: product IDs | The store's details arrived |
| `products_failed` | `error` | The store is unavailable |
| `purchase_completed` | `transaction`, `state` (`purchased` or `restored`), `receipt` | Grant it, then `finish` it. `receipt` is what the game's server checks with the store |
| `purchase_pending` | `transaction` | Waiting on the player or a parent (Ask to Buy, cash payments). It completes or fails later |
| `purchase_cancelled` | | The player cancelled it |
| `purchase_failed` | `error` | The store refused it |
| `restore_finished` | `error` if it failed | `restore_purchases` is done |

## Achievements and leaderboards

```cpp
// At the end of a race:
(void)services::submit_score(world, "best_lap", lap_ms);
(void)services::unlock_achievement(world, "first_finish");
(void)services::increment_achievement(world, "ten_laps");
for (const services::ServiceEvent& event : services::events(world)) {
    if (event.kind == "achievement_unlocked") {
        show_toast(event.id);
    }
}
```

The player is signed in when the services start, as Google Play Games and Game Center sign a
returning player in when a game launches; `services.games.sign_in_at_start` turns that off for a
game that signs in from its own button, and `sign_in` asks (showing the platform's sign-in screen
if it needs to). A game with no achievements, no leaderboards and no games backend never signs
in. `sign_in_status` is `signed_out`, `signing_in`, `signed_in` or `failed`, and `player` returns
the signed-in player's ID and name.

An achievement unlocks at once (`unlock_achievement`) or, with `steps`, in steps
(`increment_achievement` adds them and unlocks it when they are all done), as Play Games'
incremental achievements; Game Center gets the progress as a percentage. `achievement_unlocked`
arrives once, at the tick after the unlock, and unlocking one already unlocked does nothing.
`submit_score` submits a score, and `best_score` returns the best the game submitted since the
services started, by the leaderboard's `sort_order` (`high_to_low`, or `low_to_high` for times).

Progress and scores made before the player signs in are kept and sent when they do: after
`signed_in`, the player's progress on the platform arrives with `achievements_loaded`, merged with
the game's (an achievement unlocked on another device stays unlocked, and progress the platform
lacks is sent to it), and the best score made while signed out is submitted.

`load_scores` loads a leaderboard's best `count` scores (1 to 25) over `all_time`, a `week` or
`today`, of every player (`global`) or the player's `friends`, and `show_achievements` and
`show_leaderboards` show the platform's screens over the game; all three need the player signed
in.

| Event | ID | Data | When |
| --- | --- | --- | --- |
| `signed_in` | The player's ID | `name` | The player signed in |
| `sign_in_failed` | | `error` | They didn't, or there is no games backend |
| `achievements_loaded` | | `unlocked`: names | Their progress arrived after signing in |
| `achievement_unlocked` | The achievement | | It unlocked now |
| `achievement_failed` | The achievement | `error` | The platform refused its progress |
| `score_submitted` | The leaderboard | `score` | The score is on the leaderboard |
| `score_failed` | The leaderboard | `error` | The leaderboard refused it |
| `scores_loaded` | The leaderboard | `time`, `collection`, `entries` (each `rank`, `player`, `name`, `score`), `player` (the player's own entry, if any), `total` | `load_scores` is done |
| `scores_failed` | The leaderboard | `error` | Loading them failed |
| `games_ui_closed` | | | The player closed the achievements or leaderboards screen |

## Consent

`ConsentState` holds two answers, each `unknown`, `granted` or `denied`: `analytics` gates
sending events, and `personalized_ads` tells ad backends whether they may personalize ads.
Both start as `project.json`'s `services.consent` (`unknown` unless set). The game asks the
player, keeps the answer in its save data and calls `set_consent` at each start; backends get
it at once.

## The test backend

Sessions run every service on the test backend. It loads ads after `load_ticks` (1), or
fails them with `no fill`; shows interstitial and rewarded ads for `show_ticks` (1) and rewards
rewarded ones if `watch_to_end`; ends purchases after `purchase_ticks` (1) as `outcome` says
(`purchased`, `pending`, `cancelled` or `failed` with `error`); prices products at $0.99 unless
`prices` says otherwise; and gives transactions the IDs `test-1`, `test-2` and so on. Its games
service signs the player in after `games_ticks` (1) as `test-player`, named `player` ("Player"),
unless `accept_sign_in` is off; keeps their progress and best scores; ranks each leaderboard's
scores among the other players' `scores` gives it (`friends` holds only the player); and shows
its screens for `show_ticks`.
`complete_pending` ends pending purchases, as a parent approving or declining them. Its state is
part of the world's, so a failed command or a dry run leaves it as it was, and resetting the world
(as replays do) starts a fresh test store.

Exported games never use the test backend: it would hand out purchases for free.

## Writing a backend

A backend is a class an SDK package's [glue code](sdk-format.md#glue-code) adds, over the
vendor's SDK. Exports call the package's `register` function when the game starts, and C++ code
can set backends itself:

```cpp
class MyAnalytics : public services::AnalyticsBackend {
public:
    std::string_view name() const override { return "my_analytics"; }
    void log_event(std::string_view name, const json::Value& params) override {
        // Call the vendor's SDK.
    }
};

services::Backends backends;
backends.analytics.push_back(std::make_unique<MyAnalytics>());
services::set_backends(world, std::move(backends));
```

`start` hands a backend a `Reports` object, which it keeps and calls from any thread when the
SDK calls back: `ad_loaded`, `ad_failed`, `ad_shown`, `ad_rewarded`, `ad_closed`, `products`,
`products_failed`, `purchase`, `restore_finished`, `signed_in`, `sign_in_failed`,
`achievements`, `achievement_failed`, `score_submitted`, `score_failed`, `scores`,
`scores_failed` and `games_ui_closed`. The `services` system (priority -800, after
input and the GUI) takes the reports at the start of the next tick and turns them into events,
so the game sees them on its own thread, in order. `poll` runs at the start of each tick on the
game's thread, for SDKs that must be polled.

| Interface | Calls | Reports |
| --- | --- | --- |
| `AdsBackend` | `load`, `show` and `hide` an `AdUnit`: the placement, its format and its ad unit ID on this platform from the placement's `ids` | `ad_loaded` or `ad_failed`; `ad_shown`, then `ad_rewarded` if earned and `ad_closed` |
| `AnalyticsBackend` | `log_event`, `set_user_property`, `set_user_id`, `flush` | Nothing |
| `PurchasesBackend` | `fetch_products` with the catalog, `purchase`, `finish` (acknowledge, or consume a consumable), `restore` | `products` or `products_failed`, then any unfinished purchases; each purchase's outcome; restored purchases, then `restore_finished` |
| `GamesBackend` | `sign_in` (`interactive` is false at start), `load_achievements`, `unlock` and `set_steps` an `AchievementUnit` (its name, its ID on this platform and its steps), `submit_score` and `load_scores` on a `LeaderboardUnit`, `show_achievements`, `show_leaderboards` | `signed_in` or `sign_in_failed`; the player's progress with `achievements`; `achievement_failed` if the platform refuses progress; `score_submitted` or `score_failed`; `scores` or `scores_failed`; `games_ui_closed` |

Every backend also gets `set_consent` after `start` and whenever consent changes. Analytics
backends get `flush` when the game goes to the background (a phone may stop it there) and when
it quits, before the game lets go of its backends.

SDKs with only a Java or Objective-C API get backends in that language: a Java class
implementing `org.slashengine.services.AdsBackend`, `AnalyticsBackend`, `PurchasesBackend` or
`GamesBackend`, or an Objective-C class conforming to `SlashAdsBackend`,
`SlashAnalyticsBackend`, `SlashPurchasesBackend` or `SlashGamesBackend`
(`slash/services/apple.h`), which an SDK package names in its `backends`. The same calls reach
them, through JNI or the Objective-C runtime, and they report to a `Reports` (Java) or
`SlashServiceReports` (Objective-C) object from any thread.
