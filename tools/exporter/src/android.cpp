#include "android.hpp"

#include "slash/core/log.hpp"
#include "slash/exporter/presets.hpp"
#include "slash/platform/file.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace slash::exporter {
namespace {

namespace fs = std::filesystem;
using detail::environment;
using detail::Failure;
using detail::find_on_path;

constexpr std::string_view kDebugKeystorePassword = "android";
constexpr std::string_view kDebugKeyAlias = "androiddebugkey";
constexpr std::string_view kActivity = "org.slashengine.GameActivity";
constexpr std::string_view kLogTag = "SlashEngine";
constexpr auto kTestTimeout = std::chrono::seconds(120);
constexpr auto kLaunchGrace = std::chrono::seconds(20);

#ifdef _WIN32
constexpr std::string_view kExe = ".exe";
constexpr std::string_view kScript = ".bat";
constexpr char kPathSeparator = ';';
#else
constexpr std::string_view kExe{};
constexpr std::string_view kScript{};
constexpr char kPathSeparator = ':';
#endif

bool is_file(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}

bool is_folder(const fs::path& path) {
    std::error_code error;
    return !path.empty() && fs::is_directory(path, error);
}

// "36.1.0" -> {36, 1, 0}. Empty for anything but digits and dots, such as "37.0.0-rc1".
std::vector<int> parse_version(std::string_view text) {
    std::vector<int> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('.', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view part = text.substr(start, end - start);
        if (part.empty() || part.size() > 9 ||
            !std::ranges::all_of(part, [](char c) { return c >= '0' && c <= '9'; })) {
            return {};
        }
        int value = 0;
        for (const char c : part) {
            value = value * 10 + (c - '0');
        }
        parts.push_back(value);
        start = end + 1;
    }
    return parts;
}

// The newest subfolder whose name, after prefix, is a version at least minimum and which holds
// the file required. Pre-releases such as "37.0.0-rc1" or "android-37.2-beta3" are skipped.
std::optional<fs::path> newest_folder(const fs::path& parent, std::string_view prefix, int minimum,
                                      std::string_view required) {
    std::optional<fs::path> best;
    std::vector<int> best_version;
    std::error_code error;
    for (auto it = fs::directory_iterator(parent, error); !error && it != fs::directory_iterator();
         it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (!name.starts_with(prefix)) {
            continue;
        }
        const std::vector<int> version =
            parse_version(std::string_view(name).substr(prefix.size()));
        if (version.empty() || version.front() < minimum ||
            (!required.empty() && !is_file(it->path() / required))) {
            continue;
        }
        if (!best || version > best_version) {
            best = it->path();
            best_version = version;
        }
    }
    return best;
}

// The value of key in a properties file such as the NDK's source.properties or a JDK's release
// file, without quotes.
std::string read_property(const fs::path& file, std::string_view key) {
    auto text = platform::read_file(file.string());
    if (!text) {
        return {};
    }
    std::string_view rest = text.value();
    while (!rest.empty()) {
        const std::size_t end = std::min(rest.find('\n'), rest.size());
        const std::string_view line = rest.substr(0, end);
        rest.remove_prefix(std::min(end + 1, rest.size()));
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        std::string_view name = line.substr(0, equals);
        std::string_view value = line.substr(equals + 1);
        const auto trim = [](std::string_view& part) {
            while (!part.empty() && (part.front() == ' ' || part.front() == '"')) {
                part.remove_prefix(1);
            }
            while (!part.empty() &&
                   (part.back() == ' ' || part.back() == '"' || part.back() == '\r')) {
                part.remove_suffix(1);
            }
        };
        trim(name);
        trim(value);
        if (name == key) {
            return std::string(value);
        }
    }
    return {};
}

int major_version(std::string_view version) {
    std::vector<int> parts;
    const std::size_t end = version.find_first_not_of("0123456789.");
    parts = parse_version(version.substr(0, end == std::string_view::npos ? version.size() : end));
    if (parts.empty()) {
        return 0;
    }
    // Java 8 and older call themselves 1.8 and so on.
    return parts.front() == 1 && parts.size() > 1 ? parts[1] : parts.front();
}

std::string home_folder() {
#ifdef _WIN32
    return environment("USERPROFILE");
#else
    return environment("HOME");
#endif
}

std::vector<fs::path> default_sdk_folders() {
    std::vector<fs::path> folders;
#if defined(__APPLE__)
    folders.push_back(fs::path(home_folder()) / "Library" / "Android" / "sdk");
#elif defined(_WIN32)
    folders.push_back(fs::path(environment("LOCALAPPDATA")) / "Android" / "Sdk");
#else
    folders.push_back(fs::path(home_folder()) / "Android" / "Sdk");
#endif
    return folders;
}

std::vector<fs::path> default_java_homes() {
    std::vector<fs::path> folders;
#if defined(__APPLE__)
    folders.emplace_back("/Applications/Android Studio.app/Contents/jbr/Contents/Home");
#elif defined(_WIN32)
    folders.emplace_back("C:/Program Files/Android/Android Studio/jbr");
#else
    folders.emplace_back("/opt/android-studio/jbr");
    folders.push_back(fs::path(home_folder()) / "android-studio" / "jbr");
#endif
    return folders;
}

bool is_jdk(const fs::path& home) {
    const fs::path bin = home / "bin";
    return is_file(bin / ("javac" + std::string(kExe))) &&
           is_file(bin / ("jar" + std::string(kExe))) &&
           is_file(bin / ("keytool" + std::string(kExe)));
}

Result<std::string> find_java(const AndroidSearch& search) {
    std::vector<fs::path> candidates;
    if (!search.java_home.empty()) {
        candidates.emplace_back(search.java_home);
    } else {
        if (const std::string home = environment("JAVA_HOME"); !home.empty()) {
            candidates.emplace_back(home);
        }
        for (fs::path& folder : default_java_homes()) {
            candidates.push_back(std::move(folder));
        }
        if (const std::string javac = find_on_path("javac"); !javac.empty()) {
            std::error_code error;
            const fs::path real = fs::canonical(javac, error);
            if (!error) {
                candidates.push_back(real.parent_path().parent_path());
            }
        }
    }
    std::string too_old;
    for (const fs::path& home : candidates) {
        if (!is_jdk(home)) {
            continue;
        }
        const std::string version = read_property(home / "release", "JAVA_VERSION");
        if (!version.empty() && major_version(version) < kAndroidMinJava) {
            too_old = version;
            continue;
        }
        return home.string();
    }
    std::string message = "Android exports need JDK " + std::to_string(kAndroidMinJava) +
                          " or newer with javac, jar and keytool";
    if (!too_old.empty()) {
        message += " (found Java " + too_old + ")";
    }
    message += "; install one, or Android Studio, and set JAVA_HOME";
    return make_error(ErrorCode::NotFound, std::move(message));
}

Result<std::string> find_ndk(const AndroidSearch& search, const fs::path& sdk) {
    std::vector<fs::path> candidates;
    if (!search.ndk.empty()) {
        candidates.emplace_back(search.ndk);
    } else {
        for (const std::string_view name :
             {"ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "ANDROID_NDK"}) {
            if (const std::string value = environment(name); !value.empty()) {
                candidates.emplace_back(value);
            }
        }
        // Then the SDK's NDKs, newest first.
        std::vector<std::pair<std::vector<int>, fs::path>> installed;
        std::error_code error;
        for (auto it = fs::directory_iterator(sdk / "ndk", error);
             !error && it != fs::directory_iterator(); it.increment(error)) {
            if (std::vector<int> version = parse_version(it->path().filename().string());
                !version.empty()) {
                installed.emplace_back(std::move(version), it->path());
            }
        }
        std::ranges::sort(installed,
                          [](const auto& a, const auto& b) { return a.first > b.first; });
        for (auto& [version, path] : installed) {
            candidates.push_back(std::move(path));
        }
    }
    for (const fs::path& ndk : candidates) {
        if (!is_file(ndk / "build" / "cmake" / "android.toolchain.cmake")) {
            continue;
        }
        if (major_version(read_property(ndk / "source.properties", "Pkg.Revision")) >=
            kAndroidMinNdk) {
            return ndk.string();
        }
    }
    return make_error(ErrorCode::NotFound,
                      "Android exports need NDK r" + std::to_string(kAndroidMinNdk) +
                          " or newer; install it with sdkmanager \"ndk;27.3.13750724\" or set "
                          "ANDROID_NDK_HOME");
}

std::string tool(const AndroidTools& tools, std::string_view name, std::string_view suffix) {
    return (fs::path(tools.build_tools) / (std::string(name) + std::string(suffix))).string();
}

std::string java_tool(const AndroidTools& tools, std::string_view name) {
    return (fs::path(tools.java_home) / "bin" / (std::string(name) + std::string(kExe))).string();
}

std::string adb_path(const AndroidTools& tools) {
    return (fs::path(tools.sdk) / "platform-tools" / ("adb" + std::string(kExe))).string();
}

std::string android_user_home() {
    if (std::string home = environment("ANDROID_USER_HOME"); !home.empty()) {
        return home;
    }
    return (fs::path(home_folder()) / ".android").string();
}

// The .java files under folder, as a javac argument file with one quoted path per line.
std::string java_sources(const fs::path& folder) {
    std::vector<std::string> files;
    std::error_code error;
    for (auto it = fs::recursive_directory_iterator(folder, error);
         !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (it->path().extension() == ".java") {
            files.push_back(fs::absolute(it->path(), error).generic_string());
        }
    }
    std::ranges::sort(files);
    std::string text;
    for (const std::string& file : files) {
        text += '"' + file + "\"\n";
    }
    return text;
}

// Runs adb with the given arguments, returning its output, or nullopt if it failed.
std::optional<std::string> adb(const AndroidTools& tools, const std::string& serial,
                               std::vector<std::string> args, int tail_lines = 40) {
    std::vector<std::string> command{adb_path(tools)};
    if (!serial.empty()) {
        command.insert(command.end(), {"-s", serial});
    }
    command.insert(command.end(), std::make_move_iterator(args.begin()),
                   std::make_move_iterator(args.end()));
    platform::ProcessOptions process;
    process.tail_lines = tail_lines;
    auto result = platform::run_process(command, process);
    if (!result || result.value().exit_code != 0) {
        return std::nullopt;
    }
    return std::move(result).value().tail;
}

// The first connected device's serial, or "" if there is none. ANDROID_SERIAL wins if set.
std::string find_device(const AndroidTools& tools) {
    if (std::string serial = environment("ANDROID_SERIAL"); !serial.empty()) {
        return serial;
    }
    const std::optional<std::string> devices = adb(tools, {}, {"devices"});
    if (!devices) {
        return {};
    }
    std::string_view rest = *devices;
    while (!rest.empty()) {
        const std::size_t end = std::min(rest.find('\n'), rest.size());
        std::string_view line = rest.substr(0, end);
        rest.remove_prefix(std::min(end + 1, rest.size()));
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const std::size_t tab = line.find('\t');
        if (tab != std::string_view::npos && line.substr(tab + 1) == "device") {
            return std::string(line.substr(0, tab));
        }
    }
    return {};
}

std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

} // namespace

Result<AndroidTools> find_android_tools(const AndroidSearch& search) {
    AndroidTools tools;
    std::vector<fs::path> sdks;
    if (!search.sdk.empty()) {
        sdks.emplace_back(search.sdk);
    } else {
        for (const std::string_view name : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
            if (const std::string value = environment(name); !value.empty()) {
                sdks.emplace_back(value);
            }
        }
        for (fs::path& folder : default_sdk_folders()) {
            sdks.push_back(std::move(folder));
        }
    }
    const auto sdk = std::ranges::find_if(sdks, [](const fs::path& folder) {
        return is_folder(folder / "build-tools") || is_folder(folder / "platforms");
    });
    if (sdk == sdks.end()) {
        return make_error(ErrorCode::NotFound,
                          "Android exports need the Android SDK; install Android Studio or its "
                          "command-line tools and set ANDROID_HOME");
    }
    tools.sdk = sdk->string();

    const std::optional<fs::path> build_tools =
        newest_folder(*sdk / "build-tools", "", kAndroidMinBuildTools, "aapt2" + std::string(kExe));
    if (!build_tools) {
        return make_error(ErrorCode::NotFound,
                          "Android exports need build tools " +
                              std::to_string(kAndroidMinBuildTools) +
                              " or newer; install them with sdkmanager \"build-tools;36.0.0\"");
    }
    tools.build_tools = build_tools->string();

    const std::optional<fs::path> platform =
        newest_folder(*sdk / "platforms", "android-", kAndroidTargetSdk, "android.jar");
    if (!platform) {
        return make_error(
            ErrorCode::NotFound,
            "Android exports need the Android " + std::to_string(kAndroidTargetSdk) +
                " platform or newer; install it with sdkmanager \"platforms;android-" +
                std::to_string(kAndroidTargetSdk) + "\"");
    }
    tools.platform = platform->string();

    auto ndk = find_ndk(search, *sdk);
    if (!ndk) {
        return ndk.error();
    }
    tools.ndk = std::move(ndk).value();

    auto java = find_java(search);
    if (!java) {
        return java.error();
    }
    tools.java_home = std::move(java).value();
    return tools;
}

namespace detail {

bool prepare_android_key(bool debug, AndroidKey& key, Failure& failure) {
    const std::string keystore = environment(kAndroidKeystorePath);
    if (!debug && !keystore.empty()) {
        if (!is_file(keystore)) {
            failure = {"options",
                       std::string(kAndroidKeystorePath) + " names " + keystore +
                           ", which does not exist",
                       {}};
            return false;
        }
        if (environment(kAndroidKeystorePassword).empty()) {
            failure = {"options",
                       "signing with the keystore in " + std::string(kAndroidKeystorePath) +
                           " needs its password in " + std::string(kAndroidKeystorePassword),
                       {}};
            return false;
        }
        key.release = true;
        key.keystore = keystore;
        key.signer_args = {"--ks", keystore, "--ks-pass",
                           "env:" + std::string(kAndroidKeystorePassword)};
        if (const std::string alias = environment(kAndroidKeyAlias); !alias.empty()) {
            key.signer_args.insert(key.signer_args.end(), {"--ks-key-alias", alias});
        }
        const std::string_view key_password = environment(kAndroidKeyPassword).empty()
                                                  ? kAndroidKeystorePassword
                                                  : kAndroidKeyPassword;
        key.signer_args.insert(key.signer_args.end(),
                               {"--key-pass", "env:" + std::string(key_password)});
        return true;
    }
    key.release = false;
    key.keystore = (fs::path(android_user_home()) / "debug.keystore").string();
    const std::string password = "pass:" + std::string(kDebugKeystorePassword);
    key.signer_args = {"--ks",       key.keystore,     "--ks-pass",
                       password,     "--ks-key-alias", std::string(kDebugKeyAlias),
                       "--key-pass", password};
    return true;
}

platform::ProcessOptions android_process(const AndroidTools& tools,
                                         platform::ProcessOptions process) {
    process.environment.emplace_back("JAVA_HOME", tools.java_home);
    process.environment.emplace_back("PATH", (fs::path(tools.java_home) / "bin").string() +
                                                 kPathSeparator + environment("PATH"));
    return process;
}

std::string android_ninja(const AndroidTools& tools) {
    if (std::string ninja = find_on_path("ninja"); !ninja.empty()) {
        return ninja;
    }
    const std::optional<fs::path> cmake =
        newest_folder(fs::path(tools.sdk) / "cmake", "", 3, "bin/ninja" + std::string(kExe));
    return cmake ? (*cmake / "bin" / ("ninja" + std::string(kExe))).string() : std::string();
}

std::vector<std::string> android_configure_args(const AndroidTools& tools, const std::string& abi,
                                                const std::string& package) {
    return {
        "-DCMAKE_TOOLCHAIN_FILE=" +
            (fs::path(tools.ndk) / "build" / "cmake" / "android.toolchain.cmake").generic_string(),
        "-DANDROID_ABI=" + abi,
        "-DANDROID_PLATFORM=android-" + std::to_string(kAndroidMinSdk),
        "-DSLASH_ANDROID_PACKAGE=" + package,
    };
}

bool package_apk(const AndroidTools& tools, const AndroidKey& key, const ApkInfo& info,
                 const fs::path& tree, const AndroidResources& resources, const fs::path& work,
                 const fs::path& apk, const platform::ProcessOptions& process, Failure& failure) {
    std::error_code error;
    const fs::path apk_work = fs::absolute(work / "apk", error);
    fs::remove_all(apk_work, error);
    const fs::path classes = apk_work / "classes";
    const fs::path dex = apk_work / "dex";
    const fs::path compiled = apk_work / "res";
    const fs::path gen = apk_work / "gen";
    for (const fs::path& folder : {classes, dex, compiled, gen}) {
        if (fs::create_directories(folder, error); error) {
            break;
        }
    }
    if (error) {
        failure = {"package", "could not create " + display(apk_work) + ": " + error.message(), {}};
        return false;
    }
    const std::string android_jar = (fs::path(tools.platform) / "android.jar").string();
    // Java libraries from SDK packages: compiled against, and dexed into the APK with the game.
    std::vector<std::string> jars;
    for (const fs::directory_entry& entry : fs::directory_iterator(tree / "jars", error)) {
        if (entry.path().extension() == ".jar") {
            jars.push_back(fs::absolute(entry.path(), error).string());
        }
    }
    error.clear();
    std::ranges::sort(jars);
    std::string classpath = android_jar;
    for (const std::string& jar : jars) {
        classpath += kPathSeparator;
        classpath += jar;
    }

    const fs::path sources = apk_work / "sources.txt";
    const std::string classes_jar = (apk_work / "classes.jar").string();
    const std::string unaligned = (apk_work / "unaligned.apk").string();
    const std::string aligned = (apk_work / "aligned.apk").string();
    const std::string signed_apk = (apk_work / "signed.apk").string();
    std::vector<std::string> d8{tool(tools, "d8", kScript),
                                info.debug ? "--debug" : "--release",
                                "--min-api",
                                std::to_string(kAndroidMinSdk),
                                "--lib",
                                android_jar,
                                "--output",
                                dex.string(),
                                classes_jar};
    d8.insert(d8.end(), jars.begin(), jars.end());

    std::vector<std::string> link{tool(tools, "aapt2", kExe),
                                  "link",
                                  "-o",
                                  unaligned,
                                  "--manifest",
                                  (tree / "AndroidManifest.xml").string(),
                                  "-I",
                                  android_jar,
                                  "-A",
                                  (tree / "assets").string(),
                                  "--min-sdk-version",
                                  std::to_string(kAndroidMinSdk),
                                  "--target-sdk-version",
                                  std::to_string(kAndroidTargetSdk),
                                  "--version-code",
                                  std::to_string(info.version_code),
                                  "--version-name",
                                  info.version_name};
    if (info.debug) {
        link.emplace_back("--debug-mode");
    }
    // The game's own resources, its icon, which the libraries' overlay.
    if (const fs::path res = tree / "res"; fs::is_directory(res, error)) {
        const std::string flat = (compiled / "game.zip").string();
        if (!detail::step("package",
                          {tool(tools, "aapt2", kExe), "compile", "--dir",
                           fs::absolute(res, error).string(), "-o", flat},
                          process, failure)) {
            return false;
        }
        link.push_back(flat);
    }
    error.clear();
    // Library resources, compiled one folder at a time and linked as overlays: aapt2 lets the
    // last one win, so the highest priority goes last. Their packages get R classes, which the
    // libraries' code reads its resource IDs from.
    for (std::size_t i = resources.folders.size(); i-- > 0;) {
        const std::string flat = (compiled / (std::to_string(i) + ".zip")).string();
        if (!detail::step("package",
                          {tool(tools, "aapt2", kExe), "compile", "--dir",
                           fs::absolute(resources.folders[i], error).string(), "-o", flat},
                          process, failure)) {
            return false;
        }
        link.insert(link.end(), {"-R", flat});
    }
    if (!resources.folders.empty()) {
        link.emplace_back("--auto-add-overlay");
    }
    if (!resources.packages.empty()) {
        std::string packages;
        for (const std::string& package : resources.packages) {
            packages += (packages.empty() ? "" : ":") + package;
        }
        link.insert(link.end(), {"--java", gen.string(), "--extra-packages", packages});
    }
    if (!detail::step("package", link, process, failure)) {
        return false;
    }
    if (!platform::write_file(sources.string(), java_sources(tree / "java") + java_sources(gen))) {
        failure = {"package", "could not write " + display(sources), {}};
        return false;
    }
    if (!detail::step("package",
                      {java_tool(tools, "javac"), "-encoding", "UTF-8", "-source", "11", "-target",
                       "11", "-Xlint:-options", "-nowarn", "-classpath", classpath, "-d",
                       classes.string(), "@" + sources.string()},
                      process, failure) ||
        !detail::step("package",
                      {java_tool(tools, "jar"), "--create", "--file", classes_jar, "-C",
                       classes.string(), "."},
                      process, failure) ||
        !detail::step("package", d8, process, failure) ||
        // Code and libraries are stored uncompressed: Android maps the libraries straight
        // from the APK. Large libraries make d8 write several classes*.dex files.
        !detail::step("package",
                      {java_tool(tools, "jar"), "--update", "--no-compress", "--no-manifest",
                       "--file", unaligned, "-C", fs::absolute(tree, error).string(), "lib", "-C",
                       dex.string(), "."},
                      process, failure) ||
        !detail::step("package",
                      {tool(tools, "zipalign", kExe), "-f", "-P", "16", "4", unaligned, aligned},
                      process, failure)) {
        return false;
    }

    if (!key.release && !is_file(key.keystore)) {
        fs::create_directories(fs::path(key.keystore).parent_path(), error);
        if (!detail::step("sign",
                          {java_tool(tools, "keytool"), "-genkeypair", "-noprompt", "-keystore",
                           key.keystore, "-storepass", std::string(kDebugKeystorePassword),
                           "-alias", std::string(kDebugKeyAlias), "-keypass",
                           std::string(kDebugKeystorePassword), "-keyalg", "RSA", "-keysize",
                           "2048", "-validity", "10000", "-dname",
                           "CN=Android Debug,O=Android,C=US"},
                          process, failure)) {
            return false;
        }
    }
    // Without --alignment-preserved, apksigner pads the libraries again to its own default page
    // size, which may undo zipalign's 16 KB alignment.
    std::vector<std::string> sign{tool(tools, "apksigner", kScript), "sign",
                                  "--alignment-preserved"};
    sign.insert(sign.end(), key.signer_args.begin(), key.signer_args.end());
    sign.insert(sign.end(), {"--out", signed_apk, aligned});
    if (!detail::step("sign", sign, process, failure)) {
        return false;
    }
    fs::create_directories(apk.parent_path(), error);
    fs::copy_file(signed_apk, apk, fs::copy_options::overwrite_existing, error);
    if (error) {
        failure = {"package", "could not write " + display(apk) + ": " + error.message(), {}};
        return false;
    }
    return true;
}

json::Value test_apk(const AndroidTools& tools, const fs::path& apk, const std::string& package,
                     const std::vector<std::string>& abis, const fs::path& log_path) {
    json::Value test;
    if (!is_file(adb_path(tools))) {
        test["skipped"] = "adb is not installed; install it with sdkmanager \"platform-tools\"";
        return test;
    }
    const std::string serial = find_device(tools);
    if (serial.empty()) {
        test["skipped"] = "no Android device or emulator is connected (adb devices)";
        return test;
    }
    test["device"] = serial;
    const std::string device_abis =
        trimmed(adb(tools, serial, {"shell", "getprop", "ro.product.cpu.abilist"}).value_or(""));
    const auto runs = std::ranges::find_if(abis, [&](const std::string& abi) {
        std::string_view rest = device_abis;
        while (!rest.empty()) {
            const std::size_t end = std::min(rest.find(','), rest.size());
            if (rest.substr(0, end) == abi) {
                return true;
            }
            rest.remove_prefix(std::min(end + 1, rest.size()));
        }
        return false;
    });
    if (runs == abis.end()) {
        test["skipped"] = "the device runs " + device_abis + ", which this APK doesn't include";
        return test;
    }

    log::info("export step", {{"step", "test"}});
    platform::ProcessOptions process;
    process.log_path = log_path.string();
    process.tail_lines = 20;
    const std::vector<std::string> device{adb_path(tools), "-s", serial};
    const auto run = [&](std::vector<std::string> args) {
        std::vector<std::string> command = device;
        command.insert(command.end(), args.begin(), args.end());
        return platform::run_process(command, process);
    };
    auto installed = run({"install", "-r", fs::absolute(apk).string()});
    if (!installed || installed.value().exit_code != 0) {
        test["passed"] = false;
        test["message"] = "adb install failed; an APK signed with another key must be "
                          "uninstalled first (adb uninstall " +
                          package + ")";
        if (installed) {
            test["tail"] = installed.value().tail;
        }
        return test;
    }
    (void)run({"logcat", "-c"});
    auto started =
        run({"shell", "am", "start", "-S", "-n", package + "/" + std::string(kActivity), "--esa",
             "args", "--headless,--ticks," + std::to_string(detail::kTestTicks)});
    if (!started || started.value().exit_code != 0) {
        test["passed"] = false;
        test["message"] = "could not start the game's activity";
        if (started) {
            test["tail"] = started.value().tail;
        }
        return test;
    }

    // Wait for the summary line in logcat, or for the game to exit without one. The process may
    // take a moment to appear, so its absence only counts once it has run or the grace is over.
    const auto launched = std::chrono::steady_clock::now();
    std::optional<json::Value> summary;
    bool seen_running = false;
    while (std::chrono::steady_clock::now() < launched + kTestTimeout) {
        const std::optional<std::string> lines = adb(
            tools, serial, {"logcat", "-d", "-v", "raw", "-s", std::string(kLogTag) + ":I"}, 200);
        if (lines) {
            summary = detail::find_summary(*lines);
        }
        if (summary) {
            break;
        }
        const bool running = adb(tools, serial, {"shell", "pidof", package}).has_value();
        seen_running = seen_running || running;
        if (!running &&
            (seen_running || std::chrono::steady_clock::now() > launched + kLaunchGrace)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (!summary) {
        // One more look, in case the game wrote its summary and exited between checks.
        if (const auto lines =
                adb(tools, serial, {"logcat", "-d", "-v", "raw", "-s", std::string(kLogTag) + ":I"},
                    200)) {
            summary = detail::find_summary(*lines);
        }
    }
    const bool passed =
        summary.has_value() && summary->find("ticks")->is_int() &&
        summary->find("ticks")->as_int() == static_cast<std::int64_t>(detail::kTestTicks);
    test["passed"] = passed;
    if (summary) {
        detail::add_summary(test, *summary);
    }
    if (!passed) {
        test["message"] = summary ? "the game stopped early"
                                  : "the game exited or timed out "
                                    "without printing its summary";
        test["tail"] = adb(tools, serial,
                           {"logcat", "-d", "-v", "brief", "-s", std::string(kLogTag),
                            "AndroidRuntime", "DEBUG", "libc"},
                           30)
                           .value_or("");
    }
    return test;
}

} // namespace detail
} // namespace slash::exporter
