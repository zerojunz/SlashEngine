#pragma once

#include "slash/text/text.hpp"
#include "slash/ui/ui.hpp"

#include <cstdint>
#include <string>
#include <vector>

// What the gui system and the layout share about LineEdits (line_edit.cpp): the state Godot's
// LineEdit keeps between events, and the geometry it works out from its shaped text and theme
// (scene/gui/line_edit.cpp: _shape, set_caret_column, get_caret_pixel_pos,
// set_caret_at_pixel_pos, _get_right_icon_size, get_minimum_size and NOTIFICATION_DRAW).
namespace slash::ui {

// The clear button's icon, Godot's line_edit_clear: 16 by 16 pixels.
inline constexpr float kClearIconSize = 16.0F;

// What a LineEdit keeps between events, as Godot's LineEdit members. Columns count characters
// (code points).
struct EditState {
    struct Selection {
        int begin = 0;
        int end = 0;
        int start_column = 0; // where a drag or shift-move started
        bool enabled = false;
        bool creating = false; // a mouse drag is selecting
        bool double_click = false;
        friend bool operator==(const Selection&, const Selection&) = default;
    };
    // TextOperation: one entry of the undo history.
    struct Operation {
        std::string text;
        int caret_column = 0;
        float scroll_offset = 0.0F;
        friend bool operator==(const Operation&, const Operation&) = default;
    };

    bool editing = false;
    Selection selection;
    float scroll_offset = 0.0F;
    std::u32string ime_text;
    int ime_selection_start = 0; // Godot's ime_selection: x the start, y the length
    int ime_selection_length = 0;
    std::vector<Operation> undo;
    int undo_pos = -1; // the entry undo stepped back to; -1 for none (undo_stack_pos null)
    bool text_changed_dirty = false;
    bool pending_select_all = false;
    bool show_keyboard = false; // the on-screen keyboard is asked for
    text::Direction input_direction = text::Direction::Ltr;
    bool draw_caret = true; // the blink's phase
    double blink_timer = 0.0;
    double last_double_click = -1.0; // seconds of world time
    Vec2 last_double_click_position;
    bool clear_press_attempt = false;
    bool clear_pressing_inside = false;
    // selection.drag_attempt, kept out of the selection, which deselect clears: a press landed
    // on the selection, which a drag may carry away.
    bool drag_attempt = false;
    bool drag_caret_force_displayed = false; // a drag over it shows where text would drop
    // The text as last seen, so a change made from outside resets the history as set_text does.
    std::string known_text;
    bool known = false;

    friend bool operator==(const EditState&, const EditState&) = default;
};

// The EditState the gui system keeps for a LineEdit, or nullptr if it keeps none (gui.cpp).
[[nodiscard]] const EditState* edit_state(const world::World& world, entt::entity entity);
// Whether a LineEdit has focus (gui.cpp).
[[nodiscard]] bool has_focus(const world::World& world, entt::entity entity);

// A LineEdit's text shaped for its rect, and where Godot's LineEdit puts things in it.
class Field {
public:
    Field(text::Fonts& fonts, const LineEditView& view, Vec2 size);

    // _shape: lays out what it shows, the placeholder while the text is empty, the text with the
    // input method's at the caret, or as many secret characters.
    void shape(const world::LineEdit& edit, const EditState& state);

    [[nodiscard]] const text::TextLayout& layout() const { return layout_; }
    [[nodiscard]] const text::ShapedLine& line() const { return line_; }
    [[nodiscard]] const std::string& shown() const { return shown_; }
    [[nodiscard]] bool placeholder() const { return placeholder_; }
    [[nodiscard]] float text_height() const { return layout_.metrics.height(); }
    [[nodiscard]] float full_width() const { return line_.width(); }
    [[nodiscard]] const LineEditView& view() const { return *view_; }
    [[nodiscard]] Vec2 size() const { return size_; }
    [[nodiscard]] const std::string& error() const { return error_; }
    // Its rect changed size.
    void resize(Vec2 size) { size_ = size; }

    // The clear button shows (text, editable, enabled), or the right icon is set; and its size,
    // as _get_right_icon_size.
    [[nodiscard]] bool clear_icon_shown(const world::LineEdit& edit) const;
    [[nodiscard]] bool icon_shown(const world::LineEdit& edit) const;
    [[nodiscard]] Vec2 icon_size(const world::LineEdit& edit, bool clear) const;
    // Where the text starts and the last x it may reach, as the draw and caret code work them
    // out; draw rounds the right-aligned start as NOTIFICATION_DRAW does.
    [[nodiscard]] float x_ofs(const world::LineEdit& edit, const EditState& state,
                              bool draw = false) const;
    [[nodiscard]] float ofs_max(const world::LineEdit& edit) const;
    // The y of the text's top.
    [[nodiscard]] float y_ofs() const;
    // get_caret_pixel_pos: the caret's x, and the end of the input method's text (or the caret's
    // x again).
    [[nodiscard]] Vec2 caret_pixel_pos(const world::LineEdit& edit, const EditState& state) const;
    // set_caret_column, after the column is set: scrolls so the caret shows, then shows as much
    // text as fits.
    void fit(const world::LineEdit& edit, EditState& state) const;
    // set_caret_at_pixel_pos: the column a click at x puts the caret at.
    [[nodiscard]] int column_at(const world::LineEdit& edit, const EditState& state, float x) const;
    // _is_over_clear_button.
    [[nodiscard]] bool over_clear_button(const world::LineEdit& edit, Vec2 point) const;
    // get_minimum_size.
    [[nodiscard]] Vec2 minimum_size(const world::LineEdit& edit) const;

    // What NOTIFICATION_DRAW draws: under the text (style box, focus, icon and selection), the
    // text, and over it (carets and the composition's underline).
    void draw(const world::LineEdit& edit, const EditState& state, bool focused,
              std::vector<Box>& under, LabelText& text, std::vector<Box>& over,
              LineEditView& out) const;

private:
    text::Fonts* fonts_;
    const LineEditView* view_;
    Vec2 size_;
    std::string shown_;
    bool placeholder_ = true;
    text::TextLayout layout_;
    text::ShapedLine line_;
    float font_height_ = 0.0F;
    float em_ = 0.0F; // the width of "W"
    std::string error_;
};

// A LineEdit's text as Godot's String: characters (code points).
[[nodiscard]] std::u32string characters(const world::LineEdit& edit);

} // namespace slash::ui
