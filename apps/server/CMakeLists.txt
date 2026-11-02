# Standalone build of the GAGE game server and its tests, with no Windows client in
# sight: this is the entry point for a Linux VPS (and works the same on Windows).
#
#   cmake -S apps/server -B build/server -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/server --parallel
#   build/server/server/gage-server --config server.json
#   build/server/selftest/netcode_test ; build/server/selftest/server_test
#
# It defines exactly what the main build's GAGE_SERVER_ONLY option does
# (cmake -S . -B build/server -DGAGE_SERVER_ONLY=ON): enet, gage_protocol, gage_netclient
# (the tests need the client session), gage_server_core, gage-server, netcode_test and
# server_test, all from ../../cmake/gage_server.cmake.
cmake_minimum_required(VERSION 3.16)
project(GAGE_SERVER C CXX) # C only for the vendored ENet

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-O2 -fno-exceptions)
if(WIN32)
    # Embed the mingw C++ runtime so the exe runs on a clean Windows box. Never on
    # Linux: a fully static glibc binary cannot resolve names (getaddrinfo) reliably.
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

set(GAGE_SERVER_ONLY ON)
get_filename_component(GAGE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include(${GAGE_ROOT}/cmake/gage_server.cmake)
