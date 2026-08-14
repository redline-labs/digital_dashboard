# Turn a binary file into a C++ header holding its bytes.
#
#   cmake -DIN=<file> -DOUT=<header> -DSYMBOL=<name> -P EmbedBinary.cmake
#
# Used for the baked shaders in dashboard/widgets/map. The obvious alternative
# is Qt's resource system, but a .qrc inside a STATIC library needs its
# initialiser to survive the link into two different executables, and a resource
# that quietly fails to register presents as a shader that will not load at
# runtime rather than as a build error. A header is linked like any other code
# and cannot be dropped.
if(NOT IN OR NOT OUT OR NOT SYMBOL)
    message(FATAL_ERROR "EmbedBinary.cmake needs IN, OUT and SYMBOL")
endif()

file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" hexLength)
math(EXPR byteCount "${hexLength} / 2")
if(byteCount EQUAL 0)
    message(FATAL_ERROR "EmbedBinary: ${IN} is empty")
endif()

string(REGEX MATCHALL "[0-9a-f][0-9a-f]" bytes "${hex}")
list(TRANSFORM bytes PREPEND "0x")
list(JOIN bytes "," body)

file(WRITE "${OUT}"
"// Generated from ${IN} by cmake/EmbedBinary.cmake. Do not edit.
#pragma once
#include <cstddef>

namespace map_shaders
{
inline constexpr unsigned char ${SYMBOL}[] = { ${body} };
inline constexpr std::size_t ${SYMBOL}Size = ${byteCount};
}
")
