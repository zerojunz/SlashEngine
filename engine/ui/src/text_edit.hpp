#pragma once

#include "slash/input/input.hpp"
#include "slash/text/text.hpp"
#include "slash/ui/ui.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// What the gui system and the layout share about TextEdits: the state Godot's TextEdit keeps
// between events (TextEditState), its text shaped into wrapped rows with the geometry Godot works
// out from them (TextArea, text_edit.cpp: scene/gui/text_edit.cpp's Text class, the line
// wrapping and viewport functions, get_line_column_at_pos, _get_column_x_offset_for_line,
// _update_scrollbars, get_minimum_size and NOTIFICATION_DRAW), and the editing itself
// (text_editor.cpp: gui_input, the caret and selection functions and the undo history).
namespace slash::ui {

// Godot's TextEdit::SelectionMode.
enum class SelectionMode : std::uint8_t { None, Shift, Pointer, Word, Line };
// Godot's TextEdit::EditAction: what the open undo step is gathering.
enum class EditAction : std::uint8_t { None, Typing, Backspace, Delete };

// What a TextEdit keeps between events, as Godot's TextEdit members, for its one caret.
struct TextEditState {
    struct Selection {
        bool active = false;
        int origin_line = 0;
        int origin_column = 0;
        int origin_last_fit_x = 0;
        int word_begin_column = 0;
        int word_end_column = 0;
        friend bool operator==(const Selection&, const Selection&) = default;
    };
    // Its line and column mirror the component's caret_line and caret_column.
    struct Caret {
        Selection selection;
        int last_fit_x = 0; // the x the caret keeps when it moves up and down
        int line = 0;
        int column = 0;
        friend bool operator==(const Caret&, const Caret&) = default;
    };
    // TextOperation: one insertion or removal in the undo history. Chained ones undo together.
    struct Operation {
        enum class Type : std::uint8_t { None, Insert, Remove };
        Type type = Type::None;
        int from_line = 0;
        int from_column = 0;
        int to_line = 0;
        int to_column = 0;
        std::u32string text;
        std::uint32_t prev_version = 0;
        std::uint32_t version = 0;
        bool chain_forward = false;
        bool chain_backward = false;
        Caret start_caret;
        Caret end_caret;
        friend bool operator==(const Operation&, const Operation&) = default;
    };

    // The undo history.
    std::vector<Operation> undo_stack;
    Operation current_op;
    double idle_left = -1.0; // seconds until idle_detect closes the step; less than 0 when off
    int complex_operation_count = 0;
    int undo_stack_pos = -1; // the step redo would do next; -1 for none
    std::uint32_t version = 0;
    EditAction current_action = EditAction::None;
    bool pending_action_end = false;
    bool in_action = false;
    bool next_operation_is_complex = false;
    std::u32string cut_copy_line; // the lines copied with nothing selected, pasted as lines
    // The input method's composition, shown at the caret.
    std::u32string ime_text;
    int ime_selection_start = 0;
    int ime_selection_length = 0;
    // The caret's blink, and clicks.
    double blink_timer = 0.0;
    double last_double_click = -1.0; // seconds of world time
    double click_held = -1.0; // seconds until a held drag selection follows the pointer again
    Vec2 last_double_click_position;
    Vec2 mouse; // the pointer in its rect, mirrored when it lays out right to left
    // A finger dragging the text.
    Vec2 drag_accum;
    Vec2 drag_from; // the scroll it started from: horizontal pixels, vertical rows
    Caret caret;
    // Where text dragged over it would drop, drawn as a second caret (Godot's drag caret).
    std::optional<Caret> drag_caret;
    SelectionMode selecting_mode = SelectionMode::None;
    text::Direction input_direction = text::Direction::Ltr;
    bool dragging_selection = false;
    bool overtype_mode = false;
    bool draw_caret = true;
    bool show_keyboard = false;
    bool touch_dragging_starting = false;
    bool touch_dragging_in_progress = false;
    bool selection_drag_attempt = false; // a press landed on the selection, which a drag may take
    bool drag_caret_force_displayed = false;
    // The text as last seen, so a change made from outside is undone as set_text's is.
    bool known = false;
    std::string known_text;
    // Signals Godot defers to the end of the frame, in the order they were queued:
    // "text_changed" and "caret_changed".
    std::vector<std::string> deferred;

