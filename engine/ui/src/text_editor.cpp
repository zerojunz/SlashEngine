#include "text_edit.hpp"

#include "slash/core/text.hpp"
#include "slash/input/input.hpp"
#include "slash/ui/ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// TextEdit's editing, as Godot's scene/gui/text_edit.cpp: gui_input and unhandled_key_input, the
// caret and selection functions, the text operations and the undo history, for its one caret.
// Each function names the Godot function it follows. Multiple carets, drag and drop, the context
// menu, selection handles, inertial and smooth scrolling and Alt codes aren't here.
namespace slash::ui {
namespace {

using Operation = TextEditState::Operation;

// gui/timers/text_edit_idle_detect_sec: typing this long after the last edit starts a new step.
constexpr double kIdleSeconds = 3.0;
// gui/common/text_edit_undo_stack_max_size.
constexpr std::size_t kUndoStackMax = 1024;
// A third click this soon after a double click, this near it, selects a line.
constexpr double kTripleClickSeconds = 0.6;
constexpr float kTripleClickTolerance = 5.0F;
// click_select_held's wait: a drag selection held still follows the pointer this often, so it
// scrolls.
constexpr double kClickHeldSeconds = 0.05;
constexpr double kTimerEpsilon = 1e-9;

// Godot's is_whitespace.
bool is_whitespace(char32_t c) {
    return c == U' ' || c == 0x00A0U || c == 0x1680U || (c >= 0x2000U && c <= 0x200AU) ||
           c == 0x202FU || c == 0x205FU || c == 0x3000U || c == 0x2028U || c == 0x2029U ||
           (c >= 0x0009U && c <= 0x000DU) || c == 0x0085U;
}

// _is_line_col_in_range.
bool in_range(int line, int column, int from_line, int from_column, int to_line, int to_column,
              bool include_edges) {
    if (line >= from_line && line <= to_line && (line > from_line || column > from_column) &&
        (line < to_line || column < to_column)) {
        return true;
    }
    return include_edges && ((line == from_line && column == from_column) ||
                             (line == to_line && column == to_column));
}

// ---- The undo history, which needs no text ----

void start_action(TextEditState& s, EditAction action);

// _push_current_op: the operation being gathered becomes a step in the history.
void push_current_op(TextEditState& s) {
    if (s.pending_action_end) {
        start_action(s, EditAction::None);
        return;
    }
    if (s.current_op.type == Operation::Type::None) {
        return;
    }
    if (s.next_operation_is_complex) {
        s.current_op.chain_forward = true;
        s.next_operation_is_complex = false;
    }
    s.undo_stack.push_back(s.current_op);
    s.current_op.type = Operation::Type::None;
    s.current_op.text.clear();
    s.current_op.chain_forward = false;
    if (s.undo_stack.size() > kUndoStackMax) {
        s.undo_stack.erase(s.undo_stack.begin());
        if (s.undo_stack_pos > 0) {
            --s.undo_stack_pos;
        }
    }
}

// begin_complex_operation: the operations until the matching end undo as one.
void begin_complex(TextEditState& s) {
    push_current_op(s);
    if (s.complex_operation_count == 0) {
        s.next_operation_is_complex = true;
        s.current_op.start_caret = s.caret;
    }
    ++s.complex_operation_count;
}

// end_complex_operation. Godot marks the last step of the history as part of a complex operation
// that changed nothing, so undoing it undid the operation before as well; this leaves it alone.
void end_complex(TextEditState& s) {
    push_current_op(s);
    s.complex_operation_count = std::max(s.complex_operation_count - 1, 0);
    if (s.complex_operation_count > 0) {
        return;
    }
    if (s.next_operation_is_complex) {
        s.next_operation_is_complex = false;
        return;
    }
    if (s.undo_stack.empty()) {
        return;
    }
    Operation& back = s.undo_stack.back();
    back.end_caret = s.caret;
    if (back.chain_forward) {
        back.chain_forward = false;
        return;
    }
    back.chain_backward = true;
}

// start_action: typing, backspacing or deleting goes on as one step until something else
// happens.
void start_action(TextEditState& s, EditAction action) {
    if (s.current_action != action) {
        if (s.current_action != EditAction::None) {
            s.in_action = false;
            s.pending_action_end = false;
            end_complex(s);
        }
        if (action != EditAction::None) {
            s.in_action = true;
            begin_complex(s);
        }
    } else if (s.current_action != EditAction::None) {
        s.pending_action_end = false;
    }
    s.current_action = action;
}

void end_action(TextEditState& s) {
    if (s.current_action != EditAction::None) {
        s.pending_action_end = true;
    }
}

// _clear_redo: the steps undone can't be redone once something else is done.
void clear_redo(TextEditState& s) {
    if (s.undo_stack_pos < 0) {
        return;
    }
    push_current_op(s);
    const auto from =
        std::min(static_cast<std::size_t>(std::max(s.undo_stack_pos, 0)), s.undo_stack.size());
    s.undo_stack.erase(s.undo_stack.begin() + static_cast<std::ptrdiff_t>(from),
                       s.undo_stack.end());
    s.undo_stack_pos = -1;
}

// ---- Editing ----

// One TextEdit being worked on, as Godot's TextEdit methods work on its members: its lines in
// the TextArea, its caret and history in its TextEditState and its caret, text and scroll in
// the component, which finish() brings up to date.
class Editor {
public:
    Editor(const TextEditContext& context, world::TextEdit& edit, TextEditState& state,
           TextArea& area)
        : context_(context), edit_(edit), s_(state), area_(area) {
        sync();
        area_.mark_lines();
    }

    // What changed goes back to the component, after the scroll bars catch up as a redraw
    // would make them.
    void finish() {
        area_.refresh();
        if (text_dirty_) {
            edit_.text = join_lines(area_.lines());
            move_line_marks();
        }
        s_.known_text = edit_.text;
        s_.known = true;
        edit_.caret_line = s_.caret.line;
        edit_.caret_column = s_.caret.column;
        edit_.scroll_vertical = area_.v_scroll();
        edit_.scroll_horizontal = area_.h_scroll();
    }

    // ---- gui_input ----

    bool gui_input(const input::Event& event, Vec2 local, Vec2 relative) {
        const double prev_v = area_.v_scroll();
        const int prev_h = area_.h_scroll();
        const bool emulated = event.device == input::kEmulatedDevice;
        bool accepted = false;

        if (event.type == input::EventType::MouseButton && !emulated) {
            s_.mouse = mirrored(local);
            const Vec2 mpos = mirrored({std::trunc(local.x), std::trunc(local.y)});
            if (event.pressed) {
                mouse_pressed(event, mpos, local);
            } else {
                if (!s_.ime_text.empty()) {
                    return false; // mouse up is ignored while composing
                }
                if (event.button == input::MouseButton::Left) {
                    if (!context_.dragging && s_.selection_drag_attempt && over_selection(mpos)) {
                        // Not a drag after all: the click places the caret.
                        s_.selection_drag_attempt = false;
                        deselect();
                        const auto [column, line] =
                            area_.line_column_at(mpos, true, true, edit_.caret_mid_grapheme);
                        set_caret_line(line, false, -1);
                        set_caret_column(column, true);
                    }
                    s_.dragging_selection = false;
                    s_.selecting_mode = SelectionMode::None;
                    s_.click_held = -1.0;
                    if (edit_.editable) {
                        show_virtual_keyboard();
                    }
                }
            }
        }

        if (event.type == input::EventType::ScreenTouch && !emulated) {
            if (event.pressed && !event.double_click) {
                s_.touch_dragging_starting = true;
                s_.touch_dragging_in_progress = false;
                s_.drag_accum = {};
                s_.drag_from = {static_cast<float>(prev_h), static_cast<float>(prev_v)};
            }
            // A double tap selects a word as it lands; a tap places the caret as it lifts, if
            // it didn't drag.
            if ((event.pressed && event.double_click) ||
                (!event.pressed && !event.double_click && !s_.touch_dragging_in_progress)) {
                const Vec2 touch_pos = mirrored({std::trunc(local.x), std::trunc(local.y)});
                reset_caret_blink_timer();
                apply_ime();
                const auto [col, line] =
                    area_.line_column_at(touch_pos, true, true, edit_.caret_mid_grapheme);
                if (const auto [gutter, row] = area_.gutter_at(touch_pos); gutter != -1) {
                    gutter_clicked(row, gutter, event.modifiers.shift);
                    return false;
                }
                if (touch_pos.x < static_cast<float>(area_.text_start())) {
                    return false;
                }
                if (!event.double_click) {
                    deselect();
                    push_current_op(s_);
                    set_caret_line(line, false, -1);
                    set_caret_column(col, false);
                    adjust_viewport_to_caret();
                } else {
                    s_.mouse = mirrored(local); // the mouse Godot emulates is at the finger
                    s_.selecting_mode = SelectionMode::Word;
                    update_selection_mode_word(true);
                }
            }
            if (!event.pressed) {
                if (!s_.touch_dragging_in_progress && s_.ime_text.empty()) {
                    s_.dragging_selection = false;
                    s_.selecting_mode = SelectionMode::None;
                    s_.click_held = -1.0;
                    if (edit_.editable) {
                        show_virtual_keyboard();
                    }
                }
                s_.touch_dragging_starting = false;
                s_.touch_dragging_in_progress = false;
            }
        }

        if (event.type == input::EventType::ScreenDrag && !emulated) {
            if (s_.touch_dragging_starting) {
                // A finger drags the text.
                s_.touch_dragging_in_progress = true;
                s_.drag_accum = s_.drag_accum - relative;
                const float line_height = std::max(1.0F, static_cast<float>(area_.line_height()));
                const float vertical_line_offset = s_.drag_accum.y / line_height;
                area_.set_h_scroll(s_.drag_from.x + s_.drag_accum.x);
                area_.set_v_scroll(s_.drag_from.y + vertical_line_offset);
                accepted = true;
                selection_mode_update();
            } else {
                // After a double tap, it selects word by word.
                s_.mouse = mirrored(local);
                drag_or_mouse_motion(true);
            }
        }

        if (event.type == input::EventType::MouseMotion && !emulated) {
            s_.mouse = mirrored(local);
            constexpr std::uint8_t kLeft =
                1U << (static_cast<unsigned>(input::MouseButton::Left) - 1U);
            drag_or_mouse_motion((event.button_mask & kLeft) != 0);
        }

        if (area_.v_scroll() != prev_v || area_.h_scroll() != prev_h) {
            accepted = true;
        }

        if (event.type == input::EventType::Key || event.type == input::EventType::Action) {
            accepted = key_input(event) || accepted;
        }
        return accepted;
    }

