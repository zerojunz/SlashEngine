// Writes the module-definition file that makes a Windows program export the engine to gameplay
// libraries, as -rdynamic and --whole-archive do on Linux (engine/gameplay/CMakeLists.txt,
// slash_gameplay_host). It reads the engine's static libraries (COFF archives of regular or
// /bigobj objects) and lists every external symbol they define once: functions, and data marked
// DATA. Inline functions and template instantiations, which every module that uses them compiles
// for itself (COMDATs any copy of which will do), are left out, so the list stays well under the
// 65535 exports a program may have.
//
//   slash-write-def <out.def> <library.lib>...
//
// A tool built and run with the engine; it may use exceptions.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Bytes = std::vector<unsigned char>;

std::uint32_t read_u16(const Bytes& data, std::size_t at) {
    if (at + 2 > data.size()) {
        throw std::runtime_error("an object file ends early");
    }
    return static_cast<std::uint32_t>(data[at]) | (static_cast<std::uint32_t>(data[at + 1]) << 8U);
}

std::uint32_t read_u32(const Bytes& data, std::size_t at) {
    return read_u16(data, at) | (read_u16(data, at + 2) << 16U);
}

constexpr std::uint32_t kComdat = 0x1000;     // IMAGE_SCN_LNK_COMDAT
constexpr std::uint32_t kNoDuplicates = 1;    // IMAGE_COMDAT_SELECT_NODUPLICATES
constexpr std::uint32_t kClassExternal = 2;   // IMAGE_SYM_CLASS_EXTERNAL
constexpr std::uint32_t kClassStatic = 3;     // IMAGE_SYM_CLASS_STATIC
constexpr std::uint32_t kFunction = 0x20;     // IMAGE_SYM_DTYPE_FUNCTION << 4
constexpr std::uint32_t kMachineI386 = 0x14C; // whose C names start with '_'
constexpr std::array<unsigned char, 16> kBigObjClass{
    0xC7, 0xA1, 0xBA, 0xD1, 0xEE, 0xBA, 0xA9, 0x4B, 0xAF, 0x20, 0xFA, 0xF6, 0x6A, 0xA4, 0xDC, 0xB8};

