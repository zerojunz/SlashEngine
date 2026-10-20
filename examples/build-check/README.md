# Checking gameplay code

`build.check` compiles the game's C++ files that changed since the last check or build, without
linking or loading anything, and returns every error and warning as data. The project's
[src/](src/game.cpp) has two files that both include [src/bank.hpp](src/bank.hpp);
[main.cpp](main.cpp) edits them as an agent would:

1. The first check compiles both files, `"ok": true`. The next has nothing to compile:
   `"checked": []`.
2. A change to `bank.hpp` makes the next check compile both files again, since both include it.
3. A misspelled field and an unused variable in `src/game.cpp`: only that file is compiled. The
   error comes back with its file, line and column and the compiler's fix, as edits:
   `"fix": [{"column": 32, "end_column": 37, "line": 18, "text": "income"}]` (replace columns
   32 up to 37 of line 18 with `income`). The warning has its message too.
4. The agent applies the fix from the data, removes the variable and checks again: `"ok": true`.
5. `gameplay.build` only links the objects the checks compiled and swaps the code in; the scene
   then runs with it.

- An error also has the compiler's `message` and, when it adds them, `notes` such as where a name
  it suggests is declared. They are the compiler's own words, which differ between GCC and Clang,
  so this example prints only where each error is and its fix.
- Columns count bytes from 1, and an edit's range ends before `end_column`; `end_line` and `file`
  appear when the edit ends on another line or is in another file.
- A file that failed is compiled again by the next check, until it compiles. One that compiled
  is not, so its warnings come back only after it changes.
- `slash-cli build --check` does the same from a shell, exiting 1 when there are errors.
