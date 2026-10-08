# Converts a binary file into a C++ header with a byte array.
#   cmake -DIN=file.png -DOUT=file.h -DVAR=kName -P embed.cmake
file(READ "${IN}" hex HEX)
# 32 bytes per line, then "0xNN," per byte.
string(REGEX REPLACE "(................................................................)" "\\1\n" hex "${hex}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
get_filename_component(name "${IN}" NAME)
file(WRITE "${OUT}" "// Generated from ${name} by embed.cmake - do not edit.\n#pragma once\nstatic const unsigned char ${VAR}[] = {\n${hex}};\n")
