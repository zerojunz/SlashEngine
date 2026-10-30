# Store packages

A game that sells coins, a way to remove ads and a season pass ([project.json](project.json)'s
`services.purchases`) adds the engine's own store packages, so each platform's export buys
through its store: [calls.jsonl](calls.jsonl) runs `sdk.add` with `storekit` and `play-billing`,
which copies them from the engine's `export/sdks/` folder into `sdks/` and lists them in
`project.json`'s `sdks`.

- `storekit` gives iOS and macOS exports an Objective-C purchases backend over Apple's StoreKit
  and links its framework. Nothing is downloaded: StoreKit is part of the system.
- `play-billing` gives Android exports a Java purchases backend over Google Play Billing, whose
  library is Google's, so `sdk.check` for Android says to run `slash-cli sdk lock` first: it
  fetches the library from Google's Maven repository and pins it by SHA-256 in
  `sdks/play-billing/sdk.lock.json`.

Each platform's preset picks its store in `export_presets.json`:

```json
{"format": 1, "presets": {
  "android": {"platform": "android", "sdks": ["play-billing"]},
  "ios": {"platform": "ios", "sdks": ["storekit"]}}}
```

The game's code is the same everywhere (`services::buy`, `finish`, `restore_purchases`; see
[services](../services/README.md)), and headless sessions keep using the test backend.
`docs/reference/services.md` (Stores) says what each store reports as transactions and receipts.

```bash
slash-cli rpc --project . < calls.jsonl
```
