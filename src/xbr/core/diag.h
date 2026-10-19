#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>

// diagnostics switched on by environment variables, read once
namespace diag {

// XBE_PERF set: perf log lines once a second and the read-backs named
inline bool Perf() {
    static const bool on = std::getenv("XBE_PERF") != nullptr;
    return on;
}

// FCI: XBE_PERF's latency figures, steady_clock ns: per Swap, the title thread's frame from the end of the pacing
// wait (where the game reads input next) to the Swap, and when the Swap was called. A ring by Swap number: the render
// thread presents a frame or two later and reads its own frame's entry by its present count
inline int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
constexpr uint32_t kLatencyRing = 16;
inline std::atomic<int64_t> g_frame_start_ns{0}, g_frame_ns[kLatencyRing], g_swap_ns[kLatencyRing];
inline std::atomic<uint64_t> g_swaps{0};
// one entry per present queued, in queue order: a Swap's frame (from the end of the pacing wait), or -1 for a present
// that is not the title's frame (a movie shown with no swaps)
inline void NotePresent(int64_t frame_ns) {
    uint64_t i = g_swaps.load() % kLatencyRing;
    g_frame_ns[i] = frame_ns;
    g_swap_ns[i] = NowNs();
    g_swaps++;
}

// XBE_CAPTURE_DIR: where F3 saves a frame, empty: F3 does not capture
inline const std::filesystem::path& CaptureDir() {
    static const std::filesystem::path dir = [] {
#ifdef _WIN32
        const wchar_t* v = _wgetenv(L"XBE_CAPTURE_DIR");
#else
        const char* v = std::getenv("XBE_CAPTURE_DIR");
#endif
        return v ? std::filesystem::path(v) : std::filesystem::path();
    }();
    return dir;
}

}  // namespace diag
