# Water

Oceans, lakes and rivers after Unreal's Water plugin. A `.water.json` recipe
(`slash-cli schema water`) says what a body of water is; a `Water3D` or `Water2D` places one.

- [waters/lake.water.json](waters/lake.water.json) is a lake with two small Gerstner waves.
- [waters/river.water.json](waters/river.water.json) `extends` it as a river flowing 2 m/s down
  its path, slowing at its banks.
- [waters/sea.water.json](waters/sea.water.json) is an ocean whose waves a 7 m/s wind raises
  round the Pierson-Moskowitz peak.
- [waters/pond.water.json](waters/pond.water.json) is a pond seen from the side in pixels.

[calls.jsonl](calls.jsonl) places the lake and the river in [lake.scene.json](lake.scene.json),
drops a crate and a stone into the lake and lets a log go on the river, then reads the
`water_entered` events, where each ended up (the crate floats, the stone sinks, the log is
carried downstream) and `water.sample`s the lake's depth over the beach and the river's flow
midstream and by its bank, and draws the frame. Then it raises the sea round a buoy in
[sea.scene.json](sea.scene.json) and samples the waves half a second ahead, and floats a duck
while a pebble sinks in the 2D pond of [pond.scene.json](pond.scene.json).
[results.jsonl](results.jsonl) holds the replies.

- The surface is Gerstner waves: each point goes round its wave's orbit, so crests sharpen and
  foam where the surface is squeezed together. Heights under a point undo the sideways shift.
- The ground under a lake or river is measured once by rays down past what moves, so the water
  knows its depth, shrinks its waves and foams in the shallows.
- Rigid bodies float by the water they displace, measured at points through their shapes, and
  are dragged toward the water's velocity, so waves rock them and currents carry them.
- Bodies going in and moving along the surface leave rings that spread and die away.