    friend bool operator==(const TextEditState&, const TextEditState&) = default;
};

// The selection's ends in text order, as get_selection_from_line, _from_column, _to_line and
// _to_column: the caret's position twice when nothing is selected.
struct SelectionRange {
    int from_line = 0;
    int from_column = 0;
    int to_line = 0;
    int to_column = 0;
};
[[nodiscard]] SelectionRange selection_range(const TextEditState::Caret& caret);

// The TextEditState the gui system keeps for a TextEdit, or nullptr (gui.cpp).
[[nodiscard]] const TextEditState* text_edit_state(const world::World& world, entt::entity entity);

// A TextEdit's lines split at "\n", without "\r", as Godot's TextEdit keeps them.
[[nodiscard]] std::vector<std::u32string> split_lines(std::string_view text);
[[nodiscard]] std::string join_lines(const std::vector<std::u32string>& lines);

// A TextEdit's lines shaped and wrapped for its rect, and where Godot's TextEdit puts things in
// them. Rows are a line's wrapped parts; scroll_vertical counts rows, scroll_horizontal pixels.
class TextArea {
public:
    TextArea(text::Fonts& fonts, const TextEditView& view, Vec2 size, const world::TextEdit& edit,
             std::vector<std::u32string> lines);
    ~TextArea();
    TextArea(const TextArea&) = delete;
    TextArea& operator=(const TextArea&) = delete;
    TextArea(TextArea&&) noexcept;
    TextArea& operator=(TextArea&&) = delete;

    // ---- Text ----

    [[nodiscard]] int line_count() const { return static_cast<int>(lines_.size()); }
    [[nodiscard]] const std::u32string& line(int line) const;
    [[nodiscard]] int length(int line) const { return static_cast<int>(this->line(line).size()); }
    [[nodiscard]] const std::vector<std::u32string>& lines() const { return lines_; }
    // _base_insert_text: inserts at a position (padding the line with spaces up to it),
    // splitting lines at "\n" and dropping "\r"; returns where the inserted text ends.
    std::pair<int, int> insert(int line, int column, std::u32string_view inserted);
    // Replaces the lines, as Text::clear and insert do.
    void set_lines(std::vector<std::u32string> lines);
    // _base_remove_text and _base_get_text.
    void remove(int from_line, int from_column, int to_line, int to_column);
    [[nodiscard]] std::u32string get(int from_line, int from_column, int to_line,
                                     int to_column) const;
    // Text::set_ime: a line shown with the input method's text in it, or as it is (empty).
    void set_ime(int line, std::u32string shown);
    void clear_ime();
    // The line as shown: with the input method's text if it has some (get_text_with_ime).
    [[nodiscard]] const std::u32string& shown(int line) const;
    // _using_placeholder: the text is empty and nothing is being composed.
    [[nodiscard]] bool using_placeholder() const;

    // ---- Characters and words ----

    // shaped_text_get_word_breaks of the line: [start, end) pairs in a flat list.
    [[nodiscard]] std::vector<int> word_breaks(int line) const;
    // shaped_text_next_character_pos and _prev_character_pos.
    [[nodiscard]] int next_character(int line, int column) const;
    [[nodiscard]] int previous_character(int line, int column) const;
    // Whether a column is between two user-perceived characters (a character break).
    [[nodiscard]] bool at_character_break(int line, int column) const;
    // shaped_text_get_dominant_direction_in_range.
    [[nodiscard]] text::Direction dominant_direction(int line, int from, int to) const;

    // ---- Wrapping ----

    [[nodiscard]] bool wrapping() const { return wrapping_; }
    [[nodiscard]] int wrap_count(int line) const;
    [[nodiscard]] int wrap_index_at_column(int line, int column) const;
    // The [start, end) columns of each row.
    [[nodiscard]] const std::vector<std::pair<int, int>>& wrap_ranges(int line) const;
    [[nodiscard]] const text::ShapedLine& row(int line, int wrap_index) const;
    [[nodiscard]] int total_rows() const;

    // ---- Viewport ----

