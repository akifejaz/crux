# PLAN — `ytshorts`: Most-Replayed → YouTube Shorts Extractor (C++)

Implementation plan for Claude Opus. Evidence base: `FINDINGS.md` (read it first — every design
decision below cites a measured fact from it). Scope decisions confirmed by the owner:
**MVP = CLI tool · hybrid fetch (yt-dlp subprocess) · 9:16 blurred-bars output · local, C++ core.**

## 0. One-paragraph summary

Given a YouTube URL, read the public "Most replayed" heatmap (100 bins, `intensityScoreNormalized`
0–1, bin width = duration/100), detect spike regions, plan 30–90 s clips centered on each spike
(clip length auto-scales with bin width), download only the needed sections, cut and convert each
to a 1080×1920 blurred-background vertical MP4 ready for Shorts upload, plus a `manifest.json`.
All logic is C++; the only external processes are two bundled self-contained binaries: `yt-dlp`
(fetch/download) and `ffmpeg` (cut/convert).

## 1. Verified ground truth (do not re-litigate; see FINDINGS.md)

1. Heatmap = exactly 100 bins; bin duration = videoLengthSec/100 (12 s @ 20 min, 81–140 s @ 2–4 h).
2. Source of truth: watch-page `ytInitialData → frameworkUpdates…macroMarkersListEntity` with
   `markerType == "MARKER_TYPE_HEATMAP"`. yt-dlp surfaces it as `heatmap: [{start_time,end_time,value}]`.
3. Official Data API does not expose it. ~11% of tested high-view videos had NO heatmap → first-class error path.
4. Bin 0 (and often bin 1) is an intro artifact on long-form → excluded from candidacy by default.
5. Peaks form contiguous regions → region grouping required, not top-K bins.
6. Music/flat profiles (max/median < 2) have no distinct moments → warn/skip gate.
7. Shorts 2026: ≤ 3 min, square-or-taller; target 1080×1920 ≤ 60 s by default (config up to 180 s).
8. `--download-sections` avoids full downloads; bot checks mitigated with `--cookies-from-browser`.

## 2. Architecture

```
                ┌──────────────────────── C++ core (single binary: ytshorts) ───────────────────────┐
 URL ──► fetch ─┤ HeatmapSource (yt-dlp --dump-single-json │ M5: native libcurl+scanner)             │
                │        │ heatmap[100], duration, title, chapters, id                               │
                │        ▼                                                                           │
                │ SpikeDetector  ── regions[] (grouped hot bins, scored, deduped)                    │
                │        ▼                                                                           │
                │ ClipPlanner    ── clips[] {start,end,score,label}  (± context, silence-snap M4)    │
                │        ▼                                                                           │
                │ MediaEngine    ── yt-dlp --download-sections ► ffmpeg cut + 9:16 blur re-encode    │
                │        ▼                                                                           │
                │ out/<id>/clip_NN_mmss.mp4 … + manifest.json + heatmap.json                         │
                └─────────────────────────────────────────────────────────────────────────────────────┘
```

Subprocess boundary only (no Python bindings, no wrappers): `yt-dlp.exe` / `yt-dlp_linux` are
PyInstaller-standalone builds (no Python install required); `ffmpeg` is a static build. Both live in
`third_party/bin/` and are resolved in order: `--ytdlp/--ffmpeg` flag → env var → bundled → PATH.

## 3. Data contracts

### 3.1 Input from yt-dlp (subset we parse)
```json
{ "id": "hBMoPUAeLnY", "title": "...", "duration": 10730, "channel": "...", "webpage_url": "...",
  "chapters": [{"start_time": 0.0, "end_time": 123.0, "title": "Intro"}] ,
  "heatmap":  [{"start_time": 0.0, "end_time": 107.3, "value": 1.0}]  }
```
`heatmap` may be `null`/absent (FINDINGS §3.7). `chapters` often null. Command:
`yt-dlp --skip-download --dump-single-json --no-warnings [--cookies-from-browser X] URL`