    // unhandled_key_input: a character typed with modifiers no shortcut took.
    bool unhandled_key_input(const input::Event& event) {
        if (event.type != input::EventType::Key || !event.pressed) {
            return false;
        }
        if (context_.focused && edit_.editable && event.unicode >= 0x20U) {
            handle_unicode_input(event.unicode);
            return true;
        }
        return false;
    }

    // NOTIFICATION_OS_IME_UPDATE.
    void ime_update(std::u32string text, int selection_start, int selection_length) {
        if (!context_.focused || !edit_.editable) {
            return;
        }
        if (s_.ime_text == text && s_.ime_selection_start == selection_start &&
            s_.ime_selection_length == selection_length) {
            return;
        }
        s_.ime_text = std::move(text);
        s_.ime_selection_start = selection_start;
        s_.ime_selection_length = selection_length;
        if (!s_.ime_text.empty() && has_selection()) {
            s_.selecting_mode = SelectionMode::None;
            delete_selection();
        }
        update_ime_text();
        adjust_viewport_to_caret();
    }

    // _click_selection_held: the selection follows the pointer held still, so it scrolls.
    void click_selection_held(Vec2 local) {
        s_.mouse = mirrored(local);
        selection_mode_update();
    }

    // apply_ime: what the input method composed goes in at the caret.
    void apply_ime() {
        if (s_.ime_text.empty()) {
            return;
        }
        const std::u32string composed = s_.ime_text;
        cancel_ime();
        insert_text_at_caret(composed);
    }

private:
    // ---- Keeping in step with the component ----

    // Text changed from outside (a command or gameplay code) is set as set_text does, as one
    // undoable step, keeping the component's caret; a caret moved from outside drops the
    // selection. The first time, the text goes in quietly.
    void sync() {
        if (s_.known && s_.known_text == edit_.text) {
            if (edit_.caret_line != s_.caret.line || edit_.caret_column != s_.caret.column) {
                deselect();
                set_caret_line(edit_.caret_line, false, -1);
                set_caret_column(edit_.caret_column, false);
            }
        } else {
            const bool first = !s_.known;
            const int line = edit_.caret_line;
            const int column = edit_.caret_column;
            s_.ime_text.clear();
            s_.ime_selection_start = 0;
            s_.ime_selection_length = 0;
            area_.set_lines(split_lines(first ? std::string() : s_.known_text));
            quiet_ = first;
            if (!first || !edit_.text.empty()) {
                set_text(to_utf32(edit_.text));
            }
            deselect();
            set_caret_line(line, false, -1);
            set_caret_column(column, false);
            quiet_ = false;
        }
        if (!edit_.selecting_enabled) {
            deselect();
        }
    }

    // ---- Signals ----

    // Godot's call_deferred signals: once each until the tick ends.
    void defer(std::string_view kind) {
        if (quiet_) {
            return;
        }
        if (std::ranges::find(s_.deferred, kind) == s_.deferred.end()) {
            s_.deferred.emplace_back(kind);
        }
    }
    void emit(std::string_view kind, int from_line, int to_line) const {
        if (!quiet_ && context_.signals != nullptr) {
            context_.signals->push_back({.kind = std::string(kind),
                                         .from_line = from_line,
                                         .to_line = to_line,
                                         .line = 0,
                                         .gutter = {}});
        }
    }
    // _text_changed: text_changed at the end of the tick, but not for set_text.
    void text_changed() {
        text_dirty_ = true;
        if (!setting_text_) {
            defer("text_changed");
        }
    }
    void caret_changed() { defer("caret_changed"); }

    // ---- Text operations ----

    // _base_insert_text.
    std::pair<int, int> base_insert_text(int line, int column, std::u32string_view text) {
        const auto [end_line, end_column] = area_.insert(line, column, text);
        const text::Direction dir =
            area_.dominant_direction(end_line, end_line == line ? s_.caret.column : 0, end_column);
        if (dir != text::Direction::Auto) {
            s_.input_direction = dir;
        }
        text_changed();
        emit("lines_edited_from", line, end_line);
        return {end_line, end_column};
    }

    // _base_remove_text.
    void base_remove_text(int from_line, int from_column, int to_line, int to_column) {
        area_.remove(from_line, from_column, to_line, to_column);
        text_changed();
        // NOLINTNEXTLINE(readability-suspicious-call-argument): Godot's order, last line first.
        emit("lines_edited_from", to_line, from_line);
    }

    // _insert_text: an insertion, gathered for the history.
    std::pair<int, int> insert_text_op(int line, int column, std::u32string_view text) {
        if (!setting_text_) {
            s_.idle_left = kIdleSeconds;
        }
        clear_redo(s_);
        const auto [end_line, end_column] = base_insert_text(line, column, text);
        Operation op;
        op.type = Operation::Type::Insert;
        op.from_line = line;
        op.from_column = column;
        op.to_line = end_line;
        op.to_column = end_column;
        op.text = text;
        op.version = ++s_.version;
        op.start_caret = s_.next_operation_is_complex ? s_.current_op.start_caret : s_.caret;
        op.end_caret = s_.caret;
        op.prev_version = s_.current_op.version;
        push_current_op(s_);
        s_.current_op = std::move(op);
        return {end_line, end_column};
    }

    // _remove_text.
    void remove_text_op(int from_line, int from_column, int to_line, int to_column) {
        if (!setting_text_) {
            s_.idle_left = kIdleSeconds;
        }
        clear_redo(s_);
        std::u32string removed = area_.get(from_line, from_column, to_line, to_column);
        base_remove_text(from_line, from_column, to_line, to_column);
        Operation op;
        op.type = Operation::Type::Remove;
        op.from_line = from_line;
        op.from_column = from_column;
        op.to_line = to_line;
        op.to_column = to_column;
        op.text = std::move(removed);
        op.version = ++s_.version;
        op.start_caret = s_.next_operation_is_complex ? s_.current_op.start_caret : s_.caret;
        op.end_caret = s_.caret;
        op.prev_version = s_.current_op.version;
        push_current_op(s_);
        s_.current_op = std::move(op);
    }

    // _do_text_op.
    void do_text_op(const Operation& op, bool reverse) {
        bool insert = op.type == Operation::Type::Insert;
        if (reverse) {
            insert = !insert;
        }
        if (insert) {
            base_insert_text(op.from_line, op.from_column, op.text);
        } else {
            base_remove_text(op.from_line, op.from_column, op.to_line, op.to_column);
        }
    }

    // _set_text.
    void set_text(std::u32string_view text) {
        setting_text_ = true;
        set_caret_line(0, true, 0);
        set_caret_column(0, true);
        begin_complex(s_);
        deselect();
        const int last = std::max(0, area_.line_count() - 1);
        remove_text_op(0, 0, last, area_.length(last));
        insert_text_at_caret(text);
        end_complex(s_);
        set_caret_line(0, true, 0);
        set_caret_column(0, true);
        setting_text_ = false;
        emit("text_set", 0, 0);
    }

    // insert_text_at_caret.
    void insert_text_at_caret(std::u32string_view text) {
        begin_complex(s_);
        delete_selection();
        const int from_line = s_.caret.line;
        const int from_col = s_.caret.column;
        const auto [new_line, new_column] = insert_text_op(from_line, from_col, text);
        area_.refresh();
        offset_carets_after(from_line, from_col, new_line, new_column);
        set_caret_line(new_line, false, -1);
        set_caret_column(new_column, true);
        if (!s_.ime_text.empty()) {
            update_ime_text();
        }
        end_complex(s_);
    }

    // insert_text.
    void insert_text(std::u32string_view text, int line, int column, bool before_selection_begin,
                     bool before_selection_end) {
        if (line < 0 || line >= area_.line_count() || column < 0 || column > area_.length(line)) {
            return;
        }
        begin_complex(s_);
        const auto [new_line, new_column] = insert_text_op(line, column, text);
        offset_carets_after(line, column, new_line, new_column, before_selection_begin,
                            before_selection_end);
        end_complex(s_);
    }

