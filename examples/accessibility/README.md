# Accessibility

What a screen reader gets from a game's controls, as Godot 4.5's controls describe themselves.
[main.scene.json](main.scene.json) is a pause menu in a 320x240 window
([project.json](project.json), which serves a screen reader `always`): a title read out
politely when it changes, a Resume button, a Music check box, a Volume slider labeled by the
title, a Difficulty list, a name field described by a hint, and an icon button with nothing to
call it. [calls.jsonl](calls.jsonl) works it the way an agent does over MCP (tools
`ui_accessibility`, `ui_accessibility_action`, `events_read`, `step`) or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies.

```text
ui.accessibility {}
{"enabled":true,"problems":[{"id":"close","message":"a screen reader has no name for this
  button: ...",...}],"tree":[{"children":[{"id":"title","live":"polite","role":"static_text",
  "value":"Paused",...},...,{"actions":["decrement","increment","set_value"],"id":"volume",
  "labeled_by":["title"],"name":"Volume","range":[50,0,100,10],...},...],"role":"container"}]}
```

- `ui.accessibility` returns the controls' tree: each one's role (`static_text`, `button`,
  `check_box`, `slider`, `text_field`, ...), name (a button's text, a field's placeholder, else
  `accessibility_name`), value, `checked`, `range`, flags, rect and the actions it takes besides
  the ones every control takes.
- The icon button can take focus but has no name, so it is a problem, with the field to fix.
- `ui.accessibility_action` does what a screen reader asks: focus and click the check box (it
  toggles at the next tick), step the slider, type a name, open the list (its menu and items
  join the tree, the one shown checked and the menu focused) and pick an item.

Run it in a window with Narrator or NVDA on Windows, VoiceOver on a Mac or Orca on Linux, and
the screen reader reads the same tree through AccessKit and presses the same controls:

```bash
slash-cli run --project examples/accessibility --scene examples/accessibility/main.scene.json
```

[Accessibility](../../docs/reference/scene-format.md#accessibility) in the scene format reference
lists what each control reports.
