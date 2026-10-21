# Generated music

Music in the asset library, made by a generator or delivered by a composer, kept with its tempo,
beats, key, author and license. [main.cpp](main.cpp) works on `game/`, whose storyboard names a
theme for its harbor (G major, 96 beats per minute, three beats to a bar, cello and harp), and
calls the commands as an agent does over MCP (tools `asset_generate`, `asset_add` and the rest)
or JSON-RPC. `expected.txt` holds each call and its reply.

- `asset.generate` with `"type": "music"` and `theme` asks for music in the theme's key, tempo
  and meter, with its instruments and mood in the prompt. The library measures what comes back
  with `audio.analyze` and keeps what it measures: here 12 beats at 96 bpm in G major, four bars
  that loop on the beat. `path` copies it into the project, and `loop` writes the
  `.audio.json` settings beside it, with its `bpm`, `beat_count` and `bar_beats`.
- The same request again comes from the library with `"reused": true`; `asset.refine` makes a
  slower version linked to the first.
- The built-in `test` generator, used here, makes music in code with no model or key. A real
  project names a music service in `providers.json`, and the calls stay the same:

  ```json
  {"generators": {
    "score": {"type": "elevenlabs-music", "api_key_env": "ELEVENLABS_API_KEY",
              "license": "ElevenLabs terms of service"}
  }}
  ```

  `stability-audio` (Stable Audio 2 and 2.5) works the same way. Services hear the tempo, key
  and loop in words, so the library checks them: a reply lists in `problems` where the music
  measures otherwise.
- A composer's own track goes in with `asset.add`, with her `bpm` and `key`, which the library
  keeps (it says when the sound measures otherwise), her name and license, and `stems`: here
  the drums and the pads, made in this program with integer math (no audio files are
  committed). Each stem is music of its own, as long as the mix and linked to it.
- `asset.use` with `"stems": true` lays the track out to play: each stem in a folder named after
  the track, with its settings, and one `.audio.json` stream that plays them together, as
  `soundtrack.render` writes its stems. `audio.analyze` on the Master bus then measures the
  stems played together as the mix.
- `library.json` records where each file came from and under what license.
- The library would be `~/.slash/library`, or `SLASH_LIBRARY_DIR`; this example keeps its own
  in `library/`.
