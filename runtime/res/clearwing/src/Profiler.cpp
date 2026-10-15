// Sampling profiler for one thread, for platforms without a native profiler (e.g. the Switch).
// A timer thread requests a sample, the profiled thread records its own stack at its next safepoint, which is
// safe without stopping it (frame infos live on its stack). Samples land on method calls only.
// Every reportSeconds the methods most often on the stack in that window are appended to the report file.

#include "Clearwing.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef __SWITCH__
#include <unistd.h>
#endif

volatile bool profileRequested;

namespace {
    struct Counts {
        int self;  // Top of the stack
        int total; // Anywhere on the stack
    };

    std::mutex mutex;
    std::unordered_map<const char *, Counts> counts;
    std::map<std::pair<const char *, const char *>, int> calls; // Caller and callee at the top of the stack
    int samples;
    int requests; // Fewer samples than requests: time in native code or blocked
    int collections;
    double collectionSeconds;
    int64_t lastHeapUsage;
    jcontext profiled;
    std::thread *timer;
    volatile bool running;

    void report(FILE *file, double start, double end) {
        std::vector<std::pair<const char *, Counts>> sorted;
        std::vector<std::pair<std::pair<const char *, const char *>, int>> topCalls;
        int total, requested, gcs;
        double gcSeconds;
        int64_t heap;
        {
            std::lock_guard lock(mutex);
            sorted.assign(counts.begin(), counts.end());
            topCalls.assign(calls.begin(), calls.end());
            calls.clear();
            total = samples;
            requested = requests;
            counts.clear();
            samples = 0;
            requests = 0;
            gcs = collections;
            gcSeconds = collectionSeconds;
            heap = lastHeapUsage;
            collections = 0;
            collectionSeconds = 0;
        }
        fprintf(file, "=== %.1f s to %.1f s: %d samples of %d requested, %d GCs took %.2f s, heap %lld MB\n"
                "  total%%  self%%  method\n", start, end, total, requested, gcs, gcSeconds, (long long) (heap >> 20));
        if (total == 0)
            return;
        std::sort(sorted.begin(), sorted.end(), [](auto &a, auto &b) { return a.second.total > b.second.total; });
        for (size_t i = 0; i < sorted.size() && i < 30; i++)
            fprintf(file, "  %5.1f  %5.1f  %s\n", 100.0 * sorted[i].second.total / total,
                    100.0 * sorted[i].second.self / total, sorted[i].first);
        std::sort(sorted.begin(), sorted.end(), [](auto &a, auto &b) { return a.second.self > b.second.self; });
        fprintf(file, "  top self:\n");
        for (size_t i = 0; i < sorted.size() && i < 10 && sorted[i].second.self > 0; i++)
            fprintf(file, "  %5.1f  %5.1f  %s\n", 100.0 * sorted[i].second.total / total,
                    100.0 * sorted[i].second.self / total, sorted[i].first);
        std::sort(topCalls.begin(), topCalls.end(), [](auto &a, auto &b) { return a.second > b.second; });
        fprintf(file, "  top calls (caller > method on top of the stack):\n");
        for (size_t i = 0; i < topCalls.size() && i < 10; i++)
            fprintf(file, "  %5.1f         %s > %s\n", 100.0 * topCalls[i].second / total, topCalls[i].first.first,
                    topCalls[i].first.second);
        fflush(file);
#ifdef __SWITCH__
        fsync(fileno(file)); // fflush only hands the data to the fs service
#endif
    }

    void timerLoop(FILE *file, int intervalMicros, int reportSeconds) {
        auto start = std::chrono::steady_clock::now();
        auto seconds = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
        double lastReport = 0;
        while (running) {
            std::this_thread::sleep_for(std::chrono::microseconds(intervalMicros));
            {
                std::lock_guard lock(mutex);
                requests++;
            }
            profileRequested = true;
            double now = seconds();
            if (now - lastReport >= reportSeconds) {
                report(file, lastReport, now);
                lastReport = now;
            }
        }
        report(file, lastReport, seconds());
        fclose(file);
    }
}

void profileSample(jcontext ctx) {
    if (ctx != profiled)
        return;
    profileRequested = false;
    const char *seen[64];
    int seenCount = 0;
    std::lock_guard lock(mutex);
    samples++;
    for (int i = ctx->stackDepth - 1; i >= 0 && seenCount < 64; i--) {
        auto info = ctx->frames[i].info;
        if (!info || !info->method)
            continue;
        auto method = info->method;
        if (std::find(seen, seen + seenCount, method) != seen + seenCount)
            continue; // Recursion, count once
        seen[seenCount++] = method;
        auto &entry = counts[method];
        entry.total++;
        if (seenCount == 1)
            entry.self++;
        if (seenCount == 2)
            calls[{method, seen[0]}]++;
    }
}

void profileGC(double seconds, int64_t heapUsage) {
    if (!timer)
        return;
    std::lock_guard lock(mutex);
    collections++;
    collectionSeconds += seconds;
    lastHeapUsage = heapUsage;
}

void startProfiler(jcontext ctx, const char *path, int intervalMicros, int reportSeconds) {
    if (timer)
        return;
    FILE *file = fopen(path, "w");
    if (!file)
        return;
    profiled = ctx;
    running = true;
    timer = new std::thread(timerLoop, file, std::max(intervalMicros, 100), std::max(reportSeconds, 1));
}

void stopProfiler() {
    if (!timer)
        return;
    running = false;
    timer->join();
    delete timer;
    timer = nullptr;
    profiled = nullptr;
}
