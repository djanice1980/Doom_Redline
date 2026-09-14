# Converts a binary file into a C++ header with a uint32_t array (SPIR-V words).
# Usage: cmake -DINPUT=file.spv -DOUTPUT=file.h -DSYMBOL=name -P embed.cmake
file(READ ${INPUT} HEX_CONTENT HEX)
string(LENGTH "${HEX_CONTENT}" HEX_LEN)
math(EXPR WORD_COUNT "${HEX_LEN} / 8")
# Regroup 8 hex chars (one little-endian 32-bit word) into 0xXXXXXXXX literals.
string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1," WORDS "${HEX_CONTENT}")
file(WRITE ${OUTPUT}
"// Generated from ${INPUT} - do not edit.
#pragma once
#include <cstdint>
#include <cstddef>
namespace rl::shaders {
inline constexpr uint32_t ${SYMBOL}[] = { ${WORDS} };
inline constexpr size_t ${SYMBOL}_size = sizeof(${SYMBOL});
}
")
