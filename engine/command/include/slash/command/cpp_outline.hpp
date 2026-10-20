#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// A C++ file's outline, read from its text without compiling it: the types and functions it
// declares, the macros it defines and those it invokes outside functions, the string literals it
// passes, the files it includes and the names it uses. project.map and api.search build on it. It
// reads code as written, so a macro that expands to a declaration, or a declaration in a branch the
// preprocessor drops, is read as it looks.
namespace slash::command {

// A class's data member.
struct CppField {
    std::string declaration; // "int coins = 0", on one line
    std::string doc;
    std::string access; // "public", "protected" or "private"
    int line = 0;
};

struct CppSymbol {
    std::string kind;      // "struct", "class", "union", "enum", "alias" or "function"
    std::string name;      // as written, with a class it is defined outside: "Player::jump"
    std::string scope;     // the namespaces and classes it is in: "game::Player", or ""
    std::string member_of; // the class it is declared in, or ""
    std::string access;    // a member's: "public", "protected" or "private"; else ""
    // Its declaration on one line: a function's without its body ("int add(int a, int b)"), a
    // type's head ("struct Coin : Item") or an alias ("using Id = std::string").
    std::string signature;
    std::string doc; // the comment lines right above it, without the slashes
    int line = 0;    // of its name, from 1
    bool definition = false;
    // A function body's identifiers, sorted.
    std::vector<std::string> body_identifiers;
    // A class's data members, in order.
    std::vector<CppField> fields;
};

// A macro the file defines with #define.
struct CppDefine {
    std::string name;
    std::string params; // "(name, ...)" for a function-like macro, else ""
    std::string doc;
    int line = 0;
};

// A macro invoked outside functions, such as SLASH_COMPONENT(Wallet, coins, income): a name in
// capitals followed by its arguments.
struct CppMacro {
    std::string name;
    std::vector<std::string> args; // each one's tokens
    int line = 0;
};

// A string literal (adjacent ones joined), with the call it is an argument of.
struct CppString {
    std::string value;
    std::string callee;            // the function or macro called with it, or ""
    std::size_t arg = 0;           // which of its arguments, from 0
    std::vector<std::string> args; // the call's arguments, each one's tokens
    std::string function;          // the function whose body it is in, or ""
    std::string compared;          // the name it is compared with: "kind" in event.kind == "hit"
    int line = 0;
};

struct CppOutline {
    std::vector<CppSymbol> symbols;
    std::vector<CppDefine> defines;
    std::vector<CppMacro> macros;
    std::vector<CppString> strings;
    std::vector<std::string> includes;    // #include "..." paths, as written
    std::vector<std::string> identifiers; // every identifier, sorted
    int lines = 0;
};

[[nodiscard]] CppOutline outline_cpp(std::string_view text);

} // namespace slash::command