// The symbols one object defines for others: name, and whether it is data.
void read_object(const Bytes& object, const std::string& where,
                 std::map<std::string, bool>& exports) {
    if (object.size() < 20) {
        return;
    }
    std::uint32_t machine = 0;
    std::size_t sections_at = 0;
    std::uint32_t section_count = 0;
    std::size_t symbols_at = 0;
    std::uint32_t symbol_count = 0;
    std::size_t symbol_size = 18;
    if (read_u16(object, 0) == 0 && read_u16(object, 2) == 0xFFFF) {
        // An import object (version 0), a /bigobj object or one compiled with /GL.
        if (read_u16(object, 4) == 0) {
            return;
        }
        if (object.size() < 56 ||
            !std::equal(kBigObjClass.begin(), kBigObjClass.end(), object.begin() + 12)) {
            throw std::runtime_error(where +
                                     " holds an object compiled with /GL, whose symbols can't be "
                                     "read; build the engine without whole program optimization");
        }
        machine = read_u16(object, 6);
        section_count = read_u32(object, 44);
        symbols_at = read_u32(object, 48);
        symbol_count = read_u32(object, 52);
        sections_at = 56;
        symbol_size = 20;
    } else {
        machine = read_u16(object, 0);
        section_count = read_u16(object, 2);
        symbols_at = read_u32(object, 8);
        symbol_count = read_u32(object, 12);
        sections_at = 20 + read_u16(object, 16);
    }
    if (symbols_at == 0 || symbol_count == 0) {
        return;
    }
    const std::size_t strings_at = symbols_at + (symbol_count * symbol_size);
    if (sections_at + (std::size_t{section_count} * 40) > object.size() ||
        strings_at > object.size()) {
        throw std::runtime_error(where + " is not an object file this tool can read");
    }
    std::vector<std::uint32_t> flags(section_count + 1, 0);
    for (std::uint32_t section = 1; section <= section_count; ++section) {
        flags[section] = read_u32(object, sections_at + (std::size_t{section - 1} * 40) + 36);
    }
    // A COMDAT section's selection, from the auxiliary record of the first symbol naming it.
    std::vector<std::uint32_t> selection(section_count + 1, 0);
    const auto name_of = [&](std::size_t at) {
        if (read_u32(object, at) == 0) {
            const std::size_t start = strings_at + read_u32(object, at + 4);
            if (start >= object.size()) {
                throw std::runtime_error(where + " names a symbol past its string table");
            }
            const auto* text = reinterpret_cast<const char*>(object.data() + start);
            return std::string(text, strnlen(text, object.size() - start));
        }
        const auto* text = reinterpret_cast<const char*>(object.data() + at);
        return std::string(text, strnlen(text, 8));
    };
    struct Defined {
        std::string name;
        std::uint32_t section;
        bool data;
    };
    std::vector<Defined> defined;
    for (std::uint32_t index = 0; index < symbol_count;) {
        const std::size_t at = symbols_at + (static_cast<std::size_t>(index) * symbol_size);
        const std::size_t tail = at + 8 + 4; // after the name and the value
        std::int64_t section = 0;
        std::size_t type_at = 0;
        if (symbol_size == 20) {
            section = static_cast<std::int32_t>(read_u32(object, tail));
            type_at = tail + 4;
        } else {
            section = static_cast<std::int16_t>(read_u16(object, tail));
            type_at = tail + 2;
        }
        const std::uint32_t type = read_u16(object, type_at);
        const std::uint32_t storage = object.at(type_at + 2);
        const std::uint32_t aux = object.at(type_at + 3);
        if (section > 0 && section <= static_cast<std::int64_t>(section_count)) {
            const auto number = static_cast<std::uint32_t>(section);
            if (storage == kClassStatic && aux > 0 && read_u32(object, at + 8) == 0 &&
                (flags[number] & kComdat) != 0 && selection[number] == 0) {
                selection[number] = object.at(at + symbol_size + 14);
            } else if (storage == kClassExternal) {
                defined.push_back({name_of(at), number, (type & 0x30U) != kFunction});
            }
        }
        index += 1 + aux;
    }
    for (Defined& symbol : defined) {
        const bool any_copy =
            (flags[symbol.section] & kComdat) != 0 && selection[symbol.section] != kNoDuplicates;
        if (any_copy || symbol.name.starts_with("__imp_") || symbol.name.empty()) {
            continue;
        }
        if (machine == kMachineI386 && symbol.name.starts_with('_')) {
            symbol.name.erase(0, 1);
        }
        exports.emplace(std::move(symbol.name), symbol.data);
    }
}

// Every object in a COFF archive.
void read_library(const std::string& path, std::map<std::string, bool>& exports) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("can't open " + path);
    }
    const Bytes data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    constexpr std::string_view kMagic = "!<arch>\n";
    if (data.size() < kMagic.size() ||
        std::memcmp(data.data(), kMagic.data(), kMagic.size()) != 0) {
        throw std::runtime_error(path + " is not a static library");
    }
    std::size_t at = kMagic.size();
    while (at + 60 <= data.size()) {
        const std::string name(reinterpret_cast<const char*>(data.data() + at), 16);
        const std::string size_text(reinterpret_cast<const char*>(data.data() + at + 48), 10);
        const std::size_t size = std::stoul(size_text);
        const std::size_t start = at + 60;
        if (start + size > data.size()) {
            throw std::runtime_error(path + " ends inside a member");
        }
        // The linker members ("/" twice), the long names ("//") and other "/<...>/" members.
        if (!name.starts_with("/ ") && !name.starts_with("// ") && !name.starts_with("/<")) {
            const Bytes object(data.begin() + static_cast<std::ptrdiff_t>(start),
                               data.begin() + static_cast<std::ptrdiff_t>(start + size));
            read_object(object, path, exports);
        }
        at = start + size + (size % 2);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: slash-write-def <out.def> <library.lib>...\n";
        return 2;
    }
    try {
        std::map<std::string, bool> exports;
        for (int index = 2; index < argc; ++index) {
            read_library(argv[index], exports);
        }
        if (exports.size() > 65535) {
            throw std::runtime_error("the engine defines " + std::to_string(exports.size()) +
                                     " symbols, more than a program can export");
        }
        std::string text = "EXPORTS\n";
        for (const auto& [name, data] : exports) {
            text += "    " + name + (data ? " DATA\n" : "\n");
        }
        std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            throw std::runtime_error(std::string("can't write ") + argv[1]);
        }
        std::cout << exports.size() << " symbols exported to gameplay libraries\n";
    } catch (const std::exception& error) {
        std::cerr << "slash-write-def: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
