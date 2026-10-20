# Achievements and leaderboards

Sign-in, achievements and leaderboards through one API, as Unity's Social API over Google Play
Games and Game Center ([docs/reference/services.md](../../docs/reference/services.md#achievements-and-leaderboards)).
[project.json](project.json) names an achievement unlocked at once (`first_finish`), an
incremental one (`ten_laps`, 10 steps) and a leaderboard where lower is better (`best_lap`), each
with the IDs a games backend would use on Android and iOS. [main.cpp](main.cpp) adds a `race`
system: its buttons finish a lap (submitting its time, unlocking `first_finish` and adding a step
to `ten_laps`), load the best three laps and show the achievements screen, and it reads
`services::events` to update the status label. Then it plays the race the way an agent does over
MCP (tools `services_state`, `services_test`, `ui_press`, `events_read`) or JSON-RPC.
`expected.txt` holds each call and its reply.

- Sessions run the services on the test backend, so the player signs in with no account,
  deterministically: `signed_in` at the first tick, `achievements_loaded` with their progress at
  the second, as Play Games and Game Center sign a returning player in when the game starts.
- `achievement_unlocked` arrives once per achievement, at the tick after the unlock. Unlocking
  one again does nothing.
- `services.test`'s `scores` puts other players on the test leaderboard, and `increment`,
  `unlock` and `submit` call the services as the game would.
- `scores_loaded` ranks the scores by the leaderboard's `sort_order`, with the player's own
  rank, and `events.read` shows its data.
- In an exported game the same code runs on the games backend an SDK package registers, over
  Play Games or Game Center. With none, sign-in fails with `no games backend`, and progress and
  scores are kept for the game.
