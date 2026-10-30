// VoiceOver's backend on iOS (slash/accessibility/mobile.hpp): the controls' tree as
// UIAccessibilityElements in SDL's view, made by ios_element, and VoiceOver's gestures on them
// queued as ios_request makes them for the next update. UIKit asks for the elements on the main
// thread, where SDL runs the game on iOS. Built with ARC.

#include "slash/accessibility/mobile.hpp"

#include "slash/core/log.hpp"
#include "slash/ui/ui.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#import <UIKit/UIKit.h>

namespace slash::accessibility {
namespace {

// What VoiceOver asked since the last update, and the snapshot it asked it of.
struct Gestures {
    Snapshot sent;
    std::vector<Request> queued;

    bool ask(std::uint64_t node, std::string_view gesture) {
        std::optional<Request> request = ios_request(sent, node, gesture);
        if (!request) {
            return false;
        }
        queued.push_back(std::move(*request));
        return true;
    }
};

NSString* ns_string(const std::string& text) {
    return text.empty() ? nil : [NSString stringWithUTF8String:text.c_str()];
}

UIAccessibilityTraits traits_of(const std::vector<std::string>& names) {
    UIAccessibilityTraits traits = UIAccessibilityTraitNone;
    for (const std::string& name : names) {
        if (name == "button") {
            traits |= UIAccessibilityTraitButton;
        } else if (name == "toggle_button") {
            if (@available(iOS 17.0, *)) {
                traits |= UIAccessibilityTraitToggleButton;
            }
        } else if (name == "static_text") {
            traits |= UIAccessibilityTraitStaticText;
        } else if (name == "adjustable") {
            traits |= UIAccessibilityTraitAdjustable;
        } else if (name == "image") {
            traits |= UIAccessibilityTraitImage;
        } else if (name == "selected") {
            traits |= UIAccessibilityTraitSelected;
        } else if (name == "not_enabled") {
            traits |= UIAccessibilityTraitNotEnabled;
        } else if (name == "updates_frequently") {
            traits |= UIAccessibilityTraitUpdatesFrequently;
        }
    }
    return traits;
}

} // namespace
} // namespace slash::accessibility

// A node: its frame in the game's viewport, placed over the view each time VoiceOver asks.
@interface SlashAccessibilityElement : UIAccessibilityElement
@property(nonatomic) std::uint64_t node;
@property(nonatomic) CGRect viewportFrame;
@property(nonatomic) CGSize viewport;
@property(nonatomic, assign) slash::accessibility::Gestures* gestures;
@property(nonatomic, weak) UIView* view;
@end

@implementation SlashAccessibilityElement

- (CGRect)accessibilityFrame {
    UIView* view = self.view;
    if (view == nil || self.viewport.width <= 0 || self.viewport.height <= 0) {
        return CGRectZero;
    }
    const CGFloat sx = view.bounds.size.width / self.viewport.width;
    const CGFloat sy = view.bounds.size.height / self.viewport.height;
    const CGRect frame =
        CGRectMake(self.viewportFrame.origin.x * sx, self.viewportFrame.origin.y * sy,
                   self.viewportFrame.size.width * sx, self.viewportFrame.size.height * sy);
    return UIAccessibilityConvertFrameToScreenCoordinates(frame, view);
}

- (BOOL)accessibilityActivate {
    return self.gestures != nullptr && self.gestures->ask(self.node, "activate");
}

- (void)accessibilityIncrement {
    if (self.gestures != nullptr) {
        (void)self.gestures->ask(self.node, "increment");
    }
}

- (void)accessibilityDecrement {
    if (self.gestures != nullptr) {
        (void)self.gestures->ask(self.node, "decrement");
    }
}

// Three fingers: a swipe up shows the page below, as UIScrollView turns pages.
- (BOOL)accessibilityScroll:(UIAccessibilityScrollDirection)direction {
    if (self.gestures == nullptr) {
        return NO;
    }
    switch (direction) {
        case UIAccessibilityScrollDirectionUp:
        case UIAccessibilityScrollDirectionNext:
            return self.gestures->ask(self.node, "scroll_down");
        case UIAccessibilityScrollDirectionDown:
        case UIAccessibilityScrollDirectionPrevious:
            return self.gestures->ask(self.node, "scroll_up");
        case UIAccessibilityScrollDirectionLeft:
            return self.gestures->ask(self.node, "scroll_right");
        case UIAccessibilityScrollDirectionRight:
            return self.gestures->ask(self.node, "scroll_left");
    }
    return NO;
}

@end

namespace slash::accessibility {
namespace {

class IosBackend final : public Backend {
public:
    IosBackend(UIView* view, std::string name) : view_(view), name_(std::move(name)) {
        elements_ = [NSMutableDictionary dictionary];
        view_.isAccessibilityElement = NO;
    }
    IosBackend(const IosBackend&) = delete;
    IosBackend& operator=(const IosBackend&) = delete;
    IosBackend(IosBackend&&) = delete;
    IosBackend& operator=(IosBackend&&) = delete;
    ~IosBackend() override {
        view_.accessibilityElements = nil;
        for (SlashAccessibilityElement* element in elements_.allValues) {
            element.gestures = nullptr;
        }
    }

