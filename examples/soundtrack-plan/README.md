# Soundtrack plan

Planning a game's music in the storyboard before writing any. The project has a harbor, its
lighthouse and a reef, a pilot called Ada and the lighthouse keeper, two story beats, and
[music.json](storyboard/music.json): a theme for the harbor, a theme for the keeper, a loudness
target and one cue, for the menu. [calls.jsonl](calls.jsonl) finds what has no music, drafts the
missing cues, records them with changes of its own, and briefs a scene with its music.
[results.jsonl](results.jsonl) holds the responses, line for line.

```bash
slash-cli rpc --project . < calls.jsonl
```

- Once the storyboard has a cue, `storyboard.check` warns about every scene no cue plays in and
  every story beat no cue plays during.
- `soundtrack.plan` drafts a cue for each of them, and for each game state given in `states`
  that no cue names, as edits `storyboard.edit` takes. A cue plays the theme of the place it is
  set in (the lighthouse is in the harbor, so the harbor's theme), else of a faction, else of a
  character; `unthemed` lists the places, factions and characters that need a theme of their own,
  here the reef.
- The plan is a draft: the edit gives the lighthouse the keeper's theme instead, makes the wreck
  play once and gives the harbor two intensity layers before writing it.
- `storyboard.brief` with the lighthouse scene now returns its cue, the cue's theme and the
  loudness target, so the agent writing the music starts from the key, tempo, instruments, mood
  and level.