    // get_line_height: the tallest line plus line_spacing.
    [[nodiscard]] int line_height() const;
    [[nodiscard]] int visible_line_count() const;
    [[nodiscard]] int first_visible_line() const;
    [[nodiscard]] int first_visible_wrap() const { return first_visible_wrap_; }
    [[nodiscard]] int first_visible_col() const { return first_visible_col_; }
    [[nodiscard]] double v_scroll() const { return v_.value; }
    [[nodiscard]] int h_scroll() const { return static_cast<int>(h_.value); }
    [[nodiscard]] bool v_scroll_visible() const { return v_.visible; }
    [[nodiscard]] bool h_scroll_visible() const { return h_.visible; }
    // set_v_scroll and set_h_scroll: within what the scroll bars allow.
    void set_v_scroll(double value);
    void set_h_scroll(double value);
    // _update_scrollbars after the text or its state changed, then the line wrapping again if
    // the vertical scroll bar came or went, until both settle. Godot wraps again only when the
    // size changes; this wraps to the room the text has whenever it changes.
    void refresh();
    [[nodiscard]] std::pair<int, int> next_visible_line_index_offset_from(int line_from,
                                                                          int wrap_index_from,
                                                                          int visible_amount) const;
    [[nodiscard]] double scroll_pos_for_line(int line, int wrap_index) const;
    [[nodiscard]] int visible_line_count_in_range(int from_line, int to_line) const;
    [[nodiscard]] int last_full_visible_line() const;
    [[nodiscard]] int last_full_visible_line_wrap_index() const;
    void set_line_as_first_visible(int line, int wrap_index);
    void set_line_as_last_visible(int line, int wrap_index);
    // adjust_viewport_to_caret: scrolls so the caret (and the composition, or a one-line
    // selection when maximize_selection) shows.
    void adjust_viewport_to_caret(const TextEditState& state, bool maximize_selection = false);

    // ---- Positions ----

    // get_line_column_at_pos: the [column, line] a point in its rect falls on (already
    // mirrored if it lays out right to left), or [-1, -1] past the text unless clamped.
    [[nodiscard]] std::pair<int, int> line_column_at(Vec2 point, bool clamp_line, bool clamp_column,
                                                     bool mid_grapheme) const;
    // _get_column_x_offset_for_line: a caret's x in its row.
    [[nodiscard]] int column_x_offset(int line, int column, text::Direction input_direction) const;
    // _get_char_pos_for_line: the column at an x in a row.
    [[nodiscard]] int char_pos_for_line(int x, int line, int wrap_index, bool mid_grapheme) const;
    // The theme's style box for its editable state (_get_current_stylebox).
    [[nodiscard]] const StyleBox& style() const;
    // get_minimum_size.
    [[nodiscard]] Vec2 minimum_size() const;

    // ---- Drawing ----

    struct Draw {
        std::vector<Box> under; // style boxes, the current line, the selection, tab and space marks
        LabelText text;
        std::vector<Box> over; // carets, the composition's underline, the scroll bars
    };
    // NOTIFICATION_DRAW, and what it shows for ui.layout.
    void draw(const world::TextEdit& edit, const TextEditState& state, bool focused, Draw& out,
              TextEditView& view) const;

    [[nodiscard]] const std::string& error() const { return error_; }
    [[nodiscard]] Vec2 size() const { return size_; }
    [[nodiscard]] const TextEditView& view() const { return *view_; }

private:
    struct LineData;
    // A scroll bar, as Godot's Range with min 0 and step 0.
    struct Bar {
        double value = 0.0;
        double max = 0.0;
        double page = 0.0;
        bool visible = false;
        void set_value(double to);
        void set_max(double to);
        void set_page(double to);
    };
    [[nodiscard]] const LineData& data(int line) const;
    [[nodiscard]] const LineData& placeholder_data() const;
    void shape(LineData& data, std::u32string_view source, bool placeholder) const;
    void invalidate(int line);
    void update_scrollbars();
    void scroll_moved();
    // _update_wrap_at_column: whether the width lines wrap at changed.
    bool update_wrap_at_column();
    [[nodiscard]] int control_height() const;
    [[nodiscard]] double v_scroll_offset() const;
    [[nodiscard]] double visible_lines_offset() const;
    [[nodiscard]] int max_width() const;
    [[nodiscard]] int row_count(int line) const; // with the placeholder's rows
    [[nodiscard]] Vec2 v_scroll_min() const;
    [[nodiscard]] Vec2 h_scroll_min() const;
    void draw_scroll_bar(bool vertical, std::vector<Box>& out) const;

