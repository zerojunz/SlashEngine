# Soundtracks

Scores, MIDI, stems and cue sheets.

Status: current · Updated: 2026-11-20

`engine/soundtrack` turns music into text agents read and write. The storyboard's
`music.json` plans it: `theme` entries (key, tempo, instruments and mood for characters, places
or factions), `loudness` targets and the cue sheet, `cue` entries saying which theme plays in
which scenes, story beats and game states. `soundtrack.plan` (`src/plan.cpp`) drafts the cues a
game lacks as edits for `storyboard.edit`, each with the theme of the place, faction or character
its scene or beat is about; `storyboard.check` warns about scenes and beats with no cue once
there are cues, and `storyboard.brief` sends a cue's theme in full. A `.score.json` file
(`$cli schema score`; `src/score.cpp`) holds a tempo, meter, key, named sections and tracks of
General MIDI instruments or drums, each bar a line of notes such as `"D2:4 ; D3 F3 A3 D4@110"`;
`decode_bar` reads a line and `encode_bars` writes notes back as the shortest line that plays
them. `score.export` and `score.import` (`src/midi.cpp`) write and read Standard MIDI Files.
`soundtrack.render` (`src/render.cpp`) plays a score through TinySoundFont into an `.ogg` or
`.wav` stem per track, each with a `.audio.json` file giving its `bpm` and `beat_count`, and a
synchronized `.audio.json` file that plays them together; looping stems are the score's exact
length, with what rings past the end added onto the start. Its instruments are a SoundFont 2
file or the engine's own (`src/sound_bank.cpp`: a voice per General MIDI family and a drum kit,
synthesized without libm). TinySoundFont and libvorbis are fetched for the tools only. Scores
are a game's source, not its data, so exports leave them out. `soundtrack.arrange`
(`src/arrange.cpp`) writes the cues as one interactive `.audio.json` stream: a clip per cue, or
per layer as synchronized streams of that layer's file and those below it, switching on the
bar, between layers on the beat at the same place, and cues that play once as stings (a filler
clip on the way out of the cues in their `after`, else a clip that returns to the one it
interrupted). `soundtrack.check` (`src/check.cpp`) reads every music file and plays each piece
to measure it: cue music that can't play, beats longer than a sound, loops that click, clips
with no beats to switch on and loudness off the storyboard's targets are errors, which
`test.run` fails on; switches off the bar, keys that clash and tempos other than `bpm` are
warnings. As a game runs, each clip its music starts is a `clip_changed` world event
(`audio::ClipChange`, from `Playback::take_clip_changes`), with the clip before, the transition
followed and the seconds it waited for its beat or bar; `step` and `run.until` return them as a
`music` timeline and `playtest.run` sums them up per combination, so agents check the music
follows the game without listening. `examples/soundtrack-plan` plans a game's cues,
`examples/music-scores` sends a piece through MIDI and back, `examples/soundtrack-render`
renders a loop and measures it, `examples/soundtrack-arrange` arranges cues and steers them,
`examples/soundtrack-check` checks a game's music, and `examples/music-timeline` follows a boss
fight's music.
