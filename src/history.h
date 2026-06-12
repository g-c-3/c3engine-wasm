#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// history.h — Move ordering history tables for C3Engine
//
// C3Engine — JS → C++ translation
//
// Owns all history heuristics used by the move ordering and search:
//
//   Killer moves         — two quiet moves per ply that caused a beta cutoff
//   Butterfly history    — quiet history indexed by [color][from][to]
//   Continuation history — 1-ply and 2-ply follow-up history indexed by
//                          [color][piece][to] relative to prev/prevPrev move
//   Capture history      — indexed by [color][attacker][to][victim]
//   Correction history   — pawn-structure-based static eval bias (pawn hash key)
//
// ── JS → C++ translation notes ───────────────────────────────────────────────
//   JS killers[ply][0/1]              →  KILLERS[ply][0/1]  (same layout)
//   JS histTable[ci][from][to]        →  HIST[ci][from][to]  (butterfly)
//   JS contHist[ci][piece][to]        →  CONT_HIST[ci][pieceType][to]
//                                        (1-ply: uses prevMove piece/to)
//   JS contHist2[ci][piece][to]       →  CONT_HIST2[ci][pieceType][to]
//                                        (2-ply: uses prevPrevMove piece/to)
//   JS capHist[ci][attacker][to][vic] →  CAP_HIST[ci][attacker][to][victim]
//   JS corrHist[pawnKeyLow & mask]    →  CORR_HIST[color][pawnKey & CORR_MASK]
//   JS histUpdate(bonus)              →  histGravity(table, bonus)  (same formula)
//
// ── History gravity (aging / saturation) ─────────────────────────────────────
//   All tables use the same update formula (mirrors JS histUpdate):
//     entry += bonus - entry * abs(bonus) / HIST_MAX
//   This keeps values in (-HIST_MAX, +HIST_MAX) without explicit clamping and
//   slowly drags old scores toward zero ("gravity").
//   HIST_MAX = 16384 (2^14) — standard for engines at this search depth.
//
// ── Table sizes ───────────────────────────────────────────────────────────────
//   HIST          [2][64][64]        = 8192  entries  × 4B = 32 KB
//   CONT_HIST     [2][6][64]         = 768   entries  × 4B = 3 KB  (per table)
//   CONT_HIST2    [2][6][64]         = 768   entries  × 4B = 3 KB
//   CAP_HIST      [2][6][64][7]      = 5376  entries  × 4B = 21 KB
//   CORR_HIST     [2][16384]         = 32768 entries  × 4B = 128 KB
//   KILLERS       [MAX_PLY][2]       = 128   entries  (Move structs)
//   Total ≈ 190 KB — negligible compared to the TT.
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "types.h"
#include <array>
#include <cstdint>
#include <cstdlib>   // std::abs

// Forward declaration — Position is used by contHistUpdate/Score but the full
// struct definition is not needed in the inline helpers here (they only call
// methods declared in board.h which callers already include).
struct Position;

// ─── Limits ───────────────────────────────────────────────────────────────────
constexpr int HIST_MAX       = 16384;  // saturation bound for all history tables
constexpr int CORR_HIST_SIZE = 16384;  // must be a power of two
constexpr int CORR_HIST_MASK = CORR_HIST_SIZE - 1;
constexpr int CORR_HIST_MAX  = 1024;   // correction value saturation bound

// ─── Killer moves ─────────────────────────────────────────────────────────────
// Two quiet killer slots per ply.  Access via KILLERS[ply][0] and [1].
extern Move KILLERS[MAX_PLY][2];

// ─── Butterfly quiet history ──────────────────────────────────────────────────
// HIST[color][from][to]
extern int HIST[2][64][64];

// ─── Continuation history (1-ply) ─────────────────────────────────────────────
// Indexed by the previous move's (pieceType, to) — captures the "follow-up"
// tendency of moves after a given piece lands on a given square.
// CONT_HIST[color][pieceType][to]
extern int CONT_HIST[2][6][64];

// ─── Continuation history (2-ply) ─────────────────────────────────────────────
// Same shape as CONT_HIST but indexed by the move two half-moves back.
extern int CONT_HIST2[2][6][64];

// ─── Capture history ──────────────────────────────────────────────────────────
// CAP_HIST[color][attackerType][to][capturedType]
// capturedType uses NO_PIECE_TYPE (6) as the upper bound → dimension 7
extern int CAP_HIST[2][6][64][7];

// ─── Correction history ───────────────────────────────────────────────────────
// Per-color table indexed by the lower bits of the pawn Zobrist key.
// Used to bias the static evaluation based on pawn-structure tendencies.
// CORR_HIST[color][pawnZobristKey & CORR_HIST_MASK]
extern int CORR_HIST[2][CORR_HIST_SIZE];

// ─── History gravity helper ───────────────────────────────────────────────────
// Updates a single history entry using the standard "gravity" formula:
//   entry += bonus - entry * |bonus| / HIST_MAX
// This automatically saturates toward ±HIST_MAX without a clamp call.
// Mirrors JS histUpdate(entry, bonus).
inline void histGravity(int& entry, int bonus) {
    entry += bonus - entry * std::abs(bonus) / HIST_MAX;
}

// Same formula for the narrower correction history table.
inline void corrGravity(int& entry, int bonus) {
    entry += bonus - entry * std::abs(bonus) / CORR_HIST_MAX;
}

// ─── Initialisation / clear ───────────────────────────────────────────────────

// Zero every history table and clear all killers.
// Called on ucinewgame and setoption Hash (since a new game starts with stale data).
void historyClear();

// ─── Killer helpers ───────────────────────────────────────────────────────────

