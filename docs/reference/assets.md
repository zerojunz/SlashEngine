# Assets and hot reload

Asset loading and hot reload.

Status: current · Updated: 2026-11-20

`engine/asset` loads files by project path into shared, reference-counted handles
(`assets.load<json::Value>("data/tuning.json")`); `.json`, `.txt` and `.csv` load out of the box,
`canvas::add_asset_types` adds textures (`.png`, `.jpg`, `.bmp`, `.ktx2`), `.frames.json`, `.material.json`,
`.particles.json` and `.tileset.json` files, `animation::add_asset_types` adds `.anim.json` files,
`text::add_asset_types` adds fonts (`.ttf`, `.otf`, `.font.json`), `audio::add_asset_types`
adds sounds (`.wav`, `.ogg`, `.mp3`), `.audio.json` and `.bus_layout.json` files, and games add types
with `add_type`. Sessions and games own an `asset::Assets`, and prefab files are read through it.
An asset also reloads when a file its loader read changes, such as a recipe's model parts.
`Session::poll_assets()` reloads changed files, updates prefab instances (keeping overrides) and
applies a changed `project.json`'s settings again; every command polls first, and `slash-cli run`
and `serve` poll between ticks. `asset.list` shows what is loaded and the bytes each holds
(`add_type`'s optional `bytes` function, else the file's size), and subscribers get
`asset.reloaded` events.
