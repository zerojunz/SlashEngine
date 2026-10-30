// The web's screen reader backend (slash/accessibility/web.hpp): the controls' tree as hidden
// WAI-ARIA elements over the game's canvas, kept up to date by dom_changes, with the clicks and
// focus a screen reader gives them queued for the next update. Web builds have no AccessKit.
// Only web builds compile it (engine/accessibility/CMakeLists.txt); the guard keeps tools that
// read every source, such as clang-tidy, from compiling it elsewhere.
#ifdef __EMSCRIPTEN__

#include "slash/accessibility/web.hpp"

#include "slash/core/log.hpp"
#include "slash/ui/ui.hpp"

#include <SDL3/SDL_events.h>

#include <cstdlib>
#include <emscripten/em_js.h>
#include <string>
#include <utility>

namespace {

// clang-format off

// Makes the elements' root after the canvas: as large as the canvas and over it, but taking no
// pointer events and showing nothing, so the game looks and plays as before. 0 without a page.
EM_JS(int, slash_a11y_start, (), {
    if (typeof document === "undefined" || !Module["canvas"]) {
        return 0;
    }
    const canvas = Module["canvas"];
    const root = document.createElement("div");
    root.id = "slash-accessibility";
    root.style.cssText = "position:absolute;overflow:hidden;pointer-events:none;" +
                         "color:transparent;background:transparent;margin:0;padding:0;";
    canvas.insertAdjacentElement("afterend", root);
    const state = {root, canvas, elements: new Map(), queue: [], viewport: [1, 1], focusing: false};
    const request = (event, action) => {
        const actions = (event.currentTarget.dataset.actions || "").split(" ");
        if (actions.includes(action)) {
            state.queue.push({node: event.currentTarget.dataset.node, action});
        }
    };
    state.listen = (element) => {
        element.addEventListener("click", (event) => {
            event.preventDefault();
            request(event, "click");
        });
        element.addEventListener("focus", (event) => {
            if (!state.focusing) {
                request(event, "focus");
            }
        });
    };
    Module["slashAccessibility"] = state;
    return 1;
});

EM_JS_DEPS(slash_a11y, "$UTF8ToString,$stringToNewUTF8");

// Applies dom_changes' JSON, with the viewport the bounds are in, and places the elements over
// the canvas as it is now laid out on the page.
EM_JS(void, slash_a11y_apply, (const char* text, double width, double height), {
    const state = Module["slashAccessibility"];
    if (!state) {
        return;
    }
    const changes = JSON.parse(UTF8ToString(text));
    state.viewport = [width, height];
    const elementOf = (id) => {
        let element = state.elements.get(id);
        if (!element) {
            element = document.createElement("div");
            element.id = "slash-a11y-" + id;
            element.dataset.node = id;
            element.tabIndex = -1;
            element.style.cssText = "position:absolute;margin:0;padding:0;border:0;" +
                                    "overflow:hidden;white-space:nowrap;";
            element.slashText = document.createTextNode("");
            element.appendChild(element.slashText);
            element.slashBounds = [0, 0, 0, 0];
            state.listen(element);
            state.elements.set(id, element);
        }
        return element;
    };
    for (const id of changes.remove || []) {
        const element = state.elements.get(id);
        if (element) {
            element.remove();
            state.elements.delete(id);
        }
    }
    const set = changes.set || [];
    for (const node of set) {
        const element = elementOf(node.id);
        for (const name of element.getAttributeNames()) {
            const ours = name === "role" || name === "title" || name.startsWith("aria-") ||
                         name === "data-actions";
            if (ours && !(name in node.attributes) && !(name === "role" && node.role)) {
                element.removeAttribute(name);
            }
        }
        if (node.role) {
            element.setAttribute("role", node.role);
        }
        for (const name in node.attributes) {
            element.setAttribute(name, node.attributes[name]);
        }
        element.slashText.data = node.text;
        if (node.bounds.length === 4) {
            element.slashBounds = node.bounds;
        }
    }
    for (const node of set) {
        const element = elementOf(node.id);
        for (const child of node.children) {
            element.appendChild(elementOf(child));
        }
    }
    const top = state.elements.get("1");
    if (top && top.parentNode !== state.root) {
        state.root.appendChild(top);
    }
    // The window's element is the canvas, as large as the viewport.
    if (top) {
        top.slashBounds = [0, 0, width, height];
    }
    // Over the canvas, each element placed in its parent's box.
    const canvas = state.canvas;
    state.root.style.left = canvas.offsetLeft + "px";
    state.root.style.top = canvas.offsetTop + "px";
    state.root.style.width = canvas.offsetWidth + "px";
    state.root.style.height = canvas.offsetHeight + "px";
    const sx = canvas.clientWidth / Math.max(1, state.viewport[0]);
    const sy = canvas.clientHeight / Math.max(1, state.viewport[1]);
    for (const element of state.elements.values()) {
        const parent = element.parentNode;
        const from = parent && parent.slashBounds ? parent.slashBounds : [0, 0, 0, 0];
        const [x0, y0, x1, y1] = element.slashBounds;
        element.style.left = (x0 - from[0]) * sx + "px";
        element.style.top = (y0 - from[1]) * sy + "px";
        element.style.width = Math.max(0, x1 - x0) * sx + "px";
        element.style.height = Math.max(0, y1 - y0) * sy + "px";
    }
    // Focus follows the game's, unless the page's focus is somewhere else than the game.
    if ("focus" in changes) {
        const element = state.elements.get(changes.focus);
        const active = document.activeElement;
        if (element && changes.focus !== "1" &&
            (!active || active === document.body || active === canvas ||
             state.root.contains(active))) {
            state.focusing = true;
            element.focus({preventScroll: true});
            state.focusing = false;
        }
    }
});

// The requests queued since the last call, as JSON the caller frees; null when there are none.
EM_JS(char*, slash_a11y_take, (), {
    const state = Module["slashAccessibility"];
    if (!state || state.queue.length === 0) {
        return 0;
    }
    const text = JSON.stringify(state.queue);
    state.queue.length = 0;
    return stringToNewUTF8(text);
});

EM_JS(void, slash_a11y_stop, (), {
    const state = Module["slashAccessibility"];
    if (state) {
        state.root.remove();
        delete Module["slashAccessibility"];
    }
});

// clang-format on

} // namespace

