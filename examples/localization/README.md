# Localization

A title menu written in English and translated as Godot translates one: by translation files
that project.json's `internationalization` lists, looked up in the game's locale.
[translations/menu.csv](translations/menu.csv) holds French and Arabic in a column each, as
Godot's CSV translations, and [translations/pl.po](translations/pl.po) holds Polish as a gettext
file, with Polish's three plural forms and two meanings of "Open" told apart by context.
[main.scene.json](main.scene.json) is the menu in a 320x180 window ([project.json](project.json)).
[calls.jsonl](calls.jsonl) switches its language the way an agent does over MCP (tools
`l10n_state`, `l10n_set_locale`, `ui_layout`) or JSON-RPC, and [results.jsonl](results.jsonl)
holds the replies.

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
