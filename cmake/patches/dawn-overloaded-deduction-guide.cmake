# Adds the deduction guide Tint's overloaded helper needs on Xcode 15's clang (and clang 16),
# which lack C++20's class template argument deduction for aggregates. slash_add_dependency(dawn)
# runs it in Dawn's source folder as the patch step. A script rather than a patch file: it works
# with either line ending and on a source folder it already changed, and needs no patch tool.

set(file src/tint/lang/core/ir/transform/multiplanar_external_texture.cc)
file(READ ${file} text)
if(text MATCHES "overloaded\\(Ts\\.\\.\\.\\) ->")
    return()
endif()
set(struct "struct overloaded : Ts\\.\\.\\. {(\r?\n)    using Ts::operator\\(\\)\\.\\.\\.;\r?\n};\r?\n")
if(NOT text MATCHES "${struct}")
    message(FATAL_ERROR "${file} no longer defines overloaded as the patch expects")
endif()
string(REGEX REPLACE "(${struct})"
    "\\1template <class... Ts>\\2overloaded(Ts...) -> overloaded<Ts...>;\\2" text "${text}")
file(WRITE ${file} "${text}")