### 3.2 Native fallback (M5, feature-parity for heatmap only)
GET watch page (libcurl, desktop UA, cookies optional) → linear scan for
`"startMillis":"(\d+)","durationMillis":"(\d+)","intensityScoreNormalized":(float)` within the
`macroMarkersListEntity` block whose `markerType` is `MARKER_TYPE_HEATMAP`; `videoDetails` block for
id/title/lengthSeconds. Validated live on 16 videos (FINDINGS §1). Hand-rolled scanner — do NOT use
std::regex on 1.7 MB HTML.

### 3.3 Output manifest.json
```json
{ "video": {"id":"","title":"","channel":"","duration":0,"url":""},
  "params": { "...effective config..." },
  "heatmapPresent": true,
  "quality": {"maxOverMedian": 8.1, "flat": false},
  "clips": [ { "index":1, "start":745.2, "end":805.2, "duration":60.0,
               "peakBin":7, "peakScore":0.924, "centroid":805.0,
               "label":"chapter title if any", "file":"clip_01_1325.mp4",
               "snapped": {"start":true,"end":false} } ] }
```
Exit codes: 0 ok · 2 no heatmap · 3 metadata fetch failed · 4 download failed · 5 ffmpeg failed · 6 flat heatmap (with `--strict`).

## 4. Spike detection & clip planning (the core algorithm)

Input: `H[0..99]`, `binS = duration/100`, config. All defaults below are derived from FINDINGS §2–3.

```
1  CLEAN     drop bin 0 from candidacy; drop bin 1 iff H[1] > H[2] > H[3] (monotone intro decay,
             e.g. Lex/Rick profile). (--keep-intro disables)
2  SMOOTH    if binS < 60: H'[i] = (H[i-1] + 2·H[i] + H[i+1]) / 4  (skip when bins already coarse)
3  STATS     med = median(H'[1..99]), sd = stddev, mx = max
4  GATE      if mx/med < 2.0 → flat profile (music): warn; exit 6 if --strict   (FINDINGS §3.6)
5  THRESH    T = max(med + 1.5·sd, 0.4·mx)
6  REGIONS   contiguous runs with H' ≥ T; hysteresis-extend edges while H' ≥ 0.6·T;
             merge regions separated by ≤ 1 bin
7  SCORE     region.peak = max H'; region.centroid = Σ(t_i·H'_i)/ΣH'_i (bin centers)
8  SELECT    sort by peak desc; greedily keep regions whose centroid is ≥ minGap from all kept
             (minGap = max(90 s, 2·binS)); keep top K (--max-clips, default 6)
9  WINDOW    L = clamp(--clip-len | auto, 15, 180); auto = clamp(1.5·binS, 40, 90)
             clip = [centroid − L/2, centroid + L/2]; if region wider than L, keep centered on centroid;
             clamp to [0, duration] by SHIFTING (not shrinking)
10 SNAP (M4) refine both cut points to nearest silence boundary within ±5 s using
             ffmpeg silencedetect on a locally downloaded region (podcasts: don't cut mid-word)
11 EMIT      ordered by start time; write plan; --dry-run stops here
```

Worked example (measured data, JRE #2219 Trump, binS = 107.3 s, mean .152, sd .144; using mean as
median proxy — median not captured in-session): T = max(.152 + 1.5·.144, 0.4·.924) ≈ .37 →
bins above T: 7 (.92), 13 (.83), 99 (.51), 58 (.48), 75 (.40); next candidates (.34) stay below →
5 clips: 13:25, 24:09, 1:44:37, 2:15:01, end — auto L = 90 s. Sanity-matches the visible player graph.
Short-video check (Karate Kid, binS 2.4 s): single region 71–78, centroid ≈ 3:03 → one 40 s clip
≈ 2:43–3:23, not 8 duplicate clips.

Rationale for scaled clip length: on a 3 h podcast the peak position is only known to ± 70 s
(FINDINGS §3.2), so a 30 s clip would miss the moment ~50% of the time. 90 s + silence-snap is the
honest MVP; transcript-based tightening is a v2 item (§12).

## 5. Media pipeline (exact commands)