    text::Fonts* fonts_;
    const TextEditView* view_;
    Vec2 size_;
    bool editable_ = true;
    bool wrapping_ = false;
    text::Autowrap autowrap_ = text::Autowrap::WordSmart;
    text::Direction direction_ = text::Direction::Auto;
    std::string language_;
    std::u32string placeholder_;
    bool scroll_past_end_ = false;
    bool fit_content_height_ = false;
    bool fit_content_width_ = false;
    std::string font_;
    float font_height_ = 0.0F;
    float space_width_ = 0.0F; // the advance of ' '
    float m_width_ = 0.0F;     // and of 'm', for a block caret at the end of a line
    float wrap_at_ = 0.0F;
    std::vector<std::u32string> lines_;
    std::vector<std::u32string> ime_; // each line's text with the composition, or empty
    mutable std::vector<std::unique_ptr<LineData>> data_;
    mutable std::unique_ptr<LineData> placeholder_data_;
    Bar v_;
    Bar h_;
    int first_visible_line_ = 0;
    int first_visible_wrap_ = 0;
    int first_visible_col_ = 0;
    Vec2 content_size_;
    mutable std::string error_;
};

// The TextArea a TextEdit shows: its text, with the input method's composition at the caret.
[[nodiscard]] TextArea text_area_for(text::Fonts& fonts, const TextEditView& view, Vec2 size,
                                     const world::TextEdit& edit, const TextEditState& state);
// The state a TextEdit draws with: the gui system's, or, before it catches up with a text or
// caret changed from outside, the component's caret in fallback.
[[nodiscard]] const TextEditState& shown_state(const world::TextEdit& edit,
                                               const TextEditState* state, const TextArea& area,
                                               TextEditState& fallback);

// ---- Editing (text_editor.cpp) ----

// A signal a TextEdit emitted at once: "text_set" or "lines_edited_from" (with its lines).
struct TextEditSignal {
    std::string kind;
    int from_line = 0;
    int to_line = 0;
};

// What editing a TextEdit needs from the gui system.
struct TextEditContext {
    world::World* world = nullptr; // for the clipboard
    const input::Map* map = nullptr;
    double now = 0.0;      // seconds of world time, for triple clicks
    bool focused = false;  // it has focus
    bool dragging = false; // a drag with data is going on (Godot's drag_action)
    bool can_drop = false; // and the TextEdit would take it at the event's point
    std::vector<TextEditSignal>* signals = nullptr;
};

// Each call first brings the state up to the component: text changed from outside (a command
// or gameplay code) is set as set_text does, as an undoable step keeping the component's caret,
// and a caret moved from outside drops the selection. Each writes the text, caret and scroll
// back to the component, and queues text_changed and caret_changed on the state's deferred.

void sync_text_edit(const TextEditContext& context, world::TextEdit& edit, TextEditState& state,
                    TextArea& area);
// TextEdit::gui_input for one event, with its position and motion in the TextEdit's rect:
// whether it took the event (accept_event).
bool text_edit_input(const TextEditContext& context, world::TextEdit& edit, TextEditState& state,
                     TextArea& area, const input::Event& event, Vec2 local, Vec2 relative);
// unhandled_key_input: a character typed with modifiers no shortcut took.
bool text_edit_unhandled_key(const TextEditContext& context, world::TextEdit& edit,
                             TextEditState& state, TextArea& area, const input::Event& event);
// NOTIFICATION_FOCUS_ENTER and NOTIFICATION_FOCUS_EXIT; leaving needs the area only to commit
// a composition.
void text_edit_focus_enter(const world::TextEdit& edit, TextEditState& state);
void text_edit_focus_exit(const TextEditContext& context, world::TextEdit& edit,
                          TextEditState& state, TextArea* area);
// NOTIFICATION_OS_IME_UPDATE: the input method's composition changed.
void text_edit_ime_update(const TextEditContext& context, world::TextEdit& edit,
                          TextEditState& state, TextArea& area, std::u32string text,
                          int selection_start, int selection_length);
// _click_selection_held, with the pointer in the TextEdit's rect.
void text_edit_click_held(const TextEditContext& context, world::TextEdit& edit,
                          TextEditState& state, TextArea& area, Vec2 local);
// What TextEditContext::dragging changes in gui_input: with a drag going on, the pointer moves
// the drag caret where the drop would go (when can_drop) instead of selecting.
// drop_data, with the drop's point in the TextEdit's rect: the text goes in there and is
// selected; dragged from its own selection it moves there, or is copied with Command (Ctrl)
// held. Whether it went in (not on its own selection), so the TextEdit takes focus.
bool text_edit_drop_text(const TextEditContext& context, world::TextEdit& edit,
                         TextEditState& state, TextArea& area, Vec2 local, std::u32string_view text,
                         bool command);
// NOTIFICATION_DRAG_END after its selection was dropped elsewhere: the selection goes, unless
// it can't be edited or Command (Ctrl) was held to copy it.
void text_edit_drag_end(const TextEditContext& context, world::TextEdit& edit, TextEditState& state,
                        TextArea& area, bool command);
// A tick's timers: the caret's blink, idle_detect closing an undo step, and click_select_held,
// for which it returns true when a drag selection should follow the held pointer
// (text_edit_click_held).
bool text_edit_tick(const world::TextEdit& edit, TextEditState& state, bool focused,
                    double tick_seconds, bool left_held);

} // namespace slash::ui
