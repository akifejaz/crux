#pragma once

#include "core/captions.h"

namespace crux::captions {

// Tunables for the tier-1 scorer. Defaults come straight from the research
// (docs/research/README.md §"Tiered detector"); change only with data.
struct ScoreParams {
    double window_sec = 45.0;         // candidate window length
    double coldopen_end_sec = 180.0;  // teaser montage search zone: 0:00-3:00
    double coldopen_min_body_sec = 240.0; // matches must land after this point
    double nms_gap_sec = 45.0;        // min gap between kept candidates
    int    max_candidates = 12;

    double w_pre = 1.0;               // pre-marker weight multiplier
    double w_payload_numeric = 2.0;   // one digit+magnitude hit
    double w_payload_pair = 3.0;      // bonus: >=2 numeric hits within 15 s
    double w_trailing = 1.0;
    double w_coldopen = 4.0;          // editor-pre-validated crux bonus
    double four_beat_mult = 1.3;      // pre + payload + trailing, in order
};

// Scores caption cues for crux likelihood: lexicon markers (finding 1),
// numeric density (finding 2), four-beat structure (finding 3), cold-open
// replay match (finding 4), promo suppression, position prior.
// Pure function — no I/O.
CaptionScore score_captions(const CaptionDoc& doc,
                            double duration_sec,
                            const ScoreParams& p = {});

// Snaps a [start,end] window to caption cue boundaries: start moves back to
// the nearest cue start within `slack` seconds, end extends to the nearest
// cue end (so clips don't open or close mid-sentence).
void snap_to_cues(const CaptionDoc& doc, double slack,
                  double& start_sec, double& end_sec);

} // namespace crux::captions
