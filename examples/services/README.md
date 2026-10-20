# Services

Ads, analytics and in-app purchases through one API, as Unity's services
([docs/reference/services.md](../../docs/reference/services.md)). [project.json](project.json)
names a rewarded ad placement (`bonus`, 25 coins), a banner (`footer`), a consumable
(`coins_100`) and a non-consumable (`remove_ads`), each placement with the ad unit ID an ads
backend would load on Android and iOS. [main.cpp](main.cpp) adds a `shop` system: its buttons
call `show_ad` and `buy`, and it reads `services::events` to pay out coins on `ad_rewarded` and
`purchase_completed`, finish each purchase once granted, and show or hide the banner. Then it
plays the shop the way an agent does over MCP (tools `services_state`, `services_test`,
`ui_press`) or JSON-RPC. `expected.txt` holds each call and its reply.

- Sessions run the services on the test backend, so ads load and purchases complete with no
  store account, deterministically. Both ads load at the first tick, as placements auto-load,
  and the products' prices arrive.
- A rewarded ad shows at the tick after `show_ad` and closes with its reward the tick after
  that, then loads again on its own.
- The shop logs a `coins_earned` analytics event each time. `services.state` shows it was not
  sent until the player granted analytics consent.
- A purchase completes, the shop grants it and calls `finish` with its transaction, so nothing
  is left in `unfinished`. A store would deliver an unfinished purchase again at the next start.
- `services.test`'s `outcome` makes purchases end `pending` (a parent must approve,
  `complete_pending` ends it), `cancelled` or `failed`; `buy` buys as the game would.
- `remove_ads` is owned once bought, so buying it again is refused with the reason, as the game
  itself would be told.
- In an exported game the same code runs on backends an SDK package's glue code registers;
  with none, ads fail to load (`no ads backend`) and the store is unavailable (`no store`).
