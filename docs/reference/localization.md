# Localization

Translation files, locales and plural rules.

Status: current · Updated: 2026-11-20

`engine/l10n` is Godot's TranslationServer. `project.json`'s `localization` lists CSV,
`.po` and `.mo` translation files, the fallback locale, a `test` locale, pseudo-localization and
the root layout direction (`l10n::Settings`). `src/translation.cpp` reads the files as Godot's
CSV importer and `TranslationLoaderPO` do into `l10n::Translation`s (messages by context, plural
forms), and `PluralRules` splits a gettext rule at its `?` and `:` into tests run as
`slash::Expression`, as Godot's. `src/locale.cpp` ports `standardize_locale`, `compare_locales`
and the locale tables (renames, default scripts, plural rules, right-to-left languages).
`l10n::install` keeps a world's state in its context (behind a lock, since layouts and threaded
systems look up), reading the files through assets so they reload: games start in the player's
locale (`l10n::preferred_locale`, from SDL), sessions in `test` or the fallback, so replies
repeat. `l10n::tr` and `tr_n` look up a message as Godot's `TranslationDomain` (the best-scoring
translation that has it, then the fallback's, else the source text, so any language can be the
source), `tr` pseudolocalizes as `TranslationDomain::pseudolocalize`, and `l10n::set_locale`
emits `translation_changed`. `ui::layout` shows controls' text, placeholders, tooltips and items
translated by `Widget.auto_translate_mode` (Godot's `can_auto_translate`, items' modes as
`PopupMenu::_atr`), lays out by `l10n::is_layout_rtl` (Godot's `is_layout_rtl`, with
`application_locale` and `system_locale`) and shapes text whose `language` is `""` in the locale
once the project localizes (`l10n::text_language`), so fonts' `language_support` picks glyphs.
Exports ship the listed files and `project validate` checks them. `l10n.state` and
`l10n.set_locale` (`engine/command/src/l10n.cpp`) read and switch it, and `l10n.extract`
(`src/l10n_extract.cpp`, Godot's POT generator and scene parser) collects controls' text from
scenes and prefabs by their translate modes and `tr` and `tr_n` literals from the game's sources
(through `outline_cpp`), writes a `.pot` file and adds what each translation file lacks; `l10n.check`
(`src/l10n_check.cpp`) holds the files to those messages (placeholders, missing and stale
messages) and lays out every scene in a session of its own in each locale, and pseudolocalized,
for text that stops fitting or lacks glyphs, which `test.run` fails on. `l10n.translate`
(`engine/ai/src/translate.cpp`, in `slash_ai_commands`) has a model from `providers.json` draft
what a locale lacks, told the storyboard's glossary, characters and tone, checks each draft's
forms, placeholders (`l10n::placeholder_problem`) and words to avoid, and writes the rest into
a `.po` file marked `#, fuzzy` (which lookups skip until the mark is taken off) or a CSV file's
column; extraction (`command::extract_messages`, `slash/command/l10n.hpp`) and the PO and CSV
writing helpers (`l10n::po_string`, `csv_rows`) are shared with `l10n.extract`.
`examples/localization` translates a title menu.