    // remove_line_at.
    void remove_line_at(int line, bool move_carets_down) {
        if (line < 0 || line >= area_.line_count()) {
            return;
        }
        if (area_.line_count() == 1) {
            begin_complex(s_);
            const int length = area_.length(line);
            remove_text_op(line, 0, line, length);
            collapse_carets(line, 0, line, length, true);
            end_complex(s_);
            return;
        }
        begin_complex(s_);
        const bool is_last_line = line == area_.line_count() - 1;
        const int from_line = is_last_line ? line - 1 : line;
        const int next_line = is_last_line ? line : line + 1;
        const int from_column = is_last_line ? area_.length(from_line) : 0;
        const int next_column = is_last_line ? area_.length(next_line) : 0;
        const TextEditState::Selection& selection = s_.caret.selection;
        if ((!is_last_line && move_carets_down) || (line != 0 && !move_carets_down)) {
            // Each caret's last x, to line up with.
            if (s_.caret.line == line) {
                set_caret_column(s_.caret.column, false);
            }
            if (has_selection() && selection.origin_line == line) {
                set_selection_origin_column(selection.origin_column);
            }
        }
        remove_text_op(from_line, from_column, next_line, next_column);
        if ((is_last_line && move_carets_down) || (line == 0 && !move_carets_down)) {
            collapse_carets(from_line, from_column, next_line, next_column, true);
        } else {
            const int target_line = move_carets_down ? line : line - 1;
            const bool selected = has_selection();
            if (s_.caret.line == line) {
                set_caret_line(target_line, true, 0);
            }
            if (selected && selection.origin_line == line) {
                set_selection_origin_line(target_line, 0);
                select(selection.origin_line, selection.origin_column, s_.caret.line,
                       s_.caret.column);
            }
        }
        // NOLINTNEXTLINE(readability-suspicious-call-argument): from the removed text's end.
        offset_carets_after(next_line, next_column, from_line, from_column);
        end_complex(s_);
    }

    // ---- Carets ----

    [[nodiscard]] bool has_selection() const { return s_.caret.selection.active; }
    void deselect() { s_.caret.selection.active = false; }
    [[nodiscard]] bool caret_at_origin() const {
        return s_.caret.line == s_.caret.selection.origin_line &&
               s_.caret.column == s_.caret.selection.origin_column;
    }
    // is_caret_after_selection_origin.
    [[nodiscard]] bool caret_after_selection_origin() const {
        const TextEditState::Caret& caret = s_.caret;
        if (!has_selection()) {
            return true;
        }
        return caret.line > caret.selection.origin_line ||
               (caret.line == caret.selection.origin_line &&
                caret.column >= caret.selection.origin_column);
    }
    [[nodiscard]] int caret_wrap_index() const {
        return area_.wrap_index_at_column(s_.caret.line, s_.caret.column);
    }

    // A column on a row at an x the caret or selection origin keeps, one back from a wrapped
    // row's end so it stays on the row.
    [[nodiscard]] int column_at_fit_x(int x, int line, int wrap_index) const {
        int column = area_.char_pos_for_line(x, line, wrap_index, edit_.caret_mid_grapheme);
        if (column != 0 && area_.wrapping() && wrap_index < area_.wrap_count(line) &&
            column >= area_.wrap_ranges(line)[static_cast<std::size_t>(wrap_index)].second) {
            column -= 1;
        }
        return column;
    }

    // set_caret_line. With a wrap index, the caret goes to the row's column at its last x.
    void set_caret_line(int line, bool adjust_viewport, int wrap_index) {
        if (setting_caret_line_) {
            return;
        }
        setting_caret_line_ = true;
        line = std::clamp(line, 0, area_.line_count() - 1);
        bool caret_moved = s_.caret.line != line;
        s_.caret.line = line;
        const int column = wrap_index >= 0 ? column_at_fit_x(s_.caret.last_fit_x, line, wrap_index)
                                           : std::min(s_.caret.column, area_.length(line));
        caret_moved = caret_moved || s_.caret.column != column;
        s_.caret.column = column;
        if (wrap_index >= 0 && has_selection() && caret_at_origin()) {
            deselect();
        }
        if (adjust_viewport) {
            adjust_viewport_to_caret();
        }
        setting_caret_line_ = false;
        if (caret_moved) {
            caret_changed();
        }
    }

    // set_caret_column.
    void set_caret_column(int column, bool adjust_viewport) {
        column = std::clamp(column, 0, area_.length(s_.caret.line));
        const bool caret_moved = s_.caret.column != column;
        s_.caret.column = column;
        s_.caret.last_fit_x = area_.column_x_offset(s_.caret.line, column, s_.input_direction);
        if (!has_selection()) {
            s_.caret.selection.origin_last_fit_x = s_.caret.last_fit_x;
        }
        if (has_selection() && caret_at_origin()) {
            deselect();
        }
        if (adjust_viewport) {
            adjust_viewport_to_caret();
        }
        if (caret_moved) {
            caret_changed();
        }
    }

    // set_selection_origin_line.
    void set_selection_origin_line(int line, int wrap_index) {
        if (!edit_.selecting_enabled) {
            return;
        }
        TextEditState::Selection& selection = s_.caret.selection;
        line = std::clamp(line, 0, area_.line_count() - 1);
        selection.origin_line = line;
        selection.origin_column =
            wrap_index >= 0 ? column_at_fit_x(selection.origin_last_fit_x, line, wrap_index)
                            : std::min(selection.origin_column, area_.length(line));
        if (wrap_index >= 0 && has_selection() && caret_at_origin()) {
            deselect();
        }
    }

    // set_selection_origin_column.
    void set_selection_origin_column(int column) {
        if (!edit_.selecting_enabled) {
            return;
        }
        TextEditState::Selection& selection = s_.caret.selection;
        column = std::clamp(column, 0, area_.length(selection.origin_line));
        selection.origin_column = column;
        selection.origin_last_fit_x =
            area_.column_x_offset(selection.origin_line, column, s_.input_direction);
        if (has_selection() && caret_at_origin()) {
            deselect();
        }
        if (s_.selecting_mode == SelectionMode::None || s_.selecting_mode == SelectionMode::Shift) {
            selection.word_begin_column = column;
            selection.word_end_column = column;
        }
    }

    // select.
    void select(int origin_line, int origin_column, int caret_line, int caret_column) {
        caret_line = std::clamp(caret_line, 0, area_.line_count() - 1);
        caret_column = std::clamp(caret_column, 0, area_.length(caret_line));
        set_caret_line(caret_line, false, -1);
        set_caret_column(caret_column, false);
        if (!edit_.selecting_enabled) {
            return;
        }
        origin_line = std::clamp(origin_line, 0, area_.line_count() - 1);
        origin_column = std::clamp(origin_column, 0, area_.length(origin_line));
        set_selection_origin_line(origin_line, -1);
        set_selection_origin_column(origin_column);
        s_.caret.selection.active = origin_line != caret_line || origin_column != caret_column;
    }

    // delete_selection.
    void delete_selection() {
        begin_complex(s_);
        if (has_selection()) {
            const SelectionRange range = selection_range(s_.caret);
            remove_text_op(range.from_line, range.from_column, range.to_line, range.to_column);
            offset_carets_after(range.to_line, range.to_column, range.from_line, range.from_column);
            deselect();
            set_caret_line(range.from_line, false, -1);
            set_caret_column(range.from_column, true);
        }
        end_complex(s_);
    }

    // _offset_carets_after: the caret and selection origin at or after a position move with the
    // text inserted or removed there.
    void offset_carets_after(int old_line, int old_column, int new_line, int new_column,
                             bool include_selection_begin = true,
                             bool include_selection_end = true) {
        const int edit_height = new_line - old_line;
        const int edit_size = new_column - old_column;
        if (edit_height == 0 && edit_size == 0) {
            return;
        }
        const bool selected = has_selection();
        const bool caret_at_end = selected && caret_after_selection_origin();
        const bool include_caret_at =
            caret_at_end ? include_selection_end : include_selection_begin;
        int caret_line = s_.caret.line;
        int caret_column = s_.caret.column;
        const bool caret_after =
            caret_line > old_line || (caret_line == old_line && caret_column > old_column);
        const bool caret_at = caret_line == old_line && caret_column == old_column;
        if (caret_after || (caret_at && include_caret_at)) {
            caret_line += edit_height;
            if (caret_line == new_line) {
                caret_column += edit_size;
            }
            if (edit_height != 0) {
                set_caret_line(caret_line, false, -1);
            }
            set_caret_column(caret_column, false);
        }
        if (!selected) {
            return;
        }
        const bool include_origin_at =
            !caret_at_end ? include_selection_end : include_selection_begin;
        int origin_line = s_.caret.selection.origin_line;
        int origin_column = s_.caret.selection.origin_column;
        const bool origin_after =
            origin_line > old_line || (origin_line == old_line && origin_column > old_column);
        const bool origin_at = origin_line == old_line && origin_column == old_column;
        if (origin_after || (origin_at && include_origin_at)) {
            origin_line += edit_height;
            if (origin_line == new_line) {
                origin_column += edit_size;
            }
            select(origin_line, origin_column, caret_line, caret_column);
        }
    }

    // collapse_carets: a caret or selection origin inside a range goes to its start.
    void collapse_carets(int from_line, int from_column, int to_line, int to_column,
                         bool inclusive = false) {
        const int collapse_line = std::clamp(from_line, 0, area_.line_count() - 1);
        const int collapse_column = std::clamp(from_column, 0, area_.length(collapse_line));
        if (from_line > to_line) {
            std::swap(from_line, to_line);
            std::swap(from_column, to_column);
        }
        if (from_line == to_line && from_column > to_column) {
            std::swap(from_column, to_column);
        }
        const TextEditState::Selection& selection = s_.caret.selection;
        const bool caret_in = in_range(s_.caret.line, s_.caret.column, from_line, from_column,
                                       to_line, to_column, inclusive);
        if (!has_selection()) {
            if (caret_in) {
                set_caret_line(collapse_line, false, -1);
                set_caret_column(collapse_column, false);
            }
            return;
        }
        const bool origin_in = in_range(selection.origin_line, selection.origin_column, from_line,
                                        from_column, to_line, to_column, inclusive);
        if (caret_in && origin_in) {
            deselect();
            set_caret_line(collapse_line, false, -1);
            set_caret_column(collapse_column, false);
        } else if (caret_in) {
            set_caret_line(collapse_line, false, -1);
            set_caret_column(collapse_column, false);
        } else if (origin_in) {
            set_selection_origin_line(collapse_line, -1);
            set_selection_origin_column(collapse_column);
        }
    }

