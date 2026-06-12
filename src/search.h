#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// search.h — Search interface for C3Engine
//
// C3Engine — JS → C++ translation
// UPGRADE (upgrade.txt items 4 & 5): Two-tier time management + score instability
// detection; SyzygyPath option stub declared in uci.h (not here).
//
// Exposes three public entry points:
//   qsearch()            — quiescence search (called by alphaBeta at depth ≤ 0)
//   alphaBeta()          — negamax alpha-beta with all pruning/extension heuristics
//   iterativeDeepening() — the top-level search driver called by the UCI handler
//
// All search state (node count, deadline, best move, search-aborted flag) is
// stored as file-static variables in search.cpp — no hidden globals here.
// The only external state alphaBeta touches is:
//   pos            — the Position (passed by reference, make/unmake in-place)
//   TT             — via ttProbe / ttStore / ttGetBest (tt.h)
//   HIST / KILLERS — via history.h helpers
//   evaluate()     — via eval.h
//   generateMoves  — via movegen.h
//
// ── JS → C++ translation notes ──────────────────────────────────────────────
//   JS alphaBeta(depth, alpha, beta, ply, nullOk, prevMv, prevPrevMv)
//     → alphaBeta(pos, depth, alpha, beta, ply, nullOk, prevMove, prevPrevMove)
//       (pos replaces all module-scope board globals)
//
//   JS quiesce(alpha, beta, ply)
//     → qsearch(pos, alpha, beta, ply)
//
//   JS search(maxDepth, moveTimeMs)
//     → iterativeDeepening(pos, maxDepth, moveTimeMs, contempt, uciInfoMode)
//       returns the best Move found (NULL_MOVE if no legal moves)
//
//   JS Date.now() deadline   → std::chrono::steady_clock
//   JS self.postMessage info → emit() in uci.cpp; info callback passed in
//   JS bestMoveRoot (global) → returned from iterativeDeepening()
//
// ── Two-tier time management (Upgrade 4) ────────────────────────────────────
//   softDeadline = startTime + allocatedTime * SOFT_FRAC   (default 0.62)
//   hardDeadline = startTime + allocatedTime               (always honoured)
//
//   Each completed depth checks:
//     (a) Best-move stability: if the same root move has been best for
//         STABILITY_THRESHOLD (5) consecutive depths AND >= STABILITY_TIME_FRAC
//         (0.50) of the budget has been used → exit early.
//     (b) Soft deadline: if elapsed >= softDeadline → do not start next depth.
//     (c) Score instability: if |bestScore - prevBestScore| > INSTABILITY_THRESH
//         (25 cp) → extend softDeadline by allocatedTime * INSTABILITY_BONUS (0.20).
//
//   The hardDeadline is enforced inside alphaBeta's time-check poll
//   (every 2048 nodes) by setting searchAborted = true.
//
// ── LMR table ────────────────────────────────────────────────────────────────
//   LMR_TABLE[depth][moveIndex] — precomputed at startup by initSearch().
//   Formula: max(1, floor(0.77 + ln(d) * ln(m) / 2.36)) for d>=3, m>=4.
//   Must call initSearch() before the first search.
//
// ── Search info callback ─────────────────────────────────────────────────────
//   iterativeDeepening() accepts a SearchInfoCallback that fires after each
//   completed depth. The UCI handler in uci.cpp wires this to emit() calls.
//   Passing nullptr disables callbacks (used for IID sub-searches).
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "types.h"
#include "board.h"
#include <functional>
#include <cstdint>

// Forward declarations
struct Position;

// ─── Search info callback ─────────────────────────────────────────────────────
// Called after each completed depth during iterative deepening.
//   depth     — depth just completed
//   score     — best score from side-to-move perspective (centipawns or mate)
//   isMate    — true when |score| >= MATE_VAL - MAX_PLY
//   nodes     — cumulative node count since search started
//   elapsedMs — milliseconds since the search started
//   bestMove  — best move found at this depth (NULL_MOVE if none)
struct SearchInfo {
    int    depth     = 0;
    int    score     = 0;
    bool   isMate    = false;
    long long nodes  = 0;
    int    elapsedMs = 0;
    Move   bestMove  = NULL_MOVE;
};

using SearchInfoCallback = std::function<void(const SearchInfo&)>;

// ─── LMR precomputed table ────────────────────────────────────────────────────
// LMR_TABLE[depth][moveIndex] — must call initSearch() before first use.
// Declared extern so uci.cpp can optionally inspect it; do not write to it.
extern int LMR_TABLE[MAX_PLY][MAX_PLY];

// ─── Search abort flag ───────────────────────────────────────────────────────
// Set by stopSearch() (called from the UCI 'stop' handler) to interrupt
// any running alphaBeta call. Reset to false at the start of each
// iterativeDeepening() call. alphaBeta polls it every 2048 nodes.
extern bool searchAborted;