Download only what's needed (default when Σclip < 25% of video, else full download once):
```
yt-dlp -f "bv*[height<=1080][ext=mp4]+ba[ext=m4a]/b[ext=mp4]" \
       --download-sections "*{start-10}-{end+10}" --force-overwrites \
       [--cookies-from-browser chrome] -o "work/{id}_{n}.%(ext)s" URL
```
(±10 s pad because section downloads are keyframe-bounded; exact trim happens in our re-encode.)

Cut + vertical 1080×1920 blurred-bars (re-encode = frame-accurate cuts; `-ss/-to` before `-i` = fast seek):
```
ffmpeg -y -ss {S} -to {E} -i work/in.mp4 -filter_complex \
 "[0:v]split=2[bg][fg];[bg]scale=1080:1920:force_original_aspect_ratio=increase,crop=1080:1920,gblur=sigma=24[b];\
  [fg]scale=1080:-2[f];[b][f]overlay=(W-w)/2:(H-h)/2" \
 -c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p -c:a aac -b:a 160k -movflags +faststart out.mp4
```
`--format 916crop` variant: `scale=-2:1920,crop=1080:1920` (center crop). `orig` variant: stream-window
re-encode without filters. Silence-snap (M4): run `ffmpeg -i seg.m4a -af silencedetect=noise=-35dB:d=0.35 -f null -`
on ±5 s around each planned cut, parse `silence_start/_end` from stderr, move cut to nearest boundary.

## 6. CLI spec

```
ytshorts <url|video-id> [options]
  -o, --out DIR            output dir (default ./out/<id>)
  -n, --max-clips N        default 6, max 10
  -l, --clip-len SEC       default auto (§4.9); hard cap 180
      --min-gap SEC        default auto max(90, 2·binS)
      --format MODE        916blur (default) | 916crop | orig
      --cookies-from-browser NAME   passthrough to yt-dlp
      --ytdlp PATH / --ffmpeg PATH
      --dry-run            plan only (no download); prints table + writes manifest
      --dump-heatmap       write heatmap.json and ASCII sparkline to stdout
      --json               machine-readable stdout
      --keep-intro / --strict / --full-download / -v
```

## 7. Tech stack & repo layout

Language: C++20. Build: CMake ≥ 3.25 + vcpkg manifest. Cross-platform Windows + Linux from day one.
Deps (vcpkg): `cli11`, `nlohmann-json`, `spdlog`, `reproc` (subprocess), `doctest`; `curl` added in M5 only.
No Python anywhere in the runtime path; yt-dlp/ffmpeg are standalone executables (a `tools/get-bins`
script downloads pinned releases into `third_party/bin/`; pin + checksum both).

```
yt-shorts/
├─ CMakeLists.txt  vcpkg.json  .clang-format
├─ src/  main.cpp cli.{h,cpp} pipeline.{h,cpp}
│        fetch/ ytdlp_source.{h,cpp} [M5: native_source.{h,cpp}]  (both implement IHeatmapSource)
│        core/  heatmap.h detector.{h,cpp} planner.{h,cpp}        (pure functions, no I/O — unit-testable)
│        media/ downloader.{h,cpp} cutter.{h,cpp} silence.{h,cpp} proc.{h,cpp}
│        out/   manifest.{h,cpp}
├─ tests/ detector_test.cpp planner_test.cpp parse_test.cpp fixtures/*.json  ← seed from FINDINGS.md §4
├─ third_party/bin/ (gitignored)  tools/get-bins.{ps1,sh}
└─ PLAN.md FINDINGS.md README.md
```

## 8. Milestones (each ends green-build + tests passing)

| M | Deliverable | Acceptance criteria |
|---|---|---|
| M0 | Skeleton: CMake+vcpkg, CLI parsing, bin resolution, `proc` wrapper | builds Win+Linux; `ytshorts --version` runs yt-dlp/ffmpeg `-version` |
| M1 | Heatmap fetch + parse (`ytdlp_source`), `--dump-heatmap`, fixtures | 5 FINDINGS fixtures parse bit-exact; absent-heatmap → exit 2 with clear message |
| M2 | Detector + planner + `--dry-run` + manifest | unit tests: Trump fixture → 5 regions incl. bins 7,13; Karate-Kid-style single region → 1 clip; Despacito profile → flat warning; bin0 excluded; end-spike kept |
| M3 | Download + cut + 916blur/crop/orig | e2e on a short CC-licensed video: N files, 1080×1920, duration within ±0.5 s of plan, AV in sync, plays in browser |
| M4 | Section downloads by default + silence-snap + polish | 2 h podcast processed downloading < 15% of full size; cuts land on silence boundaries when within ±5 s |
| M5 | Native libcurl heatmap fallback + packaging + README | `--source native` passes same fixture/live tests; release zip with pinned bins |

