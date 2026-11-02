#include "metrics.h"

#include <cstdio>
#include <utility>
#include <vector>

namespace gage::server {

    namespace {

        using KV = std::vector<std::pair<std::string, double>>;

        // One definition of the report: `stats` prints it as lines, `stats json` as an object.
        KV Collect(const Metrics& n, const Metrics& s, double secs, uint32_t tickHz, size_t players)
        {
            if (secs <= 0.0) secs = 1e-9;
            auto d = [](uint64_t a, uint64_t b) { return (double)(a - b); };
            KV kv;
            auto put = [&](const char* k, double v) { kv.emplace_back(k, v); };
            auto hist = [&](const char* base, const LogHistogram& a, const LogHistogram& b, bool withMax) {
                put((std::string(base) + "_avg").c_str(), a.Mean(&b));
                put((std::string(base) + "_p50").c_str(), a.Quantile(0.50, &b));
                put((std::string(base) + "_p99").c_str(), a.Quantile(0.99, &b));
                if (withMax) put((std::string(base) + "_max").c_str(), (double)a.max());
            };

            put("window_s", secs);
            put("players", (double)players);
            put("tick_hz", (double)tickHz);
            put("ticks", d(n.ticks, s.ticks));
            put("ticks_per_s", d(n.ticks, s.ticks) / secs);
            put("ticks_missed", d(n.ticksMissed, s.ticksMissed));
            hist("tick_work_us", n.tickWorkUs, s.tickWorkUs, true);
            hist("tick_late_us", n.tickLateUs, s.tickLateUs, true);
            put("tick_late_us_p95", n.tickLateUs.Quantile(0.95, &s.tickLateUs));
            const double cpu = n.cpuSeconds - s.cpuSeconds;
            const double wall = n.wallSeconds - s.wallSeconds;
            put("cpu_percent", wall > 0 ? 100.0 * cpu / wall : 0.0);

            put("out_payload_bytes_per_s", d(n.payloadBytesOut, s.payloadBytesOut) / secs);
            put("out_wire_bytes_per_s", d(n.wireBytesOut, s.wireBytesOut) / secs);
            put("out_packets_per_s", d(n.packetsOut, s.packetsOut) / secs);
            put("out_datagrams_per_s", d(n.wireDatagramsOut, s.wireDatagramsOut) / secs);
            put("in_payload_bytes_per_s", d(n.payloadBytesIn, s.payloadBytesIn) / secs);
            put("in_packets_per_s", d(n.packetsIn, s.packetsIn) / secs);
            put("in_wire_bytes_per_s", d(n.wireBytesIn, s.wireBytesIn) / secs);
            put("in_datagrams_per_s", d(n.wireDatagramsIn, s.wireDatagramsIn) / secs);
            hist("client_out_bytes_per_s", n.clientBytesPerSec, s.clientBytesPerSec, true);

            put("world_packets_per_s", d(n.worldPackets, s.worldPackets) / secs);
            put("world_entries_per_s", d(n.worldEntries, s.worldEntries) / secs);
            hist("world_bytes", n.worldBytes, s.worldBytes, true);
            hist("world_entries", n.worldEntryCount, s.worldEntryCount, true);
            put("coarse_packets_per_s", d(n.coarsePackets, s.coarsePackets) / secs);
            put("coarse_entries_per_s", d(n.coarseEntries, s.coarseEntries) / secs);
            put("delta_entries_per_s", d(n.deltaEntries, s.deltaEntries) / secs);
            put("absolute_entries_per_s", d(n.absoluteEntries, s.absoluteEntries) / secs);
            put("budget_deferred_per_s", d(n.budgetDeferred, s.budgetDeferred) / secs);
            put("relevance_enters_per_s", d(n.relevanceEnters, s.relevanceEnters) / secs);
            put("relevance_leaves_per_s", d(n.relevanceLeaves, s.relevanceLeaves) / secs);
            put("relevant_pairs", (double)n.relevantPairs);

            // Sync diagnostics: per pair-tick outcome, idle wake-ups, sample accounting.
            put("pairs_sent_per_s", d(n.pairSent, s.pairSent) / secs);
            put("pairs_idle_suppressed_per_s", d(n.pairIdleSuppressed, s.pairIdleSuppressed) / secs);
            put("pairs_rate_not_due_per_s", d(n.pairRateNotDue, s.pairRateNotDue) / secs);
            put("pairs_budget_deferred_per_s", d(n.pairBudgetDeferred, s.pairBudgetDeferred) / secs);
            put("pairs_packet_cap_deferred_per_s", d(n.pairPacketCapDeferred, s.pairPacketCapDeferred) / secs);
            put("pairs_no_state_per_s", d(n.pairNoState, s.pairNoState) / secs);
            put("wakes_per_s", d(n.wakes, s.wakes) / secs);
            put("noise_wakes_held_per_s", d(n.noiseWakesHeld, s.noiseWakesHeld) / secs);
            put("samples_received_per_s", d(n.samplesReceived, s.samplesReceived) / secs);
            put("samples_forwarded_per_s", d(n.samplesForwarded, s.samplesForwarded) / secs);
            put("samples_forwarded_twice_per_s", d(n.samplesForwardedTwice, s.samplesForwardedTwice) / secs);
            const double recvd = d(n.samplesReceived, s.samplesReceived), fwd = d(n.samplesForwarded, s.samplesForwarded);
            put("samples_never_forwarded_per_s", (recvd > fwd ? recvd - fwd : 0.0) / secs);
            return kv;
        }

        std::string Num(double v)
        {
            char buf[48];
            if (v == (double)(long long)v && v > -1e15 && v < 1e15)
                std::snprintf(buf, sizeof(buf), "%lld", (long long)v);
            else
                std::snprintf(buf, sizeof(buf), "%.2f", v);
            return buf;
        }

    }

    std::string FormatMetrics(const Metrics& now, const Metrics& since, double windowSeconds, uint32_t tickHz,
                              size_t players)
    {
        const KV kv = Collect(now, since, windowSeconds, tickHz, players);
        std::string out = "stats";
        for (const auto& p : kv)
            out += "\n  " + p.first + " " + Num(p.second);
        return out;
    }

    std::string FormatMetricsJson(const Metrics& now, const Metrics& since, double windowSeconds, uint32_t tickHz,
                                  size_t players)
    {
        const KV kv = Collect(now, since, windowSeconds, tickHz, players);
        std::string out = "{";
        for (size_t i = 0; i < kv.size(); i++)
        {
            if (i) out += ",";
            out += "\"" + kv[i].first + "\":" + Num(kv[i].second);
        }
        out += "}";
        return out;
    }

}