// Store a new killer at `ply`.  Shifts the existing killer1 → killer2; does not
// store duplicate entries.
// Mirrors JS killerStore(ply, mv).
inline void killerStore(int ply, const Move& mv) {
    if (ply < 0 || ply >= MAX_PLY) return;
    if (movesEqual(KILLERS[ply][0], mv)) return;   // already stored
    KILLERS[ply][1] = KILLERS[ply][0];
    KILLERS[ply][0] = mv;
}

// Retrieve killer slots for the given ply.
inline Move killerGet1(int ply) { return (ply >= 0 && ply < MAX_PLY) ? KILLERS[ply][0] : NULL_MOVE; }
inline Move killerGet2(int ply) { return (ply >= 0 && ply < MAX_PLY) ? KILLERS[ply][1] : NULL_MOVE; }

// ─── Butterfly history helpers ────────────────────────────────────────────────

// Update butterfly history after a quiet move succeeds or fails.
// bonus > 0 for a move that caused a cutoff; bonus < 0 for all moves that
// were tried before the cutoff (the "all quiets that failed" loop).
inline void histUpdate(Color color, const Move& mv, int bonus) {
    histGravity(HIST[color][mv.from][mv.to], bonus);
}

// Read butterfly history score (used by scoreQuiets in movegen.cpp).
inline int histScore(Color color, const Move& mv) {
    return HIST[color][mv.from][mv.to];
}

// ─── Continuation history helpers ────────────────────────────────────────────

// Update the 1-ply continuation history.
// `prevMove` is the move played one half-move ago (the "parent" move).
// We update the entry corresponding to (prevMove.pieceType, prevMove.to).
inline void contHistUpdate(Color color, const Move& prevMove, const Move& mv,
                           const Position& pos, int bonus) {
    if (moveIsNull(prevMove)) return;
    PieceType pt = prevMove.attackerType;
    if (pt == NO_PIECE_TYPE) return;
    (void)pos; // pos kept for potential future use (e.g. piece-on-square tables)
    histGravity(CONT_HIST[color][pt][prevMove.to], bonus);
    (void)mv;
}

// Read 1-ply continuation history score.
inline int contHistScore(Color color, const Move& prevMove, const Move& mv,
                         const Position& pos) {
    if (moveIsNull(prevMove)) return 0;
    PieceType pt = prevMove.attackerType;
    if (pt == NO_PIECE_TYPE) return 0;
    (void)pos; (void)mv;
    return CONT_HIST[color][pt][prevMove.to];
}

// Update the 2-ply follow-up continuation history.
inline void contHistUpdate2(Color color, const Move& prevPrevMove, const Move& mv,
                            const Position& pos, int bonus) {
    if (moveIsNull(prevPrevMove)) return;
    PieceType pt = prevPrevMove.attackerType;
    if (pt == NO_PIECE_TYPE) return;
    (void)pos; (void)mv;
    histGravity(CONT_HIST2[color][pt][prevPrevMove.to], bonus);
}

// Read 2-ply follow-up continuation history score.
inline int contHistScore2(Color color, const Move& prevPrevMove, const Move& mv,
                          const Position& pos) {
    if (moveIsNull(prevPrevMove)) return 0;
    PieceType pt = prevPrevMove.attackerType;
    if (pt == NO_PIECE_TYPE) return 0;
    (void)pos; (void)mv;
    return CONT_HIST2[color][pt][prevPrevMove.to];
}

// ─── Capture history helpers ──────────────────────────────────────────────────

// Update capture history.
inline void capHistUpdate(Color color, const Move& mv, int bonus) {
    if (mv.attackerType == NO_PIECE_TYPE) return;
    int vic = (mv.capturedType == NO_PIECE_TYPE) ? 6 : static_cast<int>(mv.capturedType);
    histGravity(CAP_HIST[color][mv.attackerType][mv.to][vic], bonus);
}

// Read capture history score.
inline int capHistScore(Color color, const Move& mv) {
    if (mv.attackerType == NO_PIECE_TYPE) return 0;
    int vic = (mv.capturedType == NO_PIECE_TYPE) ? 6 : static_cast<int>(mv.capturedType);
    return CAP_HIST[color][mv.attackerType][mv.to][vic];
}

// ─── Correction history helpers ───────────────────────────────────────────────

// Update correction history after a search completes.
// `delta` = (bestScore - staticEval), used to bias future static evals at this
// pawn structure.
inline void corrHistUpdate(Color color, Bitboard pawnZobristKey, int delta) {
    int idx = static_cast<int>(pawnZobristKey & static_cast<Bitboard>(CORR_HIST_MASK));
    corrGravity(CORR_HIST[color][idx], delta);
}

// Read correction history value.  Returns 0 if the entry is unset.
inline int corrHistGet(Color color, Bitboard pawnZobristKey) {
    int idx = static_cast<int>(pawnZobristKey & static_cast<Bitboard>(CORR_HIST_MASK));
    return CORR_HIST[color][idx];
}

// ─── Bulk update helper (search.cpp uses this after a cutoff) ─────────────────

// Called by alphaBeta when a quiet move causes a beta cutoff.
// Updates killers, butterfly, continuation (1+2-ply), and penalises all
// quiets that were tried before the cutoff.
// Mirrors JS histBulkUpdate(color, bestMove, triedQuiets, ply, depth, prevMv, prevPrevMv).
void histBulkUpdate(Color color,
                    const Move& bestMove,
                    const Move* tried,   // array of moves tried before cutoff
                    int  triedCount,
                    int  ply,
                    int  depth,
                    const Move& prevMove,
                    const Move& prevPrevMove,
                    const Position& pos);

// Called when a capture causes a beta cutoff.
// Updates capture history positively for bestMove and negatively for triedCaptures.
void capHistBulkUpdate(Color color,
                       const Move& bestMove,
                       const Move* tried,
                       int triedCount,
                       int depth);
