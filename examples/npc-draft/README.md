# NPC draft

A model writes an NPC's brain. `npc.draft` sends a model from
[providers.json](providers.json) a brief, the storyboard's entries about the NPC
([storyboard/world.json](storyboard/world.json)), the entities in
[main.scene.json](main.scene.json) it may walk to and the events the game answers, with the
`.npc.json` schema. Each draft is checked as `scene validate` checks a brain, and also for
`go_to` targets the scene lacks, `emit` events the game doesn't answer and goals no chain of
actions reaches; a draft with problems goes back to the model with them, and only one that
checks is written. The replies come from [brains/ai.replay.jsonl](brains/ai.replay.jsonl),
recorded from a stand-in model, so the example runs offline.

[calls.jsonl](calls.jsonl) is the session, its replies in [results.jsonl](results.jsonl):

- `npc.draft` writes `brains/mira.npc.json` for Mira, who sells fruit at her stall. The first
  draft walks to a "market" the scene doesn't have; told so, the model's second draft walks to
  the stall (`"attempts": 2`). The reply lists the brain's goals and actions and the entities
  and events it uses.
- An `NpcAgent` takes the brain. Two customers wait and Mira has no fruit, so `npc.plan` says
  she restocks first. Ten seconds later she has picked fruit, walked to her stall and sold
  twice, each sale a `sell` event for the game's code.
- At night (a fact `npc.plan` changes for the question) she would walk home.
- A second `npc.draft` revises the brain with the brief "when thirsty, Mira drinks at the well",
  the model hearing the brain as it is, and `npc.plan` finds her new plan. `undo` puts the
  first brain back, which has no fact by that name, so asking about `"fruits"` fails with a
  "did you mean".
