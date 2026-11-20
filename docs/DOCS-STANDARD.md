# Docs standard

One standard for every doc in the owner's projects: SlashArcade (app, SuperD3D, ARM64 runtime), SlashEngine, and the agent docs (skills, worker briefs, playbooks). Adopted 20 Nov 2026 at the owner's request: "standardize the docs for all projects; make everything concise and organized".

## Rules for every doc

1. **One topic per file, one home per fact.** If a fact lives elsewhere, link to it; don't copy it.
2. **Lead with the point.** The first line says what the doc is for, and who or what it is for.
3. **Plain words.** Short declarative sentences, present tense. Explain a term the first time it appears, or don't use it.
4. **Lists and tables over paragraphs.** Steps are numbered; rules are bullets; lookups are tables.
5. **Rules keep their source, not their story.** Write `(the owner, 19 Nov)` after a rule. Incident stories go in history files or memory, not in instructions.
6. **Never drop a rule to save space.** Move detail into a linked reference file, and list every moved section in the PR.
7. **Commands are copy-pasteable.** Use a code block, absolute paths in agent docs, and repo-relative paths in repo docs.
8. **Names:** never write "DS". Use DSPACE (Dead Space, `deadspace`) and DSTRAND (Death Stranding, `deathstranding`).
9. **Length caps:** a skill or brief body is ≤ 800 words; a playbook is ≤ 300 words; AGENTS.md is ≤ 200 lines; a README is ≤ 120 lines. Go over only by splitting into references.

## Templates

### Skill (`SKILL.md`)
```
---
name: <kebab-name>
description: <what it does + when to use it, ≤ 60 words>
---
# <Title>
<One or two sentences: purpose and the goal it serves.>
## When to use        (omit if the description already says it)
## Steps              (numbered; each step ends in a check)
## Rules              (bullets; each with its source)
## Commands           (table: task | command)
## References         (links to references/*.md for detail)
```

### Playbook (`playbooks/<name>.md`)
```
# <Name>
Goal: <one line>
1. <step> → check: <how you know it worked>
...
Done when: <the bar, one line>
```

### Repo `README.md`
```
# <Project>
<Two lines: what it is and who it is for.>
## Quick start     (commands to build, test and run)
## Layout          (table: folder | what lives there)
## Docs            (link to docs/README.md)
## Contributing    (link)
```

### `docs/README.md` (the index)
One table listing every doc: `| Doc | What it covers | Status |`. Every new doc gets a row in the same PR.

### `docs/<topic>.md`
```
# <Topic>
<One-line summary.>
Status: current | draft | historical · Updated: <YYYY-MM-DD>
## <sections, one subject each>
```
Historical docs move to `docs/history/` and keep `Status: historical`.

### `AGENTS.md` (repo root) and `CLAUDE.md`
- AGENTS.md: the project in two lines; Build and test; Layout; Rules; PR flow; and a Reference index linking `docs/reference/*.md`. Subsystem detail lives in `docs/reference/<area>.md`, not in AGENTS.md.
- CLAUDE.md is only a pointer: `@AGENTS.md`, so every agent reads the same file.

## Checklist for a docs PR
- Each touched doc follows its template and length cap.
- No rule was dropped. The PR lists every moved section as old place → new place.
- Links resolve (`grep -o '](.*\.md)'` paths exist).
- docs/README.md lists every doc.
