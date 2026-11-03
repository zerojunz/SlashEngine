// TalkBack's backend (slash/accessibility/mobile.hpp): the controls' tree as virtual views of
// org.slashengine.accessibility.Bridge, an AccessibilityNodeProvider on SDL's surface, which the
// game's thread hands android_changes' JSON and takes TalkBack's actions from through JNI.
//
// Only Android builds compile it (engine/accessibility/CMakeLists.txt); the guard keeps tools that
// read every source, such as clang-tidy, from compiling it elsewhere.
#ifdef __ANDROID__

#include "slash/accessibility/mobile.hpp"
#include "slash/core/log.hpp"
#include "slash/ui/ui.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_system.h>

#include <jni.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace slash::accessibility {
namespace {

constexpr const char* kBridgeClass = "org.slashengine.accessibility.Bridge";

JNIEnv* env() {
    return static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
}

// Logs and clears a pending Java exception; true if there was one.
bool failed(JNIEnv* jni, std::string_view what) {
    if (jni->ExceptionCheck() == JNI_FALSE) {
        return false;
    }
    jni->ExceptionDescribe();
    jni->ExceptionClear();
    log::error("the screen reader bridge threw", {{"call", std::string(what)}});
    return true;
}

// The bridge's class, through the activity's class loader, which SDL's thread also reaches. A
// global reference, or null.
jclass load_bridge(JNIEnv* jni, jobject activity) {
    jclass activity_class = jni->GetObjectClass(activity);
    jmethodID get_loader =
        jni->GetMethodID(activity_class, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = jni->CallObjectMethod(activity, get_loader);
    jclass loader_class = jni->FindClass("java/lang/ClassLoader");
    jmethodID load =
        jni->GetMethodID(loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = jni->NewStringUTF(kBridgeClass);
    auto* found = static_cast<jclass>(jni->CallObjectMethod(loader, load, name));
    jclass out = nullptr;
    if (!failed(jni, "loading the bridge") && found != nullptr) {
        out = static_cast<jclass>(jni->NewGlobalRef(found));
        jni->DeleteLocalRef(found);
    }
    jni->DeleteLocalRef(name);
    jni->DeleteLocalRef(loader_class);
    jni->DeleteLocalRef(loader);
    jni->DeleteLocalRef(activity_class);
    return out;
}

class AndroidBackend final : public Backend {
public:
    AndroidBackend(jclass bridge, std::string name) : bridge_(bridge), name_(std::move(name)) {
        JNIEnv* jni = env();
        const auto method = [&](const char* method_name, const char* signature) {
            jmethodID found = jni->GetStaticMethodID(bridge_, method_name, signature);
            return failed(jni, method_name) ? nullptr : found;
        };
        apply_ = method("apply", "(Ljava/lang/String;DD)V");
        take_ = method("take", "()Ljava/lang/String;");
        active_ = method("active", "()Z");
        stop_ = method("stop", "()V");
    }
    AndroidBackend(const AndroidBackend&) = delete;
    AndroidBackend& operator=(const AndroidBackend&) = delete;
    AndroidBackend(AndroidBackend&&) = delete;
    AndroidBackend& operator=(AndroidBackend&&) = delete;
    ~AndroidBackend() override {
        JNIEnv* jni = env();
        if (stop_ != nullptr) {
            jni->CallStaticVoidMethod(bridge_, stop_);
            (void)failed(jni, "stop");
        }
        jni->DeleteGlobalRef(bridge_);
    }

    void update(world::World& world) override {
        JNIEnv* jni = env();
        if (apply_ == nullptr || take_ == nullptr || active_ == nullptr) {
            return;
        }
        if (auto* queued = static_cast<jstring>(jni->CallStaticObjectMethod(bridge_, take_));
            !failed(jni, "take") && queued != nullptr) {
            std::string text;
            if (const char* chars = jni->GetStringUTFChars(queued, nullptr); chars != nullptr) {
                text = chars;
                jni->ReleaseStringUTFChars(queued, chars);
            }
            jni->DeleteLocalRef(queued);
            if (auto parsed = json::parse(text); parsed) {
                for (const Request& request : android_requests(parsed.value(), sent_)) {
                    if (const Result<void> done = perform(world, request); !done) {
                        log::warn("a screen reader's request failed",
                                  {{"action", request.action}, {"error", done.error().message}});
                    }
                }
            }
        }
        const jboolean asked = jni->CallStaticBooleanMethod(bridge_, active_);
        const bool reading = !failed(jni, "active") && asked == JNI_TRUE;
        if (ui::Settings settings = ui::settings(world); settings.screen_reader_active != reading) {
            settings.screen_reader_active = reading;
            ui::set_settings(world, settings);
        }
        reading_ = reading;
        Snapshot now = snapshot(world, name_);
        const json::Value changes = android_changes(sent_, now);
        const Vec2 viewport = ui::settings(world).viewport;
        if (changes.size() > 0 || viewport != viewport_) {
            const std::string text = json::write(changes, json::Style::Compact);
            jstring java_text = jni->NewStringUTF(text.c_str());
            jni->CallStaticVoidMethod(bridge_, apply_, java_text, static_cast<jdouble>(viewport.x),
                                      static_cast<jdouble>(viewport.y));
            (void)failed(jni, "apply");
            jni->DeleteLocalRef(java_text);
            viewport_ = viewport;
        }
        sent_ = std::move(now);
    }

    // The surface's size and place are read from the view when TalkBack asks.
    void handle_event(const SDL_Event& /*event*/) override {}

    [[nodiscard]] bool active() const override { return reading_; }

private:
    jclass bridge_;
    std::string name_;
    jmethodID apply_ = nullptr;
    jmethodID take_ = nullptr;
    jmethodID active_ = nullptr;
    jmethodID stop_ = nullptr;
    Snapshot sent_;
    Vec2 viewport_{0.0F, 0.0F};
    bool reading_ = false;
};

} // namespace

bool available() {
    return false;
}

std::unique_ptr<Backend> Backend::attach(SDL_Window* /*window*/, std::string name) {
    JNIEnv* jni = env();
    auto* activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (jni == nullptr || activity == nullptr) {
        return nullptr;
    }
    jclass bridge = load_bridge(jni, activity);
    bool started = false;
    if (bridge != nullptr) {
        jmethodID start = jni->GetStaticMethodID(bridge, "start", "(Landroid/app/Activity;)Z");
        if (!failed(jni, "finding start") && start != nullptr) {
            const jboolean done = jni->CallStaticBooleanMethod(bridge, start, activity);
            started = !failed(jni, "start") && done == JNI_TRUE;
        }
    }
    jni->DeleteLocalRef(activity);
    if (!started) {
        if (bridge != nullptr) {
            jni->DeleteGlobalRef(bridge);
        }
        log::warn("no screen reader bridge", {{"class", kBridgeClass}});
        return nullptr;
    }
    return std::make_unique<AndroidBackend>(bridge, std::move(name));
}

} // namespace slash::accessibility

#endif