    void adjust_viewport_to_caret() { area_.adjust_viewport_to_caret(s_); }

    [[nodiscard]] Vec2 mirrored(Vec2 point) const {
        if (area_.view().rtl) {
            point.x = area_.size().x - point.x;
        }
        return point;
    }

    // _reset_caret_blink_timer.
    void reset_caret_blink_timer() {
        if (!edit_.caret_blink) {
            return;
        }
        s_.draw_caret = true;
        if (context_.focused) {
            s_.blink_timer = 0.0;
        }
    }

    void show_virtual_keyboard() { s_.show_keyboard = edit_.virtual_keyboard_enabled; }

    // ---- Input method ----

    // _update_ime_text: the caret's line shows the composition at the caret.
    void update_ime_text() {
        const int line = s_.caret.line;
        if (!s_.ime_text.empty()) {
            const std::u32string& text = area_.line(line);
            const auto column = static_cast<std::size_t>(
                std::clamp(s_.caret.column, 0, static_cast<int>(text.size())));
            area_.set_ime(line, text.substr(0, column) + s_.ime_text + text.substr(column));
        } else {
            area_.clear_ime();
        }
        emit("lines_edited_from", line, line);
    }

    void cancel_ime() {
        if (s_.ime_text.empty()) {
            return;
        }
        s_.ime_text.clear();
        s_.ime_selection_start = 0;
        s_.ime_selection_length = 0;
        update_ime_text();
    }

    // ---- Selection modes ----

    // _selection_mode_update.
    void selection_mode_update() {
        switch (s_.selecting_mode) {
        case SelectionMode::Pointer:
            update_selection_mode_pointer(false);
            break;
        case SelectionMode::Word:
            update_selection_mode_word(false);
            break;
        case SelectionMode::Line:
            update_selection_mode_line(false);
            break;
        default:
            break;
        }
    }

    [[nodiscard]] std::pair<int, int> mouse_line_column() const {
        return area_.line_column_at(s_.mouse, true, true, edit_.caret_mid_grapheme);
    }

    // _update_selection_mode_pointer: dragging selects to the pointer.
    void update_selection_mode_pointer(bool initial) {
        const auto [column, line] = mouse_line_column();
        TextEditState::Selection& selection = s_.caret.selection;
        if (initial && !has_selection()) {
            set_selection_origin_line(line, -1);
            set_selection_origin_column(column);
            selection.word_begin_column = column;
            selection.word_end_column = column;
        } else {
            const int origin_line = selection.origin_line;
            const bool right = line > origin_line ||
                               (line == origin_line && column >= selection.word_begin_column);
            const int origin_col = right ? selection.word_begin_column : selection.word_end_column;
            select(origin_line, origin_col, line, column);
        }
        adjust_viewport_to_caret();
        if (has_selection()) {
            s_.dragging_selection = true;
        }
        s_.click_held = kClickHeldSeconds;
    }

    // _update_selection_mode_word: after a double click, whole words.
    void update_selection_mode_word(bool initial) {
        s_.dragging_selection = true;
        const auto [column, line] = mouse_line_column();
        const int caret_pos = std::clamp(column, 0, area_.length(line));
        int beg = caret_pos;
        int end = beg;
        const std::vector<int> words = area_.word_breaks(line);
        for (std::size_t i = 0; i + 1 < words.size(); i += 2) {
            if ((initial && words[i] <= caret_pos && words[i + 1] >= caret_pos) ||
                (!initial && words[i] < caret_pos && words[i + 1] > caret_pos)) {
                beg = words[i];
                end = words[i + 1];
                break;
            }
        }
        TextEditState::Selection& selection = s_.caret.selection;
        if (initial && !has_selection()) {
            select(line, beg, line, end);
            selection.word_begin_column = beg;
            selection.word_end_column = end;
        } else {
            const int origin_line = selection.origin_line;
            const bool right = line > origin_line ||
                               (line == origin_line && column >= selection.word_begin_column);
            const int origin_col = right ? selection.word_begin_column : selection.word_end_column;
            const int caret_col = right ? end : beg;
            if (column != selection.word_begin_column || line != origin_line ||
                selection.word_begin_column == selection.word_end_column) {
                select(origin_line, origin_col, line, caret_col);
            }
        }
        adjust_viewport_to_caret();
        s_.click_held = kClickHeldSeconds;
    }

    // _update_selection_mode_line: after a triple click, whole lines.
    void update_selection_mode_line(bool initial) {
        s_.dragging_selection = true;
        const int line = mouse_line_column().second;
        TextEditState::Selection& selection = s_.caret.selection;
        const int origin_line = initial && !has_selection() ? line : selection.origin_line;
        const bool line_below = line >= origin_line;
        const int origin_col = line_below ? 0 : area_.length(origin_line);
        const int caret_line = line_below ? line + 1 : line;
        const int caret_col =
            caret_line < area_.line_count() ? 0 : area_.length(area_.line_count() - 1);
        select(origin_line, origin_col, caret_line, caret_col);
        adjust_viewport_to_caret();
        if (initial) {
            selection.word_begin_column = 0;
            selection.word_end_column = area_.length(origin_line);
        }
        s_.click_held = kClickHeldSeconds;
    }

    // _pre_shift_selection: Shift with a caret move selects from where the caret was.
    void pre_shift_selection() {
        if (!edit_.selecting_enabled) {
            return;
        }
        s_.selecting_mode = SelectionMode::Shift;
        if (has_selection()) {
            return;
        }
        set_selection_origin_line(s_.caret.line, -1);
        set_selection_origin_column(s_.caret.column);
        s_.caret.selection.active = true;
    }

    // ---- Gutters ----

    // gutter_clicked, then CodeEdit::_gutter_clicked: its main gutter toggles a breakpoint, or
    // with Shift a bookmark, and a line number selects the line.
    void gutter_clicked(int line, int gutter, bool shift) {
        const TextEditGutterView& clicked =
            area_.view().gutters.at(static_cast<std::size_t>(gutter));
        if (!quiet_ && context_.signals != nullptr) {
            context_.signals->push_back({.kind = "gutter_clicked",
                                         .from_line = 0,
                                         .to_line = 0,
                                         .line = line,
                                         .gutter = clicked.name});
        }
        world::CodeEdit* code = context_.code;
        if (code == nullptr || !area_.view().code) {
            return;
        }
        if (clicked.kind == TextEditGutterView::Kind::Main) {
            if (code->gutters_draw_breakpoints_gutter && !shift) {
                toggle_line(code->breakpointed_lines, line);
                emit_line("breakpoint_toggled", line);
            } else if (code->gutters_draw_bookmarks && shift) {
                toggle_line(code->bookmarked_lines, line);
            }
        } else if (clicked.kind == TextEditGutterView::Kind::LineNumbers) {
            s_.selecting_mode = SelectionMode::Line;
            if (line == area_.line_count() - 1) {
                select(line, 0, line, area_.length(line));
            } else {
                select(line, 0, line + 1, 0);
            }
        }
    }

    static void toggle_line(std::vector<int>& lines, int line) {
        if (const auto found = std::ranges::find(lines, line); found != lines.end()) {
            lines.erase(found);
            return;
        }
        lines.push_back(line);
        std::ranges::sort(lines);
    }

    void emit_line(std::string_view kind, int line) const {
        if (!quiet_ && context_.signals != nullptr) {
            context_.signals->push_back({.kind = std::string(kind),
                                         .from_line = 0,
                                         .to_line = 0,
                                         .line = line,
                                         .gutter = {}});
        }
    }

    // What the gutters show moves with the lines the edits since mark_lines moved, as Godot's
    // Text::Line keeps it, and goes with a line removed: CodeEdit's lines, with
    // breakpoint_toggled for each line a breakpoint left or came to (_text_changed, which can
    // miss a line a breakpoint came to when another moved off it), and the TextEdit's gutters'
    // entries. Lines past the end, which nothing moves, stay as they are.
    void move_line_marks() {
        const std::vector<int> to = area_.moved_lines();
        const auto moved = [&to](int line) {
            return line >= 0 && std::cmp_less(line, to.size()) ? to[static_cast<std::size_t>(line)]
                                                               : line;
        };
        const auto move = [&](std::vector<int>& lines) {
            std::vector<int> kept;
            for (const int line : lines) {
                if (const int now = moved(line); now != -1) {
                    kept.push_back(now);
                }
            }
            lines = std::move(kept);
        };
        if (world::CodeEdit* code = context_.code; code != nullptr) {
            std::vector<int> before = code->breakpointed_lines;
            std::sort(before.begin(), before.end());
            move(code->breakpointed_lines);
            move(code->bookmarked_lines);
            move(code->executing_lines);
            for (const int line : before) {
                if (!has(code->breakpointed_lines, line)) {
                    emit_line("breakpoint_toggled", line);
                }
                if (const int now = moved(line); now != -1 && !has(before, now)) {
                    emit_line("breakpoint_toggled", now);
                }
            }
        }
        for (world::TextEditGutter& gutter : edit_.gutters) {
            std::vector<world::TextEditGutterLine> kept;
            for (world::TextEditGutterLine& entry : gutter.lines) {
                if (const int now = moved(entry.line); now != -1) {
                    entry.line = now;
                    kept.push_back(std::move(entry));
                }
            }
            gutter.lines = std::move(kept);
        }
    }

    [[nodiscard]] static bool has(const std::vector<int>& lines, int line) {
        return std::ranges::find(lines, line) != lines.end();
    }

