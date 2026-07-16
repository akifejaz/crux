# ytshorts

**Turn any YouTube video into ready-to-upload Shorts by mining the public
"Most replayed" heatmap.**

Given a URL, ytshorts reads the 100-bin heatmap YouTube exposes on eligible
videos, detects the spike regions viewers actually rewatched, downloads only
what it needs, and cuts each spike into a 1080×1920 vertical MP4 with a
blurred background — plus a `manifest.json` describing what came from where.

Cross-platform C++20 CLI plus an optional local web dashboard. See
[`PLAN.md`](PLAN.md) for the full design and [`FINDINGS.md`](FINDINGS.md) for
the empirical basis (18 videos measured live, 5 fixtures wired into unit
tests).

---

## Table of contents

- [Status](#status)
- [Prerequisites](#prerequisites)
- [Quickstart (Windows)](#quickstart-windows)
- [Quickstart (Linux)](#quickstart-linux)
- [Dashboard](#dashboard)
- [CLI reference](#cli-reference)
- [How it works](#how-it-works)
- [Project layout](#project-layout)
- [Development](#development)
- [Legal](#legal)
- [License](#license)

---

## Status

| Milestone | State |
|-----------|-------|
| M0  Skeleton, CLI, binary resolution, subprocess wrapper                    | done |
| M1  yt-dlp heatmap fetch + JSON parser + `--dump-heatmap`                   | done |
| M2  Detector, planner, `--dry-run`, `manifest.json`                         | done — 5 fixtures wired into tests |
| M3  Download + cut + 9:16 blur/crop/orig                                    | done, end-to-end verified |
| M4  Section downloads + auto-fallback to full + silence-snap                | done |
| M5  Native libcurl heatmap fallback + release packaging                     | stub — v2 hardening |
| +    Local web dashboard                                                    | done |

The M0-M4 core is a working MVP. Standalone unit tests cover the detection
and planning algorithm against all 5 measured fixtures plus 3 synthetic edge
cases — see [Development](#development) for how to run them.

---

## Prerequisites

Install these once system-wide:

- **CMake ≥ 3.25** — https://cmake.org/download/ (tick *Add to PATH*)
- **A C++20 compiler**
  - Windows: **Visual Studio 2022** with the *"Desktop development with C++"* workload — https://visualstudio.microsoft.com/downloads/
  - Linux: `g++ 11+` or `clang++ 14+`
- **Git** — needed by CMake FetchContent to pull the header-only deps
  - Windows: https://git-scm.com/download/win

Everything else (`CLI11`, `nlohmann/json`, `spdlog`, `doctest`, `cpp-httplib`)
is fetched by CMake automatically the first time you configure. `yt-dlp.exe`
and `ffmpeg.exe` are downloaded by the setup script (see below).

---

## Quickstart (Windows)

```
git clone <your-fork-url> yt-shorts
cd yt-shorts
.\setup.bat
```

`setup.bat` does everything the first time:

1. Verifies `cmake` and `git` are on PATH.
2. Downloads `yt-dlp.exe` (GitHub release) and a static `ffmpeg` build
   (gyan.dev) into `third_party\bin\`.
3. Configures a Visual Studio 2022 x64 build tree.
4. Builds Release (`ytshorts.exe`, `ytshorts_server.exe`, `ytshorts_tests.exe`).
5. Runs `--version` and the unit test suite as a smoke check.

Then run a job:

```
:: dry-run — plan only, prints the sparkline and clip table
.\run.bat "https://www.youtube.com/watch?v=iIY9fPgY5wM" --dry-run --dump-heatmap

:: full run — downloads and cuts 3 clips
.\run.bat "iIY9fPgY5wM" --max-clips 3 --format 916blur

:: launch the local dashboard (URL + selectors + live progress + video previews)
.\dashboard.bat
```

Rebuild after code edits:

```
.\rebuild.bat            :: incremental Release
.\rebuild.bat Debug
.\rebuild.bat --clean    :: nuke build\ first
```

---

## Quickstart (Linux)

```
git clone <your-fork-url> yt-shorts
cd yt-shorts

# Fetch bundled binaries (or install yt-dlp/ffmpeg via your package manager)
mkdir -p third_party/bin
curl -L -o third_party/bin/yt-dlp \
    https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp
chmod +x third_party/bin/yt-dlp
# ffmpeg: install from your distro or drop a static build in third_party/bin/

cmake -S . -B build
cmake --build build -j
./build/ytshorts --version
```

Or, if `VCPKG_ROOT` is set in your environment, the CMake config will
automatically use the vcpkg toolchain and the deps declared in `vcpkg.json`.

---

## Dashboard

`.\dashboard.bat` (or `./build/ytshorts_server` on Linux) starts a tiny local
HTTP server on `http://127.0.0.1:8181/` and opens your browser. Paste a URL,
pick parameters with the selectors, hit **Run**.

- The **Log** tab streams the same `[step/total]` output you get from the
  CLI, with color highlights for warnings and errors.
- The **Results** tab shows the video metadata, a heatmap sparkline SVG
  with the planned clip windows highlighted in amber, and one card per
  clip with an inline `<video>` preview and a download link.
- Every run is persisted under `out\web-<timestamp>\` and appears in the
  sidebar so you can revisit it later.

Endpoints exposed by the server (useful if you want to script it):

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

## CLI reference

```
ytshorts <url|video-id> [options]

Selection
  -n, --max-clips N              default 6, cap 10
  -l, --clip-len SEC             default auto = clamp(1.5·binS, 40, 90); cap 180
      --min-gap SEC              default auto = max(90, 2·binS)
      --keep-intro               keep bin 0/1 in candidacy
      --strict                   exit 6 on flat heatmap

Output
  -o, --out DIR                  default ./out/<id>
      --format MODE              916blur (default) | 916crop | orig
      --dry-run                  plan only (writes manifest.json, no download)
      --dump-heatmap             also write heatmap.json + ASCII sparkline
      --json                     machine-readable stdout on success
  -v, --verbose                  debug logging

Fetch
      --source MODE              ytdlp (default) | native (M5, stub for now)
      --cookies-from-browser NAME
                                 chrome | firefox | edge | brave …
      --full-download            skip section mode, download whole video

Binaries
      --ytdlp   PATH             override yt-dlp lookup
      --ffmpeg  PATH             override ffmpeg lookup

  -h, --help                     show help
      --version                  print version and exit
```

### Exit codes

| Code | Meaning |
|------|---------|
| 0 | ok |
| 2 | no heatmap available for this video (~11% of high-view videos, per FINDINGS) |
| 3 | metadata fetch failed / live or upcoming video |
| 4 | download failed (see stderr for the retry hint) |
| 5 | ffmpeg cut failed |
| 6 | flat heatmap and `--strict` was set |

---

## How it works

```
                ┌── C++ core (single binary: ytshorts) ────────────────┐
 URL ──► fetch ─┤ HeatmapSource (yt-dlp --dump-single-json)            │
                │        │ heatmap[100], duration, title, chapters, id │
                │        ▼                                             │
                │ SpikeDetector  ── regions[] (grouped hot bins)       │
                │        ▼                                             │
                │ ClipPlanner    ── clips[] {start,end,score,label}    │
                │        ▼                                             │
                │ MediaEngine    ── yt-dlp ► ffmpeg cut + 9:16 blur    │
                │        ▼                                             │
                │ out/<id>/clip_NN_hhmmss.mp4 + manifest.json          │
                └──────────────────────────────────────────────────────┘
```

Detection and planning are **pure functions** in `src/core/`, unit-tested
against the five FINDINGS fixtures. Fetch, download, cut, and silence-snap
are I/O-shaped and live in `src/fetch/`, `src/media/`, and `src/out/`. See
[`PLAN.md`](PLAN.md) §4 for the algorithm and defaults; every constant was
derived from measured video data documented in [`FINDINGS.md`](FINDINGS.md).

---

## Project layout

```
yt-shorts/
├─ CMakeLists.txt          Top-level build (FetchContent + vcpkg-friendly)
├─ vcpkg.json              Manifest for `vcpkg install`
├─ setup.bat               First-run installer (Windows)
├─ rebuild.bat             Incremental rebuild (Windows)
├─ run.bat                 Wrapper for ytshorts.exe (Windows)
├─ dashboard.bat           Launches the local web dashboard (Windows)
├─ src/
│  ├─ main.cpp             CLI entry point
│  ├─ cli.{h,cpp}          CLI11 parsing → Config
│  ├─ config.h             Runtime configuration record
│  ├─ pipeline.{h,cpp}     Orchestrates fetch → plan → media → manifest
│  ├─ binres.{h,cpp}       yt-dlp/ffmpeg resolution: flag → env → bundled → PATH
│  ├─ core/                Pure-function core (unit tested)
│  │   ├─ heatmap.h        Data types
│  │   ├─ detector.{h,cpp} Spike detection (PLAN §4 steps 1-7)
│  │   └─ planner.{h,cpp}  Clip selection + windowing (steps 8-11)
│  ├─ fetch/
│  │   ├─ source.h         IHeatmapSource interface
│  │   ├─ ytdlp_source.cpp yt-dlp subprocess backend (M1)
│  │   └─ parse.{h,cpp}    yt-dlp JSON → data types (unit tested)
│  ├─ media/
│  │   ├─ proc.{h,cpp}     Cross-platform subprocess wrapper
│  │   ├─ downloader.{h,cpp}   yt-dlp download + auto full-fallback
│  │   ├─ cutter.{h,cpp}   ffmpeg cut + 916blur/crop/orig filter graphs
│  │   └─ silence.{h,cpp}  ffmpeg silencedetect wrapper (M4)
│  ├─ out/
│  │   └─ manifest.{h,cpp} JSON manifest + heatmap sparkline
│  └─ server/
│      └─ server.cpp       cpp-httplib local dashboard server
├─ dashboard/
│  └─ index.html           Single-page UI (no build step, no framework)
├─ tests/
│  ├─ main_test.cpp        doctest entry
│  ├─ parse_test.cpp       yt-dlp JSON parsing
│  ├─ detector_test.cpp    Spike detection against FINDINGS fixtures
│  ├─ planner_test.cpp     Clip planning
│  ├─ standalone_verify.cpp    Zero-dependency verifier for CI sandboxes
│  └─ fixtures/            5 heatmap fixtures + sample yt-dlp JSON
├─ third_party/bin/        yt-dlp + ffmpeg binaries (gitignored)
├─ out/                    Generated clips + manifests (gitignored)
├─ PLAN.md                 Full design document
├─ FINDINGS.md             Empirical study that grounds the design
├─ LICENSE                 MIT
└─ README.md               This file
```

---

## Development

### Running the tests

```
:: Windows: setup.bat runs them automatically. To re-run:
.\build\Release\ytshorts_tests.exe

:: Linux
./build/ytshorts_tests

:: Zero-dependency verifier (works with plain g++, no CMake / no deps):
g++ -std=c++20 -Isrc tests/standalone_verify.cpp \
    src/core/detector.cpp src/core/planner.cpp -o verify && ./verify
```

Expected: `=== ALL PASSED (0 failures) ===`.

### Regenerating fixtures

Add a video to the dataset in `FINDINGS.md` §2, then update the
corresponding `scores` array in `tests/fixtures/heatmap_scores.json`:

```
yt-dlp --skip-download --dump-single-json "https://youtu.be/<id>" | jq '.heatmap'
```

### Formatting

`.clang-format` sits at the repo root; run `clang-format -i` on any file
you touch.

### Deviating from PLAN.md

When behavior differs from the plan, trust measured reality, update
`FINDINGS.md`, and note the deviation in the commit message. Numbers in
PLAN.md §4 are defaults, not constants — every threshold lives in one
`Config` struct.

---

## Legal

Downloading videos from YouTube may violate YouTube's Terms of Service.
Re-uploading third-party clips risks copyright strikes and demonetization;
fair use is case-by-case, never automatic. This tool is safest on channels
you own or have licensed content from. **The maintainers do not encourage or
condone unauthorized use of copyrighted material.**

The tool itself is neutral — it exposes a public YouTube data field
(the "Most replayed" heatmap) and slices already-public video streams. It
is the user's responsibility to comply with local laws, YouTube's terms,
and third-party rights.

---

## License

MIT — see [`LICENSE`](LICENSE). Bundled third-party binaries and libraries
retain their own licenses; the LICENSE file lists them.
