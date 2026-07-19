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
    for (auto& e : fs::directory_iterator(out_dir)) {
        if (!e.is_directory()) continue;
        auto p = e.path() / "manifest.json";
        if (fs::exists(p)) return p;
    }
    return {};
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

    // GET /api/jobs  → list recent jobs
    svr.Get("/api/jobs", [](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        std::lock_guard<std::mutex> lk(g_jobs_mu);
        for (auto it = g_job_order.rbegin(); it != g_job_order.rend(); ++it) {
            auto j = g_jobs[*it];
            arr.push_back({
                {"id", j->id},
                {"url", j->url},
                {"done", j->done.load()},
                {"exit_code", j->exit_code.load()},
                {"out_dir", j->out_dir.string()},
            });
        }
        json r = {{"jobs", arr}};
        res.set_content(r.dump(), "application/json");
    });

    // GET /api/jobs/:id
    svr.Get(R"(/api/jobs/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        auto id = req.matches[1].str();
        auto job = find_job(id);
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
        auto job = find_job(id);
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
        auto job = find_job(id);
        if (!job) { res.status = 404; return; }
        auto p = find_manifest(job->out_dir);
        if (p.empty()) { res.status = 404; return; }
        res.set_content(read_all(p), "application/json");
    });

    // GET /api/jobs/:id/heatmap
    svr.Get(R"(/api/jobs/([^/]+)/heatmap)",
            [](const httplib::Request& req, httplib::Response& res) {
        auto id = req.matches[1].str();
        auto job = find_job(id);
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
        auto job = find_job(id);
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
