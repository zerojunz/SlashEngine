// What OpenXR needs of the platform before an instance, ported from Godot's
// modules/openxr/extensions/platform/openxr_android_extension.cpp (OpenXRAndroidExtension): on
// Android the loader and the instance are given the app's Java VM and activity. Elsewhere there
// is nothing to do.
#include "openxr_graphics.hpp"

#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>

#include <jni.h>
#include <openxr/openxr_platform.h>
#include <string>
#endif

namespace slash::xr::detail {

#if defined(__ANDROID__)
namespace {

struct Android {
    JavaVM* vm = nullptr;
    jobject activity = nullptr; // a global reference, kept for the process's life
    bool loader_initialized = false;
    XrInstanceCreateInfoAndroidKHR instance_info{};
};

Android& android() {
    static Android state;
    return state;
}

// The VM and a global reference to SDL's activity, once.
Result<void> find_activity() {
    Android& state = android();
    if (state.activity != nullptr) {
        return {};
    }
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto* activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (env == nullptr || activity == nullptr || env->GetJavaVM(&state.vm) != JNI_OK) {
        return make_error(ErrorCode::PlatformFailure,
                          "OpenXR: the app's Java VM and activity couldn't be found");
    }
    state.activity = env->NewGlobalRef(activity);
    env->DeleteLocalRef(activity);
    return {};
}

} // namespace

// OpenXRAndroidExtension::on_before_instance_created.
Result<void> initialize_platform_loader() {
    Android& state = android();
    if (state.loader_initialized) {
        return {};
    }
    if (auto found = find_activity(); !found) {
        return found;
    }
    PFN_xrInitializeLoaderKHR initialize = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                        reinterpret_cast<PFN_xrVoidFunction*>(&initialize))) ||
        initialize == nullptr) {
        return make_error(ErrorCode::PlatformFailure,
                          "OpenXR: the loader lacks xrInitializeLoaderKHR");
    }
    XrLoaderInitInfoAndroidKHR info{};
    info.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
    info.applicationVM = state.vm;
    info.applicationContext = state.activity;
    if (const XrResult result =
            initialize(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&info));
        XR_FAILED(result)) {
        return make_error(ErrorCode::PlatformFailure, "OpenXR: the loader didn't start [XrResult " +
                                                          std::to_string(result) + "]");
    }
    state.loader_initialized = true;
    return {};
}

std::string_view platform_instance_extension() {
    return XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
}

// OpenXRAndroidExtension::set_instance_create_info_and_get_next_pointer.
const void* platform_instance_info(const void* next) {
    Android& state = android();
    state.instance_info = {};
    state.instance_info.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
    state.instance_info.next = next;
    state.instance_info.applicationVM = state.vm;
    state.instance_info.applicationActivity = state.activity;
    return &state.instance_info;
}
#else
Result<void> initialize_platform_loader() {
    return {};
}

std::string_view platform_instance_extension() {
    return {};
}

const void* platform_instance_info(const void* next) {
    return next;
}
#endif

} // namespace slash::xr::detail