Estimated effort: M0–M3 is the working MVP; M4–M5 hardening.

## 9. Edge cases (implement as tests where marked ★)

| Case | Behavior |
|---|---|
| No heatmap (11% observed, incl. 69M-view videos) ★ | exit 2, message explains YouTube eligibility; suggest `--dump-heatmap` on another video |
| Flat/music profile (max/median < 2) ★ | warn, proceed unless `--strict` |
| Video < 3 min ★ | if best region ≈ whole video, emit single clip = whole video (≤ 180 s) |
| Peak at bin 99 ★ | window shifts left, never exceeds duration |
| Age/member/region-locked | yt-dlp error surfaced verbatim + hint `--cookies-from-browser` |
| Live/premiere/upcoming | detect `is_live`/`live_status` in JSON → exit 3 with message |
| Vertical/square source | blur pipeline handles any AR (force_original_aspect_ratio) |
| Overlapping planned clips ★ | merged in SELECT via minGap; assert no overlap in manifest |
| yt-dlp bot-check failure | detect "confirm you're not a bot" in stderr → actionable hint |
| Chapters present | attach containing chapter title as clip label (nice titles for podcasts) |

## 10. Testing strategy

Pure-function core (`detector`, `planner`) tested against the 5 measured fixtures in FINDINGS §4 plus
synthetic shapes (impulse, plateau, double-peak, all-flat, end-spike). Media layer tested e2e against a
synthetic local video (`ffmpeg -f lavfi -i testsrc2=duration=300` + tone audio with silences at known
timestamps) so CI needs no network; one manual live test per release. Parser fuzz: truncated/garbage
JSON must fail cleanly.

## 11. Risks & mitigations

| Risk | Mitigation |
|---|---|
| YouTube changes heatmap JSON shape | two isolated sources behind `IHeatmapSource`; fixtures detect drift; yt-dlp community fixes fast (history: PR #8299) |
| Bot checks / PO tokens escalate (ongoing 2026) | `--cookies-from-browser`, pinned-but-updatable yt-dlp, rate-limit politely |
| Legal/ToS | Downloading violates YouTube ToS; re-uploading third-party clips risks copyright strikes and demonetization (fair use is case-by-case, never automatic). Safest on owned/licensed channels — surface this in README; tool itself stays neutral |
| Heatmap made private by YouTube someday | fallback roadmap: chapters, audio-energy/scene heuristics (§12) |
| 2–4 h videos: coarse bins | scaled clip length + silence-snap (§4); transcript refinement in v2 |

## 12. Out of scope for MVP (v2 roadmap, in priority order)

1. Transcript-aware refinement + auto-captions via **whisper.cpp** (C++ ✓): snap cuts to sentence
   boundaries, burn subtitles — biggest quality win for podcast shorts.
2. Auto face/speaker crop for `916crop` (OpenCV C++).
3. Batch mode (channel/playlist), config file, dedup memory.
4. Upload automation via YouTube Data API (`videos.insert`, costs 1600 quota units ≈ 6 uploads/day
   at default quota) or browser automation; title/hashtag generation.
5. Local web UI wrapping the CLI (the "platform" vision).

## 13. Execution notes for the implementing agent (Opus)

Work milestone-by-milestone in order; do not start M(n+1) with M(n) tests red. Keep `core/` free of
I/O so fixtures run without network. Never call YouTube in CI. When behavior differs from this plan,
trust measured reality, update FINDINGS.md, and note the deviation in the commit message. Numbers in
§4 are defaults, not constants — put them in one `Config` struct. Windows first-class: paths, process
creation and console UTF-8 must work under MSVC; CI matrix `windows-latest` + `ubuntu-latest`.
