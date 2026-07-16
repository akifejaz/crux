// crux downloader — strategy selection + yt-dlp invocation.
//
// Two production paths:
//
//   Full            : `yt-dlp <URL> -o <id>.mp4`  (whole video)
//   PerClipParallel : N concurrent `yt-dlp --download-sections *S-E`
//                     invocations, each writing <id>_sec<NN>.mp4
//
// Per-clip mode is picked when:
//   - user did NOT pass --full-download
//   - clips.size() >= 2  (single clip is simpler as full download for cache-hit reasons)
//   - Σ(clip lengths) < 30% of duration  (we're actually saving bandwidth)
//   - duration > 15 minutes             (short videos: just get them whole)
//
// Any failure of the parallel batch (bad exit / empty output / tiny file)
// triggers a clean fall-back to the Full path, so a rate-limited /
// bot-checked run still completes.

#include "media/downloader.h"

#include "binres.h"
#include "media/proc.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace crux::media {

namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr std::uintmax_t kMinPlausibleBytes = 64 * 1024;   // 64 KiB
constexpr double kPerClipMaxFrac  = 0.30;                  // clip total < 30% of video
constexpr double kPerClipMinDurationSec = 15.0 * 60.0;     // long-video threshold
constexpr int    kMaxParallelDownloads = 4;                // hard cap; more risks 429s

// ---------------------------------------------------------------------------
// Small utils
// ---------------------------------------------------------------------------
std::string format_time_range(double s, double e) {
    std::ostringstream oss;
    oss.setf(std::ios::fixed); oss.precision(2);
    oss << "*" << s << "-" << e;
    return oss.str();
}

double total_clip_seconds(const std::vector<Clip>& clips) {
    double s = 0.0;
    for (const auto& c : clips) s += c.end_sec - c.start_sec;
    return s;
}

// Common yt-dlp args used by every strategy.
std::vector<std::string> base_args(const std::string& out_template,
                                   const Config& cfg) {
    std::vector<std::string> a = {
        // Prefer H.264 — faster to decode, HTTP-range friendly.
        "-f",
        "bv*[height<=1080][vcodec^=avc1]+ba[ext=m4a]"
        "/bv*[height<=720][vcodec^=avc1]+ba"
        "/bv*[height<=1080][ext=mp4]+ba[ext=m4a]"
        "/b[ext=mp4]/b",
        "--concurrent-fragments", "4",
        "--merge-output-format", "mp4",
        "--retries", "10",
        "--fragment-retries", "10",
        "--no-abort-on-error",
        "--force-overwrites",
        "--no-warnings",
        "--no-playlist",
        "--newline",       // force progress on its own line for piped output
        "-o", out_template,
    };
    if (cfg.cookies_from_browser) {
        a.push_back("--cookies-from-browser");
        a.push_back(*cfg.cookies_from_browser);
    }
    return a;
}

std::string find_output_mp4_matching(const std::string& work_dir,
                                     const std::string& stem) {
    fs::path expected = fs::path(work_dir) / (stem + ".mp4");
    if (fs::exists(expected)) return expected.string();
    for (auto& e : fs::directory_iterator(work_dir)) {
        if (e.is_regular_file() && e.path().extension() == ".mp4" &&
            e.path().filename().string().rfind(stem, 0) == 0) {
            return e.path().string();
        }
    }
    return {};
}

std::optional<std::string> validate_output(const std::string& path,
                                           double min_seconds_expected) {
    if (path.empty()) return "no output mp4 found";
    std::error_code ec;
    auto sz = fs::file_size(path, ec);
    if (ec) return "cannot stat output file";
    if (sz < kMinPlausibleBytes) {
        return "output file is " + std::to_string(sz) +
               " bytes — download almost certainly failed";
    }
    if (min_seconds_expected > 0.0) {
        const double bytes_per_sec = static_cast<double>(sz) / min_seconds_expected;
        if (bytes_per_sec < 50000.0) {
            return "output is only " + std::to_string(sz) +
                   " bytes for ~" + std::to_string(static_cast<int>(min_seconds_expected)) +
                   "s of video — likely truncated";
        }
    }
    return std::nullopt;
}

void remove_stale(const std::string& work_dir, const std::string& id_prefix) {
    for (auto& e : fs::directory_iterator(work_dir)) {
        auto name = e.path().filename().string();
        if (name.rfind(id_prefix, 0) == 0) {
            std::error_code ec;
            fs::remove(e.path(), ec);
        }
    }
}

