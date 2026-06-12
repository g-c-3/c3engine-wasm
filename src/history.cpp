// ═══════════════════════════════════════════════════════════════════════════════
// history.cpp — Move ordering history table storage + bulk update
//
// C3Engine — JS → C++ translation
//
// All history tables are plain global arrays.  Inlined accessors (in history.h)
// handle per-entry reads and single updates.  This file owns:
//   • Table definitions (storage)
//   • historyClear() — zero everything on new game
//   • histBulkUpdate() — called by search after a quiet beta cutoff
//   • capHistBulkUpdate() — called by search after a capture beta cutoff
//
// ── JS → C++ translation notes ──────────────────────────────────────────────
//   JS histUpdate(entry, bonus)         →  histGravity(entry, bonus) in history.h
//   JS killers[ply] = [mv, prev]        →  killerStore(ply, mv) shifts the pair
//   JS histBulkUpdate(…)                →  histBulkUpdate(…) below
//   JS bonus = Math.min(depth*depth, maxBonus) — same formula here
//
// ── Bonus scaling ────────────────────────────────────────────────────────────
//   The standard formula scales the update with search depth so deeper cutoffs
//   receive stronger reinforcement:
//     bonus = clamp(depth * depth, 1, HIST_MAX / 2)
//   Negative updates (penalty for tried-but-failed quiets) use the same
//   magnitude with the sign flipped.
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "history.h"
#include "types.h"
#include <cstring>    // memset
#include <algorithm>  // std::min, std::max

// ─── Table storage (definitions) ──────────────────────────────────────────────

Move KILLERS[MAX_PLY][2];
int  HIST[2][64][64];
int  CONT_HIST[2][6][64];
int  CONT_HIST2[2][6][64];
int  CAP_HIST[2][6][64][7];
int  CORR_HIST[2][CORR_HIST_SIZE];

// ─── historyClear ─────────────────────────────────────────────────────────────

void historyClear() {
    // Zero out numeric tables
    std::memset(HIST,       0, sizeof(HIST));
    std::memset(CONT_HIST,  0, sizeof(CONT_HIST));
    std::memset(CONT_HIST2, 0, sizeof(CONT_HIST2));
    std::memset(CAP_HIST,   0, sizeof(CAP_HIST));
    std::memset(CORR_HIST,  0, sizeof(CORR_HIST));

    // Reset killers to NULL_MOVE
    for (int ply = 0; ply < MAX_PLY; ply++) {
        KILLERS[ply][0] = NULL_MOVE;
        KILLERS[ply][1] = NULL_MOVE;
    }
}

// ─── Bonus helper ─────────────────────────────────────────────────────────────
// depth * depth, clamped to [1, HIST_MAX / 2].
// Identical to JS: bonus = Math.min(depth * depth, HIST_MAX / 2)
static inline int calcBonus(int depth) {
    int raw = depth * depth;
    return std::min(raw, HIST_MAX / 2);
}

// ─── histBulkUpdate ───────────────────────────────────────────────────────────
// Called by alphaBeta when a quiet move causes a beta cutoff.
// Updates (positively) the best move and (negatively) every quiet tried before it.

void histBulkUpdate(Color color,
                    const Move& bestMove,
                    const Move* tried,
                    int  triedCount,
                    int  ply,
                    int  depth,
                    const Move& prevMove,
                    const Move& prevPrevMove,
                    const Position& pos)
{
    const int bonus   = calcBonus(depth);
    const int penalty = -bonus;

    // ── Store killer ─────────────────────────────────────────────────────────
    killerStore(ply, bestMove);

    // ── Positive update for the cutoff move ──────────────────────────────────
    histUpdate(color, bestMove, bonus);
    contHistUpdate (color, prevMove,     bestMove, pos, bonus);
    contHistUpdate2(color, prevPrevMove, bestMove, pos, bonus);

    // ── Negative update for all moves tried before the cutoff ─────────────────
    for (int i = 0; i < triedCount; i++) {
        const Move& mv = tried[i];
        if (moveIsNull(mv)) continue;
        // Don't double-penalise the best move (shouldn't appear in tried[], but
        // guard defensively in case search pushes it before the break).
        if (movesEqual(mv, bestMove)) continue;
        histUpdate(color, mv, penalty);
        contHistUpdate (color, prevMove,     mv, pos, penalty);
        contHistUpdate2(color, prevPrevMove, mv, pos, penalty);
    }
}

// ─── capHistBulkUpdate ────────────────────────────────────────────────────────
// Called by alphaBeta when a capture causes a beta cutoff.

void capHistBulkUpdate(Color color,
                       const Move& bestMove,
                       const Move* tried,
                       int triedCount,
                       int depth)
{
    const int bonus   = calcBonus(depth);
    const int penalty = -bonus;

    capHistUpdate(color, bestMove, bonus);

    for (int i = 0; i < triedCount; i++) {
        const Move& mv = tried[i];
        if (moveIsNull(mv)) continue;
        if (movesEqual(mv, bestMove)) continue;
        capHistUpdate(color, mv, penalty);
    }
}
