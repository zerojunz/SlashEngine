# Storyboard review

A model checks new dialogue against the storyboard. The project's storyboard says Mira has never
left Lowtown and Oren is her brother; a scene's label calls Oren her sister, and a dialogue file
has her taking a ferry out of town. [main.cpp](main.cpp) starts a stand-in for a model server on
the loopback interface, so the example needs no network or key, and calls `storyboard.review` on
both files. [expected.txt](expected.txt) holds its output.

- `storyboard.check` runs first: it finds broken references and banned words with no model, and
  passes here, because nothing it checks is wrong. Contradictions in prose need a reader.
- `storyboard.review` takes `paths` (scenes and prefabs give their controls' text, text files
  their whole text, images go to a model that takes them), `text` or storyboard `entries`. It
  sends the model only the facts the material touches: entries whose names it mentions, the
  scene's own entries, rules and tone that apply everywhere, and the entries those name.
- Each finding gives the file, the JSON Pointer of the text in a scene, the quote, the entry it
  contradicts and the model's confidence. Findings are advisory: a model's judgment is not proof.
- The stand-in's answer has a third finding quoting words the dialogue never says.
  `storyboard.review` keeps only findings whose quote is in the material and whose entry exists,
  and counts the rest as `unverified`.
- Agents call it over MCP as `storyboard_review`; see
  [the command reference](../../docs/reference/commands.md#storyboardreview).
