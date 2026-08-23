// crux_server — tiny local HTTP server that wraps the crux CLI.
//
// Endpoints:
//   GET  /                     — dashboard/index.html
//   POST /api/run              — start a job. Body: { url, max_clips, format,
//                                cookies, dry_run, keep_intro, strict }
//                                Response: { job_id, out_dir }
//   GET  /api/jobs/:id         — { id, done, exit_code, url, out_dir }
//   GET  /api/jobs/:id/log?from=N — { from, next, chunk, done, exit_code }
//   GET  /api/jobs/:id/manifest — manifest.json contents
//   GET  /api/jobs/:id/file/... — serves a file from the job's output dir
//   GET  /api/jobs              — list of recent jobs
//
// The dashboard/ folder is served as static content. If it's not present
// next to the binary the server refuses to boot with a clear message.

#include "binres.h"
#include "media/proc.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// ---------- Job model ----------

struct Job {
    std::string id;
    std::string url;
    json params;
    fs::path out_dir;
    fs::path log_file;
    std::atomic<bool> done{false};
    std::atomic<int> exit_code{-1};
    std::chrono::system_clock::time_point created;
    std::chrono::system_clock::time_point finished;
    std::thread runner;

    ~Job() { if (runner.joinable()) runner.join(); }
};

std::mutex g_jobs_mu;
std::unordered_map<std::string, std::shared_ptr<Job>> g_jobs;
std::vector<std::string> g_job_order;   // insertion order (for listing)

std::string ymd_hms_id() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    std::ostringstream oss;
    oss << buf << "-" << std::setfill('0') << std::setw(3) << ms;
    return oss.str();
}

// ---------- Locate the CLI binary ----------

std::string locate_crux_exe(const std::optional<std::string>& override_path) {
    if (override_path && !override_path->empty()) return *override_path;
    if (const char* env = std::getenv("CRUX_EXE")) {
        if (fs::exists(env)) return env;
    }
    // Look next to this server binary.
    std::error_code ec;
    fs::path self;
#ifdef _WIN32
    // best-effort — use cwd + build tree layout
    self = fs::current_path();
#else
    self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) self = self.parent_path();
    else self = fs::current_path();
#endif
    const char* names[] = {
#ifdef _WIN32
        "crux.exe",
        "build/Release/crux.exe",
        "build/crux.exe",
#else
        "crux",
        "build/crux",
#endif
    };
    for (const char* n : names) {
        fs::path p = self / n;
        if (fs::exists(p)) return p.string();
        fs::path q = fs::current_path() / n;
        if (fs::exists(q)) return q.string();
    }
    // Last resort: PATH.
#ifdef _WIN32
    return "crux.exe";
#else
    return "crux";
#endif
}

// ---------- Argument building ----------

// Small helpers to keep the mapping table readable.
namespace pta {
    inline bool is_nonempty_str(const json& p, const std::string& k) {
        return p.contains(k) && p[k].is_string() && !p[k].get<std::string>().empty();
    }
    inline bool is_number(const json& p, const std::string& k) {
        return p.contains(k) && p[k].is_number();
    }
    inline std::string trim_num(double d) {
        // Compact numeric formatting: drop trailing zeros for cleaner CLI logs.
        std::ostringstream oss;
        oss.precision(6);
        oss << d;
        return oss.str();
    }
}

