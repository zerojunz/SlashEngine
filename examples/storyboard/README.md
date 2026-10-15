# Storyboard

Working from the game's recorded facts instead of guessing them. The project has a character,
her faction, a place, a design rule and a glossary term, and its scene's `mira` entity has a
`StoryRef` naming `character/mira`. [calls.jsonl](calls.jsonl) briefs before a task, records a
new character, has a second agent's stale edit refused, catches a word the glossary bans, fixes
it, and links a new entity to the new entry. [results.jsonl](results.jsonl) holds the responses,
line for line.

```bash
slash-cli rpc --project . < calls.jsonl
```

- `storyboard.brief` with a scene returns the entries its entities stand for, entries linked to
  it and the rules that apply everywhere, each with the version an edit needs; `related` names
  what those mention.
- `storyboard.edit` takes the version it read. An edit based on an older version fails with
  `-32005` and the entry as it is now, so nothing newer is overwritten.
- `storyboard.check` reports words a glossary `term` says to avoid, references to missing entries
  and `StoryRef` IDs that name nothing.
- `since` returns what changed after an earlier `seq`, so an agent rereads only that.
