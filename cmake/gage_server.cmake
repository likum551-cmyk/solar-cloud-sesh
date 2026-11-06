# GAGE server and netcode targets: ENet, the protocol library, the client session
# library (the server tests need it), the server core, the gage-server executable
# and the server tests. Nothing here needs a Windows-only library, so this file is
# shared by
#   * the main build (CMakeLists.txt includes it), and
#   * the standalone server build (apps/server/CMakeLists.txt, the Linux VPS entry point;
#     the GAGE_SERVER_ONLY option of the main build ends up here too).
# The includer sets GAGE_ROOT (the repository root) first; paths below are absolute
# because CMAKE_CURRENT_SOURCE_DIR differs between the two entry points.
#
# None of it is shipped by the installer: gage-server builds into <build>/server,
# the tests into <build>/selftest.
if(NOT GAGE_ROOT)
    message(FATAL_ERROR "gage_server.cmake: set GAGE_ROOT to the repository root before including it")
endif()

set(ENET_DIR ${GAGE_ROOT}/third_party/enet)
add_library(enet STATIC
    ${ENET_DIR}/callbacks.c
    ${ENET_DIR}/compress.c
    ${ENET_DIR}/host.c
    ${ENET_DIR}/list.c
    ${ENET_DIR}/packet.c
    ${ENET_DIR}/peer.c
    ${ENET_DIR}/protocol.c
)
target_include_directories(enet PUBLIC ${ENET_DIR}/include)
if(WIN32)
    target_sources(enet PRIVATE ${ENET_DIR}/win32.c)
    target_link_libraries(enet PUBLIC ws2_32 winmm)
else()
    # What ENet's own CMake probes for; every one of these exists on Linux/glibc.
    target_sources(enet PRIVATE ${ENET_DIR}/unix.c)
    target_compile_definitions(enet PRIVATE
        HAS_FCNTL=1 HAS_POLL=1 HAS_GETNAMEINFO=1 HAS_GETADDRINFO=1 HAS_GETHOSTBYNAME_R=1
        HAS_GETHOSTBYADDR_R=1 HAS_INET_PTON=1 HAS_INET_NTOP=1 HAS_MSGHDR_FLAGS=1 HAS_SOCKLEN_T=1)
endif()

find_package(Threads REQUIRED)

# Wire format, validators and the UDP query codec: portable, no platform headers
# (client engine, server, launcher, tools).
add_library(gage_protocol STATIC
    ${GAGE_ROOT}/libs/protocol/protocol.cpp
    ${GAGE_ROOT}/libs/protocol/worldcodec.cpp
    ${GAGE_ROOT}/libs/protocol/query.cpp
)
# libs/ is the include root of every library: "protocol/protocol.h", "netclient/session.h",
# "netcommon/netcommon.h", "common/json.h" (header-only), "sync/smoother.h", ...
target_include_directories(gage_protocol PUBLIC ${GAGE_ROOT}/libs)

# ClientSession: no global state, no stdout; it is what the in-game engine links later.
add_library(gage_netclient STATIC ${GAGE_ROOT}/libs/netclient/session.cpp)
target_link_libraries(gage_netclient PUBLIC gage_protocol PRIVATE enet)

# The server core (class gage::server::Server) without main(), so tests run it in-process.
# sock.cpp is the tiny TCP shim (Winsock / POSIX) behind the master announce client and
# the loopback admin channel; the announcer runs one std::thread.
add_library(gage_server_core STATIC
    ${GAGE_ROOT}/apps/server/server.cpp
    ${GAGE_ROOT}/apps/server/metrics.cpp
    ${GAGE_ROOT}/apps/server/replication.cpp
    ${GAGE_ROOT}/apps/server/config.cpp
    ${GAGE_ROOT}/apps/server/util.cpp
    ${GAGE_ROOT}/apps/server/banlist.cpp
    ${GAGE_ROOT}/apps/server/inventory_store.cpp
    ${GAGE_ROOT}/apps/server/sock.cpp
    ${GAGE_ROOT}/apps/server/httpclient.cpp
    ${GAGE_ROOT}/apps/server/announce.cpp
    ${GAGE_ROOT}/apps/server/admin.cpp
)
target_include_directories(gage_server_core PUBLIC ${GAGE_ROOT}/apps/server)
target_link_libraries(gage_server_core PUBLIC gage_protocol Threads::Threads PRIVATE enet)
if(WIN32)
    target_link_libraries(gage_server_core PRIVATE ws2_32)