    void update(world::World& world) override {
        for (const Request& request : std::exchange(gestures_.queued, {})) {
            if (const Result<void> done = perform(world, request); !done) {
                log::warn("a screen reader's request failed",
                          {{"action", request.action}, {"error", done.error().message}});
            }
        }
        const bool reading = UIAccessibilityIsVoiceOverRunning();
        if (ui::Settings settings = ui::settings(world); settings.screen_reader_active != reading) {
            settings.screen_reader_active = reading;
            ui::set_settings(world, settings);
        }
        reading_ = reading;
        Snapshot now = snapshot(world, name_);
        const Vec2 viewport = ui::settings(world).viewport;
        const std::vector<std::size_t> changed_nodes = changed(gestures_.sent, now);
        const bool first = gestures_.sent.nodes.empty();
        const bool focus_moved = gestures_.sent.focus != now.focus;
        if (!changed_nodes.empty() || viewport != viewport_ || gone(now)) {
            apply(now, changed_nodes, viewport, first);
        }
        viewport_ = viewport;
        gestures_.sent = std::move(now);
        if (!reading) {
            return;
        }
        if (first) {
            UIAccessibilityPostNotification(UIAccessibilityScreenChangedNotification, nil);
        } else if (focus_moved) {
            // VoiceOver follows the game's focus.
            SlashAccessibilityElement* focused = elements_[@(gestures_.sent.focus)];
            UIAccessibilityPostNotification(UIAccessibilityLayoutChangedNotification, focused);
        }
    }

    // Elements place themselves from the view's size each time VoiceOver asks.
    void handle_event(const SDL_Event& /*event*/) override {}

    [[nodiscard]] bool active() const override { return reading_; }

private:
    // Whether a node the elements have is gone.
    bool gone(const Snapshot& now) const {
        std::unordered_map<std::uint64_t, bool> kept;
        for (const auto& [id, node] : now.nodes) {
            kept.emplace(id, true);
        }
        for (const auto& [id, node] : gestures_.sent.nodes) {
            if (!kept.contains(id)) {
                return true;
            }
        }
        return false;
    }

    void apply(const Snapshot& now, const std::vector<std::size_t>& changed_nodes, Vec2 viewport,
               bool first) {
        std::unordered_map<std::uint64_t, std::uint64_t> parents;
        std::unordered_map<std::uint64_t, const Node*> by_id;
        for (const auto& [id, node] : now.nodes) {
            by_id.emplace(id, &node);
            for (const std::uint64_t child : node.children) {
                parents.emplace(child, id);
            }
        }
        // Live regions announce what changed in them, as a page's aria-live does.
        const auto live = [&](std::uint64_t id) {
            for (std::size_t steps = 0; steps <= now.nodes.size(); ++steps) {
                const auto node = by_id.find(id);
                if (node != by_id.end() && node->second->live != "off" &&
                    !node->second->live.empty()) {
                    return true;
                }
                const auto parent = parents.find(id);
                if (parent == parents.end()) {
                    return false;
                }
                id = parent->second;
            }
            return false;
        };
        std::vector<std::string> announcements;
        for (const std::size_t at : changed_nodes) {
            const auto& [id, node] = now.nodes[at];
            const IosElement made = ios_element(node);
            SlashAccessibilityElement* element = elements_[@(id)];
            const bool known = element != nil;
            if (!known) {
                element = [[SlashAccessibilityElement alloc] initWithAccessibilityContainer:view_];
                element.node = id;
                element.gestures = &gestures_;
                element.view = view_;
                elements_[@(id)] = element;
            }
            element.isAccessibilityElement = made.element ? YES : NO;
            element.accessibilityLabel = ns_string(made.label);
            element.accessibilityValue = ns_string(made.value);
            element.accessibilityHint = ns_string(made.hint);
            element.accessibilityTraits = traits_of(made.traits);
            if (node.bounds) {
                const auto& [x0, y0, x1, y1] = *node.bounds;
                element.viewportFrame = CGRectMake(x0, y0, x1 - x0, y1 - y0);
            }
            if (known && !first && made.element && live(id)) {
                std::string said = made.label;
                if (!made.value.empty()) {
                    said += (said.empty() ? "" : ", ") + made.value;
                }
                if (!said.empty()) {
                    announcements.push_back(std::move(said));
                }
            }
        }
        // The elements in the tree's order, the window and containers left out.
        NSMutableArray* ordered = [NSMutableArray arrayWithCapacity:now.nodes.size()];
        NSMutableDictionary* kept = [NSMutableDictionary dictionaryWithCapacity:now.nodes.size()];
        for (const auto& [id, node] : now.nodes) {
            SlashAccessibilityElement* element = elements_[@(id)];
            if (element == nil) {
                continue;
            }
            element.viewport = CGSizeMake(viewport.x, viewport.y);
            kept[@(id)] = element;
            if (element.isAccessibilityElement) {
                [ordered addObject:element];
            }
        }
        for (SlashAccessibilityElement* element in elements_.allValues) {
            if (kept[@(element.node)] == nil) {
                element.gestures = nullptr;
            }
        }
        elements_ = kept;
        view_.accessibilityElements = ordered;
        if (reading_ && !first) {
            UIAccessibilityPostNotification(UIAccessibilityLayoutChangedNotification, nil);
            for (const std::string& said : announcements) {
                UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification,
                                                ns_string(said));
            }
        }
    }

    UIView* view_;
    std::string name_;
    NSMutableDictionary<NSNumber*, SlashAccessibilityElement*>* elements_;
    Gestures gestures_;
    Vec2 viewport_{0.0F, 0.0F};
    bool reading_ = false;
};

} // namespace

bool available() {
    return false;
}

std::unique_ptr<Backend> Backend::attach(SDL_Window* window, std::string name) {
    if (window == nullptr) {
        return nullptr;
    }
    auto* ui_window = (__bridge UIWindow*)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    UIView* view = ui_window.rootViewController.view;
    if (view == nil) {
        log::warn("no view for screen readers to read");
        return nullptr;
    }
    return std::make_unique<IosBackend>(view, std::move(name));
}

} // namespace slash::accessibility
