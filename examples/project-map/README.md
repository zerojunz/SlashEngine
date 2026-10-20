# Project map

Finding code and content without reading every file. The project is a small game with C++
sources (two components, two systems, an input action and events), a scene, a prefab and a
storyboard. [calls.jsonl](calls.jsonl) asks `project.map` for the whole map, then where one
event is emitted and read, what a header change touches and what uses a prefab, and misspells a
path. [results.jsonl](results.jsonl) holds the responses, line for line.

```bash
slash-cli rpc --project . < calls.jsonl
```

- The map is made from the files on every call, so it is never out of date. The sources are read
  as text, not compiled: `collect` and `move_runner` are listed with `"loaded": false` because
  the code isn't built, and with where their functions are defined.
- Each source lists its types and functions by line, the components it names, the systems it
  adds, the events it emits or reads and the actions it reads. Each scene lists its entities'
  components, prefab ones included, and the storyboard entries it stands for.
- `find` keeps only the names that contain it, and the files that name them: `coin_collected`
  is emitted in `coins.cpp` and read in `runner.cpp`.
- `paths` keeps the files in those paths and those that extend, use or include them.
