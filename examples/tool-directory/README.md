# Tool directory

What an MCP client starts with, and how an agent loads the rest of the engine's commands only
when a task needs them. `slash-mcp` offers the commands most tasks need as tools, plus
`tools_load`: with no arguments it lists every command by group; with `groups` or `commands` it
adds them to the tools, returns their params and sends `notifications/tools/list_changed` so the
client lists the tools again. OpenAI's API takes at most 128 tools and some clients fewer, and
each tool definition is read again on every turn, so this keeps sessions cheap and works with
every client. `slash-mcp --tools all` lists every command from the start instead.

[main.cpp](main.cpp) lists the starting tools, reads the directory, loads the `audio` group and
`light.sample`, and asks for a misspelled group, which is named back. Its output is in
[expected.txt](expected.txt).

See [MCP](../../docs/reference/protocols.md#mcp).