// ─── Public interface ─────────────────────────────────────────────────────────

// ── initSearch ───────────────────────────────────────────────────────────────
// Precomputes LMR_TABLE. Must be called once from main() after initZobrist()
// and initBitboards(). Safe to call multiple times (idempotent).
void initSearch();

// ── stopSearch ───────────────────────────────────────────────────────────────
// Signal the running search to abort at the next time-check poll.
// Thread-safe to call from a different thread or signal handler
// because it writes a single bool (atomic on all relevant platforms).
// Mirrors JS: searchAborted = true.
void stopSearch();

// ── qsearch ──────────────────────────────────────────────────────────────────
// Quiescence search — called by alphaBeta when depth <= 0.
// Generates captures + promotions only (no quiet moves).
// Applies stand-pat, delta pruning, and SEE-based skip of losing captures.
// Mirrors JS quiesce(alpha, beta, ply).
//
// Returns the quiescence score from pos.turn's perspective.
int qsearch(Position& pos, int alpha, int beta, int ply);

// ── alphaBeta ────────────────────────────────────────────────────────────────
// Full negamax alpha-beta with:
//   • Threefold repetition detection (Stockfish-style; checks both in-search
//     stack and game history, respecting halfClock)
//   • Draw contempt (material-scaled; ply-parity nudge for AI-vs-AI)
//   • TT probe + store (dual-bucket D1 policy)
//   • Lazy evaluation (evaluateLazy guard for non-PV nodes)
//   • Improving heuristic (compare staticEval to ply-2 eval)
//   • Reverse futility pruning (static eval pruning, depths 1–8)
//   • Null-move pruning (adaptive R = 3 + depth/6 + evalTerm ± improving)
//   • ProbCut (depth >= 5; qsearch pre-verification)
//   • Razoring (depth 1: if lazyEval + 300 < alpha → qsearch)
//   • Internal iterative deepening (depth >= 5, no TT move, ply > 0)
//   • Multi-cut pruning (depth >= 8; MC_TRIES=6, MC_MIN=3, MC_RDEPTH=4)
//   • LMP (late move pruning; depths 1–8; LMP_BASE thresholds)
//   • Futility pruning (depths 1–6; BASE_FUTILITY array; improving adjustment)
//   • SEE-based quiet move skip (depth <= 6; -50*depth threshold)
//   • History leaf pruning (depth 1; combined hist < -2000)
//   • Singular extensions (depth >= 8, ply <= 4; singularBeta = ttScore - 60)
//   • Double extension (singular + gives check; check + rook/queen recapture)
//   • Negative singular extension (sets extension = -1 → depth reduction)
//   • Check extension (extension = 1 when move gives check)
//   • Threat extension (ply <= 3, depth >= 3; opponent wins major piece next)
//   • Recapture extension (captures on same square as prevMove)
//   • Passed pawn extension (pawn to rank 7, passed)
//   • Late Move Reductions (LMR_TABLE; history / cont-hist / improving adjustments)
//   • Capture LMR (capHistScore < -200 at movesDone >= 6)
//   • SEE pruning for losing captures (depth <= 6; -MAT[PAWN]*depth threshold)
//   • PVS (principal variation search; re-search from null window if score > alpha)
//   • History bulk update on beta cutoff (killers, butterfly, cont-hist 1+2-ply,
//     capture hist, countermove; malus for all quiets tried before cutoff)
//   • Correction history update on exact TT flag
//
// Returns score from pos.turn's perspective.
// Mirrors JS alphaBeta(depth, alpha, beta, ply, nullOk, prevMv, prevPrevMv).
int alphaBeta(Position& pos,
              int depth, int alpha, int beta,
              int ply, bool nullOk,
              const Move& prevMove,
              const Move& prevPrevMove,
              int contempt);

// ── iterativeDeepening ───────────────────────────────────────────────────────
// Top-level search driver. Runs alphaBeta from depth 1 up to maxDepth
// (clamped to MAX_PLY), using aspiration windows and two-tier time management.
//
// Parameters:
//   pos         — current position (will be left at the search-root state after
//                 each call; make/unmake always restores on abort)
//   maxDepth    — ceiling depth (MAX_PLY when no explicit depth is given)
//   moveTimeMs  — hard time budget in milliseconds
//   contempt    — draw contempt in centipawns (from uciContempt option)
//   onInfo      — callback fired after each completed depth (nullptr = silent)
//
// Returns the best Move found. If no legal moves exist (checkmate / stalemate),
// returns NULL_MOVE. If the search is aborted before depth 1 completes, falls
// back to the first legal move so the engine never returns NULL_MOVE in a
// non-terminal position.
//
// Mirrors JS search(maxDepth, moveTimeMs).
Move iterativeDeepening(Position& pos,
                        int maxDepth,
                        int moveTimeMs,
                        int contempt,
                        const SearchInfoCallback& onInfo);
