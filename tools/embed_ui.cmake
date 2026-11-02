# Convert an HTML file into a C header holding the bytes as a static array.
# Keeps the launcher a single exe: gage_gui.cpp includes ui_html.h and serves
# the bytes from memory (WebResourceRequested on https://app.gage/).
# Usage: cmake -DINPUT=<html> -DOUTPUT=<header> -P tools/embed_ui.cmake
if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
  message(FATAL_ERROR "embed_ui.cmake requires -DINPUT and -DOUTPUT")
endif()
if(NOT EXISTS ${INPUT})
  message(FATAL_ERROR "embed_ui.cmake: input not found: ${INPUT}")
endif()

file(READ ${INPUT} DATA HEX)
string(LENGTH "${DATA}" hexlen)
math(EXPR nbyte "${hexlen} / 2")
if(nbyte EQUAL 0)
  message(FATAL_ERROR "embed_ui.cmake: input is empty: ${INPUT}")
endif()

# Whole-string regex passes, not a per-byte loop: indexing the hex string byte
# by byte is quadratic and never finishes on a ~1 MB bundle.
# CMake regex has no {n} quantifier, so the 16-bytes-per-line group is spelled out.
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " DATA "${DATA}")
string(REPEAT "0x[0-9a-f][0-9a-f], " 16 row)
string(REGEX REPLACE "(${row})" "\\1\n" DATA "${DATA}")
if(NOT DATA MATCHES "\n$")
  string(APPEND DATA "\n")
endif()

file(WRITE ${OUTPUT}
"#ifndef GAGE_UI_HTML_H\n#define GAGE_UI_HTML_H\n\n// Generated from GAGE_UI_HTML_IN (CMakeLists.txt) by tools/embed_ui.cmake — do not edit.\nstatic const unsigned char kUiHtml[] = {\n${DATA}};\nstatic const unsigned long kUiHtmlLen = ${nbyte};\n#endif\n")

message(STATUS "Embedded ${nbyte} bytes of UI into ${OUTPUT}")
