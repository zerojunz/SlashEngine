# Localization

A title menu written in English and translated as Godot translates one: by translation files
that project.json's `internationalization` lists, looked up in the game's locale.
[translations/menu.csv](translations/menu.csv) holds French and Arabic in a column each, as
Godot's CSV translations, and [translations/pl.po](translations/pl.po) holds Polish as a gettext
file, with Polish's three plural forms and two meanings of "Open" told apart by context.
[main.scene.json](main.scene.json) is the menu in a 320x180 window ([project.json](project.json)).
[calls.jsonl](calls.jsonl) switches its language the way an agent does over MCP (tools
`l10n_state`, `l10n_set_locale`, `l10n_extract`, `l10n_check`, `l10n_translate`, `ui_layout`)
or JSON-RPC, and [results.jsonl](results.jsonl) holds the replies.

```text
l10n.set_locale {"locale":"fr-CA"}
{"locale":"fr_CA","was":"en"}
ui.layout {"ids":["title","start","quit","version"]}
{"controls":[{"id":"title","lines":["Chasse au trésor"],...},{"id":"start","lines":["Commencer"],...},
  {"id":"quit","lines":["Quitter"],...},{"id":"version","lines":["Version 1.0"],...}],...}
```

- Labels and buttons show their text translated, as Godot's controls do, unless their
  `Control.auto_translate_mode` (or a parent's) is `disabled`, as the version label's is.
- A locale takes the translations that match it best: `fr_CA` has none of its own, so it takes
  French's. Where no translation has a message, the fallback locale's (`en`) answers, and else
  the text as written: Polish has no "Options", so it stays in English. The menu could as well
  be written in Japanese and translated into English.
- `l10n.state`'s `tr` shows messages as gameplay code gets them from `l10n::tr` and
  `l10n::tr_n`: "%d coin" in Polish is "%d moneta" for 1, "%d monety" for 3 and 22, and
  "%d monet" for 12, as the file's Plural-Forms rule says.
- Arabic lays the menu out right to left (`rtl`), since the root control's direction follows
  the locale. The built-in font has no Arabic letters (`missing_glyphs`); a game gives one with
  a font whose `language_support` names Arabic, which controls pick once the game is in it.
- Switching emits `translation_changed`, and a locale no translation has gets a warning.
- `l10n.extract` collects every message the game shows, as Godot's Generate POT: the menu's
  labels and buttons (not the version label, which stays as written) and the `l10n::tr` and
  `tr_n` calls in [src/shop.cpp](src/shop.cpp), each with where it is. It writes them to
  `translations/menu.pot`, as Godot's POT generator writes one, and adds what each translation
  file lacks, untranslated: "Thanks for shopping!" to the CSV file (which has no `?context` or
  `?plural` column, so the shop's "Open" and the coin count are skipped there), and "Options"
  and "Thanks for shopping!" to the Polish file.
- `l10n.check` holds the translations to the game: no placeholder is wrong, but the shop's
  messages are missing in French and Arabic, "Options" in Polish, and the Polish door's "Open"
  is translated but nothing shows it. Laid out in Arabic, the menu's text needs glyphs the
  built-in font lacks: errors, which `test.run` would fail on until the menu gets a font with
  Arabic letters.
- `l10n.translate` has a model draft what Polish lacks, told the storyboard's
  [glossary](storyboard/glossary.json) and the translations Polish already has. Its answer comes
  from [translations/ai.replay.jsonl](translations/ai.replay.jsonl), as
  [providers.json](providers.json)'s `replay` says, so the example runs with no model; delete
  the file and point `url` at a model to draft again. The drafts go into the Polish file marked
  `#, fuzzy`, so the game still shows "Options" in English until someone reviews "Opcje" and
  takes the mark off.
