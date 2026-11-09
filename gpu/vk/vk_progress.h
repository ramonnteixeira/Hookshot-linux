// (hookshot) The start-up screen's numbers (vk_main.cpp, vk_menu.cpp): how far the shaders' preparation is, and about how long
// it has left. Plain C++, no Vulkan: tests/test_startup_progress.py compiles it on its own.
#pragma once

#include <cstddef>
#include <cstdio>
#include <algorithm>
#include <string>

namespace mkwvk {

/// The time left, from the rate over the last few seconds, not from the start: a start after one that was closed part-way skips
/// what is on disk already in a second or two, and that burst would have promised the rest at the same speed. Nothing is said
/// before a sixth is done (the programs differ a lot: on an RTX 3090 the first 1,500 of 32,000 took half a minute, the next 10,000
/// ten seconds, and the early estimates said 83 minutes for a 70-second job), and what is said is smoothed over about 8 s
/// (it went 1 min, 5 min, 1 min from one 10-second line to the next)
struct ProgressClock {
    static constexpr int kSamples = 64;
    static constexpr double kWindow = 15.0;    // (seconds the rate is taken over)
    static constexpr double kFirst = 3.0;      // (seconds of samples before any estimate)
    static constexpr double kMinShare = 1.0 / 6;
    static constexpr double kSmooth = 8.0;     // (seconds: the estimate's time constant)
    double t[kSamples]{};
    size_t done[kSamples]{};
    int n = 0, head = 0;   // (the samples, oldest at head)
    double last_note = -1, shown = -1;

    void reset() { n = head = 0, last_note = shown = -1; }
    /// A sample: at seconds t_now, done of total (four a second at most are kept)
    void note(double t_now, size_t done_now, size_t total) {
        if (n && done_now < done[(head + n - 1) % kSamples]) reset();   // (a new stage)
        if (last_note >= 0 && t_now - last_note < 0.25) return;
        const double dt = last_note < 0 ? 0 : t_now - last_note;
        last_note = t_now;
        if (n == kSamples) head = (head + 1) % kSamples, n--;
        t[(head + n) % kSamples] = t_now, done[(head + n) % kSamples] = done_now, n++;
        while (n > 2 && t_now - t[(head + 1) % kSamples] >= kWindow) head = (head + 1) % kSamples, n--;
        const double raw = estimate(total);
        if (raw < 0) shown = -1;
        else if (shown < 0 || raw == 0) shown = raw;   // (the first one, or all done)
        else {
            shown = shown > dt ? shown - dt : 0;   // (time passing)
            shown += (raw - shown) * (dt < kSmooth ? dt / kSmooth : 1.0);
        }
    }
    /// Seconds left, or a negative number when it cannot be said yet
    double left() const { return shown; }
    /// The rate over the window alone, unsmoothed
    double estimate(size_t total) const {
        if (n < 2) return -1;
        const int last = (head + n - 1) % kSamples;
        const double span = t[last] - t[head];
        if (done[last] >= total) return 0;
        if (span < kFirst || done[last] <= done[head] || (double)done[last] < (double)total * kMinShare) return -1;
        const double rate = (double)(done[last] - done[head]) / span;
        return (double)(total - done[last]) / rate;
    }
};

/// 12345 as "12,345"
inline std::string with_commas(size_t v) {
    std::string s = std::to_string(v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
    return s;
}

/// "about 40 s left": up to 5 s under a minute, whole minutes above; empty when unknown
inline std::string left_text(double secs) {
    if (secs < 0) return "";
    char b[48];
    if (secs < 10) return "a few seconds left";
    if (secs < 55) snprintf(b, sizeof b, "about %d s left", (int)((secs + 4.999) / 5) * 5);
    else snprintf(b, sizeof b, "about %d min left", secs < 90 ? 1 : (int)(secs / 60 + 0.5));
    return b;
}

/// The version line from the build's name (nx2/version.py, "v2.0.5 (source 1a2b3c4d)"): the release alone, "v2.0.5"; a build
/// between releases ("v2.0.5-2-g619c897-dirty", git describe) shows as "v2.0.5 dev"; anything else as it is
inline std::string version_text(std::string name) {
    if (const size_t at = name.find(" ("); at != std::string::npos) name.resize(at);
    size_t i = 0;
    if (name.size() > 1 && name[0] == 'v') {
        int dots = 0;
        for (i = 1; i < name.size() && ((name[i] >= '0' && name[i] <= '9') || name[i] == '.'); i++) dots += name[i] == '.';
        if (dots == 2 && i < name.size() && name[i] == '-') return name.substr(0, i) + " dev";
    }
    return name;
}

/// "32%": rounded down, and 99% until all is done
inline std::string percent_text(size_t done, size_t total) {
    if (!total) return "";
    size_t p = done >= total ? 100 : done * 100 / total;
    if (p > 99 && done < total) p = 99;
    return std::to_string(p) + "%";
}

/// What the start-up screen shows (vk_startup_paint.h draws it)
struct StartupView {
    std::string version;            // ("v2.0.5"; empty: the build's)
    std::string step, title, pct;   // ("STEP 1 OF 2", "Preparing shaders", "32%"); an empty title: the wordmark alone
    std::string left, right;        // (under the bar: "12,345 / 38,809 shaders", "about 40 s left")
    std::string hint;
    double fraction = -1;           // (the bar, 0 to 1; negative: none)
    bool operator==(const StartupView&) const = default;
};

/// The state of the start: stage 0 the shader programs, 1 the pipelines the notes predict, 2 the noted pipelines; done of total
/// in it (total 0: not counted yet); left the seconds left (negative: unknown); prepared: this start prepares programs (a first
/// start after an update: two steps, else the pipelines alone); secs since the window opened
struct StartupState {
    int stage = 0;
    size_t done = 0, total = 0;
    double left = -1, secs = 0;
    bool prepared = false, finished = false, stopping = false;
};

/// The screen for a state. The first second shows the wordmark alone: a start with next to nothing to prepare is over by then
inline StartupView startup_view(const StartupState& s) {
    StartupView v;
    if (s.stopping) {
        v.title = "Closing";
        v.left = "Keeping what is prepared for the next start";
        return v;
    }
    if (s.finished) {
        if (s.secs >= 1.0) v.title = "Starting the game";
        return v;
    }
    if (s.secs < 1.0) return v;
    const bool pipelines = s.stage > 0;
    if (s.prepared) v.step = pipelines ? "STEP 2 OF 2" : "STEP 1 OF 2";
    v.title = !pipelines ? "Preparing shaders" : s.prepared ? "Building pipelines" : "Loading pipelines";
    if (s.prepared) v.hint = "This happens once after an update. Closing the window keeps what is done.";
    if (!s.total) {
        v.left = !pipelines ? "Reading the game's shaders" : s.stage == 1 ? "Reading the pipeline notes" : "Loading the shader cache";
        return v;
    }
    v.fraction = (double)std::min(s.done, s.total) / (double)s.total;
    v.pct = percent_text(s.done, s.total);
    v.left = with_commas(s.done) + " / " + with_commas(s.total) + (pipelines ? " pipelines" : " shaders");
    v.right = left_text(s.left);
    return v;
}

} // namespace mkwvk
