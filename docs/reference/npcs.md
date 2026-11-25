# NPCs

NPC brains, planning, quests, dialogue and fauna.

Status: current · Updated: 2026-11-20

`engine/npc` is NPCs that plan, as F.E.A.R.'s goal-oriented action planning (Jeff Orkin's "Three
States and a Plan"). A `.npc.json` brain (`$cli schema npc`; `slash/npc/npc.hpp`,
`src/brain.cpp`) names facts (whole numbers, booleans as 0 and 1), goals with a priority and
`when` and `want` conditions, and actions with `needs`, `effects` (a value, or `"+1"` to add), a
cost and a `do` (`wait`, `go_to` an entity, `emit` an event). `plan` is A* over the facts' values,
ties going to the action earlier in the file, bounded by `search_limit`; `choose` takes the first
goal by priority that applies, is unmet and is reached; `check_brain` warns about goals no chain
reaches and actions no plan uses. An `NpcAgent` names its brain and keeps its facts (those that
differ from the brain's), goal, plan and timers, so saving, undo and replays keep them. The `npc`
system (priority 965, after navigation; `src/npc.cpp`) runs each agent's action (a `go_to` walks
along its `NavAgent3D` or 2D's path when it has one), then has those due to think choose on
the world's job pool, each from its own facts, and starts their plans in entity order, emitting
`npc_goal_changed`, `npc_action_started`, `npc_action_finished` and `npc_action_failed`.
Gameplay code answers an `emit` action's event and sets facts with `npc::set_fact`, which has the
agent think at the next tick. `npc::check_scene` backs `scene validate` and exports, and
`npc.plan` (`engine/command/src/npc.cpp`) says what an agent or a brain would choose, or why no
goal is chosen. `npc.draft` (`engine/ai/src/npc_draft.cpp`, in `slash_ai_commands`) has a
model from `providers.json` write or revise a brain from a brief and storyboard entries, sending
each draft's problems (`validate_brain`, `go_to` targets, `emit` events, goals `check_brain`
finds out of reach) back until one checks, and only then writes it. `examples/npc-planner` has
two farmers and a guard who rings a bell, and `examples/npc-draft` a fruit seller a model wrote.

NPCs have stories of their own (`slash/npc/story.hpp`). A brain `extends` a shared one
(`resolve_brain` in `src/brain.cpp` merges the chain: facts, `traits` and `routes` by key, goals
and actions by name field by field, `"remove"` dropping one), `traits` are facts no action
changes, and a list of conditions on one fact must all hold. project.json's `npc` sets the npc
clock (`npc::clock`, from the world's ticks), which fills brains' `hour` and `day` facts;
`follow_route` walks a brain's `routes` from `NpcAgent::waypoint`. Facts named `story.<name>`
are a `QuestLog`'s (`npc::story_log`), written back when an action ends. `.quest.json` quests
and `.dialogue.json` conversations (`src/story.cpp`) run in `src/story_world.cpp`: the `quests`
system (priority 966) and `set_story_fact` move each `QuestLog`'s quests through their stages,
and `start_dialogue` and `advance_dialogue` keep a listener's `Conversation`, its effects on the
listener's story facts or the speaker's own (`self.<name>`), translating through `l10n::tr`.
`check_story` (`src/story_check.cpp`) works out every value any file sets each story fact to and
reports quests that can't finish and lines nothing reaches; `quest.check` serves it and
`test.run` runs it. `quest.*` and `dialogue.*` are in `engine/command/src/npc.cpp`, and
`l10n.extract` collects quests' and dialogue's text. `examples/npc-stories` runs a questline,
a conversation in two languages and watchmen in 2D and 3D.
