# out/ — generated clips and run artifacts

Every crux run writes one directory here. Two naming schemes:

| Directory | Created by |
|---|---|
| `out/<video-id>/` | the CLI (`crux <url>`), e.g. `out/iW6_RGwfZ64/` |
| `out/web-<timestamp>/` | the dashboard (`crux_server`), one per Run click |

## Inside a run directory

```
out/<run>/
├─ clip_NN_HHMMSS.mp4   the generated shorts — 9:16, intro pre-roll included.
│                       NN = clip index, HHMMSS = start position in the video
├─ manifest.json        run summary: video metadata, params, quality gate,
│                       clip list (start/end/score/label/file)
├─ heatmap.json         the 100-bin profile detection ran on (with
│                       --dump-heatmap; dashboard runs always write it)
├─ captions.json        caption crux candidates: start/end, score, hook line,
│                       matched signals, four-beat / cold-open flags
├─ run.log              full CLI output (dashboard runs only)
└─ work/                intermediates
   ├─ subs/subs.<lang>.vtt   downloaded subtitles (small, kept)
   ├─ assets/thumb.jpg       source thumbnail used by the intro pre-roll
   ├─ assets/play.png        generated intro graphics
   ├─ assets/cursor.png
   └─ <video-id>.mp4         downloaded source video — DELETED automatically
                             after a successful run (pass --keep-source to
                             keep it, e.g. to iterate on clip parameters
                             without re-downloading)
```

The clips are the deliverable; everything else exists so a run can be
inspected (dashboard Results tab) or reproduced. Failed runs keep whatever
they produced — including the source download — so a retry can resume.

Delete any run directory freely; nothing outside it references it. The
dashboard lists every directory here that contains a `manifest.json`,
including runs from past sessions and CLI runs.

This folder is gitignored (except this README).
