# Animation tuning

Agents fine-tune a creature's animations as numbers, in 3D and in 2D. A recipe's `motion` block
retunes its built-in animations by name (length, speed, loop, amplitude, each bone's `scale`,
`shift` and `hold`, added `waves`) and makes new ones `from` them; a placed creature's own
`motion` is a merge patch over the recipe's, for that one alone. `animation.preview` measures an
animation, and `animation.tune` changes it toward what it is asked for and measures it again.

[deer.creature.json](creatures/deer.creature.json) is a spotted deer that walks at 1.2 meters a
second, and [fox.creature.json](creatures/fox.creature.json) a wary fox that runs at 6. The scene
is a meadow with a camera and a sun.

[calls.jsonl](calls.jsonl) works on them the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `animation.preview` measures the deer's walk: each bone's range by axis (the near front leg
  swings 0.45 radians each way) and its gait, worked out from where the feet go by forward
  kinematics. Its stride is 0.729 meters, but a planted foot sweeps back at 1.305 meters a second
  while the deer moves at 1.2, so its feet `slide` by -0.105.
- `animation.tune` with `slide: "length"` plants them by slowing the walk: `speed` 0.9196 makes
  it 1.215 seconds long and the slide 0. It writes the recipe's `motion` block.
- A new `stalk` comes `from` the walk: 2.4 seconds, at 0.6 of the swing, head low and neck
  raised, with a tail flick added twice a cycle. Moving at 0.4 meters a second, `slide: "swing"`
  plants its feet by scaling the legs' swing to 1.0755. It has no `before`, as it is new.
- An old stag is placed with a `motion` of its own: its walk swings at 0.7, so its feet slide by
  0.345. `animation.tune` on the stag plants them (legs at 1.4285) and holds its tail to a span of
  0.15 radians (`ranges`), writing only the stag's `Creature3D`. A filmstrip of six frames shows
  the result.
- In 2D the fox is a cut-out at 120 pixels a meter. Its run is previewed and drawn as four
  frames, then its tail swings two and a half times as far, and the run is retimed by `speed`
  0.9986 so its feet stay planted.
- A misspelled animation is `-32602` at `/animation`, with the one meant.
