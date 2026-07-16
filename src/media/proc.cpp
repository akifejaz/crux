#include "media/proc.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <poll.h>
#  include <signal.h>
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

namespace crux::proc {

namespace {

#ifdef _WIN32
// Quote an argument for CommandLineToArgvW rules (used by CreateProcess).
std::string win_quote(const std::string& s) {
    if (!s.empty() && s.find_first_of(" \t\"\n\v") == std::string::npos) return s;
    std::string out; out.reserve(s.size() + 2);
    out.push_back('"');
    for (size_t i = 0; i < s.size(); ) {
        size_t bs = 0;
        while (i < s.size() && s[i] == '\\') { ++bs; ++i; }
        if (i == s.size())      { out.append(bs * 2, '\\'); break; }
        if (s[i] == '"')        { out.append(bs * 2 + 1, '\\'); out.push_back('"'); }
        else                    { out.append(bs, '\\'); out.push_back(s[i]); }
        ++i;
    }
    out.push_back('"');
    return out;
}

std::string join_cmdline(const std::string& exe, const std::vector<std::string>& args) {
    std::string cmd = win_quote(exe);
    for (const auto& a : args) { cmd.push_back(' '); cmd += win_quote(a); }
    return cmd;
}

RunResult run_win_redirected(const std::string& exe,
                             const std::vector<std::string>& args,
                             const std::string& log_path,
                             std::optional<int> timeout_ms) {
    RunResult rr;
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE log_h = CreateFileA(log_path.c_str(),
        GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log_h == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot open log for redirect: " + log_path);
    STARTUPINFOA si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = log_h;
    si.hStdError  = log_h;
    PROCESS_INFORMATION pi{};

    std::string cmd = join_cmdline(exe, args);
    std::vector<char> cmdbuf(cmd.begin(), cmd.end()); cmdbuf.push_back('\0');
    BOOL ok = CreateProcessA(nullptr, cmdbuf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) { CloseHandle(log_h); throw std::runtime_error("CreateProcess failed: " + exe); }
    CloseHandle(log_h);   // child holds its own copy.

    DWORD wait_ms = timeout_ms ? static_cast<DWORD>(*timeout_ms) : INFINITE;
    DWORD w = WaitForSingleObject(pi.hProcess, wait_ms);
    if (w == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        rr.timed_out = true;
        WaitForSingleObject(pi.hProcess, 2000);
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    rr.exit_code = static_cast<int>(code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return rr;
}

RunResult run_win(const std::string& exe, const std::vector<std::string>& args,
                  const RunOptions& opts) {
    if (opts.redirect_output_to) {
        return run_win_redirected(exe, args, *opts.redirect_output_to, opts.timeout_ms);
    }
    RunResult rr;
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;

    HANDLE out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr;
    const bool inherit_out = opts.inherit_stdout;
    const bool inherit_err = opts.inherit_stderr && !opts.merge_streams;

    if (inherit_out) {
        out_w = GetStdHandle(STD_OUTPUT_HANDLE);
    } else {
        if (!CreatePipe(&out_r, &out_w, &sa, 0)) throw std::runtime_error("CreatePipe stdout");
        SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    }
    if (opts.merge_streams) {
        err_r = out_r; err_w = out_w;
    } else if (inherit_err) {
        err_w = GetStdHandle(STD_ERROR_HANDLE);
    } else {
        if (!CreatePipe(&err_r, &err_w, &sa, 0)) throw std::runtime_error("CreatePipe stderr");
        SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOA si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_w;
    si.hStdError  = err_w;
    PROCESS_INFORMATION pi{};

    std::string cmd = join_cmdline(exe, args);
    std::vector<char> cmdbuf(cmd.begin(), cmd.end()); cmdbuf.push_back('\0');

    // If we are inheriting handles, don't use CREATE_NO_WINDOW — we want the
    // child to write directly to our console.
    DWORD flags = (inherit_out || inherit_err) ? 0 : CREATE_NO_WINDOW;
    BOOL ok = CreateProcessA(nullptr, cmdbuf.data(), nullptr, nullptr, TRUE,
                             flags, nullptr, nullptr, &si, &pi);
    if (!ok) {
        if (!inherit_out) { CloseHandle(out_w); CloseHandle(out_r); }
        if (!opts.merge_streams && !inherit_err) { CloseHandle(err_w); CloseHandle(err_r); }
        throw std::runtime_error("CreateProcess failed: " + exe);
    }
    if (!inherit_out) CloseHandle(out_w);
    if (!opts.merge_streams && !inherit_err) CloseHandle(err_w);

    auto drain = [](HANDLE h, std::string& sink) {
        char buf[4096]; DWORD n = 0;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0)
            sink.append(buf, n);
    };

    // Drain non-inherited pipes.
    if (!inherit_out) drain(out_r, rr.stdout_text);
    if (!opts.merge_streams && !inherit_err) drain(err_r, rr.stderr_text);

    DWORD wait_ms = opts.timeout_ms ? static_cast<DWORD>(*opts.timeout_ms) : INFINITE;
    DWORD w = WaitForSingleObject(pi.hProcess, wait_ms);
    if (w == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        rr.timed_out = true;
        WaitForSingleObject(pi.hProcess, 2000);
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    rr.exit_code = static_cast<int>(code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (!inherit_out) CloseHandle(out_r);
    if (!opts.merge_streams && !inherit_err) CloseHandle(err_r);
    return rr;
}
#else
RunResult run_posix_redirected(const std::string& exe,
                               const std::vector<std::string>& args,
                               const std::string& log_path,
                               std::optional<int> timeout_ms) {
    RunResult rr;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, log_path.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);

    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    std::string arg0 = exe;
    argv.push_back(arg0.data());
    std::vector<std::string> owned = args;
    for (auto& a : owned) argv.push_back(a.data());
    argv.push_back(nullptr);

    pid_t pid = 0;
    int rc = posix_spawnp(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        throw std::runtime_error("posix_spawnp failed: " + exe + " (" + std::strerror(rc) + ")");
    }
    // Timeout supervision: poll waitpid(WNOHANG) at slice intervals so we can
    // kill on timeout.
    struct timespec t0{}; clock_gettime(CLOCK_MONOTONIC, &t0);
    auto elapsed_ms = [](const struct timespec& a, const struct timespec& b) {
        return (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    };
    int status = 0;
    while (true) {
        int wr = waitpid(pid, &status, timeout_ms ? WNOHANG : 0);
        if (wr < 0) { if (errno == EINTR) continue; break; }
        if (wr > 0) break;
        struct timespec now{}; clock_gettime(CLOCK_MONOTONIC, &now);
        if (timeout_ms && elapsed_ms(t0, now) >= *timeout_ms) {
            kill(pid, SIGKILL);
            rr.timed_out = true;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
        struct timespec ts{0, 100 * 1000 * 1000}; // 100 ms
        nanosleep(&ts, nullptr);
    }
    if (WIFEXITED(status))       rr.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) rr.exit_code = 128 + WTERMSIG(status);
    else                          rr.exit_code = 1;
    return rr;
}

RunResult run_posix(const std::string& exe, const std::vector<std::string>& args,
                    const RunOptions& opts) {
    if (opts.redirect_output_to) {
        return run_posix_redirected(exe, args, *opts.redirect_output_to, opts.timeout_ms);
    }
    RunResult rr;
    const bool inherit_out = opts.inherit_stdout;
    const bool inherit_err = opts.inherit_stderr && !opts.merge_streams;
    int out_p[2] = {-1, -1};
    int err_p[2] = {-1, -1};
    if (!inherit_out) {
        if (pipe(out_p) != 0) throw std::runtime_error("pipe stdout");
    }
    if (!opts.merge_streams && !inherit_err) {
        if (pipe(err_p) != 0) {
            if (!inherit_out) { close(out_p[0]); close(out_p[1]); }
            throw std::runtime_error("pipe stderr");
        }
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (!inherit_out) {
        posix_spawn_file_actions_addclose(&fa, out_p[0]);
        posix_spawn_file_actions_adddup2(&fa, out_p[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&fa, out_p[1]);
    }
    // else: leave STDOUT_FILENO inherited from parent.
    if (opts.merge_streams) {
        posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);
    } else if (!inherit_err) {
        posix_spawn_file_actions_addclose(&fa, err_p[0]);
        posix_spawn_file_actions_adddup2(&fa, err_p[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&fa, err_p[1]);
    }
    // else: leave STDERR_FILENO inherited.

    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    std::string arg0 = exe;
    argv.push_back(arg0.data());
    std::vector<std::string> owned = args;
    for (auto& a : owned) argv.push_back(a.data());
    argv.push_back(nullptr);

    pid_t pid = 0;
    int rc = posix_spawnp(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (!inherit_out) close(out_p[1]);
    if (!opts.merge_streams && !inherit_err) close(err_p[1]);
    if (rc != 0) {
        if (!inherit_out) close(out_p[0]);
        if (!opts.merge_streams && !inherit_err) close(err_p[0]);
        throw std::runtime_error("posix_spawnp failed: " + exe + " (" + std::strerror(rc) + ")");
    }

    // Set non-blocking, drain non-inherited pipes via poll.
    auto set_nb = [](int fd) {
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    };
    if (!inherit_out) set_nb(out_p[0]);
    if (!opts.merge_streams && !inherit_err) set_nb(err_p[0]);

    struct pollfd fds[2];
    int nfds = 0;
    if (!inherit_out) { fds[nfds].fd = out_p[0]; fds[nfds].events = POLLIN; ++nfds; }
    if (!opts.merge_streams && !inherit_err) { fds[nfds].fd = err_p[0]; fds[nfds].events = POLLIN; ++nfds; }

    auto elapsed_ms = [](const struct timespec& a, const struct timespec& b) {
        return (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    };
    struct timespec t0{}; clock_gettime(CLOCK_MONOTONIC, &t0);

    // If we're not capturing anything, just waitpid.
    if (nfds == 0) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        if (WIFEXITED(status))       rr.exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) rr.exit_code = 128 + WTERMSIG(status);
        else                          rr.exit_code = 1;
        return rr;
    }

    bool alive_out = !inherit_out;
    bool alive_err = !opts.merge_streams && !inherit_err;
    while (alive_out || alive_err) {
        int slice = 200;
        if (opts.timeout_ms) {
            struct timespec now{}; clock_gettime(CLOCK_MONOTONIC, &now);
            auto used = elapsed_ms(t0, now);
            if (used >= *opts.timeout_ms) { rr.timed_out = true; break; }
            slice = std::min(slice, static_cast<int>(*opts.timeout_ms - used));
        }
        int pr = poll(fds, nfds, slice);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        auto drain = [&](int fd, std::string& sink) {
            char buf[4096];
            while (true) {
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n > 0) sink.append(buf, static_cast<size_t>(n));
                else if (n == 0) return false; // eof
                else if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
                else return false;
            }
        };
        // Map fds[] indices to which stream they represent (out first if used).
        int idx = 0;
        if (alive_out) {
            if (fds[idx].revents & (POLLIN | POLLHUP))
                alive_out = drain(out_p[0], rr.stdout_text);
            ++idx;
        }
        if (alive_err) {
            if (fds[idx].revents & (POLLIN | POLLHUP))
                alive_err = drain(err_p[0], rr.stderr_text);
        }
        // If child has exited AND both pipes are drained/closed, waitpid will
        // reap it next; the loop terminates naturally when alive_* become false.
    }

    if (rr.timed_out) kill(pid, SIGKILL);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!inherit_out) close(out_p[0]);
    if (!opts.merge_streams && !inherit_err) close(err_p[0]);

    if (WIFEXITED(status))       rr.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) rr.exit_code = 128 + WTERMSIG(status);
    else                          rr.exit_code = 1;
    return rr;
}
#endif

} // namespace

RunResult run(const std::string& exe,
              const std::vector<std::string>& args,
              const RunOptions& opts) {
#ifdef _WIN32
    return run_win(exe, args, opts);
#else
    return run_posix(exe, args, opts);
#endif
}

RunResult check_run(const std::string& exe,
                    const std::vector<std::string>& args,
                    const RunOptions& opts) {
    RunResult r = run(exe, args, opts);
    if (r.exit_code != 0) {
        throw std::runtime_error(exe + " exited " + std::to_string(r.exit_code) +
                                 (r.stderr_text.empty() ? "" : ": " + r.stderr_text));
    }
    return r;
}

} // namespace crux::proc
