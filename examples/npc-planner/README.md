# NPC planner

NPCs that plan, as F.E.A.R.'s goal-oriented action planning. [main.scene.json](main.scene.json)
has a village: two farmers, Ann and Bob, and a guard, each an `NpcAgent` naming a `.npc.json`
brain of facts, goals and actions. A farmer
([brains/villager.npc.json](brains/villager.npc.json)) wants to hide in the hall when the alarm
rings, to eat when hungry and otherwise to bring in three crops:

```json
"goals": [
  {"name": "hide", "priority": 3, "want": {"at_hall": true}, "when": {"alarm": true}},
  {"name": "eat", "priority": 2, "want": {"hungry": false}, "when": {"hungry": true}},
  {"name": "farm", "priority": 1, "want": {"crops": ">=3"}, "when": {"alarm": false}}
]
```

Its actions say what each needs, changes, costs and does: walk to the field or the pantry
(`go_to`), harvest or eat (`wait`). The guard ([brains/guard.npc.json](brains/guard.npc.json))
keeps watch from the tower and, when he sees a thief, rings the bell, an action that emits a
`bell` event. The npc system plans each agent's cheapest chain of actions to its goal and runs
it; [main.cpp](main.cpp) adds the game's one line of NPC code, a system that answers the bell by
setting each farmer's `alarm` fact with `npc::set_fact`.

The program prints each call and its reply and, each second, where the three are and what they
are doing ([expected.txt](expected.txt)):

- `npc.plan` on the farmer's brain gives the plan from its defaults: walk to the field and
  harvest three times. Bob starts hungry, so his plan is to fetch food from the pantry and eat.
- Four seconds on, Ann is harvesting, Bob has eaten and is on his way to the field, and the
  guard is on his tower.
- The guard sees a thief: his goal changes to `raise_alarm`, he rings the bell, and both farmers
  run to the hall, Bob dropping his walk to the field. `events.read` has the goal changes and the
  bell.
- In the hall Ann wants nothing more, and `npc.plan` says why for each goal. With the pantry
  empty, eating is out of reach, and a misspelled fact fails with a "did you mean".