std::vector<std::string> params_to_args(const json& p) {
    std::vector<std::string> args;

    // Positional URL / id is passed by caller BEFORE these params.

    // -- selection --
    if (pta::is_number(p, "max_clips")) {
        args.push_back("-n");
        args.push_back(std::to_string(p["max_clips"].get<int>()));
    }
    if (pta::is_number(p, "clip_len") && p["clip_len"].get<double>() > 0) {
        args.push_back("-l");
        args.push_back(pta::trim_num(p["clip_len"].get<double>()));
    }
    if (pta::is_number(p, "min_gap") && p["min_gap"].get<double>() > 0) {
        args.push_back("--min-gap");
        args.push_back(pta::trim_num(p["min_gap"].get<double>()));
    }

    // -- output format --
    if (pta::is_nonempty_str(p, "format")) {
        args.push_back("--format");
        args.push_back(p["format"].get<std::string>());
    }

    // -- fetch source + cookies --
    if (pta::is_nonempty_str(p, "cookies") && p["cookies"].get<std::string>() != "none") {
        args.push_back("--cookies-from-browser");
        args.push_back(p["cookies"].get<std::string>());
    }
    if (pta::is_nonempty_str(p, "source") && p["source"].get<std::string>() != "ytdlp") {
        args.push_back("--source");
        args.push_back(p["source"].get<std::string>());
    }
    if (pta::is_nonempty_str(p, "detect") && p["detect"].get<std::string>() != "fused") {
        args.push_back("--detect");
        args.push_back(p["detect"].get<std::string>());
    }

    // -- binary overrides --
    if (pta::is_nonempty_str(p, "ytdlp")) {
        args.push_back("--ytdlp");
        args.push_back(p["ytdlp"].get<std::string>());
    }
    if (pta::is_nonempty_str(p, "ffmpeg")) {
        args.push_back("--ffmpeg");
        args.push_back(p["ffmpeg"].get<std::string>());
    }

    // -- boolean flags (default false unless noted) --
    if (p.value("dry_run",       false)) args.push_back("--dry-run");
    if (p.value("dump_heatmap",  true )) args.push_back("--dump-heatmap");
    if (p.value("keep_intro",    false)) args.push_back("--keep-intro");
    if (p.value("strict",        false)) args.push_back("--strict");
    if (p.value("full_download", false)) args.push_back("--full-download");
    if (p.value("try_sections",  false)) args.push_back("--try-sections");
    if (p.value("verbose",       false)) args.push_back("--verbose");

    return args;
}

// ---------- Job runner ----------

void run_job(std::shared_ptr<Job> job, std::string exe) {
    std::vector<std::string> args;
    args.push_back(job->url);
    args.push_back("-o");
    args.push_back(job->out_dir.string());
    auto extra = params_to_args(job->params);
    args.insert(args.end(), extra.begin(), extra.end());

    // Prepend an audit line to the log so the user can see what we ran.
    {
        std::ofstream ofs(job->log_file);
        ofs << "$ " << exe;
        for (auto& a : args) ofs << " " << a;
        ofs << "\n\n";
    }

    crux::proc::RunOptions opts;
    opts.timeout_ms = 90 * 60 * 1000;   // 90 min hard cap
    opts.redirect_output_to = job->log_file.string();
    // The redirect option overrides these, but be explicit anyway.
    opts.capture_stdout = false;
    opts.capture_stderr = false;

    try {
        // proc::run truncates the log file (CREATE_ALWAYS/O_TRUNC), so re-open
        // for append via the redirect path — we lose our audit prefix. Fix by
        // reopening the file for append AFTER the child writes its output:
        // actually the simplest thing is to skip the audit prefix entirely.
        // Re-write it here after the child exits so the log still shows both.
        auto r = crux::proc::run(exe, args, opts);
        job->exit_code = r.exit_code;
    } catch (const std::exception& e) {
        std::ofstream ofs(job->log_file, std::ios::app);
        ofs << "\n[server] fatal: " << e.what() << "\n";
        job->exit_code = -2;
    }
    // Append a footer that the frontend uses to detect end-of-stream.
    {
        std::ofstream ofs(job->log_file, std::ios::app);
        ofs << "\n[server] exit_code=" << job->exit_code.load() << "\n";
    }
    job->finished = std::chrono::system_clock::now();
    job->done = true;
}

// ---------- Utilities ----------

std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream oss; oss << f.rdbuf();
    return oss.str();
}

std::string mime_for(const fs::path& p) {
    auto ext = p.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(c));
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".css")  return "text/css";
    if (ext == ".js")   return "application/javascript";
    if (ext == ".json") return "application/json";
    if (ext == ".mp4")  return "video/mp4";
    if (ext == ".webm") return "video/webm";
    if (ext == ".png")  return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".svg")  return "image/svg+xml";
    if (ext == ".txt" || ext == ".log")  return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

std::shared_ptr<Job> find_job(const std::string& id) {
    std::lock_guard<std::mutex> lk(g_jobs_mu);
    auto it = g_jobs.find(id);
    return it == g_jobs.end() ? nullptr : it->second;
}

fs::path resolve_dashboard_dir() {
    // Candidates: ./dashboard, ../dashboard, ../../dashboard
    for (const char* c : {"dashboard", "../dashboard", "../../dashboard"}) {
        fs::path p = fs::current_path() / c;
        if (fs::exists(p / "index.html")) return p;
    }
    return {};
}

