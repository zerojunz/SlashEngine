#pragma once

#include "slash/core/result.hpp"
#include "slash/exporter/xml.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Android libraries an export adds to a game's APK, as Android's Gradle plugin adds them but
// without Gradle: .aar files from Maven or an SDK package, .jar files, and folders laid out like
// an unpacked .aar (as Unity's .androidlib folders are).
namespace slash::exporter {

struct AndroidLibrary {
    std::string name;           // in messages: "androidx.core:core:1.13.1", or the path
    std::filesystem::path path; // an .aar or .jar file, or a library folder
};

// A library's AndroidManifest.xml, with the library's name for messages.
struct LibraryManifest {
    std::string library;
    XmlElement manifest;
};

// Merges library manifests into the app's, as Android's manifest merger does for what libraries
// declare. Elements of <manifest> and <application> are matched by tag and android:name:
// new ones are added, matching ones merged attribute by attribute, and where two disagree the
// earlier wins (the app, then the libraries in order). ${applicationId} becomes
// application_id; other placeholders, and a library whose minSdkVersion is above min_sdk, are
// errors. tools:node="remove", "removeAll" and "replace" are honored and tools: attributes
// dropped.
[[nodiscard]] Result<XmlElement>
merge_android_manifests(XmlElement app, const std::vector<LibraryManifest>& libraries,
                        std::string_view application_id, int min_sdk);

// What the APK still needs from the libraries: their resource folders, highest priority first,
// and the Java packages that get R classes.
struct AndroidResources {
    std::vector<std::filesystem::path> folders;
    std::vector<std::string> packages;
};

// Unpacks the libraries into work and adds them to tree, the APK's files: Java libraries to
// tree/jars, native libraries for abis to tree/lib/<abi>, assets to tree/assets, and their
// manifests merged into tree/AndroidManifest.xml. Where two libraries ship the same file, the
// earlier one's is kept. The error names the library.
[[nodiscard]] Result<AndroidResources>
add_android_libraries(const std::vector<AndroidLibrary>& libraries,
                      const std::filesystem::path& tree, const std::filesystem::path& work,
                      const std::vector<std::string>& abis, std::string_view application_id,
                      int min_sdk, const std::string& log_path = {});

} // namespace slash::exporter