namespace slash::accessibility {
namespace {

class WebBackend final : public Backend {
public:
    explicit WebBackend(std::string name) : name_(std::move(name)) {}
    WebBackend(const WebBackend&) = delete;
    WebBackend& operator=(const WebBackend&) = delete;
    WebBackend(WebBackend&&) = delete;
    WebBackend& operator=(WebBackend&&) = delete;
    ~WebBackend() override { slash_a11y_stop(); }

    void update(world::World& world) override {
        if (char* queued = slash_a11y_take(); queued != nullptr) {
            auto parsed = json::parse(queued);
            std::free(queued); // NOLINT(cppcoreguidelines-no-malloc)
            if (parsed) {
                for (const Request& request : dom_requests(parsed.value())) {
                    // Only a screen reader clicks or focuses the hidden elements.
                    active_ = true;
                    if (const Result<void> done = perform(world, request); !done) {
                        log::warn("a screen reader's request failed",
                                  {{"action", request.action}, {"error", done.error().message}});
                    }
                }
            }
        }
        if (ui::Settings settings = ui::settings(world); settings.screen_reader_active != active_) {
            settings.screen_reader_active = active_;
            ui::set_settings(world, settings);
        }
        Snapshot now = snapshot(world, name_);
        const json::Value changes = dom_changes(sent_, now);
        const Vec2 viewport = ui::settings(world).viewport;
        if (changes.size() > 0 || moved_ || viewport != viewport_) {
            const std::string text = json::write(changes, json::Style::Compact);
            slash_a11y_apply(text.c_str(), viewport.x, viewport.y);
            moved_ = false;
            viewport_ = viewport;
        }
        sent_ = std::move(now);
    }

    void handle_event(const SDL_Event& event) override {
        // The canvas moves or changes size on the page; the elements follow it.
        if (event.type == SDL_EVENT_WINDOW_RESIZED ||
            event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
            event.type == SDL_EVENT_WINDOW_MOVED) {
            moved_ = true;
        }
    }

    [[nodiscard]] bool active() const override { return active_; }

private:
    std::string name_;
    Snapshot sent_;
    Vec2 viewport_{0.0F, 0.0F};
    bool moved_ = true;
    bool active_ = false;
};

} // namespace

bool available() {
    return false;
}

std::unique_ptr<Backend> Backend::attach(SDL_Window* /*window*/, std::string name) {
    if (slash_a11y_start() == 0) {
        return nullptr;
    }
    return std::make_unique<WebBackend>(std::move(name));
}

} // namespace slash::accessibility

#endif