    // ---- Mouse ----

    void mouse_pressed(const input::Event& event, Vec2 mpos, Vec2 local) {
        const input::Modifiers& held = event.modifiers;
        const bool command = input::host_keyboard() == input::Keyboard::Mac ? held.meta : held.ctrl;
        const auto factor = static_cast<double>(event.factor);
        switch (event.button) {
        case input::MouseButton::WheelUp:
            if (!command) {
                if (held.shift) {
                    area_.set_h_scroll(area_.h_scroll() - (100.0 * factor));
                } else if (held.alt) {
                    scroll_up(15.0 * factor);
                } else if (area_.v_scroll_visible()) {
                    scroll_up(3.0 * factor);
                }
                selection_mode_update();
            }
            break;
        case input::MouseButton::WheelDown:
            if (!command) {
                if (held.shift) {
                    area_.set_h_scroll(area_.h_scroll() + (100.0 * factor));
                } else if (held.alt) {
                    scroll_down(15.0 * factor);
                } else if (area_.v_scroll_visible()) {
                    scroll_down(3.0 * factor);
                }
                selection_mode_update();
            }
            break;
        case input::MouseButton::WheelLeft:
            area_.set_h_scroll(area_.h_scroll() - (100.0 * factor));
            selection_mode_update();
            break;
        case input::MouseButton::WheelRight:
            area_.set_h_scroll(area_.h_scroll() + (100.0 * factor));
            selection_mode_update();
            break;
        case input::MouseButton::Left:
            left_pressed(event, mpos, local);
            break;
        case input::MouseButton::Right:
            right_pressed(mpos);
            break;
        default:
            break;
        }
    }

    // A click places the caret (Shift selects to it), a double click selects a word and a
    // triple click the line. Alt+click adds a caret in Godot; here it places the one caret.
    void left_pressed(const input::Event& event, Vec2 mpos, Vec2 local) {
        reset_caret_blink_timer();
        apply_ime();
        const auto [col, line] = area_.line_column_at(mpos, true, true, edit_.caret_mid_grapheme);
        if (const auto [gutter, row] = area_.gutter_at(mpos); gutter != -1) {
            gutter_clicked(row, gutter, event.modifiers.shift);
            return;
        }
        if (mpos.x < static_cast<float>(area_.text_start())) {
            return;
        }
        const int prev_col = s_.caret.column;
        const int prev_line = s_.caret.line;
        const Vec2 apart = local - s_.last_double_click_position;
        const bool triple_click = !event.double_click && s_.last_double_click >= 0.0 &&
                                  context_.now - s_.last_double_click < kTripleClickSeconds &&
                                  std::hypot(apart.x, apart.y) < kTripleClickTolerance;
        if (!event.double_click && !triple_click) {
            if (!event.modifiers.shift) {
                if (edit_.drag_and_drop_selection_enabled && over_selection(mpos)) {
                    // The selection may be dragged away; the caret waits to see.
                    s_.selecting_mode = SelectionMode::None;
                    s_.selection_drag_attempt = true;
                    s_.last_double_click = -1.0;
                    return;
                }
                deselect();
            }
            push_current_op(s_);
            set_caret_line(line, false, -1);
            set_caret_column(col, false);
            s_.selection_drag_attempt = false;
            const bool caret_moved = s_.caret.column != prev_col || s_.caret.line != prev_line;
            if (edit_.selecting_enabled && event.modifiers.shift && !has_selection() &&
                caret_moved) {
                select(prev_line, prev_col, line, col);
            }
            s_.selecting_mode = SelectionMode::Pointer;
            update_selection_mode_pointer(true);
        } else if (triple_click) {
            s_.selecting_mode = SelectionMode::Line;
            update_selection_mode_line(true);
            s_.last_double_click = -1.0;
        } else {
            s_.selecting_mode = SelectionMode::Word;
            update_selection_mode_word(true);
            s_.last_double_click = context_.now;
            s_.last_double_click_position = local;
        }
    }

    // A right click moves the caret, unless it's on the selection.
    void right_pressed(Vec2 mpos) {
        if (!edit_.caret_move_on_right_click) {
            return;
        }
        push_current_op(s_);
        reset_caret_blink_timer();
        apply_ime();
        s_.selecting_mode = SelectionMode::None;
        const auto [column, line] =
            area_.line_column_at(mpos, true, true, edit_.caret_mid_grapheme);
        const SelectionRange range = selection_range(s_.caret);
        const bool selection_clicked =
            has_selection() && in_range(line, column, range.from_line, range.from_column,
                                        range.to_line, range.to_column, true);
        if (!selection_clicked) {
            deselect();
            set_caret_line(line, false, -1);
            set_caret_column(column, true);
        }
    }

    // _on_drag_or_mouse_motion_event: a held button selects, unless a drag is going on; a drag
    // that can drop here moves the drag caret to where it would go.
    void drag_or_mouse_motion(bool left) {
        if (left && !context_.dragging) {
            reset_caret_blink_timer();
            if (s_.ime_text.empty()) {
                selection_mode_update();
            }
        }
        if (!context_.dragging || !context_.can_drop) {
            return;
        }
        apply_ime();
        const auto [column, line] =
            area_.line_column_at(s_.mouse, true, true, edit_.caret_mid_grapheme);
        TextEditState::Caret drag;
        drag.line = line;
        drag.column = column;
        drag.last_fit_x = area_.column_x_offset(line, column, s_.input_direction);
        s_.drag_caret = drag;
        s_.drag_caret_force_displayed = true;
        // set_caret_column with adjust_viewport, for the drag caret.
        std::swap(s_.caret, *s_.drag_caret);
        adjust_viewport_to_caret();
        std::swap(s_.caret, *s_.drag_caret);
        s_.dragging_selection = true;
    }

    // is_mouse_over_selection: a point (mirrored) falls on the selection or its ends.
    [[nodiscard]] bool over_selection(Vec2 point) const {
        if (!has_selection()) {
            return false;
        }
        const auto [column, line] =
            area_.line_column_at(point, true, true, edit_.caret_mid_grapheme);
        const SelectionRange range = selection_range(s_.caret);
        return in_range(line, column, range.from_line, range.from_column, range.to_line,
                        range.to_column, true);
    }

public:
    // drop_data, for text: in at a point in its rect, then selected. Dragged from its own
    // selection, the selection moves there unless Command (Ctrl) is held to copy it; dropped
    // on that selection, nothing changes. Whether the text went in.
    bool drop_text(Vec2 local, std::u32string_view text, bool command) {
        if (!edit_.editable) {
            return false;
        }
        const auto [column, line] =
            area_.line_column_at(mirrored(local), true, true, edit_.caret_mid_grapheme);
        int drop_line = line;
        int drop_column = column;
        const SelectionRange range = selection_range(s_.caret);
        const bool on_selection =
            has_selection() && in_range(line, column, range.from_line, range.from_column,
                                        range.to_line, range.to_column, !command);
        s_.drag_caret.reset();
        if (s_.selection_drag_attempt && on_selection) {
            s_.selection_drag_attempt = false;
            return false;
        }
        begin_complex(s_);
        if (s_.selection_drag_attempt) {
            s_.selection_drag_attempt = false;
            if (!command && has_selection()) {
                // The drop point moves back over the selection taken out before it.
                const bool after = drop_line > range.to_line ||
                                   (drop_line == range.to_line && drop_column >= range.to_column);
                if (after) {
                    if (drop_line == range.to_line) {
                        drop_column += range.from_column - range.to_column;
                    }
                    drop_line += range.from_line - range.to_line;
                }
                delete_selection();
            }
        }
        deselect();
        set_caret_line(drop_line, true, -1);
        set_caret_column(drop_column, true);
        insert_text_at_caret(text);
        select(drop_line, drop_column, s_.caret.line, s_.caret.column);
        adjust_viewport_to_caret();
        end_complex(s_);
        return true;
    }

    // NOTIFICATION_DRAG_END, after its selection was dropped elsewhere.
    void drag_end(bool command) {
        s_.drag_caret.reset();
        if (s_.selection_drag_attempt) {
            if (edit_.editable && !command) {
                delete_selection();
            } else if (edit_.deselect_on_focus_loss_enabled) {
                deselect();
            }
        }
        s_.selection_drag_attempt = false;
        s_.drag_caret_force_displayed = false;
        s_.dragging_selection = false;
    }

private:
    // _scroll_up and _scroll_down, without smooth scrolling.
    void scroll_up(double delta) { area_.set_v_scroll(area_.v_scroll() - delta); }
    void scroll_down(double delta) { area_.set_v_scroll(area_.v_scroll() + delta); }

    // _scroll_lines_up: up a row, keeping the caret in view if nothing is selected.
    void scroll_lines_up() {
        area_.set_v_scroll(area_.v_scroll() - 1.0);
        if (!has_selection()) {
            const int last_line = area_.last_full_visible_line();
            const int last_wrap = area_.last_full_visible_line_wrap_index();
            if (s_.caret.line > last_line ||
                (s_.caret.line == last_line && caret_wrap_index() > last_wrap)) {
                set_caret_line(last_line, false, last_wrap);
            }
        }
        selection_mode_update();
    }

    void scroll_lines_down() {
        area_.set_v_scroll(area_.v_scroll() + 1.0);
        if (!has_selection()) {
            const int first_line = area_.first_visible_line();
            const int first_wrap = area_.first_visible_wrap();
            if (s_.caret.line < first_line ||
                (s_.caret.line == first_line && caret_wrap_index() < first_wrap)) {
                set_caret_line(first_line, false, first_wrap);
            }
        }
        selection_mode_update();
    }

    // ---- Keys ----

