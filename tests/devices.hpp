#pragma once

#include "slash/platform/process.hpp"
#include "slash/render/device.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <utility>
#include <vector>

// The render devices tests draw pixels with, so each check runs on every one that works here.
namespace slash::test {

// A GPU device with no window, or null when this machine has no GPU backend. CI sets
// SLASH_EXPECT_GPU where one works (software Vulkan on Linux), so a broken GPU path fails there
// instead of being skipped.
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

// An OpenGL ES 3.0 device with no window, or null where the system has no OpenGL ES (macOS, and
// Windows without a GPU driver that has it). Linux CI has Mesa's, so there it must work.
inline std::unique_ptr<render::RenderDevice> gl_device() {
    auto device = render::create_device({.window = nullptr, .driver = "opengl", .debug = false});
    if (!device) {
#ifdef __linux__
        if (!platform::environment_variable("SLASH_EXPECT_GPU").empty()) {
            FAIL("SLASH_EXPECT_GPU is set, but OpenGL ES doesn't work: " << device.error().message);
        }
#endif
        MESSAGE("no OpenGL ES here, so its checks are skipped: " << device.error().message);
        return nullptr;
    }
    return std::move(device).value();
}

// The devices that draw on a GPU: a GPU device and an OpenGL ES device, where they work.
inline std::vector<std::unique_ptr<render::RenderDevice>> hardware_devices() {
    std::vector<std::unique_ptr<render::RenderDevice>> devices;
    if (auto gpu = gpu_device(); gpu != nullptr) {
        devices.push_back(std::move(gpu));
    }
    if (auto gl = gl_device(); gl != nullptr) {
        devices.push_back(std::move(gl));
    }
    return devices;
}

// The devices that draw pixels: the software device, then the hardware devices.
inline std::vector<std::unique_ptr<render::RenderDevice>> drawing_devices() {
    std::vector<std::unique_ptr<render::RenderDevice>> devices = hardware_devices();
    devices.insert(devices.begin(), render::create_software_device());
    return devices;
}

} // namespace slash::test
