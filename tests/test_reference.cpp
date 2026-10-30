#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/reference.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/reflect.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace reftest {

struct Coil {
    float turns = 3.0F;
};
SLASH_REFLECT(Coil, turns)

struct Spring {
    float stiffness = 1.0F;
    std::vector<Coil> coils;
};
SLASH_COMPONENT(Spring, stiffness, coils)

} // namespace reftest

namespace {

using namespace slash;

// engine.describe's full reply from a session over the built-in components and commands, with
// Spring described as describe_spring does.
template <typename Describe>
json::Value describe_with_spring(Describe describe_spring) {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    describe_spring(components.add<reftest::Spring>());
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::Session session(components, commands, {});
    command::Reply reply =
        session.call("engine.describe", json::parse(R"({"full": true})").value());
    REQUIRE_MESSAGE(reply.ok, reply.message);
    return std::move(reply.value);
}

const std::string& page(const std::vector<command::ReferencePage>& pages, std::string_view path) {
    for (const command::ReferencePage& page : pages) {
        if (page.path == path) {
            return page.text;
        }
    }
    FAIL("no page " << path);
    static const std::string none;
    return none;
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("the reference lists what has no description, nested struct members included") {
    const json::Value bare = describe_with_spring([](world::ComponentType& type) {
        type.describe("stiffness", {.doc = "How hard it pulls back.", .min = 0.0});
    });
    // Every built-in component, field, command and param has a description; only Spring lacks.
    CHECK(command::undocumented(bare) ==
          std::vector<std::string>{"component Spring", "field Coil.turns", "field Spring.coils"});

    const json::Value full = describe_with_spring([](world::ComponentType& type) {
        type.set_doc("Pulls what it holds back to where it was.")
            .describe("stiffness", {.doc = "How hard it pulls back.", .min = 0.0})
            .describe("coils", {.doc = "Its coils, in order."})
            .describe("coils[].turns", {.doc = "How many times the coil winds."});
    });
    CHECK(command::undocumented(full).empty());
}

TEST_CASE("the reference writes components, commands and systems as Markdown tables") {
    const json::Value describe = describe_with_spring([](world::ComponentType& type) {
        type.set_doc("Pulls what it holds back to where it was.")
            .describe("stiffness", {.doc = "How hard it pulls back.", .min = 0.0})
            .describe("coils", {.doc = "Its coils, in order."})
            .describe("coils[].turns", {.doc = "How many times the coil winds."});
    });
    const std::vector<command::ReferencePage> pages = command::reference_pages(describe);
    REQUIRE(pages.size() == 4);

    const std::string& readme = page(pages, "README.md");
    CHECK(contains(readme, "- [Components](components.md): "));
    CHECK(contains(readme, "`read`, `edit`, `run`"));

    // A nested struct gets a section of its own, which its field links to.
    const std::string& components = page(pages, "components.md");
    CHECK(contains(components, "## Spring\n\nPulls what it holds back to where it was.\n"));
    CHECK(contains(components, "| `coils` | array of [Coil](#spring-coil) | `[]` | Its coils, "
                               "in order. |"));
    CHECK(contains(components, "| `stiffness` | number, at least 0 | `1.0` | How hard it pulls "
                               "back. |"));
    CHECK(contains(components, "### Spring Coil"));
    CHECK(contains(components, "| `turns` | number |  | How many times the coil winds. |"));

    const std::string& commands = page(pages, "commands.md");
    CHECK(contains(commands, "## step\n"));
    CHECK(contains(commands, "Needs `run`."));
    CHECK(contains(commands, "| `ticks` | integer, 1 to 100000 | `1` | Ticks to run. |"));

    const std::string& systems = page(pages, "systems.md");
    CHECK(contains(systems, "| -1000 | `input` | Applies the input events queued since the last "
                            "tick. |"));

    // The same engine writes the same pages.
    CHECK(command::reference_pages(describe)[1].text == components);
}