int run_ytdlp_visible(const std::string& exe, const std::vector<std::string>& args) {
    // Passthrough mode — used for full-video download so the user sees
    // yt-dlp's progress bar in real time.
    proc::RunOptions opts;
    opts.timeout_ms = 60 * 60 * 1000;
    opts.inherit_stdout = true;
    opts.inherit_stderr = true;
    opts.capture_stdout = false;
    opts.capture_stderr = false;
    proc::RunResult r = proc::run(exe, args, opts);
    return r.exit_code;
}

// Hard wall-clock cap per section worker. yt-dlp on DASH streams can hang
// silently after "Destination:" with no error — this ensures we never wait
// forever, regardless of what yt-dlp is doing.
constexpr int kSectionWorkerTimeoutMs = 5 * 60 * 1000;   // 5 min

int run_ytdlp_to_file(const std::string& exe,
                      const std::vector<std::string>& args,
                      const std::string& log_path) {
    // Used for per-clip parallel spawns — each child writes its progress
    // to its own log file so the terminal isn't a wall of interleaved lines.
    proc::RunOptions opts;
    opts.timeout_ms = kSectionWorkerTimeoutMs;
    opts.redirect_output_to = log_path;
    opts.capture_stdout = false;
    opts.capture_stderr = false;
    proc::RunResult r = proc::run(exe, args, opts);
    if (r.timed_out) return -1000;   // sentinel: distinguish from yt-dlp exit codes
    return r.exit_code;
}

std::string cookies_hint(const Config& cfg) {
    if (cfg.cookies_from_browser) return {};
    return "\n"
           "hint: YouTube's bot check often rejects downloads.\n"
           "      Retry with:  --cookies-from-browser chrome\n"
           "      (or firefox / edge — whichever browser you're signed in to).";
}

std::string zero_pad(int n, int width) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%0*d", width, n);
    return buf;
}

// ---------------------------------------------------------------------------
// Shared stderr — a mutex prevents interleaved bytes from different workers.
// ---------------------------------------------------------------------------
std::mutex g_stderr_mu;

void emit_prefixed(const std::string& prefix, const std::string& line) {
    std::lock_guard<std::mutex> lk(g_stderr_mu);
    std::fprintf(stderr, "%s %s\n", prefix.c_str(), line.c_str());
    std::fflush(stderr);
}

void emit_plain(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_stderr_mu);
    std::fprintf(stderr, "%s\n", line.c_str());
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// LogTailer — a per-worker background thread that reads new bytes from a
// yt-dlp log file, splits into complete lines, and prints each to stderr
// prefixed with e.g. "[c03]". Enables live per-worker progress in parallel
// mode without interleaved-byte chaos.
// ---------------------------------------------------------------------------
class LogTailer {
public:
    void start(std::string log_path, std::string prefix) {
        log_path_ = std::move(log_path);
        prefix_   = std::move(prefix);
        stop_.store(false);
        thr_ = std::thread([this]{ run(); });
    }
    void stop() {
        stop_.store(true);
        if (thr_.joinable()) thr_.join();
    }
    ~LogTailer() { stop(); }

private:
    void run() {
        std::string leftover;
        std::uint64_t pos = 0;
        while (!stop_.load()) {
            drain(pos, leftover);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        // Final drain after the child exits + a moment for FS flush.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        drain(pos, leftover);
        if (!leftover.empty()) emit_prefixed(prefix_, leftover);
    }
    void drain(std::uint64_t& pos, std::string& leftover) {
        std::ifstream f(log_path_, std::ios::binary);
        if (!f) return;
        f.seekg(0, std::ios::end);
        auto end = static_cast<std::uint64_t>(f.tellg());
        if (end <= pos) return;
        f.seekg(static_cast<std::streamoff>(pos));
        std::string chunk(end - pos, '\0');
        f.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        pos = end;
        leftover += chunk;
        std::size_t nl;
        while ((nl = leftover.find('\n')) != std::string::npos) {
            std::string line = leftover.substr(0, nl);
            leftover.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) emit_prefixed(prefix_, line);
        }
    }

    std::string log_path_;
    std::string prefix_;
    std::atomic<bool> stop_{false};
    std::thread thr_;
};

