# Alpha hash

Godot's alpha hash transparency, which it draws hair cards, eyelashes and leaves with: each pixel
is drawn whole or not at all, where its alpha reaches a threshold from noise fixed to the mesh,
so a card fades out by how many of its pixels it keeps, with no sorting, and casts a shadow
dithered the same way. [main.scene.json](main.scene.json) has an unshaded yellow card of alpha
0.4 (`"transparency": "alpha_hash"`) in front of a blue wall, seen in a 64x48 window
([project.json](project.json)).

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[20,24],[23,24],[26,24],[29,24],[32,24],[35,24],[38,24],[41,24],[44,24]]}
{"colors":[[0.196,0.302,0.8,1],[0.196,0.302,0.8,1],[0.898,0.749,0.302,1],[0.196,0.302,0.8,1],
 [0.898,0.749,0.302,1],[0.196,0.302,0.8,1],...],"drawn":{"meshes":2},"height":48,"width":64}
```

- `frame.capture` reads nine pixels across the card: at alpha 0.4, two of them are the card's
  yellow and the rest the wall's blue behind it.
- `component.set` raises the card's alpha to 0.8, and six of the nine are yellow.
- `component.set` makes it `"alpha_scissor"` instead, whose threshold is 0.5 everywhere, and all
  nine are yellow: a scissor keeps all of a card or none of it, where a hash keeps a share.

`alpha_hash_scale` sets the dither's grain. The noise is a hash of whole numbers where Godot's
uses sines, so every device keeps the same pixels.
