// Text in every language, as Godot's: main.scene.json is a title menu in Arabic, laid out right
// to left by the menu's layout_direction, with a Hebrew greeting and an exit sign in Japanese and
// in Chinese. fonts/ui.font.json is Roboto with fallback fonts for the characters Roboto lacks,
// and the project's theme makes it every control's font.
//
// This program writes the fonts, checks the menu with ui.layout, prints which font drew each
// label, measures text with text.measure, lays the menu out left to right for comparison, then
// draws it into a texture.

#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/device.hpp"
#include "slash/text/text.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace canvas = slash::canvas;
namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;
namespace text = slash::text;
namespace ui = slash::ui;
namespace world = slash::world;

// Stops with the error if a call fails, so the output never hides one.
void check(const slash::Result<void>& result) {
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().message.c_str());
        std::exit(1);
    }
}

void write_font(const std::string& path, std::span<const std::uint8_t> bytes) {
    check(slash::platform::write_file(
        path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
    std::printf("wrote %s\n", path.c_str());
}

// Games ship real fonts (Noto Sans Arabic, Noto Sans Hebrew, Noto Sans JP and so on). So that it
// has no binary file, this example writes Roboto, the engine's built-in font, and placeholder
// fonts whose glyphs are boxes for the rest: every Arabic and Hebrew letter, and 出口 ("exit")
// twice, standing in for a Japanese and a Chinese font.
void write_fonts() {
    std::u32string arabic;
    for (char32_t c = U'ء'; c <= U'ي'; ++c) {
        arabic += c;
    }
    std::u32string hebrew;
    for (char32_t c = U'א'; c <= U'ת'; ++c) {
        hebrew += c;
    }
    write_font("fonts/roboto.ttf", text::default_font());
    write_font("fonts/arabic.ttf", text::box_font(arabic, "Arabic Boxes"));
    write_font("fonts/hebrew.ttf", text::box_font(hebrew, "Hebrew Boxes"));
    write_font("fonts/japanese.ttf", text::box_font(U"出口", "Japanese Boxes"));
    write_font("fonts/chinese.ttf", text::box_font(U"出口", "Chinese Boxes"));
}

// Prints the call as an agent would make it, then the reply.
void call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
}

// The fonts that drew a label's glyphs, left to right, from C++: each glyph names its font face.
void print_fonts(world::World& w, const ui::Layout& layout, std::string_view id) {
    const text::Fonts& fonts = *ui::fonts(w);
    const ui::LabelText* label = layout.label(w.find(id));
    std::string used;
    std::string last;
    for (const text::Glyph& glyph : label->text.glyphs) {
        const std::string path = fonts.face_info(glyph.face).path;
        if (path != last) {
            used += (used.empty() ? "" : ", ") + path;
            last = path;
        }
    }
    std::printf("%.*s: %s\n", static_cast<int>(id.size()), id.data(), used.c_str());
}

} // namespace

int main() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = ".";
    options.capture_logs = false;
    command::Session session(components, commands, options);
    world::World& w = session.world();

    write_fonts();
    call(session, "scene.open", R"({"path": "main.scene.json"})");

    // The menu is mirrored: its column hugs the right edge, the title starts at the right, Start
    // is right of Quit, and the bar fills from the right. Lines that go right to left say rtl;
    // the Hebrew greeting goes right to left too, but its control is laid out left to right, so
    // it stays on the left.
    call(session, "ui.layout", "{}");

    // Each character comes from the first font in fonts/ui.font.json that has it: Arabic from
    // arabic.ttf, the version's digits and Latin letters from Roboto. A language picks between
    // fonts that both have a character: the Japanese sign takes japanese.ttf, the Chinese one
    // chinese.ttf.
    const ui::Layout layout = ui::layout(w, *ui::fonts(w), ui::settings(w).viewport);
    for (const std::string_view id : {"title", "version", "greeting", "sign", "sign_chinese"}) {
        print_fonts(w, layout, id);
    }

    // text.measure takes a direction and a language too. Roboto alone has no Arabic, so the last
    // call counts the letters as missing: project.json turns off fonts installed on the machine,
    // which would otherwise draw them (counted in system_glyphs), so this output is the same
    // everywhere.
    call(session, "text.measure",
         R"json({"text": "الإصدار 1.2 (beta)", "font": "fonts/ui.font.json"})json");
    call(session, "text.measure",
         R"({"text": "Score: 120", "direction": "rtl", "font": "fonts/ui.font.json"})");
    call(session, "text.measure", R"({"text": "مرحبا", "font": "fonts/roboto.ttf"})");

    // Laid out left to right, the same menu starts at the left.
    call(session, "component.set",
         R"({"id": "menu", "components": {"Control": {"layout_direction": "ltr"}}})");
    call(session, "ui.layout", R"({"ids": ["menu", "title", "start", "quit"]})");
    call(session, "undo", "{}");

    // Draw into a 400x240 texture: the boxes draw where each letter would.
    const auto device =
        render::create_device_or_null({.window = nullptr, .driver = "auto", .debug = false});
    canvas::CanvasRenderer renderer(*device, session.assets());
    const render::TextureId target = device
                                         ->create_texture({.width = 400,
                                                           .height = 240,
                                                           .filter = render::Filter::Linear,
                                                           .render_target = true})
                                         .value();
    check(renderer.draw(w, {.target = target,
                            .clear = render::kDefaultClearColor,
                            .width = 0.0F,
                            .height = 0.0F,
                            .interpolation = 1.0,
                            .texture_filter = render::Filter::Linear,
                            .time = 0.0}));
    std::printf("controls drawn: %zu, glyphs drawn: %zu\n", renderer.controls_drawn(),
                renderer.glyphs_drawn());
    return 0;
}