// ---------------------------------------------------------------------------
// Strategies
// ---------------------------------------------------------------------------

// FULL DOWNLOAD -------------------------------------------------------------
DownloadResult do_full(const std::string& url,
                       const VideoMeta& meta,
                       const Config& cfg,
                       const std::string& exe,
                       const std::string& work_dir) {
    const fs::path outpl = fs::path(work_dir) / (meta.id + ".%(ext)s");
    std::vector<std::string> args = base_args(outpl.string(), cfg);
    args.push_back(url);

    const int rc = run_ytdlp_visible(exe, args);
    std::string path = find_output_mp4_matching(work_dir, meta.id);
    auto err = validate_output(path, meta.duration_sec);
    if (rc != 0 || err) {
        std::string msg = "yt-dlp full download failed";
        if (rc != 0) msg += " (exit " + std::to_string(rc) + ")";
        if (err)     msg += ": " + *err;
        msg += cookies_hint(cfg);
        throw std::runtime_error(msg);
    }
    DownloadResult out;
    out.source_file = path;
    out.strategy = Strategy::Full;
    return out;
}

// PER-CLIP PARALLEL ---------------------------------------------------------
// Returns a DownloadResult on success; throws on failure so the caller can
// fall back to Full.
DownloadResult do_per_clip_parallel(const std::string& url,
                                    const VideoMeta& meta,
                                    const std::vector<Clip>& clips,
                                    const Config& cfg,
                                    const std::string& exe,
                                    const std::string& work_dir) {
    const int workers = std::min<int>(kMaxParallelDownloads,
                                       static_cast<int>(clips.size()));
    emit_plain("[download] parallel per-clip mode: " +
               std::to_string(clips.size()) + " clip(s), " +
               std::to_string(workers) + " worker(s) — live output prefixed [cNN]");

    struct Task {
        int clip_index_1based = 0;
        double src_start = 0.0;
        double src_end = 0.0;
        std::string out_stem;
        std::string out_path;    // final mp4 (filled by find_output_mp4)
        std::string log_path;
        int rc = -1;
        std::string err_reason;
    };

    std::vector<Task> tasks;
    tasks.reserve(clips.size());
    const int idx_width = static_cast<int>(
        std::to_string(clips.size()).size());
    for (const auto& c : clips) {
        Task t;
        t.clip_index_1based = c.index;
        t.src_start = std::max(0.0, c.start_sec - 10.0);   // ±10 s pad
        t.src_end   = std::min(meta.duration_sec, c.end_sec + 10.0);
        t.out_stem  = meta.id + "_sec" + zero_pad(c.index, idx_width);
        t.log_path  = (fs::path(work_dir) /
                       (t.out_stem + ".log")).string();
        tasks.push_back(std::move(t));
    }

    // Simple bounded-worker pool via a semaphore-ish counter.
    std::mutex active_mu;
    int active = 0;
    std::condition_variable active_cv;
    std::atomic<int> completed{0};
    std::vector<std::future<void>> futures;

    auto worker = [&](Task& t) {
        {
            std::unique_lock<std::mutex> lk(active_mu);
            active_cv.wait(lk, [&]{ return active < workers; });
            ++active;
        }

        const std::string tag = "[c" + zero_pad(t.clip_index_1based, idx_width) + "]";
        char range_buf[96];
        std::snprintf(range_buf, sizeof(range_buf),
            "starting yt-dlp for %.1fs..%.1fs (%.0fs range) -> %s",
            t.src_start, t.src_end, t.src_end - t.src_start, t.out_stem.c_str());
        emit_prefixed(tag, range_buf);

        // Ensure the log file exists before the tailer opens it — otherwise
        // the tailer's first read races the child's first write.
        { std::ofstream touch(t.log_path); }

        LogTailer tailer;
        tailer.start(t.log_path, tag);

        const auto t0 = std::chrono::steady_clock::now();
        const fs::path outpl = fs::path(work_dir) / (t.out_stem + ".%(ext)s");
        std::vector<std::string> args = base_args(outpl.string(), cfg);
        args.push_back("--download-sections");
        args.push_back(format_time_range(t.src_start, t.src_end));
        args.push_back(url);

        t.rc = run_ytdlp_to_file(exe, args, t.log_path);

        tailer.stop();   // flushes any trailing lines

        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - t0).count();

        t.out_path = find_output_mp4_matching(work_dir, t.out_stem);
        auto err = validate_output(t.out_path, t.src_end - t.src_start);
        if (err) t.err_reason = *err;

        int done = ++completed;
        const char* status =
            (t.rc == -1000)         ? "TIMEOUT"
          : (t.rc == 0 && !err)     ? "done"
          :                            "FAILED";
        char summary[224];
        std::snprintf(summary, sizeof(summary),
            "[download] clip %d/%zu %s in %llds (%.0fs..%.0fs, exit=%d)",
            done, clips.size(), status,
            static_cast<long long>(elapsed),
            t.src_start, t.src_end, t.rc);
        emit_plain(summary);

        {
            std::lock_guard<std::mutex> lk(active_mu);
            --active;
        }
        active_cv.notify_one();
    };

    for (auto& t : tasks) {
        futures.emplace_back(
            std::async(std::launch::async, worker, std::ref(t)));
    }
    for (auto& f : futures) f.get();

    // Check results; any failure → throw so caller can fall back.
    int failed_count = 0;
    for (const auto& t : tasks) {
        if (t.rc != 0 || !t.err_reason.empty() || t.out_path.empty()) {
            ++failed_count;
        }
    }
    if (failed_count > 0) {
        std::ostringstream oss;
        oss << failed_count << " of " << tasks.size()
            << " per-clip download(s) failed:";
        for (const auto& t : tasks) {
            if (t.rc != 0 || !t.err_reason.empty()) {
                oss << "\n  clip #" << t.clip_index_1based
                    << " (rc=" << t.rc << ")"
                    << (t.err_reason.empty() ? "" : ": " + t.err_reason)
                    << " — log: " << t.log_path;
            }
        }
        throw std::runtime_error(oss.str());
    }

    // Success — assemble DownloadResult indexed by clip.index-1.
    DownloadResult out;
    out.strategy = Strategy::PerClipParallel;
    out.per_clip_files.resize(clips.size());
    out.sections.resize(clips.size());
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        const auto& t = tasks[i];
        // clip.index is 1-based; store at index-1 to keep ordering with
        // the clips vector (which is what pipeline iterates over).
        std::size_t slot = static_cast<std::size_t>(t.clip_index_1based - 1);
        if (slot >= clips.size()) slot = i;
        out.per_clip_files[slot] = t.out_path;
        out.sections[slot] = { t.src_start, t.src_end, 0.0 };
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Strategy pick_strategy(const VideoMeta& meta,
                       const std::vector<Clip>& clips,
                       const Config& cfg) {
    // Full download is the default. yt-dlp's --download-sections is
    // unreliable on YouTube DASH — workers hang after "Destination" with
    // no error. Users can opt in explicitly with --try-sections when they
    // want to save bandwidth on very long videos with tiny clip footprints,
    // knowing the tool may need to fall back to full download.
    if (cfg.full_download)          return Strategy::Full;
    if (!cfg.try_sections)          return Strategy::Full;
    if (clips.size() < 2)           return Strategy::Full;
    if (meta.duration_sec <= 0.0)   return Strategy::Full;
    const double sum = total_clip_seconds(clips);
    const bool small_enough = sum < kPerClipMaxFrac * meta.duration_sec;
    const bool long_enough  = meta.duration_sec > kPerClipMinDurationSec;
    if (small_enough && long_enough) return Strategy::PerClipParallel;
    return Strategy::Full;
}

DownloadResult download(const std::string& url,
                        const VideoMeta& meta,
                        const std::vector<Clip>& clips,
                        const Config& cfg,
                        const std::string& work_dir) {
    const std::string exe = binres::resolve_ytdlp(cfg.ytdlp_path);
    fs::create_directories(work_dir);

    const Strategy strat = pick_strategy(meta, clips, cfg);

    // Attempt 1: parallel per-clip, if selected.
    if (strat == Strategy::PerClipParallel) {
        spdlog::debug("strategy = PerClipParallel ({} clips)", clips.size());
        try {
            return do_per_clip_parallel(url, meta, clips, cfg, exe, work_dir);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "\n[warn] per-clip parallel mode failed: %s\n"
                "        falling back to full download.\n", e.what());
            remove_stale(work_dir, meta.id + "_sec");
        }
    }

    // Attempt 2 (or 1): full download.
    spdlog::debug("strategy = Full");
    return do_full(url, meta, cfg, exe, work_dir);
}

} // namespace crux::media
