# FINDINGS — YouTube "Most Replayed" Heatmap (Empirical Study)

Date: 2026-07-05. Method: live inspection of 18 real videos through a logged-in Chrome session.
This file is the evidence base for `PLAN.md`. All numbers below were measured in-session, not assumed.

## 1. Method

For each video, the raw watch-page HTML (`https://www.youtube.com/watch?v=<id>`) was fetched and scanned for
heatmap markers. The data lives in the initial page payload (`ytInitialData`) at:

```
frameworkUpdates.entityBatchUpdate.mutations[]
  .payload.macroMarkersListEntity.markersList
    .markerType == "MARKER_TYPE_HEATMAP"
    .markers[] = {
        "startMillis": "0",              // string, ms
        "durationMillis": "15420",       // string, ms — always videoLength/100
        "intensityScoreNormalized": 1.0  // float 0..1, 1.0 = most replayed moment of THIS video
    }
```

Extraction pattern that worked on every video that had a heatmap (linear scan, no DOM needed):

```
"startMillis":"(\d+)","durationMillis":"(\d+)","intensityScoreNormalized":([0-9.eE+-]+)
```

Notes measured in-session:
- Watch-page HTML is ~1.7 MB; fetch ≈ 0.3 s, full text + regex scan ≈ 1 s in-browser.
- `window.ytInitialData.frameworkUpdates` is consumed/emptied after the YouTube app hydrates —
  the RAW HTML must be parsed, not the live JS object.
