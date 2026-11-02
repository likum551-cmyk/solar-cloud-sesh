// gage-server: the GAGE multiplayer server executable. Loads server.json, runs
// gage::server::Server and reads admin commands from stdin.
//
//   gage-server [--config <path>] [--port <n>]
//
// Without --config it reads ./server.json when that exists and otherwise runs on
// the built-in defaults; a config that exists but is invalid is a fatal error.
// Console: list | kick <id|name> | say <text> | ban <id|name> [reason] | banip <ip>
// [reason] | unban <name|ip> | bans | help | quit (docs/api/server.md has the grammar).
// The same commands are available over the optional loopback admin channel
// (server.json adminPort/adminToken; the token may come from GAGE_ADMIN_TOKEN). A
// `quit` from either place stops the server. SIGINT/SIGTERM stop it cleanly too.
// Exit codes: 0 clean stop, 1 config or bind failure, 2 usage.
#include "server.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

    volatile std::sig_atomic_t g_stop = 0;

    void OnSignal(int)
    {
        g_stop = 1;
    }

    // stdin is read on its own thread (getline blocks); the main loop drains it.
    struct ConsoleQueue
    {
        std::mutex m;
        std::deque<std::string> lines;
    };

    int Usage(const char* why)
    {
        if (why) std::fprintf(stderr, "gage-server: %s\n", why);
        std::fprintf(stderr, "usage: gage-server [--config <path>] [--port <1..65535>]\n");
        return 2;
    }

}

int main(int argc, char** argv)
{
    std::string configPath;
    bool configGiven = false;
    long portOverride = 0;

    for (int i = 1; i < argc; i++)
    {
        if (!std::strcmp(argv[i], "--config") && i + 1 < argc)
        {
            configPath = argv[++i];
            configGiven = true;
        }
        else if (!std::strcmp(argv[i], "--port") && i + 1 < argc)
        {
            char* end = nullptr;
            portOverride = std::strtol(argv[++i], &end, 10);
            if (*end != '\0' || portOverride < 1 || portOverride > 65535)
                return Usage("--port must be a whole number 1..65535");
        }
        else if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h"))
        {
            Usage(nullptr);
            return 0;
        }
        else
        {
            return Usage((std::string("unknown or incomplete argument: ") + argv[i]).c_str());
        }
    }
    if (!configGiven) configPath = "server.json";

    gage::server::Config cfg;
    std::string err;
    bool missing = false;
    if (!gage::server::LoadConfigFile(configPath, cfg, &err, &missing))
    {
        if (!missing || configGiven)
        {
            std::fprintf(stderr, "gage-server: %s\n", err.c_str());
            return 1;
        }
        std::printf("no %s, using the built-in defaults\n", configPath.c_str());
    }
    if (portOverride) cfg.port = (uint16_t)portOverride;

    gage::server::Server server;
    if (!server.Start(cfg, &err))
    {
        std::fprintf(stderr, "gage-server: %s\n", err.c_str());
        return 1;
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
#ifdef SIGPIPE
    // A peer that vanishes mid-write on the announce or admin TCP socket must be an
    // error return, never a process-killing signal (sock.cpp also passes MSG_NOSIGNAL).
    std::signal(SIGPIPE, SIG_IGN);
#endif

    auto console = std::make_shared<ConsoleQueue>();
    std::thread([console] {
        std::string line;
        while (std::getline(std::cin, line))
        {
            std::lock_guard<std::mutex> lock(console->m);
            console->lines.push_back(line);
        }
        // EOF (stdin closed, e.g. run as a service): keep serving, no more commands.
    }).detach();

    std::printf("type help for commands\n");
    while (!g_stop && !server.quitRequested())
    {
        server.Poll(10);

        std::deque<std::string> pending;
        {
            std::lock_guard<std::mutex> lock(console->m);
            pending.swap(console->lines);
        }
        for (const std::string& line : pending)
        {
            std::string out;
            const gage::server::CommandResult r = server.Execute(line, out);
            if (!out.empty()) std::printf("%s\n", out.c_str());
            std::fflush(stdout);
            if (r == gage::server::CommandResult::Quit) g_stop = 1;
        }
    }
    server.Stop();
    return 0;
}
