# crux

<img src="dashboard/logo.svg" width="72" height="72" align="right" alt="">

[![CI](https://github.com/OWNER/crux/actions/workflows/ci.yml/badge.svg)](https://github.com/OWNER/crux/actions/workflows/ci.yml)

Crux pulls the most-replayed part out of a long video and turns it into a
short vertical clip you can drop into Shorts, Reels, or TikTok.

It works by reading YouTube's public "Most replayed" heatmap — the same
graph you see on the seek bar — finding where people rewound, and cutting
those moments into 1080×1920 MP4s with a blurred background so the source
frame isn't stretched or letterboxed.

You get a small C++ CLI (`crux`) and a local web dashboard
(`crux_server`) that wraps it. No accounts, no cloud, nothing runs off
your machine.



---

## Contents

- [What you need](#what-you-need)
- [Windows quickstart](#windows-quickstart)
- [Linux quickstart](#linux-quickstart)
- [Dashboard](#dashboard)
- [CLI](#cli)
- [How it works](#how-it-works)
- [Project layout](#project-layout)
- [Development](#development)
- [Legal](#legal)
- [License](#license)

---

## What you need

- **CMake ≥ 3.25** — https://cmake.org/download/ (tick *Add to PATH*)
- A C++20 compiler
  - Windows: **Visual Studio 2022** with the *Desktop development with C++*
    workload — https://visualstudio.microsoft.com/downloads/
  - Linux: `g++ 11+` or `clang++ 14+`
- **Git** — CMake uses it to fetch header-only deps
  (Windows: https://git-scm.com/download/win)

The C++ deps (`CLI11`, `nlohmann/json`, `spdlog`, `doctest`, `cpp-httplib`)
are pulled by CMake automatically. `yt-dlp` and `ffmpeg` are fetched by
the setup script.

---

## Windows quickstart

```
git clone <your-fork-url> crux
cd crux
.\setup.bat
```

`setup.bat` checks your tools, downloads `yt-dlp.exe` and a static
`ffmpeg` build into `third_party\bin\`, configures a Visual Studio 2022
x64 build tree, builds Release, and runs the tests as a smoke check.

Then:

```
:: plan only — prints the sparkline and clip table, no download
.\run.bat "https://www.youtube.com/watch?v=iIY9fPgY5wM" --dry-run --dump-heatmap

:: full run — 3 clips, 9:16 with blurred bars
.\run.bat "iIY9fPgY5wM" --max-clips 3 --format 916blur

:: or use the dashboard
.\dashboard.bat
```

After editing code:

```
.\rebuild.bat            :: incremental Release
.\rebuild.bat Debug
.\rebuild.bat --clean    :: wipe build\ first
```

---

## Linux quickstart

```
git clone <your-fork-url> crux
cd crux

mkdir -p third_party/bin
curl -L -o third_party/bin/yt-dlp \
    https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp
chmod +x third_party/bin/yt-dlp
# ffmpeg: install via your package manager, or drop a static build in third_party/bin/

cmake -S . -B build
cmake --build build -j
./build/crux --version
```

Set `VCPKG_ROOT` if you'd rather resolve deps through vcpkg; the CMake
config picks it up automatically and uses the `vcpkg.json` manifest.

---

## Dashboard

Run `.\dashboard.bat` (or `./build/crux_server` on Linux). It starts a
local HTTP server on `http://127.0.0.1:8181/` and opens your browser.

Paste a URL, pick the parameters, hit **Run**. The **Log** tab streams
the same `[step/total]` output the CLI prints. The **Results** tab shows
the heatmap sparkline with the planned clip windows highlighted, and one
card per clip with an inline `<video>` preview and a download link.

Every run lives under `out\web-<timestamp>\` and shows up in the sidebar
so you can jump back to it later.

If you want to script the server, the endpoints are:

| Endpoint | Purpose |
|----------|---------|
| `POST /api/run`                       | Start a job. JSON body → `{job_id, out_dir}` |
| `GET  /api/jobs`                       | List recent jobs |
| `GET  /api/jobs/:id`                   | Job metadata + status |
| `GET  /api/jobs/:id/log?from=N`        | Poll log bytes since offset N |
| `GET  /api/jobs/:id/manifest`          | `manifest.json` contents |
| `GET  /api/jobs/:id/heatmap`           | `heatmap.json` contents |
| `GET  /api/jobs/:id/file/<name>`       | Serves clip mp4s + other outputs |

---

## CLI

```
crux <url|video-id> [options]

Selection
  -n, --max-clips N              default 6, cap 10
  -l, --clip-len SEC             default auto = clamp(1.5·binS, 40, 90); cap 180
      --min-gap SEC              default auto = max(90, 2·binS)
      --keep-intro               keep bin 0/1 in candidacy
      --strict                   exit 6 on flat heatmap

Output
  -o, --out DIR                  default ./out/<id>
      --format MODE              916blur (default) | 916crop | orig
      --dry-run                  plan only, no download
      --dump-heatmap             also write heatmap.json + ASCII sparkline
      --json                     machine-readable stdout
  -v, --verbose                  debug logging

Fetch
      --source MODE              ytdlp (default) | native  (native is a stub)
      --cookies-from-browser NAME
                                 chrome | firefox | edge | brave …
      --full-download            skip section mode, download the whole video

Binaries
      --ytdlp   PATH             override yt-dlp lookup
      --ffmpeg  PATH             override ffmpeg lookup

  -h, --help                     show help
      --version                  print version
```

### Exit codes

| Code | Meaning |
|------|---------|
| 0 | ok |
| 2 | no heatmap available for this video (about 11% of high-view videos) |
| 3 | metadata fetch failed / live or upcoming video |
| 4 | download failed (see stderr for a retry hint) |
| 5 | ffmpeg cut failed |
| 6 | flat heatmap and `--strict` was set |

---

## How it works

```
                 ┌── crux ─────────────────────────────────────────────┐
  URL ──► fetch ─┤ HeatmapSource   yt-dlp --dump-single-json           │
                 │        │ heatmap[100], duration, title, chapters    │
                 │        ▼                                            │
                 │ SpikeDetector   regions[] (grouped hot bins)        │
                 │        ▼                                            │
                 │ ClipPlanner     clips[]  {start,end,score,label}    │
                 │        ▼                                            │
                 │ MediaEngine     yt-dlp ► ffmpeg cut + 9:16 blur     │
                 │        ▼                                            │
                 │ out/<id>/clip_NN_hhmmss.mp4 + manifest.json         │
                 └─────────────────────────────────────────────────────┘
```

Detection and planning are pure functions in `src/core/`, tested against
five real-video fixtures. The I/O-shaped bits (fetch, download, cut,
silence-snap) live under `src/fetch/`, `src/media/`, and `src/out/`.
`PLAN.md` §4 has the algorithm and the constants; every threshold comes
from a measurement in `FINDINGS.md`.

---

## Project layout

```
crux/
├─ CMakeLists.txt          Top-level build (FetchContent + vcpkg-friendly)
├─ vcpkg.json              Manifest for vcpkg install
├─ setup.bat               First-run installer (Windows)
├─ rebuild.bat             Incremental rebuild (Windows)
├─ run.bat                 Wrapper for crux.exe (Windows)
├─ dashboard.bat           Launches the local web dashboard (Windows)
├─ src/
│  ├─ main.cpp             CLI entry point
│  ├─ cli.{h,cpp}          CLI11 parsing → Config
│  ├─ config.h             Runtime configuration record
│  ├─ pipeline.{h,cpp}     Orchestrates fetch → plan → media → manifest
│  ├─ binres.{h,cpp}       yt-dlp/ffmpeg lookup: flag → env → bundled → PATH
│  ├─ core/                Pure-function core (unit tested)
│  │   ├─ heatmap.h        Data types
│  │   ├─ detector.{h,cpp} Spike detection
│  │   └─ planner.{h,cpp}  Clip selection + windowing
│  ├─ fetch/
│  │   ├─ source.h         IHeatmapSource interface
│  │   ├─ ytdlp_source.cpp yt-dlp subprocess backend
│  │   └─ parse.{h,cpp}    yt-dlp JSON → data types
│  ├─ media/
│  │   ├─ proc.{h,cpp}     Cross-platform subprocess wrapper
│  │   ├─ downloader.{h,cpp}   yt-dlp download + full-download fallback
│  │   ├─ cutter.{h,cpp}   ffmpeg cut + 916blur/crop/orig filter graphs
│  │   └─ silence.{h,cpp}  ffmpeg silencedetect wrapper
│  ├─ out/
│  │   └─ manifest.{h,cpp} JSON manifest + heatmap sparkline
│  └─ server/
│      └─ server.cpp       Local dashboard server (cpp-httplib)
├─ dashboard/
│  ├─ index.html           Single-page UI (no build step, no framework)
│  ├─ logo.svg
│  └─ favicon.svg
├─ tests/
│  ├─ parse_test.cpp
│  ├─ detector_test.cpp
│  ├─ planner_test.cpp
│  ├─ standalone_verify.cpp    Zero-dependency verifier
│  └─ fixtures/                5 heatmap fixtures + a sample yt-dlp JSON
├─ third_party/bin/        yt-dlp + ffmpeg binaries (gitignored)
├─ out/                    Generated clips + manifests (gitignored)
├─ PLAN.md
├─ FINDINGS.md
├─ LICENSE
└─ README.md
```

---

## Development

Run the tests:

### One-command test run

```
.\test-all.bat
```

Runs, in order:

1. **Standalone verifier** — 13 pure-function cases (detector + planner)
   covering all 5 FINDINGS fixtures plus edge shapes (all-zero, all-max,
   single-bin spike, adjacent tied peaks, hysteresis merge, `--keep-intro`,
   end-of-video clamp, coarse-bin smoothing skip). No external deps.
2. **Unit tests** (`crux_tests.exe`, ~60 asserts across 9 suites):
   - `parse_test` + `parse_edge_test` — yt-dlp JSON parsing, fuzz, malformed
     inputs, `is_live` detection, chapter shapes.
   - `detector_test` + `detector_extra_test` — spike detection against
     fixtures and edge cases.
   - `planner_test` — clip selection, min-gap, windowing, short-video path.
   - `cli_test` — table-driven CLI parsing (every flag, every enum, bounds).
   - `manifest_test` — JSON schema contract + sparkline shape (dashboard
     depends on this).
   - `proc_test` — subprocess wrapper: capture, non-zero exit, redirect to
     file, timeout supervision, unknown binary.
3. **Executable smoke** (`tests/e2e/smoke.bat`) — `crux --version`, `--help`,
   missing-arg behaviour, bogus URL clean-fail.
4. **Dashboard server smoke** (`tests/e2e/server_smoke.bat`) — starts
   `crux_server` on port 18181, curls `/api/info`, `/`, `/logo.svg`,
   `/api/jobs`, verifies `POST /api/run` with no URL → 404, kills the
   server.

Any layer failing → non-zero exit for CI.

### Running individual layers

```
:: unit tests only
.\build\Release\crux_tests.exe

:: zero-dependency standalone verifier (no cmake, no deps)
g++ -std=c++20 -Isrc tests/standalone_verify.cpp \
    src/core/detector.cpp src/core/planner.cpp -o verify && ./verify
```

Expected: `=== ALL PASSED (0 failures) ===`.

### Continuous integration

`.github/workflows/ci.yml` runs the same four layers on both
**ubuntu-latest** and **windows-latest** for every push and pull request.
Steps:

1. Show tool versions
2. Cache `build/_deps` (CLI11, nlohmann-json, spdlog, doctest, cpp-httplib)
3. Configure + build Release
4. `ctest` (unit tests + standalone verifier)
5. Executable smoke — `--version`, `--help`, missing/bogus URL
6. Dashboard server smoke — start `crux_server`, curl 6 endpoints, kill

Release binaries are uploaded as workflow artifacts on green builds.

To make the badge above render, replace `OWNER` in the README with your
GitHub username or org after the first push.

### Adding a fixture

Append the video to the table in `FINDINGS.md` §2 and
paste the scores array into `tests/fixtures/heatmap_scores.json`. Get
the array with:

```
yt-dlp --skip-download --dump-single-json "https://youtu.be/<id>" | jq '.heatmap'
```

There's a `.clang-format` at the repo root; run `clang-format -i` on
files you touch.

If the tool's behavior drifts from `PLAN.md`, trust the measured
reality, update `FINDINGS.md`, and note the deviation in the commit.
The defaults in PLAN.md §4 are not constants — they all live in the
`Config` struct.

---

## Legal

Downloading videos from YouTube can violate YouTube's Terms of Service,
and re-uploading someone else's clips can get you copyright-struck.
Fair use isn't automatic. Use crux on content you own or have permission
to use; anything else is on you.

The tool itself only reads a public YouTube field ("Most replayed") and
slices already-public streams.

---

## License

MIT — see [`LICENSE`](LICENSE). Bundled binaries (yt-dlp, ffmpeg) and
libraries keep their own licenses; the LICENSE file lists them.
