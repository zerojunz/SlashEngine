#include "slash/platform/fonts.hpp"

#include <string>
#include <string_view>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>
#include <array>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// DirectWrite's header needs windows.h first.
// clang-format off
#include <windows.h>
#include <dwrite_2.h>
// clang-format on

#include <algorithm>
#include <cstddef>
#include <utility>
#elif defined(__ANDROID__)
#include "slash/platform/file.hpp"

#include <algorithm>
#include <cstddef>
#elif !defined(__EMSCRIPTEN__)
#include <cstdint>
#include <dlfcn.h>
#endif

namespace slash::platform {

#if defined(__APPLE__)

// CoreText's fallback for the text after the system UI font, in the language when one is given.
std::vector<std::string> system_fonts_for(std::string_view text, std::string_view language,
                                          std::string_view /*script*/) {
    std::vector<std::string> out;
    CFStringRef string =
        CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),
                                static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false);
    if (string == nullptr) {
        return out;
    }
    CFStringRef lang = nullptr;
    if (!language.empty()) {
        lang = CFStringCreateWithBytes(
            kCFAllocatorDefault, reinterpret_cast<const UInt8*>(language.data()),
            static_cast<CFIndex>(language.size()), kCFStringEncodingUTF8, false);
    }
    CTFontRef base = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 16.0, lang);
    if (base != nullptr) {
        const CFRange range = CFRangeMake(0, CFStringGetLength(string));
        CTFontRef found = lang != nullptr
                              ? CTFontCreateForStringWithLanguage(base, string, range, lang)
                              : CTFontCreateForString(base, string, range);
        if (found != nullptr) {
            auto* url = static_cast<CFURLRef>(CTFontCopyAttribute(found, kCTFontURLAttribute));
            if (url != nullptr) {
                std::array<UInt8, 4096> path{};
                if (CFURLGetFileSystemRepresentation(url, true, path.data(),
                                                     static_cast<CFIndex>(path.size()))) {
                    const std::string file(reinterpret_cast<const char*>(path.data()));
                    // LastResort.otf draws every character as a box naming its block.
                    if (file.find("LastResort") == std::string::npos) {
                        out.push_back(file);
                    }
                }
                CFRelease(url);
            }
            CFRelease(found);
        }
        CFRelease(base);
    }
    if (lang != nullptr) {
        CFRelease(lang);
    }
    CFRelease(string);
    return out;
}

#elif defined(_WIN32)