endif()

add_executable(gage-server ${GAGE_ROOT}/apps/server/main.cpp)
target_link_libraries(gage-server PRIVATE gage_server_core Threads::Threads)
set_target_properties(gage-server PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/server)

# netcode_test: the codec, the config loader and the game traffic over real ENet on
# 127.0.0.1 against an in-process server.
add_executable(netcode_test ${GAGE_ROOT}/tests/netcode_test.cpp)
target_link_libraries(netcode_test PRIVATE gage_server_core gage_netclient)
set_target_properties(netcode_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# quant_test: protocol v2 quantisation error bounds (exhaustive where it can be), the range check,
# the delta codec over a lossy link, acknowledgements and hostile input. Needs only the protocol library.
add_executable(quant_test ${GAGE_ROOT}/tests/quant_test.cpp)
target_link_libraries(quant_test PRIVATE gage_protocol)
set_target_properties(quant_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# decode_fuzz_test: every protocol Decode and the UDP query datagrams against truncation, trailing bytes, every
# single-bit flip and random / mutated packets (deterministic): refused or canonical, never a crash, refusals leave
# `out` alone. Needs only the protocol library.
add_executable(decode_fuzz_test ${GAGE_ROOT}/tests/decode_fuzz_test.cpp)
target_link_libraries(decode_fuzz_test PRIVATE gage_protocol)
set_target_properties(decode_fuzz_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# interest_test: the Replicator driven directly (no sockets): relevance radii and hysteresis, the
# always-relevant override, send rates, the bandwidth budget, the coarse channel, a 600-player crowd.
add_executable(interest_test ${GAGE_ROOT}/tests/interest_test.cpp)
target_link_libraries(interest_test PRIVATE gage_server_core)
set_target_properties(interest_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# server_test: the UDP query, master announce (against an in-process fake HTTP master),
# chat flood limit, per-address cap, bans and the loopback admin channel.
add_executable(server_test ${GAGE_ROOT}/tests/server_test.cpp)
target_link_libraries(server_test PRIVATE gage_server_core gage_netclient enet)
set_target_properties(server_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# t3_vehvisual_test (Desync Overhaul round 2, T3): vehicle visual state for the other clients (burst tyres, smashed windows,
# brake lights, steering): the v6 wire bytes, the VehicleStore merge, libs/sync/vehicle_visual.h against a fake car, and the
# whole chain through the real in-process server over loopback ENet. vehicle_drive.cpp is compiled in (the steering helpers
# and quaternion math live there); nothing here needs a Windows-only library.
add_executable(t3_vehvisual_test ${GAGE_ROOT}/tests/t3_vehvisual_test.cpp ${GAGE_ROOT}/libs/sync/vehicle_drive.cpp)
target_link_libraries(t3_vehvisual_test PRIVATE gage_server_core gage_netclient enet)
set_target_properties(t3_vehvisual_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# gage-loadtest: N headless ClientSessions against an embedded (or external) server,
# reporting tick time, CPU, bytes and packets (apps/loadtest/main.cpp, docs/perf/).
add_executable(gage-loadtest ${GAGE_ROOT}/apps/loadtest/main.cpp)
target_link_libraries(gage-loadtest PRIVATE gage_server_core gage_netclient Threads::Threads)
set_target_properties(gage-loadtest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)