    [[nodiscard]] bool is_action(const input::Event& event, std::string_view action) const {
        return context_.map != nullptr && context_.map->find_event(action, event, true).has_value();
    }

    // The key part of gui_input. Action events edit as keys do, so agents can send ui_text_*
    // actions.
    bool key_input(const input::Event& event) {
        if (!event.pressed) {
            return false;
        }
        const bool key = event.type == input::EventType::Key;
        if (key && (event.key == input::Key::Ctrl || event.key == input::Key::Alt ||
                    event.key == input::Key::Shift || event.key == input::Key::Meta ||
                    event.key == input::Key::CapsLock)) {
            return false;
        }
        reset_caret_blink_timer();
        const input::Modifiers& held = event.modifiers;
        const bool allow_unicode_handling = !(held.ctrl || held.alt || held.meta);

        // Newlines.
        if (is_action(event, "ui_text_newline_above")) {
            new_line(false, true);
            return true;
        }
        if (is_action(event, "ui_text_newline_blank")) {
            new_line(false, false);
            return true;
        }
        if (is_action(event, "ui_text_newline")) {
            new_line(true, false);
            return true;
        }
        // Backspace and delete.
        if (is_action(event, "ui_text_backspace_all_to_left")) {
            do_backspace(false, true);
            return true;
        }
        if (is_action(event, "ui_text_backspace_word")) {
            do_backspace(true, false);
            return true;
        }
        if (is_action(event, "ui_text_backspace")) {
            do_backspace(false, false);
            return true;
        }
        if (is_action(event, "ui_text_delete_all_to_right")) {
            do_delete(false, true);
            return true;
        }
        if (is_action(event, "ui_text_delete_word")) {
            do_delete(true, false);
            return true;
        }
        if (is_action(event, "ui_text_delete")) {
            do_delete(false, false);
            return true;
        }
        // Scrolling.
        if (is_action(event, "ui_text_scroll_up")) {
            scroll_lines_up();
            return true;
        }
        if (is_action(event, "ui_text_scroll_down")) {
            scroll_lines_down();
            return true;
        }
        if (edit_.shortcut_keys_enabled && shortcut(event)) {
            return true;
        }
        // Misc.
        if (is_action(event, "ui_menu")) {
            push_current_op(s_); // no context menu here
            return true;
        }
        if (is_action(event, "ui_text_toggle_insert_mode")) {
            s_.overtype_mode = !s_.overtype_mode;
            return true;
        }
        if (is_action(event, "ui_swap_input_direction")) {
            swap_current_input_direction();
            return true;
        }
        // Caret moves match without Shift, which selects.
        input::Event moved = event;
        const bool shift = moved.modifiers.shift;
        moved.modifiers.shift = false;
        if (is_action(moved, "ui_text_caret_word_left")) {
            move_caret_left(shift, true);
            return true;
        }
        if (is_action(moved, "ui_text_caret_left")) {
            move_caret_left(shift, false);
            return true;
        }
        if (is_action(moved, "ui_text_caret_word_right")) {
            move_caret_right(shift, true);
            return true;
        }
        if (is_action(moved, "ui_text_caret_right")) {
            move_caret_right(shift, false);
            return true;
        }
        if (is_action(moved, "ui_text_caret_up")) {
            move_caret_up(shift);
            return true;
        }
        if (is_action(moved, "ui_text_caret_down")) {
            move_caret_down(shift);
            return true;
        }
        if (is_action(moved, "ui_text_caret_document_start")) {
            move_caret_document_start(shift);
            return true;
        }
        if (is_action(moved, "ui_text_caret_document_end")) {
            move_caret_document_end(shift);
            return true;
        }
        if (is_action(moved, "ui_text_caret_line_start")) {
            move_caret_to_line_start(shift);
            return true;
        }
        if (is_action(moved, "ui_text_caret_line_end")) {
            move_caret_to_line_end(shift);
            return true;
        }
        if (is_action(moved, "ui_text_caret_page_up")) {
            move_caret_page_up(shift);
            return true;
        }
        if (is_action(moved, "ui_text_caret_page_down")) {
            move_caret_page_down(shift);
            return true;
        }
        // Tab types a tab, or moves focus once ui_focus_mode turns that off.
        if (is_action(moved, "ui_focus_mode")) {
            edit_.tab_input_mode = !edit_.tab_input_mode;
            return true;
        }
        if (edit_.tab_input_mode && is_action(moved, "ui_text_indent")) {
            if (edit_.editable) {
                insert_text_at_caret(U"\t");
            }
            return true;
        }
        // Typed characters, when no modifier but Shift is held.
        if (key && allow_unicode_handling && edit_.editable && event.unicode >= 0x20U) {
            handle_unicode_input(event.unicode);
            return true;
        }
        return false;
    }

    // Select all, select the word under the caret, clear the selection, cut, copy, paste, undo
    // and redo.
    bool shortcut(const input::Event& event) {
        if (is_action(event, "ui_text_select_all")) {
            select_all();
            return true;
        }
        if (is_action(event, "ui_text_select_word_under_caret")) {
            select_word_under_caret();
            return true;
        }
        // Escape by default, so it's taken only if there was a selection to clear.
        if (is_action(event, "ui_text_clear_carets_and_selection") &&
            clear_carets_and_selection()) {
            return true;
        }
        if (is_action(event, "ui_cut")) {
            cut();
            return true;
        }
        if (is_action(event, "ui_copy")) {
            copy();
            return true;
        }
        if (is_action(event, "ui_paste")) {
            paste();
            return true;
        }
        if (is_action(event, "ui_undo")) {
            undo();
            return true;
        }
        if (is_action(event, "ui_redo")) {
            redo();
            return true;
        }
        return false;
    }

    // handle_unicode_input: a typed character replaces the selection, or in overtype mode the
    // character after the caret.
    void handle_unicode_input(char32_t c) {
        if (!edit_.editable) {
            return;
        }
        start_action(s_, EditAction::Typing);
        if (s_.overtype_mode && !has_selection()) {
            const int line = s_.caret.line;
            const int column = s_.caret.column;
            if (column < area_.length(line)) {
                remove_text_op(line, column, line, column + 1);
            }
        }
        insert_text_at_caret(std::u32string(1, c));
        end_action(s_);
    }

    // _swap_current_input_direction.
    void swap_current_input_direction() {
        s_.input_direction = s_.input_direction == text::Direction::Ltr ? text::Direction::Rtl
                                                                        : text::Direction::Ltr;
        set_caret_column(s_.caret.column, true);
    }

    // _new_line: splits the line at the caret, or starts a blank line below or above it.
    void new_line(bool split_current_line, bool above) {
        if (!edit_.editable) {
            return;
        }
        begin_complex(s_);
        if (split_current_line) {
            insert_text_at_caret(U"\n");
        } else {
            const int line = s_.caret.line;
            insert_text(U"\n", line, above ? 0 : area_.length(line), above, above);
            deselect();
            set_caret_line(above ? line : line + 1, false, -1);
            set_caret_column(0, true);
        }
        end_complex(s_);
    }

    // _move_caret_left.
    void move_caret_left(bool select, bool by_word) {
        push_current_op(s_);
        if (select) {
            pre_shift_selection();
        } else if (has_selection() && !by_word) {
            // To the selection's start.
            const SelectionRange range = selection_range(s_.caret);
            set_caret_line(range.from_line, false, -1);
            set_caret_column(range.from_column, true);
            deselect();
            return;
        } else {
            deselect();
        }
        const int line = s_.caret.line;
        const int column = s_.caret.column;
        if (column == 0) {
            if (line == 0) {
                return;
            }
            set_caret_line(line - 1, false, -1);
            set_caret_column(area_.length(s_.caret.line), true);
        } else if (by_word) {
            int to = column;
            const std::vector<int> words = area_.word_breaks(line);
            if (words.empty() || to <= words.front()) {
                to = 0;
            } else {
                for (auto j = static_cast<std::ptrdiff_t>(words.size()) - 2; j >= 0; j -= 2) {
                    if (words[static_cast<std::size_t>(j)] < to) {
                        to = words[static_cast<std::size_t>(j)];
                        break;
                    }
                }
            }
            set_caret_column(to, true);
        } else {
            set_caret_column(edit_.caret_mid_grapheme ? column - 1
                                                      : area_.previous_character(line, column),
                             true);
        }
    }

    // _move_caret_right.
    void move_caret_right(bool select, bool by_word) {
        push_current_op(s_);
        if (select) {
            pre_shift_selection();
        } else if (has_selection() && !by_word) {
            // To the selection's end.
            const SelectionRange range = selection_range(s_.caret);
            set_caret_line(range.to_line, false, -1);
            set_caret_column(range.to_column, true);
            deselect();
            return;
        } else {
            deselect();
        }
        const int line = s_.caret.line;
        const int column = s_.caret.column;
        if (column == area_.length(line)) {
            if (line >= area_.line_count() - 1) {
                return;
            }
            set_caret_line(line + 1, false, -1);
            set_caret_column(0, true);
        } else if (by_word) {
            int to = column;
            const std::vector<int> words = area_.word_breaks(line);
            if (words.empty() || to >= words.back()) {
                to = area_.length(line);
            } else {
                for (std::size_t j = 1; j < words.size(); j += 2) {
                    if (words[j] > to) {
                        to = words[j];
                        break;
                    }
                }
            }
            set_caret_column(to, true);
        } else {
            set_caret_column(
                edit_.caret_mid_grapheme ? column + 1 : area_.next_character(line, column), true);
        }
    }

