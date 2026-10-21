#pragma once

#include "slash/core/result.hpp"

#include <string>

// What Android exports need from the machine: the Android SDK with its build tools, a platform
// and the NDK, and a JDK. No Gradle: export compiles the Java with javac, converts it with d8,
// links the APK with aapt2, aligns it with zipalign and signs it with apksigner.
namespace slash::exporter {

inline constexpr int kAndroidMinSdk = 30;        // Android 11, the first release from 2020
inline constexpr int kAndroidTargetSdk = 36;     // what Google Play requires of apps
inline constexpr int kAndroidMinBuildTools = 35; // zipalign -P aligns libraries to 16 KB pages
inline constexpr int kAndroidMinNdk = 27;
inline constexpr int kAndroidMinJava = 17;

struct AndroidTools {
    std::string sdk;         // the Android SDK
    std::string ndk;         // the NDK, with build/cmake/android.toolchain.cmake
    std::string build_tools; // <sdk>/build-tools/<version>, with aapt2, d8, zipalign, apksigner
    std::string platform;    // <sdk>/platforms/android-<level>, with android.jar
    std::string java_home;   // a JDK with javac, jar and keytool in bin
};

// Where to look. Empty fields mean the usual places: ANDROID_HOME or ANDROID_SDK_ROOT, the SDK
// slash-cli toolchain install android installed (install_android) and then Android Studio's
// default SDK folder; ANDROID_NDK_HOME, ANDROID_NDK_ROOT or ANDROID_NDK and then the newest NDK
// in the SDK; JAVA_HOME, the JDK slash-cli installed, Android Studio's bundled JDK and then javac
// on PATH.
struct AndroidSearch {
    std::string sdk;
    std::string ndk;
    std::string java_home;
};

// Finds the newest stable build tools (35 or later), the newest stable platform (36 or later), an
// NDK (r27 or later) and a JDK (17 or later). The error names what is missing and how to install
// it.
[[nodiscard]] Result<AndroidTools> find_android_tools(const AndroidSearch& search = {});

} // namespace slash::exporter