namespace {

// Interface IDs, spelled out so MSVC and MinGW headers both work.
constexpr IID kIUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr IID kIDWriteFactory2 = {
    0x0439fc60, 0xca44, 0x4994, {0x8d, 0xee, 0x3a, 0x9a, 0xf7, 0xb7, 0x32, 0xec}};
constexpr IID kIDWriteTextAnalysisSource = {
    0x688e1a58, 0x5094, 0x47c8, {0xad, 0xc8, 0xfb, 0xce, 0xa6, 0x0a, 0xe9, 0x2b}};
constexpr IID kIDWriteLocalFontFileLoader = {
    0xb2d9f3ec, 0xc9fe, 0x4a11, {0xa2, 0xec, 0xd8, 0x62, 0x08, 0xf7, 0xc0, 0xa2}};

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string narrow(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    out.pop_back();
    return out;
}

template <typename T>
void release(T*& object) {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

// The text DirectWrite's fallback reads: one run in one locale. It lives on the stack, so
// reference counting does nothing.
class AnalysisSource final : public IDWriteTextAnalysisSource {
public:
    AnalysisSource(std::wstring text, std::wstring locale)
        : text_(std::move(text)), locale_(std::move(locale)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (IsEqualIID(iid, kIUnknown) || IsEqualIID(iid, kIDWriteTextAnalysisSource)) {
            *object = this;
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 position, const WCHAR** text,
                                                UINT32* length) override {
        if (position >= text_.size()) {
            *text = nullptr;
            *length = 0;
        } else {
            *text = text_.data() + position;
            *length = static_cast<UINT32>(text_.size()) - position;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 position, const WCHAR** text,
                                                    UINT32* length) override {
        if (position == 0 || position > text_.size()) {
            *text = nullptr;
            *length = 0;
        } else {
            *text = text_.data();
            *length = position;
        }
        return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 position, UINT32* length,
                                            const WCHAR** locale) override {
        *locale = locale_.c_str();
        *length = static_cast<UINT32>(text_.size()) -
                  std::min<UINT32>(position, static_cast<UINT32>(text_.size()));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(
        UINT32 position, UINT32* length, IDWriteNumberSubstitution** substitution) override {
        *substitution = nullptr;
        *length = static_cast<UINT32>(text_.size()) -
                  std::min<UINT32>(position, static_cast<UINT32>(text_.size()));
        return S_OK;
    }

private:
    std::wstring text_;
    std::wstring locale_;
};

// The shared DirectWrite factory and its system fallback, made once.
struct DirectWrite {
    IDWriteFactory2* factory = nullptr;
    IDWriteFontFallback* fallback = nullptr;

    DirectWrite() {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, kIDWriteFactory2,
                                       reinterpret_cast<IUnknown**>(&factory)))) {
            factory = nullptr;
            return;
        }
        if (FAILED(factory->GetSystemFontFallback(&fallback))) {
            fallback = nullptr;
        }
    }
    DirectWrite(const DirectWrite&) = delete;
    DirectWrite& operator=(const DirectWrite&) = delete;
    DirectWrite(DirectWrite&&) = delete;
    DirectWrite& operator=(DirectWrite&&) = delete;
    ~DirectWrite() {
        release(fallback);
        release(factory);
    }
};

// The files of the font DirectWrite's fallback maps the text to, after Segoe UI.
std::vector<std::string> map_characters(const DirectWrite& dwrite, std::string_view text,
                                        std::string_view language) {
    std::vector<std::string> out;
    AnalysisSource source(widen(text), language.empty() ? L"en-us" : widen(language));
    UINT32 mapped = 0;
    FLOAT scale = 0.0F;
    IDWriteFont* font = nullptr;
    UINT32 length = 0;
    const WCHAR* unused = nullptr;
    source.GetTextAtPosition(0, &unused, &length);
    if (FAILED(dwrite.fallback->MapCharacters(
            &source, 0, length, nullptr, L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, &mapped, &font, &scale)) ||
        font == nullptr) {
        return out;
    }
    IDWriteFontFace* face = nullptr;
    if (SUCCEEDED(font->CreateFontFace(&face)) && face != nullptr) {
        UINT32 count = 0;
        if (SUCCEEDED(face->GetFiles(&count, nullptr)) && count > 0) {
            std::vector<IDWriteFontFile*> files(count, nullptr);
            if (SUCCEEDED(face->GetFiles(&count, files.data()))) {
                for (IDWriteFontFile* file : files) {
                    IDWriteFontFileLoader* loader = nullptr;
                    IDWriteLocalFontFileLoader* local = nullptr;
                    const void* key = nullptr;
                    UINT32 key_size = 0;
                    if (SUCCEEDED(file->GetLoader(&loader)) &&
                        SUCCEEDED(loader->QueryInterface(kIDWriteLocalFontFileLoader,
                                                         reinterpret_cast<void**>(&local))) &&
                        SUCCEEDED(file->GetReferenceKey(&key, &key_size))) {
                        UINT32 path_length = 0;
                        if (SUCCEEDED(
                                local->GetFilePathLengthFromKey(key, key_size, &path_length))) {
                            std::wstring path(path_length + 1, L'\0');
                            if (SUCCEEDED(local->GetFilePathFromKey(key, key_size, path.data(),
                                                                    path_length + 1))) {
                                std::string utf8 = narrow(path.c_str());
                                std::replace(utf8.begin(), utf8.end(), '\\', '/');
                                out.push_back(std::move(utf8));
                            }
                        }
                    }
                    release(local);
                    release(loader);
                    release(file);
                }
            }
        }
    }
    release(face);
    release(font);
    return out;
}

} // namespace

std::vector<std::string> system_fonts_for(std::string_view text, std::string_view language,
                                          std::string_view /*script*/) {
    static const DirectWrite dwrite;
    if (dwrite.fallback == nullptr || text.empty()) {
        return {};
    }
    return map_characters(dwrite, text, language);
}

#elif defined(__ANDROID__)

namespace {

// A family of Android's font list: the languages it is for and its files.
struct Family {
    std::vector<std::string> languages; // BCP 47 tags, such as "ja" or "und-Arab"
    std::vector<std::string> files;
    bool named = false; // a named family, such as sans-serif, rather than a fallback
};

// An attribute's value in an XML start tag, or "".
std::string attribute(std::string_view tag, std::string_view name) {
    const std::string key = " " + std::string(name) + "=\"";
    const std::size_t at = tag.find(key);
    if (at == std::string_view::npos) {
        return {};
    }
    const std::size_t start = at + key.size();
    const std::size_t end = tag.find('"', start);
    return end == std::string_view::npos ? std::string()
                                         : std::string(tag.substr(start, end - start));
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find(separator, start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        if (end > start) {
            out.emplace_back(text.substr(start, end - start));
        }
        start = end + 1;
    }
    return out;
}

std::string trimmed(std::string_view text) {
    const std::size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(start, end - start + 1));
}

// The families in /system/etc/font_fallback.xml (Android 15 on) or fonts.xml.
std::vector<Family> read_families() {
    std::vector<Family> out;
    auto xml = read_file("/system/etc/font_fallback.xml");
    if (!xml) {
        xml = read_file("/system/etc/fonts.xml");
    }
    if (!xml) {
        return out;
    }
    const std::string& text = xml.value();
    std::size_t at = 0;
    while ((at = text.find("<family", at)) != std::string::npos) {
        const std::size_t tag_end = text.find('>', at);
        const std::size_t close = text.find("</family>", at);
        if (tag_end == std::string::npos || close == std::string::npos) {
            break;
        }
        const std::string_view tag(text.data() + at, tag_end - at);
        Family family;
        family.languages = split(attribute(tag, "lang"), ' ');
        family.named = !attribute(tag, "name").empty();
        std::size_t font = tag_end;
        while ((font = text.find("<font", font)) != std::string::npos && font < close) {
            const std::size_t open_end = text.find('>', font);
            const std::size_t font_close = text.find("</font>", font);
            if (open_end == std::string::npos || font_close == std::string::npos) {
                break;
            }
            const std::string_view font_tag(text.data() + font, open_end - font);
            // Upright regular faces only.
            const std::string style = attribute(font_tag, "style");
            const std::string weight = attribute(font_tag, "weight");
            if ((style.empty() || style == "normal") && (weight.empty() || weight == "400")) {
                std::string name = trimmed(
                    std::string_view(text.data() + open_end + 1, font_close - open_end - 1));
                // Variable fonts list their axes inside the element.
                const std::size_t axis = name.find('<');
                if (axis != std::string::npos) {
                    name = trimmed(name.substr(0, axis));
                }
                if (!name.empty()) {
                    family.files.push_back("/system/fonts/" + name);
                }
            }
            font = font_close;
        }
        out.push_back(std::move(family));
        at = close;
    }
    return out;
}

// Whether a family is for the language or script: a tag with the same language ("ja" for
// "ja-JP") or the same script ("und-Arab" for "Arab").
bool fits(const Family& family, std::string_view language, std::string_view script) {
    const std::string primary = language.substr(0, language.find('-')).empty()
                                    ? std::string()
                                    : std::string(language.substr(0, language.find('-')));
    for (const std::string& tag : family.languages) {
        const std::vector<std::string> parts = split(tag, '-');
        if (parts.empty()) {
            continue;
        }
        if (!primary.empty() && parts[0] == primary) {
            return true;
        }
        if (!script.empty() &&
            std::find(parts.begin() + 1, parts.end(), std::string(script)) != parts.end()) {
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<std::string> system_fonts_for(std::string_view /*text*/, std::string_view language,
                                          std::string_view script) {
    static const std::vector<Family> families = read_families();
    // Han, kana and Hangul are listed by language.
    std::string_view lang = language;
    if (lang.empty() && (script == "Hira" || script == "Kana")) {
        lang = "ja";
    } else if (lang.empty() && script == "Hang") {
        lang = "ko";
    } else if (lang.empty() && script == "Hani") {
        lang = "zh-Hans";
    }
    std::vector<std::string> out;
    const auto add = [&out](const Family& family) {
        for (const std::string& file : family.files) {
            if (std::find(out.begin(), out.end(), file) == out.end()) {
                out.push_back(file);
            }
        }
    };
    for (const Family& family : families) {
        if (fits(family, lang, script)) {
            add(family);
        }
    }
    // Then the default family, which covers Latin, Greek and Cyrillic.
    for (const Family& family : families) {
        if (family.named) {
            add(family);
            break;
        }
    }
    return out;
}

#elif defined(__EMSCRIPTEN__)

// Browsers don't list the machine's fonts to pages.
std::vector<std::string> system_fonts_for(std::string_view /*text*/, std::string_view /*language*/,
                                          std::string_view /*script*/) {
    return {};
}

#else

namespace {

// fontconfig, loaded when first needed so the engine neither links it nor needs its headers; the
// few calls used are declared here with its types made opaque.
struct Fontconfig {
    using Bool = int;
    using Pattern = void;
    using CharSet = void;
    using LangSet = void;
    using Config = void;
    // FcResult and FcMatchKind are enums; FcResultMatch and FcMatchPattern are 0.
    static constexpr int kResultMatch = 0;
    static constexpr int kMatchPattern = 0;

    void* library = nullptr;
    Bool (*init)() = nullptr;
    Pattern* (*pattern_create)() = nullptr;
    void (*pattern_destroy)(Pattern*) = nullptr;
    Bool (*pattern_add_bool)(Pattern*, const char*, Bool) = nullptr;
    Bool (*pattern_add_char_set)(Pattern*, const char*, const CharSet*) = nullptr;
    Bool (*pattern_add_lang_set)(Pattern*, const char*, const LangSet*) = nullptr;
    int (*pattern_get_string)(const Pattern*, const char*, int, const unsigned char**) = nullptr;
    CharSet* (*char_set_create)() = nullptr;
    Bool (*char_set_add_char)(CharSet*, std::uint32_t) = nullptr;
    void (*char_set_destroy)(CharSet*) = nullptr;
    LangSet* (*lang_set_create)() = nullptr;
    Bool (*lang_set_add)(LangSet*, const unsigned char*) = nullptr;
    void (*lang_set_destroy)(LangSet*) = nullptr;
    Bool (*config_substitute)(Config*, Pattern*, int) = nullptr;
    void (*default_substitute)(Pattern*) = nullptr;
    Pattern* (*font_match)(Config*, Pattern*, int*) = nullptr;
    bool ready = false;

    template <typename F>
    bool find(F& function, const char* name) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): dlsym's documented use
        function = reinterpret_cast<F>(dlsym(library, name));
        return function != nullptr;
    }

    Fontconfig() {
        library = dlopen("libfontconfig.so.1", RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) {
            return;
        }
        ready = find(init, "FcInit") && find(pattern_create, "FcPatternCreate") &&
                find(pattern_destroy, "FcPatternDestroy") &&
                find(pattern_add_bool, "FcPatternAddBool") &&
                find(pattern_add_char_set, "FcPatternAddCharSet") &&
                find(pattern_add_lang_set, "FcPatternAddLangSet") &&
                find(pattern_get_string, "FcPatternGetString") &&
                find(char_set_create, "FcCharSetCreate") &&
                find(char_set_add_char, "FcCharSetAddChar") &&
                find(char_set_destroy, "FcCharSetDestroy") &&
                find(lang_set_create, "FcLangSetCreate") && find(lang_set_add, "FcLangSetAdd") &&
                find(lang_set_destroy, "FcLangSetDestroy") &&
                find(config_substitute, "FcConfigSubstitute") &&
                find(default_substitute, "FcDefaultSubstitute") &&
                find(font_match, "FcFontMatch") && init() != 0;
    }
    Fontconfig(const Fontconfig&) = delete;
    Fontconfig& operator=(const Fontconfig&) = delete;
    Fontconfig(Fontconfig&&) = delete;
    Fontconfig& operator=(Fontconfig&&) = delete;
    // fontconfig stays loaded until the process ends: games ask for fonts until then.
    ~Fontconfig() = default;
};

// The characters of UTF-8 text.
std::vector<std::uint32_t> characters(std::string_view text) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        std::size_t length = 1;
        std::uint32_t code = first;
        if (first >= 0xF0U) {
            length = 4;
            code = first & 0x07U;
        } else if (first >= 0xE0U) {
            length = 3;
            code = first & 0x0FU;
        } else if (first >= 0xC0U) {
            length = 2;
            code = first & 0x1FU;
        }
        if (i + length > text.size()) {
            break;
        }
        for (std::size_t k = 1; k < length; ++k) {
            code = (code << 6U) | (static_cast<unsigned char>(text[i + k]) & 0x3FU);
        }
        out.push_back(code);
        i += length;
    }
    return out;
}

} // namespace

