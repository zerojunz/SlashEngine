# Writes a C++ source file holding a file's bytes, for data built into the engine such as the
# default font. Runs at build time:
#
#   cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DNAMESPACE=<ns> -DNAME=<symbol> -P embed.cmake
#
# The source defines `const unsigned char NAME[]` and `const std::size_t NAME_size` in NAMESPACE.

cmake_minimum_required(VERSION 3.25)

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" length)
math(EXPR bytes "${length} / 2")
# 20 bytes a line, each as 0xNN.
string(REGEX REPLACE "(........................................)" "\\1\n" hex "${hex}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
get_filename_component(source "${INPUT}" NAME)
file(WRITE "${OUTPUT}.tmp"
    "// Generated from ${source} by cmake/embed.cmake; do not edit.\n"
    "#include <cstddef>\n\n"
    "namespace ${NAMESPACE} {\n\n"
    "extern const unsigned char ${NAME}[];\n"
    "extern const std::size_t ${NAME}_size;\n\n"
    "const unsigned char ${NAME}[] = {\n${hex}\n};\n"
    "const std::size_t ${NAME}_size = ${bytes};\n\n"
    "} // namespace ${NAMESPACE}\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