- yt-dlp extracts this exact entity into its info JSON as `heatmap: [{start_time, end_time, value}]`
  (seconds/floats). Verified against yt-dlp source, commit `03e85ea` ("[ie/youtube] Fix heatmap extraction", PR #8299).

## 2. Dataset (18 videos, measured 2026-07-05)

`binS` = seconds per heatmap bin = duration/100. `bin0` = score of the first bin. `mean` excludes bin 0.
Peak times are bin centers: t = (i + 0.5) × binS.

| # | Video (id) | Genre | Views | Length | binS | Heatmap | bin0 | mean | Top peaks (bin: score → time) |
|---|---|---|---|---|---|---|---|---|---|
| 1 | JRE #1169 Elon Musk (ycPr5-27vSI) | podcast | 69.2M | 2:37:02 | — | **ABSENT** | — | — | — |
| 2 | JRE #1315 Bob Lazar (BEWz4SXfyCQ) | podcast | 66.5M | 2:14:44 | 80.9s | yes | 1.00 | 0.165 | 44: 0.44 → 59:58 · 99: 0.39 → 2:14:04 · 81: 0.36 → 1:49:49 · 84: 0.34 · 55: 0.32 |
| 3 | JRE #2219 Donald Trump (hBMoPUAeLnY) | podcast | 62.4M | 2:58:50 | 107.3s | yes | 1.00 | 0.152 | 7: 0.92 → 13:25 · 13: 0.83 → 24:09 · 99: 0.51 → end · 58: 0.48 → 1:44:37 · 75: 0.40 → 2:15:01 |
| 4 | JRE #1070 Jordan Peterson (6T7pUEZfgdI) | podcast | 39.9M | 2:28:53 | 89.3s | yes | 1.00 | 0.183 | 36: 0.73 → 54:21 · 69: 0.62 → 1:43:28 · 79: 0.58 → 1:58:22 · 99: 0.48 · 54: 0.48 |
| 5 | JRE #1368 Edward Snowden (efs3QRr8LWw) | podcast | 40.0M | 2:49:32 | 101.7s | yes | 1.00 | 0.076 | 30: 0.28 → 51:42 · 36: 0.23 → 1:01:53 · 53: 0.21 → 1:27:22 · 64: 0.20 |
| 6 | Lex #310 Andrew Bustamante (T3FC7qIAGZk) | podcast | ≥20M* | 3:53:09 | 139.9s | yes | 1.00 | 0.047 | 1: 0.30 (intro decay) · 79: 0.30 → 3:05:21 · 99: 0.18 · 29: 0.18 → 1:08:47 |
| 7 | DOAC Mel Robbins (0kOtvoX88J0) | podcast | 4.3M | 1:58:59 | 71.4s | yes | 1.00 | 0.088 | 2–3: 0.31/0.24 (intro) · 94–95: 0.21/0.22 (end) · 27: 0.20 → 32:43 |
| 8 | MrBeast $456,000 Squid Game (0e3GPea1Tyg) | entertainment | 939.6M | 25:41 | 15.4s | yes | 1.00 | 0.127 | 49: 0.48 → 12:43 · 23: 0.41 → 6:02 · 35: 0.34 → 9:07 · 99: 0.29 · 69: 0.28 → 17:51 |
| 9 | MrBeast 30 Days Chained (iYlODtkyw_I) | entertainment | 60.4M | 35:04 | — | **ABSENT** | — | — | — |
| 10 | Dream Speedrunner VS 3 Hunters Finale (tylNqtyj0gs) | gaming | 141.2M | 41:47 | 25.1s | yes | 0.55 | 0.110 | 96: 1.00 → 40:19 · 97: 0.79 · 99: 0.69 · 43: 0.54 → 18:11 · 55: 0.36 → 23:11 |
| 11 | MoreSidemen WE WENT TO THE WORLD CUP! (iIY9fPgY5wM) | vlog | 2.6M | 20:01 | 12.0s | yes | 0.29 | 0.130 | 59: 1.00 → 11:55 · 58: 0.89 → 11:43 · 54: 0.55 → 10:54 · 37: 0.36 → 7:30 |
| 12 | 2018 World Cup Final highlights (GrsEAvRerTg) | sports | 69.3M | 2:10 | 1.3s | yes | 0.90 | 0.468 | 55: 1.00 → 1:12 (cluster 53–58) · 64–65: 0.78/0.82 → 1:24 |
| 13 | Karate Kid (2010) victory scene (Pt6VzQ_0k_I) | movie clip | 126.6M | 4:02 | 2.4s | yes | 0.13 | 0.133 | 75: 1.00 → 3:03 (cluster 71–78 = 2:52–3:10) |
| 14 | TED Amy Cuddy body language (Ks-_Mh1QhMc) | talk | 28.7M | 21:03 | 12.6s | yes | 0.59 | 0.294 | 7: 1.00 → 1:35 (broad 1–8) · 79: 0.52 → 16:44 · 51–52: 0.51 → 10:50 |
| 15 | Despacito (kJQP7kiw5Fk) | music | 9,064M | 4:42 | 2.8s | yes | 0.70 | 0.691 | 30: 1.00 → 1:26 (chorus) · 44–48: ~0.95 → 2:11 — FLAT/HIGH profile |
| 16 | Gangnam Style (9bZkp7q19f0) | music | 5,988M | 4:12 | 2.5s | yes | 1.00 | 0.407 | 25–28: ~0.54 → 1:07 · 45–47: ~0.53 → 1:57 — FLAT profile |
| 17 | Never Gonna Give You Up (dQw4w9WgXcQ) | music | ~1.7B* | 3:33 | 2.1s | yes | 1.00 | 0.148 | 1–4 decay · 19–22: ~0.26 → 0:44 (chorus) |
| 18 | Veritasium "Faster Than Light" (NIk_0AW5hFU) | education | n/a* | 44:15 | 26.6s | yes | 0.76 | 0.318 | 38: 1.00 → 17:02 · 4: 0.83 → 1:59 · 42: 0.76 → 18:48 · 27: 0.76 → 12:10 |

\* View count regex missed on 3 rows (long descriptions pushed `viewCount` out of the scanned window — fix: scan wider);
values marked * come from YouTube search listings/common knowledge, not in-session measurement.

## 3. Key insights (drive the algorithm design in PLAN.md)

1. **Format is 100 fixed bins, always.** Confirmed on all 16 heatmaps: exactly 100 markers,
   `durationMillis` = video length / 100 at ms precision (1541 s → 15420 ms; 10730 s → 107300 ms).
   Occasionally ±10 ms vs `lengthSeconds × 10` from fractional-second rounding (8084 s → 80850 ms),
   so derive binS from `durationMillis`, not from `lengthSeconds`.
2. **Resolution degrades with video length.** A bin is 12 s on a 20-min video but **81–140 s on 2–4 h podcasts**.
   The requested "peak ± 15 s" window is NARROWER than one bin for any video longer than ~50 min.
   Peak position uncertainty = ± binS/2 (up to ± 70 s on a 4 h video). Clip length must scale with binS,
   and cut points need audio-based refinement (silence snapping) for long videos.
3. **Bin 0 is an artifact on long-form content.** bin0 = 1.0 on 9/16 heatmaps (all podcasts, MrBeast, Gangnam, Rick):
   everyone starts at 0:00, so YouTube's normalization pins the start at max. On clip-style videos where viewers
   scrub to the money shot, bin0 is low (Karate Kid 0.13, Sidemen 0.29). Rule: exclude bin 0 (and decayed bin 1,
   e.g. Lex 0.30, Rick 0.42) from peak candidacy by default.
4. **End spikes are real, not artifacts.** bin 99 elevated on Bob Lazar (0.39), Trump (0.51), Peterson (0.48),
   Dream (0.69 — the actual finale at 96–99). Keep end peaks; just clamp the clip window to video end.
5. **Peaks come as contiguous regions, not single bins.** Karate Kid 71–78, Despacito 29–31 + 44–48,
   WC Final 53–58 + 64–65, Sidemen 58–59. Detection must group adjacent hot bins into regions and pick one
   clip per region — naive top-K bins would produce 5 near-duplicate clips of the same moment.
6. **Genre determines viability.** Podcasts/vlogs/gaming: low baseline (mean 0.05–0.18) with peaks 3–8× the median
   → ideal. Music videos: flat-high (Despacito mean 0.69; Gangnam 0.41) → no distinct "moment", the whole song
   is replayed. Measured discriminator: `peakScore / median` ≈ 1.4 on music vs ≥ 3 elsewhere.
   Gate: warn "flat heatmap, no distinct moments" when max/median < 2.
7. **Availability is NOT guaranteed and NOT view-gated.** 16/18 present (89%). Absent on JRE #1169 Elon Musk (69M views)
   and a recent MrBeast video (60M), while present on a 2.6M vlog and a 4.3M podcast. The tool must handle absence
   as a first-class case (clear error + optional fallbacks), and availability cannot be predicted from view count.
8. **Sanity check vs. UI:** peaks match visible graph spikes, including the user's own screenshot example
   (Sidemen WE WENT TO THE WORLD CUP → dominant spike at ~11:45–11:55, ≈60% through the 20:01 video, matching the screenshot).

## 4. Fixture data (full 100-bin arrays, scores ×100, for unit tests)

```json
{"id":"0e3GPea1Tyg","lenSec":1541,"binMs":15420,"scores100":[100,25,16,13,14,14,18,15,6,7,5,26,12,12,14,18,17,14,9,14,14,7,21,41,10,9,16,15,10,11,8,6,4,8,29,34,15,14,13,13,15,10,15,12,10,10,6,12,10,47,10,14,12,16,16,18,18,14,16,18,17,11,11,10,9,5,18,14,1,28,14,4,3,4,4,1,0,1,1,16,2,11,11,11,10,9,9,11,9,10,11,12,9,6,4,3,8,22,18,29]}
{"id":"tylNqtyj0gs","lenSec":2507,"binMs":25080,"scores100":[55,10,9,15,4,12,11,4,3,2,2,8,1,12,6,6,1,5,2,5,13,2,5,5,2,3,0,4,6,11,6,6,3,5,4,5,5,4,9,6,4,3,14,54,16,3,2,26,9,6,6,2,3,2,5,36,18,22,13,11,2,3,8,6,5,16,2,3,2,5,11,13,1,2,6,7,25,18,10,25,23,3,2,8,2,7,5,4,5,5,8,11,9,8,13,28,100,79,50,69]}
{"id":"Ks-_Mh1QhMc","lenSec":1263,"binMs":12630,"scores100":[59,56,55,47,37,34,54,100,62,27,26,38,32,32,25,25,23,30,23,24,19,15,14,40,23,20,16,15,14,22,13,11,11,18,28,29,25,16,21,25,32,33,37,35,27,23,30,34,31,37,45,51,51,32,25,31,28,21,19,22,21,45,16,26,20,19,20,17,23,25,18,26,29,30,41,42,30,28,28,52,21,24,26,24,23,28,41,47,45,33,33,26,47,46,30,22,21,18,9,0]}
{"id":"iIY9fPgY5wM","lenSec":1201,"binMs":12010,"scores100":[29,25,11,13,28,14,16,27,23,6,7,5,4,5,2,11,18,10,11,13,4,7,12,4,12,3,3,4,3,5,10,14,32,15,5,5,20,36,17,30,22,9,8,2,6,5,1,9,8,8,8,10,8,52,55,8,7,14,89,100,15,9,9,6,2,17,17,12,20,28,9,3,0,3,5,12,14,21,13,8,5,6,2,6,7,7,14,21,12,14,3,2,4,1,5,9,10,2,7,7]}
{"id":"BEWz4SXfyCQ","lenSec":8084,"binMs":80850,"scores1000":[1000,76,16,0,86,239,181,125,54,129,67,80,61,90,298,299,165,158,150,284,114,221,175,249,108,160,148,94,74,237,170,165,229,167,111,148,137,182,147,192,305,117,250,134,444,220,106,166,156,219,143,224,119,138,194,323,153,162,187,180,185,221,226,202,156,151,152,277,153,235,283,150,124,244,296,115,133,60,65,112,187,363,179,148,339,304,137,115,60,107,168,72,75,157,112,50,38,78,19,385]}
```

More fixtures can be regenerated any time with:
`yt-dlp --skip-download --dump-single-json <url> | jq '.heatmap'`

## 5. Ecosystem facts verified via web research (2026-07)

- **yt-dlp** exposes the heatmap in `--dump-json` output (`heatmap` field, list of `{start_time, end_time, value}`);
  extraction path in source matches Section 1 exactly.
- **The official YouTube Data API v3 does NOT expose most-replayed data.** Scraping the watch page (or yt-dlp) is the
  only local route; commercial scrapers (Apify etc.) confirm the same 100-segment structure.
- **Shorts rules (2026):** up to **3 minutes** (raised from 60 s in Oct 2024), aspect ratio **square or taller**
  (9:16 standard, 1080×1920). A 16:9 clip does NOT qualify — vertical conversion is required.
- **yt-dlp --download-sections "*START-END"** downloads only a time range (keyframe-accurate without re-encode);
  ideal to avoid downloading 3 h podcasts for 6 clips.
- **Bot checks are the main operational risk** ("Sign in to confirm you're not a bot", ongoing through 2026).
  Standard mitigations: `--cookies-from-browser chrome`, keeping yt-dlp current, residential IP, rate limiting.
- "Most replayed" rolled out publicly in May 2022; YouTube Help documents it only as available on "eligible" videos
  (no public criteria — consistent with insight #7).

## 6. Sources

- https://github.com/yt-dlp/yt-dlp/commit/03e85ea99db76a2fddb65bf46f8819bda780aaf3 (heatmap extraction path)
- https://github.com/yt-dlp/yt-dlp/issues/3888 (feature history)
- https://priyavr.at/blog/reversing-most-replayed/ (independent reverse-engineering, 100 segments)
- https://support.google.com/youtube/answer/15424877 (3-minute Shorts)
- https://www.shortsync.app/resources/youtube-shorts-upload-requirements-2026 (Shorts specs 2026)
- https://github.com/yt-dlp/yt-dlp/issues/8011, /issues/10181 (--download-sections behavior)
- https://github.com/yt-dlp/yt-dlp/issues/15865 (bot-check status 2026)
- In-session measurements: 18 videos, Chrome, 2026-07-05 (Section 2)
