# Cross-compile the Solar RP Windows client (SolarLauncher.exe, gageinjector.dll, gage_loader.dll, gage_mp.dll,
# gage_launch_shim.dll and the Windows selftests) on Ubuntu 24.04 with the distro's mingw-w64.
# Used by tools/deploy/build-client.sh on the VPS:
#
#   cmake -S . -B build/win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-linux.cmake \
#         -DCMAKE_BUILD_TYPE=Release -DGAGE_UPDATE_URL=... -DGAGE_UPDATE_PUBKEY=...
#
# Packages (tools/deploy/bootstrap-vps.sh installs them):
#   gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64 mingw-w64-x86-64-dev
#
# Why the -posix compilers by name: on Ubuntu/Debian the unsuffixed x86_64-w64-mingw32-g++ is the *win32*
# thread-model variant by default (update-alternatives), whose libstdc++ has no std::thread/std::mutex,
# and apps/server/announce.cpp, tests/gage_mp_test.cpp and tests/join_test.cpp use them. The WinLibs
# toolchain the project is built with today is also posix threads (x86_64-msvcrt-posix-seh), so posix
# is the matching model. Calling the -posix names directly also leaves the system alternatives alone.
#
# Toolchain ABI this file insists on (it is verified below, and again on the built binaries by
# build-client.sh through tools/deploy/lib/pe-imports.mjs): x86_64, SEH exceptions, posix threads and the
# msvcrt.dll C runtime, exactly what the WinLibs build links against. The root CMakeLists.txt statically
# links libstdc++, libgcc and winpthread (-static -static-libgcc -static-libstdc++), so no runtime DLL
# other than the system's msvcrt.dll is needed next to GTA.
#
# Environment overrides: GAGE_MINGW_TRIPLET (default x86_64-w64-mingw32), GAGE_MINGW_THREADS (posix),
# GAGE_MINGW_SUFFIX (default -<threads>; "none" for no suffix), GAGE_ALLOW_NON_MSVCRT=1 (accept a compiler whose default C runtime is not msvcrt; the resulting
# binaries then import ucrtbase/api-ms-win-crt-* and build-client.sh will refuse them).

if(CMAKE_VERSION VERSION_LESS 3.18)
    message(FATAL_ERROR "toolchain-mingw-linux.cmake needs CMake 3.18 or newer (found ${CMAKE_VERSION})")
endif()

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_gage_triplet x86_64-w64-mingw32)
if(DEFINED ENV{GAGE_MINGW_TRIPLET} AND NOT "$ENV{GAGE_MINGW_TRIPLET}" STREQUAL "")
    set(_gage_triplet "$ENV{GAGE_MINGW_TRIPLET}")
endif()
set(_gage_threads posix)
if(DEFINED ENV{GAGE_MINGW_THREADS} AND NOT "$ENV{GAGE_MINGW_THREADS}" STREQUAL "")
    set(_gage_threads "$ENV{GAGE_MINGW_THREADS}")
endif()

# Compiler name suffix: "-posix" by default; GAGE_MINGW_SUFFIX=none uses the unsuffixed names (a toolchain
# whose default already is the posix model, e.g. WinLibs; used to exercise this file on a Windows host).
set(_gage_suffix -${_gage_threads})
if(DEFINED ENV{GAGE_MINGW_SUFFIX} AND NOT "$ENV{GAGE_MINGW_SUFFIX}" STREQUAL "")
    if("$ENV{GAGE_MINGW_SUFFIX}" STREQUAL "none")
        set(_gage_suffix "")
    else()
        set(_gage_suffix "$ENV{GAGE_MINGW_SUFFIX}")
    endif()
endif()

set(CMAKE_C_COMPILER   ${_gage_triplet}-gcc${_gage_suffix})
set(CMAKE_CXX_COMPILER ${_gage_triplet}-g++${_gage_suffix})
# Resource compiler for apps/launcher/gage_gui.rc (the icon and the requireAdministrator manifest).
set(CMAKE_RC_COMPILER  ${_gage_triplet}-windres)

# CMake derives the binutils prefix from the compiler name, and a "-posix" suffix defeats that, which
# would pick the host's ar/ranlib (they cannot index PE objects reliably). Name the cross tools.
foreach(_tool AR RANLIB)
    string(TOLOWER ${_tool} _tool_lower)
    find_program(CMAKE_${_tool} NAMES ${_gage_triplet}-${_tool_lower} NO_CMAKE_FIND_ROOT_PATH)
    if(NOT CMAKE_${_tool})
        message(FATAL_ERROR "toolchain-mingw-linux.cmake: ${_gage_triplet}-${_tool_lower} not found (apt install binutils-mingw-w64-x86-64)")
    endif()
endforeach()

# Headers and import libraries come from the mingw sysroot, programs (cmake, node, windres) from the host.
set(CMAKE_FIND_ROOT_PATH /usr/${_gage_triplet})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ---- ABI guards -----------------------------------------------------------------------------
# Run the compiler once to read its configuration. Failing here beats a link error 40 minutes in or,
# worse, a build that works but needs libwinpthread-1.dll next to GTA.
execute_process(COMMAND ${CMAKE_CXX_COMPILER} -v
                OUTPUT_QUIET ERROR_VARIABLE _gage_gxx_v RESULT_VARIABLE _gage_gxx_rc)
if(NOT _gage_gxx_rc EQUAL 0)
    message(FATAL_ERROR "toolchain-mingw-linux.cmake: cannot run ${CMAKE_CXX_COMPILER} "
                        "(apt install g++-mingw-w64-x86-64-posix)")
endif()
if(NOT _gage_gxx_v MATCHES "Thread model: ${_gage_threads}")
    message(FATAL_ERROR "toolchain-mingw-linux.cmake: ${CMAKE_CXX_COMPILER} is not a '${_gage_threads}' thread-model compiler:\n${_gage_gxx_v}")
endif()
if(NOT _gage_gxx_v MATCHES "Target: ${_gage_triplet}")
    message(FATAL_ERROR "toolchain-mingw-linux.cmake: ${CMAKE_CXX_COMPILER} does not target ${_gage_triplet}:\n${_gage_gxx_v}")
endif()
string(REGEX MATCH "gcc version [^\n]*" _gage_gcc_version "${_gage_gxx_v}")

execute_process(COMMAND ${CMAKE_CXX_COMPILER} -dumpspecs
                OUTPUT_VARIABLE _gage_specs ERROR_QUIET RESULT_VARIABLE _gage_specs_rc)
set(_gage_crt unknown)
if(_gage_specs_rc EQUAL 0)
    if(_gage_specs MATCHES "[ \t]-lmsvcrt[ \t\n]")
        set(_gage_crt msvcrt)
    elseif(_gage_specs MATCHES "[ \t]-lucrt[ \t\n]")
        set(_gage_crt ucrt)
    endif()
endif()
if(NOT _gage_crt STREQUAL "msvcrt" AND NOT "$ENV{GAGE_ALLOW_NON_MSVCRT}" STREQUAL "1")
    message(FATAL_ERROR "toolchain-mingw-linux.cmake: the default C runtime of ${CMAKE_CXX_COMPILER} is '${_gage_crt}', "
                        "the WinLibs build links msvcrt. Set GAGE_ALLOW_NON_MSVCRT=1 to build anyway "
                        "(the binaries will import ucrtbase/api-ms-win-crt-* and build-client.sh will reject them).")
endif()
message(STATUS "GAGE mingw toolchain: ${_gage_gcc_version}; threads ${_gage_threads}; C runtime ${_gage_crt}; "
               "triplet ${_gage_triplet}")