// fontconfig's best match for a scalable font with the text's characters, in the language.
std::vector<std::string> system_fonts_for(std::string_view text, std::string_view language,
                                          std::string_view /*script*/) {
    static const Fontconfig fc;
    if (!fc.ready || text.empty()) {
        return {};
    }
    std::vector<std::string> out;
    Fontconfig::Pattern* pattern = fc.pattern_create();
    if (pattern == nullptr) {
        return out;
    }
    fc.pattern_add_bool(pattern, "scalable", 1);
    Fontconfig::CharSet* chars = fc.char_set_create();
    for (const std::uint32_t c : characters(text)) {
        fc.char_set_add_char(chars, c);
    }
    fc.pattern_add_char_set(pattern, "charset", chars);
    Fontconfig::LangSet* langs = nullptr;
    if (!language.empty()) {
        langs = fc.lang_set_create();
        const std::string lang(language);
        fc.lang_set_add(langs, reinterpret_cast<const unsigned char*>(lang.c_str()));
        fc.pattern_add_lang_set(pattern, "lang", langs);
    }
    fc.config_substitute(nullptr, pattern, Fontconfig::kMatchPattern);
    fc.default_substitute(pattern);
    int result = 0;
    Fontconfig::Pattern* match = fc.font_match(nullptr, pattern, &result);
    if (match != nullptr) {
        const unsigned char* file = nullptr;
        if (fc.pattern_get_string(match, "file", 0, &file) == Fontconfig::kResultMatch &&
            file != nullptr) {
            out.emplace_back(reinterpret_cast<const char*>(file));
        }
        fc.pattern_destroy(match);
    }
    fc.pattern_destroy(pattern);
    fc.char_set_destroy(chars);
    if (langs != nullptr) {
        fc.lang_set_destroy(langs);
    }
    return out;
}

#endif

} // namespace slash::platform
