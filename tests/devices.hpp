#pragma once

#include "slash/platform/process.hpp"
#include "slash/render/device.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <utility>
#include <vector>

// The render devices tests draw pixels with, so each check runs on every one that works here.
namespace slash::test {

// A GPU device with no window, or null when this machine has no GPU backend: WebGPU in the web
// build. CI sets SLASH_EXPECT_GPU where one works (software Vulkan on Linux), so a broken GPU path
// fails there instead of being skipped.
inline std::unique_ptr<render::RenderDevice> gpu_device() {
    auto device = render::create_device({.window = nullptr, .driver = "auto", .debug = false});
    if (!device) {
        if (!platform::environment_variable("SLASH_EXPECT_GPU").empty()) {
            FAIL("SLASH_EXPECT_GPU is set, but no GPU device works: " << device.error().message);
        }
        MESSAGE("no GPU here, so GPU checks are skipped: " << device.error().message);
        return nullptr;
    }
    return std::move(device).value();
}

// An OpenGL device with no window for a driver, "opengl" (OpenGL 3.3, or OpenGL ES 3.0 where
// that doesn't start) or "opengl_es", or null where the system has neither (Windows without a GPU
// driver that has them; macOS has no OpenGL ES). Linux CI has Mesa's, so there both must work.
inline std::unique_ptr<render::RenderDevice> gl_device(const char* driver = "opengl") {
#ifdef __EMSCRIPTEN__
    MESSAGE("the web build runs in Node, which has no WebGL, so " << driver
                                                                  << " checks are skipped");
    return nullptr;
#endif
    auto device = render::create_device({.window = nullptr, .driver = driver, .debug = false});
    if (!device) {
#ifdef __linux__
        if (!platform::environment_variable("SLASH_EXPECT_GPU").empty()) {
            FAIL("SLASH_EXPECT_GPU is set, but " << driver
                                                 << " doesn't work: " << device.error().message);
        }
#endif
        MESSAGE("no " << driver << " here, so its checks are skipped: " << device.error().message);
        return nullptr;
    }
    return std::move(device).value();
}

// The devices that draw on a GPU: a GPU device, an OpenGL 3.3 device and an OpenGL ES 3.0 device,
// where they work. In the web build (tests/web), the GPU device is WebGPU's, and there is no
// OpenGL: Node has no WebGL.
inline std::vector<std::unique_ptr<render::RenderDevice>> hardware_devices() {
    std::vector<std::unique_ptr<render::RenderDevice>> devices;
    if (auto gpu = gpu_device(); gpu != nullptr) {
        devices.push_back(std::move(gpu));
    }
#ifndef __EMSCRIPTEN__
    for (const char* driver : {"opengl", "opengl_es"}) {
        if (auto gl = gl_device(driver); gl != nullptr) {
            devices.push_back(std::move(gl));
        }
    }
#endif
    return devices;
}

// The devices that draw pixels: the software device, then the hardware devices.
inline std::vector<std::unique_ptr<render::RenderDevice>> drawing_devices() {
    std::vector<std::unique_ptr<render::RenderDevice>> devices = hardware_devices();
    devices.insert(devices.begin(), render::create_software_device());
    return devices;
}

} // namespace slash::test