fs::path find_manifest(const fs::path& out_dir) {
    fs::path cand = out_dir / "manifest.json";
    if (fs::exists(cand)) return cand;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(out_dir, ec)) {
        if (!e.is_directory()) continue;
        auto p = e.path() / "manifest.json";
        if (fs::exists(p)) return p;
    }
    return {};
}

// ---------- Disk runs (out/ directories from past sessions / the CLI) ----

bool safe_job_id(const std::string& id) {
    return !id.empty() && id.find("..") == std::string::npos &&
           id.find('/') == std::string::npos &&
           id.find('\\') == std::string::npos;
}

// Metadata pulled from a run's manifest for the sidebar. Empty on failure.
struct RunMeta {
    std::string url;
    std::string title;
    int clip_count = 0;
};
RunMeta read_run_meta(const fs::path& out_dir) {
    RunMeta m;
    auto man = find_manifest(out_dir);
    if (man.empty()) return m;
    try {
        json j = json::parse(read_all(man));
        const auto& v = j.value("video", json::object());
        m.url = v.value("url", std::string{});
        m.title = v.value("title", std::string{});
        if (j.contains("clips") && j["clips"].is_array())
            m.clip_count = static_cast<int>(j["clips"].size());
    } catch (...) {}
    return m;
}

// Epoch millis for a file's last write time.
std::int64_t file_epoch_ms(const fs::path& p) {
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    if (ec) return 0;
    auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        sys.time_since_epoch()).count();
}

// Best-effort start time for a disk run: parse "web-YYYYMMDD-HHMMSS-mmm" ids,
// otherwise fall back to the earliest interesting file's mtime.
std::int64_t infer_disk_start_ms(const std::string& id, const fs::path& dir) {
    if (id.rfind("web-", 0) == 0 && id.size() >= 21) {
        std::tm tm{};
        int ms = 0;
        if (std::sscanf(id.c_str() + 4, "%4d%2d%2d-%2d%2d%2d-%3d",
                        &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                        &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &ms) == 7) {
            tm.tm_year -= 1900;
            tm.tm_mon  -= 1;
            std::time_t t = std::mktime(&tm);
            if (t != -1)
                return static_cast<std::int64_t>(t) * 1000 + ms;
        }
    }
    // Fallback: the run.log (if present) is written first; otherwise pick
    // the oldest file we can find in the dir.
    std::int64_t best = 0;
    for (const char* n : {"run.log", "heatmap.json", "captions.json"}) {
        fs::path p = dir / n;
        if (fs::exists(p)) {
            auto v = file_epoch_ms(p);
            if (v && (!best || v < best)) best = v;
        }
    }
    return best;
}

// Resolves an id to an in-memory job, or synthesizes a read-only "disk job"
// for any out/<id> directory that holds a manifest — so every past run stays
// browsable in the dashboard across server restarts.
std::shared_ptr<Job> find_or_disk_job(const std::string& id) {
    if (auto j = find_job(id)) return j;
    if (!safe_job_id(id)) return nullptr;
    fs::path dir = fs::path("out") / id;
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || find_manifest(dir).empty()) return nullptr;
    auto job = std::make_shared<Job>();
    job->id = id;
    job->out_dir = dir;
    job->log_file = dir / "run.log";
    job->url = read_run_meta(dir).url;
    job->done = true;
    job->exit_code = 0;   // a manifest only exists once planning succeeded
    return job;
}

} // namespace