    // _move_caret_up: up a row, keeping the caret's x.
    void move_caret_up(bool select) {
        push_current_op(s_);
        if (select) {
            pre_shift_selection();
        } else {
            deselect();
        }
        const int wrap = caret_wrap_index();
        if (wrap > 0) {
            set_caret_line(s_.caret.line, true, wrap - 1);
        } else if (s_.caret.line == 0) {
            set_caret_column(0, true);
        } else {
            const int line = s_.caret.line - 1;
            set_caret_line(line, true, area_.wrap_count(line));
        }
    }

    // _move_caret_down.
    void move_caret_down(bool select) {
        push_current_op(s_);
        if (select) {
            pre_shift_selection();
        } else {
            deselect();
        }
        const int wrap = caret_wrap_index();
        if (wrap < area_.wrap_count(s_.caret.line)) {
            set_caret_line(s_.caret.line, true, wrap + 1);
        } else if (s_.caret.line == area_.line_count() - 1) {
            set_caret_column(area_.length(s_.caret.line), true);
        } else {
            set_caret_line(s_.caret.line + 1, true, 0);
        }
    }

    // get_first_non_whitespace_column.
    [[nodiscard]] int first_non_whitespace_column(int line) const {
        const std::u32string& text = area_.line(line);
        int column = 0;
        while (column < static_cast<int>(text.size()) &&
               is_whitespace(text[static_cast<std::size_t>(column)])) {
            ++column;
        }
        return column;
    }

    // _move_caret_to_line_start: to the row's start, then the line's first non-space, then its
    // start ("smart home").
    void move_caret_to_line_start(bool select) {
        push_current_op(s_);
        if (select) {
            pre_shift_selection();
        } else {
            deselect();
        }
        const std::vector<std::pair<int, int>>& rows = area_.wrap_ranges(s_.caret.line);
        const int wi = caret_wrap_index();
        int row_start_col = 0;
        for (int j = 0; j < wi && std::cmp_less(j, rows.size()); ++j) {
            row_start_col +=
                rows[static_cast<std::size_t>(j)].second - rows[static_cast<std::size_t>(j)].first;
        }
        if (s_.caret.column == row_start_col || wi == 0) {
            const int whitespace = first_non_whitespace_column(s_.caret.line);
            set_caret_column(s_.caret.column == whitespace ? 0 : whitespace, true);
        } else {
            set_caret_column(row_start_col, true);
        }
    }

    // _move_caret_to_line_end: to the row's end, then the line's.
    void move_caret_to_line_end(bool select) {
        push_current_op(s_);
        if (select) {
            pre_shift_selection();
        } else {
            deselect();
        }
        const std::vector<std::pair<int, int>>& rows = area_.wrap_ranges(s_.caret.line);
        const int wi = caret_wrap_index();
        int row_end_col = -1;
        for (int j = 0; j < wi + 1 && std::cmp_less(j, rows.size()); ++j) {
            row_end_col +=
                rows[static_cast<std::size_t>(j)].second - rows[static_cast<std::size_t>(j)].first;
        }
        if (std::cmp_equal(wi, rows.size() - 1) || s_.caret.column == row_end_col) {
            set_caret_column(area_.length(s_.caret.line), true);
        } else {
            set_caret_column(row_end_col, true);
        }
    }

    // _move_caret_page_up and _move_caret_page_down: a screenful of rows.
    void move_caret_page_up(bool select) {
        push_current_op(s_);
        if (select) {
            pre_shift_selection();
        } else {
            deselect();
        }
        const auto [lines, wrap] = area_.next_visible_line_index_offset_from(
            s_.caret.line, caret_wrap_index(), -area_.visible_line_count());
        set_caret_line(s_.caret.line - lines + 1, true, wrap);
    }

    void move_caret_page_down(bool select) {
        push_current_op(s_);
        if (select) {
            pre_shift_selection();
        } else {
            deselect();
        }
        const auto [lines, wrap] = area_.next_visible_line_index_offset_from(
            s_.caret.line, caret_wrap_index(), area_.visible_line_count());
        set_caret_line(s_.caret.line + lines - 1, true, wrap);
    }

    // _move_caret_document_start and _move_caret_document_end.
    void move_caret_document_start(bool select) {
        if (select) {
            pre_shift_selection();
        } else {
            deselect();
        }
        set_caret_line(0, false, -1);
        set_caret_column(0, true);
    }

    void move_caret_document_end(bool select) {
        if (select) {
            pre_shift_selection();
        } else {
            deselect();
        }
        set_caret_line(area_.line_count() - 1, true, -1);
        set_caret_column(area_.length(s_.caret.line), true);
    }

    // _do_backspace: a character (backspace), back to the word's start, or to the line's.
    void do_backspace(bool word, bool all_to_left) {
        if (!edit_.editable) {
            return;
        }
        start_action(s_, EditAction::Backspace);
        const int line = s_.caret.line;
        const int column = s_.caret.column;
        if (column == 0 && line == 0 && !has_selection()) {
            end_action(s_);
            return;
        }
        if (has_selection() || (!all_to_left && !word) || column == 0) {
            backspace();
        } else if (all_to_left) {
            remove_text_op(line, 0, line, column);
            collapse_carets(line, 0, line, column);
            set_caret_column(0, true);
            offset_carets_after(line, column, line, 0);
        } else {
            int to = column;
            // "word <caret>" deletes the word, not just the space.
            if (is_whitespace(area_.line(line)[static_cast<std::size_t>(column - 1)])) {
                to -= 1;
            }
            const std::vector<int> words = area_.word_breaks(line);
            if (words.empty() || to <= words.front()) {
                to = 0;
            } else {
                for (auto c = static_cast<std::ptrdiff_t>(words.size()) - 2; c >= 0; c -= 2) {
                    if (words[static_cast<std::size_t>(c)] < to) {
                        to = words[static_cast<std::size_t>(c)];
                        break;
                    }
                }
            }
            remove_text_op(line, to, line, column);
            collapse_carets(line, to, line, column);
            set_caret_column(to, true);
            offset_carets_after(line, column, line, to);
        }
        end_action(s_);
    }

    // _backspace_internal: the character before the caret, or the whole composite character.
    void backspace() {
        if (!edit_.editable) {
            return;
        }
        if (has_selection()) {
            delete_selection();
            return;
        }
        begin_complex(s_);
        const int to_line = s_.caret.line;
        const int to_column = s_.caret.column;
        if (to_column != 0 || to_line != 0) {
            const int from_line = to_column > 0 ? to_line : to_line - 1;
            int from_column = 0;
            if (to_column == 0) {
                from_column = area_.length(to_line - 1);
            } else if (edit_.caret_mid_grapheme ||
                       !edit_.backspace_deletes_composite_character_enabled) {
                from_column = to_column - 1;
            } else {
                from_column = area_.previous_character(to_line, to_column);
            }
            remove_text_op(from_line, from_column, to_line, to_column);
            collapse_carets(from_line, from_column, to_line, to_column);
            offset_carets_after(to_line, to_column, from_line, from_column);
            set_caret_line(from_line, false, -1);
            set_caret_column(from_column, true);
        }
        end_complex(s_);
    }

    // _delete: the character after the caret, to the word's end, or to the line's.
    void do_delete(bool word, bool all_to_right) {
        if (!edit_.editable) {
            return;
        }
        start_action(s_, EditAction::Delete);
        if (has_selection()) {
            delete_selection();
            end_action(s_);
            return;
        }
        const int line = s_.caret.line;
        const int column = s_.caret.column;
        const int length = area_.length(line);
        if (line == area_.line_count() - 1 && column == length) {
            end_action(s_); // the last line's end: nothing to delete
            return;
        }
        int next_line = column < length ? line : line + 1;
        int next_column = 0;
        if (all_to_right) {
            if (column == length) {
                end_action(s_);
                return;
            }
            next_column = length;
            next_line = line;
        } else if (word && column < length - 1) {
            next_column = column;
            const std::vector<int> words = area_.word_breaks(line);
            if (words.empty() || next_column >= words.back()) {
                next_column = length;
            } else {
                for (std::size_t j = 1; j < words.size(); j += 2) {
                    if (words[j] > next_column) {
                        next_column = words[j];
                        break;
                    }
                }
            }
            next_line = line;
        } else if (column < length) {
            next_column =
                edit_.caret_mid_grapheme ? column + 1 : area_.next_character(line, column);
        }
        remove_text_op(line, column, next_line, next_column);
        collapse_carets(line, column, next_line, next_column);
        // NOLINTNEXTLINE(readability-suspicious-call-argument): from the removed text's end.
        offset_carets_after(next_line, next_column, line, column);
        end_action(s_);
    }

    // select_all.
    void select_all() {
        push_current_op(s_);
        if (!edit_.selecting_enabled || (area_.line_count() == 1 && area_.line(0).empty())) {
            return;
        }
        s_.selecting_mode = SelectionMode::Shift;
        const int last = area_.line_count() - 1;
        select(0, 0, last, area_.length(last));
    }

    // select_word_under_caret: a second press drops the selection.
    void select_word_under_caret() {
        push_current_op(s_);
        if (!edit_.selecting_enabled || (area_.line_count() == 1 && area_.line(0).empty())) {
            return;
        }
        s_.selecting_mode = SelectionMode::None;
        if (has_selection()) {
            deselect();
            return;
        }
        const int column = s_.caret.column;
        int begin = 0;
        int end = 0;
        const std::vector<int> words = area_.word_breaks(s_.caret.line);
        for (std::size_t i = 0; i + 1 < words.size(); i += 2) {
            if ((words[i] <= column && words[i + 1] >= column) ||
                (i + 2 == words.size() && column == words[i + 1])) {
                begin = words[i];
                end = words[i + 1];
                break;
            }
        }
        if (begin == 0 && end == 0) {
            return;
        }
        select(s_.caret.line, begin, s_.caret.line, end);
    }

