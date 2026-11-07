# Lighting check

`lighting.check` looks at a 3D frame's lighting for an agent, which can't judge a picture by
eye. [main.scene.json](main.scene.json) is a courtyard with a crate, a pillar and a ball under a
`sun`, seen through a `Camera3D` in a 160x90 window, with two badly placed lights:

- `flood` is a `SpotLight3D` at an energy of 40, aimed at the crate.
- `porch_lamp` is an `OmniLight3D` with a range of 2, fourteen meters behind the pillar, where
  the camera sees nothing it could light.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
lighting.check {"scale":1}
{"exposure":{"clipped":0.0617,"crushed":0.0,"mean":0.6461,"p5":0.5793,"p95":0.946},
 "lights":[{"clips":0.0617,"id":"flood","lit":0.0701,"share":0.042,"type":"SpotLight3D"},
           {"id":"porch_lamp","lit":0.0,"share":0.0,"type":"OmniLight3D"}, ...],
 "problems":[{"kind":"overexposed","message":"6.2% of the frame is clipped to full brightness"},
             {"id":"flood","kind":"clips","message":"\"flood\" pushes 6.2% of the frame to full brightness"},
             {"id":"porch_lamp","kind":"unlit","message":"\"porch_lamp\" lights nothing in view: ..."}],
 "size":[160,90]}
```

- It draws the frame on the CPU, as `frame.capture` does, so it needs no GPU and gives the same
  numbers on every machine. `exposure` is the frame's brightness: its mean, 5th and 95th
  percentiles, and the shares of pixels clipped to full brightness and crushed to black.
- It draws the frame again without each light, so `lights` says what share of the frame each one
  lights (`lit`), how much of the frame's brightness it adds (`share`) and what it pushes into clipping
  (`clips`: what would not be clipped without it). The lights are put back as they were; the check changes nothing.
- `problems` names the flood that washes the crate out and the lamp that lights nothing.
- `component.set` turns the flood down to 3 and moves the lamp beside the ball, and a second check
  finds no problems.
- `"lights": false` measures only the exposure, drawing the frame once, here at half size.
- A misspelled light in `ids` gets a "did you mean".