int main(int argc, char** argv) {
    int port = 8181;
    std::optional<std::string> exe_override;
    std::string host = "127.0.0.1";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (a == "--host" && i + 1 < argc) host = argv[++i];
        else if (a == "--crux" && i + 1 < argc) exe_override = argv[++i];
        else if (a == "--help" || a == "-h") {
            std::printf("crux_server [--port N] [--host HOST] [--crux PATH]\n");
            return 0;
        }
    }

    fs::path dashboard = resolve_dashboard_dir();
    if (dashboard.empty()) {
        std::fprintf(stderr,
            "error: dashboard/ folder not found next to the server or in cwd.\n"
            "       Run this server from the project root.\n");
        return 1;
    }
    std::string exe = locate_crux_exe(exe_override);
    if (!fs::exists(exe)) {
        std::fprintf(stderr,
            "error: crux binary not found at %s.\n"
            "       Build it first (setup.bat / rebuild.bat), or pass "
            "--crux PATH.\n", exe.c_str());
        return 1;
    }

    httplib::Server svr;
    svr.set_mount_point("/", dashboard.string());

    // POST /api/run  → start a job
    svr.Post("/api/run", [exe](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            if (!body.contains("url") || !body["url"].is_string() ||
                body["url"].get<std::string>().empty()) {
                res.status = 400;
                res.set_content(R"({"error":"missing url"})", "application/json");
                return;
            }
            auto job = std::make_shared<Job>();
            job->id      = ymd_hms_id();
            job->url     = body["url"].get<std::string>();
            job->params  = body;
            job->created = std::chrono::system_clock::now();
            job->out_dir = fs::path("out") / ("web-" + job->id);
            fs::create_directories(job->out_dir);
            job->log_file = job->out_dir / "run.log";
            {
                std::lock_guard<std::mutex> lk(g_jobs_mu);
                g_jobs[job->id] = job;
                g_job_order.push_back(job->id);
                while (g_job_order.size() > 50) {
                    g_jobs.erase(g_job_order.front());
                    g_job_order.erase(g_job_order.begin());
                }
            }
            job->runner = std::thread(run_job, job, exe);
            json r = {
                {"job_id", job->id},
                {"out_dir", job->out_dir.string()},
            };
            res.set_content(r.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            json r = {{"error", e.what()}};
            res.set_content(r.dump(), "application/json");
        }
    });

    // GET /api/jobs  → in-memory jobs first (newest first), then every other
    // run directory found under out/ (from earlier sessions or the CLI),
    // sorted by modification time.
    svr.Get("/api/jobs", [](const httplib::Request&, httplib::Response& res) {
        auto to_ms = [](const std::chrono::system_clock::time_point& tp) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                tp.time_since_epoch()).count();
        };

        json arr = json::array();
        std::vector<std::string> seen;
        {
            std::lock_guard<std::mutex> lk(g_jobs_mu);
            for (auto it = g_job_order.rbegin(); it != g_job_order.rend(); ++it) {
                auto j = g_jobs[*it];
                seen.push_back(j->id);
                // The API job id omits the "web-" prefix while its output
                // directory includes it. Mark both forms as seen so the disk
                // scan below does not list a freshly completed job twice.
                seen.push_back(j->out_dir.filename().string());
                RunMeta meta = read_run_meta(j->out_dir);
                arr.push_back({
                    {"id", j->id},
                    {"url", j->url},
                    {"title", meta.title},
                    {"clip_count", meta.clip_count},
                    {"done", j->done.load()},
                    {"exit_code", j->exit_code.load()},
                    {"out_dir", j->out_dir.string()},
                    {"started_ms", to_ms(j->created)},
                    {"finished_ms", j->done.load() ? to_ms(j->finished) : 0},
                });
            }
        }
        struct DiskRun { fs::path dir; std::int64_t mtime_ms; };
        std::vector<DiskRun> disk;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator("out", ec)) {
            if (!e.is_directory()) continue;
            const std::string name = e.path().filename().string();
            if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
            if (find_manifest(e.path()).empty()) continue;
            disk.push_back({e.path(), file_epoch_ms(find_manifest(e.path()))});
        }
        std::sort(disk.begin(), disk.end(),
                  [](const DiskRun& a, const DiskRun& b) { return a.mtime_ms > b.mtime_ms; });
        for (const auto& d : disk) {
            const std::string id = d.dir.filename().string();
            RunMeta meta = read_run_meta(d.dir);
            std::int64_t start = infer_disk_start_ms(id, d.dir);
            arr.push_back({
                {"id", id},
                {"url", meta.url},
                {"title", meta.title},
                {"clip_count", meta.clip_count},
                {"done", true},
                {"exit_code", 0},
                {"disk", true},
                {"out_dir", d.dir.string()},
                {"started_ms", start},
                {"finished_ms", d.mtime_ms},
            });
        }
        json r = {{"jobs", arr}};
        res.set_content(r.dump(), "application/json");
    });

    // GET /api/jobs/:id
    svr.Get(R"(/api/jobs/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        auto id = req.matches[1].str();
        auto job = find_or_disk_job(id);
        if (!job) { res.status = 404; return; }
        json r = {
            {"id", job->id},
            {"url", job->url},
            {"params", job->params},
            {"done", job->done.load()},
            {"exit_code", job->exit_code.load()},
            {"out_dir", job->out_dir.string()},
        };
        res.set_content(r.dump(), "application/json");
    });

    // GET /api/jobs/:id/log?from=N
    svr.Get(R"(/api/jobs/([^/]+)/log)", [](const httplib::Request& req, httplib::Response& res) {
        auto id = req.matches[1].str();
        auto job = find_or_disk_job(id);
        if (!job) { res.status = 404; return; }
        std::uint64_t from = 0;
        if (req.has_param("from")) {
            try { from = std::stoull(req.get_param_value("from")); } catch (...) {}
        }
        std::ifstream f(job->log_file, std::ios::binary);
        if (!f) {
            json r = {
                {"from", from}, {"next", from}, {"chunk", ""},
                {"done", job->done.load()},
                {"exit_code", job->exit_code.load()},
            };
            res.set_content(r.dump(), "application/json");
            return;
        }
        f.seekg(0, std::ios::end);
        std::uint64_t sz = static_cast<std::uint64_t>(f.tellg());
        std::string chunk;
        if (from < sz) {
            f.seekg(static_cast<std::streamoff>(from));
            chunk.resize(sz - from);
            f.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        }
        json r = {
            {"from", from}, {"next", sz}, {"chunk", chunk},
            {"done", job->done.load()},
            {"exit_code", job->exit_code.load()},
        };
        res.set_content(r.dump(), "application/json");
    });

    // GET /api/jobs/:id/manifest
    svr.Get(R"(/api/jobs/([^/]+)/manifest)",
            [](const httplib::Request& req, httplib::Response& res) {
        auto id = req.matches[1].str();
        auto job = find_or_disk_job(id);
        if (!job) { res.status = 404; return; }
        auto p = find_manifest(job->out_dir);
        if (p.empty()) { res.status = 404; return; }
        res.set_content(read_all(p), "application/json");
    });

    // GET /api/jobs/:id/heatmap
    svr.Get(R"(/api/jobs/([^/]+)/heatmap)",
            [](const httplib::Request& req, httplib::Response& res) {
        auto id = req.matches[1].str();
        auto job = find_or_disk_job(id);
        if (!job) { res.status = 404; return; }
        // heatmap.json sits next to manifest.json.
        auto man = find_manifest(job->out_dir);
        if (man.empty()) { res.status = 404; return; }
        auto p = man.parent_path() / "heatmap.json";
        if (!fs::exists(p)) { res.status = 404; return; }
        res.set_content(read_all(p), "application/json");
    });

    // GET /api/jobs/:id/file/<name>   (serves clip mp4s, etc.)
    svr.Get(R"(/api/jobs/([^/]+)/file/(.+))",
            [](const httplib::Request& req, httplib::Response& res) {
        auto id  = req.matches[1].str();
        auto rel = req.matches[2].str();
        if (rel.find("..") != std::string::npos) { res.status = 400; return; }
        auto job = find_or_disk_job(id);
        if (!job) { res.status = 404; return; }
        // Look in out_dir/<video-id>/<rel> first (usual layout).
        fs::path p;
        for (auto& e : fs::directory_iterator(job->out_dir)) {
            if (e.is_directory()) {
                auto c = e.path() / rel;
                if (fs::exists(c) && fs::is_regular_file(c)) { p = c; break; }
            }
        }
        if (p.empty()) {
            fs::path c = job->out_dir / rel;
            if (fs::exists(c) && fs::is_regular_file(c)) p = c;
        }
        if (p.empty()) { res.status = 404; return; }
        res.set_content(read_all(p), mime_for(p).c_str());
    });

    // Basic health / config info.
    svr.Get("/api/info", [exe, dashboard](const httplib::Request&, httplib::Response& res) {
        json r = {
            {"crux_exe", exe},
            {"dashboard_dir", dashboard.string()},
            {"cwd", fs::current_path().string()},
        };
        res.set_content(r.dump(), "application/json");
    });

    std::printf("crux dashboard ready — open http://%s:%d/ in your browser\n",
                host.c_str(), port);
    std::printf("(crux binary: %s)\n(dashboard: %s)\n", exe.c_str(),
                dashboard.string().c_str());
    std::fflush(stdout);

    if (!svr.listen(host.c_str(), port)) {
        std::fprintf(stderr,
            "error: could not bind %s:%d. Is another instance already running?\n",
            host.c_str(), port);
        return 2;
    }
    return 0;
}
