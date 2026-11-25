# UI and text

UI controls, themes and text.

Status: current · Updated: 2026-11-20

Parts of this area are ported from [Godot Engine](https://godotengine.org) (MIT); see [THIRD_PARTY.md](../../THIRD_PARTY.md).

`engine/text` is fonts and text layout: `.ttf` and `.otf` files, or `.font.json` files that name
one with settings (`$cli schema font`), shaped with HarfBuzz and broken into lines that wrap.
`text::Fonts` makes glyph images with FreeType, or multichannel signed distance fields with
msdfgen for fonts that ask, and packs them into atlas pages. A font path of `""` is Roboto,
built into the engine from the fetched dependency (no font file is committed). Glyph advances
are whole pixels except in distance field fonts.

Each paragraph is ordered by the Unicode Bidirectional Algorithm with SheenBidi and split into
runs by script; glyphs are stored per line in drawing order, and right-to-left lines
(`Line::rtl`) hang their trailing spaces off the left and are trimmed there. A run shapes with
the font, then the clusters it lacks with its `.font.json` file's `fallbacks` (ordered by
`language_support` and `script_support` for the text's `LayoutOptions::language` and script),
then with fonts installed on the machine (`platform::system_fonts_for`: CoreText, DirectWrite,
fontconfig loaded with `dlopen`, Android's font list; none on the web), counted in
`TextLayout::system` and turned off by `ui.system_font_fallback`. Tests and examples that print
text layout turn them off so their output is the same on every machine, and make the fonts they
need with `text::box_font` (a TrueType font of box glyphs) rather than committing font files.
`Widget.layout_direction` mirrors layout; `examples/text-languages` is a menu in Arabic.

`engine/ui` is UI. `Widget` places a rect in its parent's (the entity listed in a `Widget`'s
`children`, or the viewport) by anchors, offsets and grow directions, turned and scaled around
`pivot`; `Label`, `ColorFill`, `Panel`, `Picture`, `NineSlice` and `ProgressBar` draw in it. A
container component (`HBoxContainer`, `GridBox`, `FlowBox`, `MarginBox`, `CenterBox`, `PanelBox` and
the rest) places the control's children instead of their anchors. `ui::layout` (`src/layout.cpp`)
works out minimum sizes, size flags and stretch ratios, wrapping, `max_lines_visible`, trimming and
alignment. Theme items come from `theme_override_*` fields, then `.theme.json` themes
(`src/theme.cpp`; a control's own and its parents' `Widget.theme`, then the project's `ui.theme`;
`$cli schema theme`), then `ui::default_theme`; `ui.theme` says which one each item came from. Style
boxes (`src/style.cpp`) are flat boxes and nine-part textures. `ui::draw_calls` (`src/draw.cpp`)
turns a layout into triangles, which `CanvasRenderer` draws above the world (`controls_drawn()`,
`boxes_drawn()`, `glyphs_drawn()`); distance field glyphs shade in `material.hlsl` through
`render::Material::field`, and `render::distance_field_coverage` is its CPU twin, compared with it
in a test. The viewport is the project's window size (`ui::settings`; windowed games and `slash-cli
run` set it to the window's each tick with `ui::set_viewport`); `ui.layout` returns every control's
rect, lines and problems, `text.measure` sizes text, and `ui::check_scene` backs `scene validate`
and exports. `examples/ui-and-text` is a pause screen and `examples/ui-containers` an inventory laid
out by containers and styled by a theme.

Buttons and focus: the `gui` system (`src/gui.cpp`, priority -900, right after `input`) sends each
tick's `input::events` to controls: hover by `pointer_mode` (`update_mouse_over`), mouse focus and
clicks, touches per finger, then keys, gamepads and actions to the control with focus and the `ui_*`
actions that move focus (`find_next_valid_focus`, `focus_neighbor` with a distance score), then
shortcuts. It keeps its state by entity ID in the world's context, so only controls with IDs take
part; `ui::save` and `ui::restore` let sessions put it back with the world. UI events (`ui::events`,
`ui::was_pressed`) are world events too. `ui::layout` lays buttons out by `ui::draw_mode`
(`layout.buttons`), `ui.press` and `ui.focus` drive them, and `ui::check_buttons` catches bad
shortcuts, groups and focus fields. `examples/ui-buttons` is a title menu worked by mouse, keys and
touch.

`CheckBox` and `ToggleSwitch` are structs derived from `Button` (reflection reads a base's fields),
so `button_of` serves every button type; layout adds their icon and an internal margin on its side
(`CheckWork` in `src/layout.cpp`). `Slider` is a horizontal slider (vertical with `vertical`) over a
range: `range_of`, `range_value` and `range_ratio` in `src/ui.cpp` convert between values and
ratios, `slider_input` in `src/gui.cpp` handles pointer and key input, and `track_sliders` snaps
values set from outside and emits their `value_changed`. Theme `icons` name PNG files or default
icons, which `src/icons.cpp` draws in code from the same shapes (`ui::builtin_icon`, `builtin:`
paths, which `CanvasRenderer` uploads like files). `examples/ui-settings` is a settings screen.

`ScrollBar` and `ScrollBox` scroll content. Layout places a container's children and bars
and records each one's
`ui::ScrollView` in `Layout::scrolls`; a control under a clip (`clip_children`, every scroll
container) gets a `Placed::clip`, which drawing cuts its triangles to and hit tests honor. In
`src/gui.cpp`, `bar_input` handles a scroll bar's input and `scroll_input`
a scroll box's (the wheel, and touch drags with a deadzone and inertia); a
container's own bars are pointer targets named `<id>/h_scroll` and `<id>/v_scroll` (`Target`),
which never take focus or emit events. The gui state keeps scrolls as doubles while the
component holds whole pixels: `absorb_scrolls` takes in changes made outside, and
`process_scrolls` glides touch drags, follows focus (`ensure_control_visible`) and holds each
scroll to the layout's range every tick. `examples/ui-scrolling` is a quest log.

`Dropdown`, `MenuButton` and `PopupList` open menus. A `PopupList` is an entity of its own, not a
control; the gui state keeps each open popup's `PopupStatus` (focused item, scroll, the held click
that opened it) by owner ID, one open at a time as embedded windows are. `ui::popup_items` gives an
owner's items, an option button's as radio items. Layout places and draws each open popup
(`Layout::popups`, `ui::PopupView`); `CanvasRenderer` draws popups over every control
(`canvas::FrameItem::Kind::Popup`). In `src/gui.cpp` the focused popup takes every event first
(`forward_to_popup`): `popup_menu_input` handles it, a left press outside marks it closing (hidden
at the end of the tick) and goes on to the controls; `show_popup`, `hide_popup` and `activate_item`
open, close and pick. `ui.popup`, `ui.select` (`ui::open_popup`, `ui::select_item`) drive them.
`examples/ui-menus` is a settings screen with a menu bar.

Tooltips and drag and drop live in the gui state: its
`TooltipStatus` holds the tooltip (the rest timer runs in world time, so ticks repeat; an open
popup's items have tooltips) and `DragStatus` the drag. The `DragAndDrop` component says what
a control gives and accepts; `ui::set_drag_forwarding` keeps a game's C++ calls by ID in a
holder snapshots leave alone. Text fields drag their selection
(`drag_attempt`, the drop caret, `end_drag`). Layout makes
`Layout::tooltip` (`TooltipLabel` in a `TooltipPanel`) and
`Layout::drag` (the preview root placed at the pointer, or a text field's text): the tooltip
draws over the popups (`canvas::FrameItem::Kind::Tooltip`), the preview under them. `ui.drag`
and `ui.tooltip` drive them. `examples/ui-drag-and-drop` is an inventory.

The gui system edits `TextField`s (`Editor` in `src/gui.cpp`, with an undo stack and caret and
selection calls) from key events
with a `unicode` character, `ime` events and the `ui_text_*`, `ui_cut`/`copy`/`paste` and
`ui_undo`/`redo` actions, which `Map::with_defaults` binds to Ctrl on a PC and Command and
Option on a Mac (`input::Keyboard`; tests pin `Keyboard::Pc`). `src/line_edit.cpp` is the
geometry they share with the layout: shaping, scrolling, carets, hit tests and drawing, on
`text::ShapedLine`'s carets, selections and word breaks (`engine/text/src/editing.cpp`).
`TextField.text` and `caret_column` are the truth; the rest stays in the gui state, and a text
change from outside resets the undo history. `ui::text_input` tells the platform where the caret
is and which on-screen keyboard to show (`SdlReader::set_text_input`), and
`ui::set_clipboard_backend` connects SDL's clipboard in a window; `ui.clipboard` reads and sets
it. `examples/text-input` is a sign-up form typed into with `input.inject`'s `text` and `ime`
events.

`TextArea` is a text field of several lines. `src/text_edit.cpp` is its geometry, shared
with the layout: `EditArea` holds the lines shaped and wrapped (tabs every four spaces through
`text::LayoutOptions::tab_stops`) and handles the viewport, scroll bar, hit test, minimum size
and drawing. `src/text_editor.cpp` is the editing: `gui_input`, the caret and selection
calls and the undo history (operations chained into complex steps, typing merged until a
three-second pause). `TextArea.text`, `caret_line`, `caret_column` and the scroll are the truth;
`TextEditState` keeps the rest in the gui state, and text set from outside goes in, as an
undoable step. `text_changed` and `caret_changed` are deferred to the end of the tick. Its
scroll bars are the gui system's virtual bar targets, as a `ScrollBox`'s are.
`src/syntax_highlighter.cpp` colors lines for the `SyntaxColors`
component, and `text_edit_search` (in `src/text_edit.cpp`) searches a `TextArea` for
`search_text` and `ui.search`. `EditArea` sizes gutters, draws and hit tests them
(a `CodeEditor`'s `main_gutter` and `line_numbers`, then the `TextArea`'s own), and tags each
line with the line it came from (`mark_lines`, `moved_lines`) so the editor's `move_line_marks`
keeps breakpoints and gutter entries on their lines. `examples/text-editor` writes a letter and
`examples/code-editor` colors, numbers, searches and scrolls code.

`src/accessibility.cpp` (`slash/ui/accessibility.hpp`) builds the
accessibility tree its controls fill, base class first
(the widget's, then the control's class): roles, names (a button's text, a text
field's placeholder), values, checked states, ranges, flags, bounds, actions and related controls
from `Widget`'s `accessibility_*` fields, with open popup menus and their items.
`ui::accessibility_action` runs the actions controls add (click, focus, increment,
set_value, scrolling), `ui::accessibility_problems` the accessibility warnings, and
`ui.accessibility` and `ui.accessibility_action` serve them. `ui.accessibility_support` says when
controls serve a screen reader (`ui::accessibility_enabled`), and while they do `focus_mode`
`"accessibility"` (`MenuButton`'s and `ScrollBar`'s) takes focus. `engine/accessibility` hands
the tree to the system's screen reader, through AccessKit's C library (accesskit-c, which
`cmake/Dependencies.cmake` builds with cargo from its `Cargo.lock`; builds where cargo or the
target's Rust standard library isn't installed leave it out): `accessibility::snapshot` turns the
tree into nodes with node IDs from entities (`node_id`), `changed` picks what an update sends,
`make_node` and `make_update` (`slash/accessibility/accesskit.hpp`) make AccessKit's, and
`Backend::attach` puts the subclassing adapter on the window's HWND (before it is shown:
`platform::Config::hidden`) or NSWindow, or the AT-SPI adapter on Linux. Games and `slash-cli
run` call `Backend::update` after each tick, which runs queued requests through
`ui::accessibility_action` and sets `ui::Settings::screen_reader_active`. The `crate_licenses`
test holds every crate to the allowed licenses and THIRD_PARTY.md's crate table;
`atspi_screen_reader` reads and presses a menu through AT-SPI under `dbus-run-session` (needs
`at-spi2-core`). Browsers have no AccessKit adapter, so on the web `Backend::attach` mirrors the
tree as WAI-ARIA elements over the canvas (`src/web.cpp`), hidden and out of the pointer's way,
which the page's screen reader reads: `dom_element` maps a node to its role and attributes and
`dom_changes` says what the page adds, changes and removes (`slash/accessibility/web.hpp`,
`src/dom.cpp`, the same on every platform so tests read it natively), and the clicks and focus
the elements get come back through `dom_requests`. Phones keep elements of their own the same way
(`slash/accessibility/mobile.hpp`, `src/mobile.cpp`): on Android `src/android.cpp` hands
`android_changes`' JSON through JNI to `org.slashengine.accessibility.Bridge`
(`engine/accessibility/android/`, compiled by Android exports), an `AccessibilityNodeProvider` of
virtual views on SDL's surface that answers TalkBack's touch exploration and queues its actions
for `android_requests`; on iOS `src/ios.mm` puts a `UIAccessibilityElement` per node
(`ios_element`'s label, value, hint and traits) in SDL's view, and VoiceOver's gestures become
requests through `ios_request`. `examples/accessibility` works a pause menu as a screen reader
would.