    // _clear_carets_and_selection.
    bool clear_carets_and_selection() {
        push_current_op(s_);
        if (has_selection()) {
            deselect();
            return true;
        }
        return false;
    }

    [[nodiscard]] std::u32string selected_text() const {
        if (!has_selection()) {
            return {};
        }
        const SelectionRange range = selection_range(s_.caret);
        return area_.get(range.from_line, range.from_column, range.to_line, range.to_column);
    }

    // _copy_internal: the selection, or with nothing selected the caret's line, to paste as a
    // line.
    void copy() {
        if (context_.world == nullptr) {
            return;
        }
        if (has_selection()) {
            set_clipboard(*context_.world, to_utf8(selected_text()));
            s_.cut_copy_line.clear();
            return;
        }
        if (!edit_.empty_selection_clipboard_enabled) {
            return;
        }
        std::u32string line = area_.line(s_.caret.line);
        line.push_back(U'\n');
        set_clipboard(*context_.world, to_utf8(line));
        s_.cut_copy_line = std::move(line);
    }

    // _cut_internal.
    void cut() {
        copy();
        if (!edit_.editable) {
            return;
        }
        if (has_selection()) {
            delete_selection();
            return;
        }
        if (!edit_.empty_selection_clipboard_enabled) {
            return;
        }
        begin_complex(s_);
        remove_line_at(s_.caret.line, true);
        end_complex(s_);
    }

    // _paste_internal: a line copied with nothing selected goes in above the caret's line.
    void paste() {
        if (!edit_.editable || context_.world == nullptr) {
            return;
        }
        const std::u32string pasted = to_utf32(clipboard(*context_.world));
        if (pasted.empty()) {
            return;
        }
        std::u32string without_cr = pasted;
        std::erase(without_cr, U'\r');
        if (!has_selection() && !s_.cut_copy_line.empty() && s_.cut_copy_line == without_cr) {
            insert_text(pasted, s_.caret.line, 0, true, false);
            area_.refresh();
            adjust_viewport_to_caret();
            return;
        }
        begin_complex(s_);
        if (has_selection()) {
            delete_selection();
        }
        insert_text_at_caret(pasted);
        end_complex(s_);
    }

    // undo: back a step, and a complex operation's steps together.
    void undo() {
        if (!edit_.editable) {
            return;
        }
        if (s_.in_action) {
            s_.pending_action_end = true;
        }
        push_current_op(s_);
        std::vector<Operation>& stack = s_.undo_stack;
        int pos = s_.undo_stack_pos;
        if (pos < 0) {
            if (stack.empty()) {
                return;
            }
            pos = static_cast<int>(stack.size()) - 1;
        } else if (pos == 0) {
            return;
        } else {
            --pos;
        }
        deselect();
        Operation op = stack[static_cast<std::size_t>(pos)];
        do_text_op(op, true);
        s_.current_op.version = op.prev_version;
        if (op.chain_backward) {
            while (pos > 0) {
                --pos;
                op = stack[static_cast<std::size_t>(pos)];
                do_text_op(op, true);
                s_.current_op.version = op.prev_version;
                if (op.chain_forward) {
                    break;
                }
            }
        }
        s_.undo_stack_pos = pos;
        area_.refresh();
        const TextEditState::Caret start = stack[static_cast<std::size_t>(pos)].start_caret;
        const bool dirty = s_.caret.line != start.line || s_.caret.column != start.column;
        s_.caret = start;
        if (dirty) {
            caret_changed();
        }
        adjust_viewport_to_caret();
    }

    // redo.
    void redo() {
        if (!edit_.editable) {
            return;
        }
        if (s_.in_action) {
            s_.pending_action_end = true;
        }
        push_current_op(s_);
        std::vector<Operation>& stack = s_.undo_stack;
        int pos = s_.undo_stack_pos;
        if (pos < 0 || std::cmp_greater_equal(pos, stack.size())) {
            return;
        }
        deselect();
        Operation op = stack[static_cast<std::size_t>(pos)];
        do_text_op(op, false);
        s_.current_op.version = op.version;
        if (op.chain_forward) {
            while (std::cmp_less(pos + 1, stack.size())) {
                ++pos;
                op = stack[static_cast<std::size_t>(pos)];
                do_text_op(op, false);
                s_.current_op.version = op.version;
                if (op.chain_backward) {
                    break;
                }
            }
        }
        area_.refresh();
        const TextEditState::Caret end = stack[static_cast<std::size_t>(pos)].end_caret;
        const bool dirty = s_.caret.line != end.line || s_.caret.column != end.column;
        s_.caret = end;
        s_.undo_stack_pos = std::cmp_less(pos + 1, stack.size()) ? pos + 1 : -1;
        if (dirty) {
            caret_changed();
        }
        adjust_viewport_to_caret();
    }

    const TextEditContext& context_;
    world::TextEdit& edit_;
    TextEditState& s_;
    TextArea& area_;
    bool quiet_ = false;        // signals aren't emitted
    bool setting_text_ = false; // in set_text, which doesn't emit text_changed
    bool setting_caret_line_ = false;
    bool text_dirty_ = false; // the component's text needs writing
};

} // namespace

void sync_text_edit(const TextEditContext& context, world::TextEdit& edit, TextEditState& state,
                    TextArea& area) {
    Editor editor(context, edit, state, area);
    editor.finish();
}

bool text_edit_input(const TextEditContext& context, world::TextEdit& edit, TextEditState& state,
                     TextArea& area, const input::Event& event, Vec2 local, Vec2 relative) {
    Editor editor(context, edit, state, area);
    const bool taken = editor.gui_input(event, local, relative);
    editor.finish();
    return taken;
}

void text_edit_scroll_to(const TextEditContext& context, world::TextEdit& edit,
                         TextEditState& state, TextArea& area, bool vertical, double value) {
    Editor editor(context, edit, state, area);
    if (vertical) {
        area.set_v_scroll(value);
    } else {
        area.set_h_scroll(value);
    }
    editor.finish();
}

bool text_edit_unhandled_key(const TextEditContext& context, world::TextEdit& edit,
                             TextEditState& state, TextArea& area, const input::Event& event) {
    Editor editor(context, edit, state, area);
    const bool taken = editor.unhandled_key_input(event);
    editor.finish();
    return taken;
}

void text_edit_focus_enter(const world::TextEdit& edit, TextEditState& state) {
    if (edit.caret_blink) {
        state.blink_timer = 0.0;
    } else {
        state.draw_caret = true;
    }
    if (edit.editable && edit.virtual_keyboard_show_on_focus) {
        state.show_keyboard = edit.virtual_keyboard_enabled;
    }
}

void text_edit_focus_exit(const TextEditContext& context, world::TextEdit& edit,
                          TextEditState& state, TextArea* area) {
    if (area != nullptr) {
        Editor editor(context, edit, state, *area);
        editor.apply_ime();
        editor.finish();
    }
    state.show_keyboard = false;
    if (edit.caret_blink) {
        state.draw_caret = false; // as a redraw without focus leaves it
    }
    if (edit.deselect_on_focus_loss_enabled && !state.selection_drag_attempt) {
        state.caret.selection.active = false;
    }
}

bool text_edit_drop_text(const TextEditContext& context, world::TextEdit& edit,
                         TextEditState& state, TextArea& area, Vec2 local, std::u32string_view text,
                         bool command) {
    Editor editor(context, edit, state, area);
    const bool dropped = editor.drop_text(local, text, command);
    editor.finish();
    return dropped;
}

void text_edit_drag_end(const TextEditContext& context, world::TextEdit& edit, TextEditState& state,
                        TextArea& area, bool command) {
    Editor editor(context, edit, state, area);
    editor.drag_end(command);
    editor.finish();
}

void text_edit_ime_update(const TextEditContext& context, world::TextEdit& edit,
                          TextEditState& state, TextArea& area, std::u32string text,
                          int selection_start, int selection_length) {
    Editor editor(context, edit, state, area);
    editor.ime_update(std::move(text), selection_start, selection_length);
    editor.finish();
}

void text_edit_click_held(const TextEditContext& context, world::TextEdit& edit,
                          TextEditState& state, TextArea& area, Vec2 local) {
    Editor editor(context, edit, state, area);
    editor.click_selection_held(local);
    editor.finish();
}

bool text_edit_tick(const world::TextEdit& edit, TextEditState& state, bool focused,
                    double tick_seconds, bool left_held) {
    // The caret's blink, while it has focus.
    if (edit.caret_blink && focused && edit.caret_blink_interval > 0.0F) {
        state.blink_timer += tick_seconds;
        if (state.blink_timer >= static_cast<double>(edit.caret_blink_interval) - kTimerEpsilon) {
            state.blink_timer = 0.0;
            state.draw_caret = !state.draw_caret;
        }
    } else if (!edit.caret_blink) {
        state.draw_caret = true;
    }
    // idle_detect: typing stopped, so the next typing is a new step to undo.
    if (state.idle_left >= 0.0) {
        state.idle_left -= tick_seconds;
        if (state.idle_left <= kTimerEpsilon) {
            state.idle_left = -1.0;
            push_current_op(state);
        }
    }
    // click_select_held, while the left button is held.
    if (state.click_held >= 0.0) {
        state.click_held -= tick_seconds;
        if (state.click_held <= kTimerEpsilon) {
            state.click_held = -1.0;
            const bool selecting = state.selecting_mode == SelectionMode::Pointer ||
                                   state.selecting_mode == SelectionMode::Word ||
                                   state.selecting_mode == SelectionMode::Line;
            return left_held && selecting;
        }
    }
    return false;
}

} // namespace slash::ui
