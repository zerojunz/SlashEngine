# NPC stories

NPCs with stories of their own: a questline, a conversation, daily schedules and routes, and
brains that extend a shared one. [village.scene.json](village.scene.json) has a 2D village
(Mira the shepherd, a watchman and the player) and a guard walking the same round in 3D, and
[project.json](project.json) sets the npc clock to an hour a second from 7 o'clock
(`"npc": {"day_seconds": 24, "start_hour": 7}`).

- [brains/villager.npc.json](brains/villager.npc.json) is the brain every villager shares: eat
  and rest. Mira's ([brains/mira.npc.json](brains/mira.npc.json)) `extends` it with a trait,
  `kindness`, which no action changes, a `hopeful` fact and a goal of waiting by the fold.
- The watchman's ([brains/watchman.npc.json](brains/watchman.npc.json)) goals read the clock's
  `hour`: from 8 to 20 o'clock (`"hour": [">=8", "<20"]`) he walks his `round`, a closed
  route through the gate and the well (`{"follow_route": "round"}`); after 20 he goes to bed.
  The 3D guard ([brains/watchman_3d.npc.json](brains/watchman_3d.npc.json)) extends him and
  changes only the round, now three points in meters, and where his bed is.
- [quests/lamb.quest.json](quests/lamb.quest.json) is a questline over story facts: find
  Mira's lamb (spotting the wolf is optional), bring it back for five coins, or keep it and
  fail. It starts once `met_mira` is set. `external` names the facts the game's code sets, so
  `quest.check` knows they can happen.
- [dialogue/mira.dialogue.json](dialogue/mira.dialogue.json) is Mira's conversation. It starts
  at `found` once the lamb is found, else at `hello`, whose first choice sets `met_mira`,
  starts the quest and makes Mira hopeful (`self.hopeful`, her own fact).
  [translations/story.csv](translations/story.csv) has it and the quest in French.

The calls ([calls.jsonl](calls.jsonl), replies in [results.jsonl](results.jsonl)):

1. `quest.check` finds nothing wrong: the quest can be finished and every line reached.
2. `npc.plan` on the watchman's brain at 9 o'clock plans his round. At 8 o'clock both
   watchmen set off: the 2D one toward the gate, the 3D one toward his first point.
3. The player talks to Mira (`dialogue.start`, `dialogue.choose`) and takes the quest; Mira,
   hopeful now, walks to the fold. Setting `lamb_found` with `quest.set` moves the quest to its
   `return` stage.
4. In French (`l10n.set_locale`), Mira asks again; a choice she didn't show fails, and handing
   back the lamb ends the quest with its reward. `quest.state` gives the journal in French, and
   `events.read` the goals, quest stages and objectives in order.
5. Twelve hours on, both watchmen have gone to bed. A quest path that isn't a `.quest.json`
   file fails at `/quest`.
