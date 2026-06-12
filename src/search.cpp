// ═══════════════════════════════════════════════════════════════════════════════
// search.cpp — Iterative deepening, alpha-beta, quiescence search for C3Engine
//
// C3Engine — JS → C++ translation
// UPGRADE (upgrade.txt items 4 & 5): Two-tier time management + score instability
// detection; SyzygyPath stub is in uci.h (not here).
//
// ── What this file owns ──────────────────────────────────────────────────────
//   LMR_TABLE[MAX_PLY][MAX_PLY]   — precomputed log-log reduction table
//   searchAborted                 — global abort flag (polled every 2048 nodes)
//   searchDeadlineMs              — hard deadline (Unix ms from chrono)
//   searchStartMs                 — search start time (for elapsed calculation)
//   nodeCount                     — cumulative nodes searched in this call
//   bestMoveRoot                  — best root move across ID iterations
//   initSearch()                  — fills LMR_TABLE once at startup
//   stopSearch()                  — sets searchAborted = true
//   qsearch()                     — quiescence search
//   alphaBeta()                   — negamax alpha-beta with all pruning
//   iterativeDeepening()          — ID loop, aspiration windows, time management
//
// ── JS → C++ translation notes ──────────────────────────────────────────────
//   JS Date.now()                 → timeNowMs() using std::chrono::steady_clock
//   JS self.postMessage info      → SearchInfoCallback passed into iterativeDeepening
//   JS searchStack / stackLen     → pos.searchStack / pos.searchStackLen (board.h)
//   JS gameHistoryKeys / Len      → pos.gameHistory vector (board.h)
//   JS killers / countermoves     → KILLERS[] / histBulkUpdate() (history.h)
//   JS clearCountermoves()        → historyClear() covers all tables on new game;
//                                   per-search killers reset in iterativeDeepening
//   JS ageHistory / ageCaptureHistory / ageContHist →
//                                   gravity formula in histGravity() keeps tables
//                                   naturally aged; no separate half-decay needed
//                                   (the gravity update already prevents overflow)
//
// ── History note ─────────────────────────────────────────────────────────────
//   The JS engine called separate ageHistory() / ageContHist() functions at the
//   start of each search to halve all history values. The C++ translation uses
//   the histGravity() formula (history.h) which auto-saturates toward ±HIST_MAX
//   without overflow — explicit aging is unnecessary. Killers are cleared per
//   search (below) to avoid cross-search contamination, matching JS behaviour.
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "search.h"
#include "board.h"
#include "movegen.h"
#include "tt.h"
#include "history.h"
#include "eval.h"
#include "bitboard.h"
#include "types.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <climits>

// ─── Chrono helpers ───────────────────────────────────────────────────────────
// Return the current time as milliseconds since an arbitrary epoch.
// Consistent within a process; used only for deadline arithmetic.
static inline long long timeNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ─── Global search state ──────────────────────────────────────────────────────

int  LMR_TABLE[MAX_PLY][MAX_PLY];
bool searchAborted  = false;

static long long searchStartMs    = 0;  // time iterativeDeepening was called
static long long searchDeadlineMs = 0;  // hard time limit (absolute ms)
static long long searchBudgetMs   = 0;  // the moveTimeMs value for current search
static long long nodeCount        = 0;  // nodes searched since search started
static Move      bestMoveRoot     = NULL_MOVE;  // best root move across ID depths

// ─── initSearch ───────────────────────────────────────────────────────────────
void initSearch() {
    for (int d = 0; d < MAX_PLY; d++) {
        for (int m = 0; m < MAX_PLY; m++) {
            if (d < 3 || m < 4)
                LMR_TABLE[d][m] = 0;
            else
                LMR_TABLE[d][m] = std::max(1,
                    static_cast<int>(std::floor(0.77 + std::log(d) * std::log(m) / 2.36)));
        }
    }
}

// ─── stopSearch ───────────────────────────────────────────────────────────────
void stopSearch() {
    searchAborted = true;
}

// ─── Draw contempt helper ─────────────────────────────────────────────────────
// Mirrors JS: scale contempt by material difference so that when we're clearly
// ahead we avoid draws, and when clearly behind we accept them.
// contempt is the uciContempt value in centipawns (default 25).
static int contemptScore(const Position& pos, int ply, int contempt) {
    const int ci  = static_cast<int>(pos.turn);
    const int opp = 1 - ci;

    int ownMat = 0, oppMat = 0;
    for (int t = QUEEN; t <= PAWN; t++) {
        ownMat += bbCount(pos.bb[ci ][t]) * MAT[t];
        oppMat += bbCount(pos.bb[opp][t]) * MAT[t];
    }
    const int matDiff = ownMat - oppMat;

    if (matDiff > 150) {
        // Clearly ahead — avoid draw
        return -(static_cast<int>(contempt * 1.6));  // e.g. 25 → -40
    } else if (matDiff < -150) {
        // Clearly behind — accept draw
        return contempt;
    } else {
        // Near-equal — ply-parity nudge to break AI-vs-AI loops
        const int nudge = std::max(1, static_cast<int>(contempt * 0.4));
        return (ply % 2 == 1) ? -nudge : nudge;
    }
}

// ─── Repetition detection ─────────────────────────────────────────────────────
// Mirrors JS Stockfish-style repetition check.
// Returns the number of prior occurrences of pos.zobristKey (capped at 2).
// Searches both the in-search stack (pos.searchStack) and the game history.
// Only looks back pos.halfClock half-moves (irreversible boundary).
static int repetitionCount(const Position& pos) {
    const Bitboard key     = pos.zobristKey;
    const int      half    = pos.halfClock;
    const int      stkLen  = pos.searchStackLen;
    int reps = 0;

    // 1. In-search stack: walk back by 2 (same side to move) from current top.
    //    The top of the stack is the position we JUST pushed (current), so start
    //    at stkLen-2 (one full move back = same side to move).
    for (int i = stkLen - 2; i >= 0; i -= 2) {
        if (pos.searchStack[i] == key) {
            reps++;
            break; // at most one in-search match (avoids double-counting within tree)
        }
    }

    // 2. Game history: positions from before the search started.
    //    Entries are indexed 0..gameHistoryLen-1.
    //    Align parity: gameHistory[last] is the pre-search root, same side as
    //    pos.turn when stkLen is even; offset by 1 when odd.
    const int ghLen = static_cast<int>(pos.gameHistory.size());
    if (ghLen > 0) {
        const int limit = std::max(0, ghLen - half - 1);
        int hStart = ghLen - 1;
        if (stkLen % 2 == 1) hStart--;  // flip parity to match current side
        for (int h = hStart; h >= limit; h -= 2) {
            if (pos.gameHistory[h] == key) {
                reps++;
                if (reps >= 2) break;  // third occurrence confirmed
            }
        }
    }

    return reps;
}

// ─── Null-move material check ─────────────────────────────────────────────────
// Returns the non-pawn, non-king material for pos.turn.
// NMP only fires when this exceeds 600 (6 pawns worth of non-pawn material)
// to avoid null-move blunders in pure endgames.
static int nonPawnMaterial(const Position& pos) {
    const int ci = static_cast<int>(pos.turn);
    int mat = 0;
    for (int t = QUEEN; t <= KNIGHT; t++)  // QUEEN=1 ROOK=2 BISHOP=3 KNIGHT=4
        mat += bbCount(pos.bb[ci][t]) * MAT[t];
    return mat;
}

// ─── Threat detection for threat extension ────────────────────────────────────
// After makeMove, check if the opponent (now pos.turn) can immediately win
// material worth more than a minor piece with a SEE-positive capture.
// Used by the threat extension guard (ply <= 3, depth >= 3, no check).
static bool opponentHasWinningCapture(const Position& pos) {
    const int ci  = static_cast<int>(pos.turn);    // opponent (they just became to-move)

    // Iterate over opponent's pieces (non-king, non-pawn first for efficiency)
    for (int t = QUEEN; t <= PAWN; t++) {
        Bitboard attackers = pos.bb[ci][t];
        while (attackers) {
            const Square aSq = popLsb(attackers);

            // Find squares this piece can capture our pieces on
            Bitboard targets = BB_ZERO;
            switch (t) {
                case QUEEN:  targets = queenAttacks(aSq, pos.occAll) & pos.occB; break;
                case ROOK:   targets = rookAttacks  (aSq, pos.occAll) & pos.occB; break;
                case BISHOP: targets = bishopAttacks(aSq, pos.occAll) & pos.occB; break;
                case KNIGHT: targets = KNIGHT_ATTACKS[aSq]            & pos.occB; break;
                case PAWN:   targets = PAWN_ATTACKS[ci][aSq]          & pos.occB; break;
                default:     break;
            }
            // Flip: ci is the opponent (now to-move), so their targets = our pieces
            // We stored occB above but need to remap correctly:
            // ci=WHITE means pos.turn==WHITE after flip, so their pieces attack pos.occB (black) — wrong
            // Actually we need: my pieces = opp (the side that just moved), targets = opp's pieces
            // Redo with correct occupancies:
            (void)targets; // discard above — recalculate correctly below
            Bitboard myOcc = (ci == WHITE) ? pos.occB : pos.occW;   // pieces of 'opp' (the side that moved)
            Bitboard tgts  = BB_ZERO;
            switch (t) {
                case QUEEN:  tgts = queenAttacks(aSq, pos.occAll) & myOcc; break;
                case ROOK:   tgts = rookAttacks  (aSq, pos.occAll) & myOcc; break;
                case BISHOP: tgts = bishopAttacks(aSq, pos.occAll) & myOcc; break;
                case KNIGHT: tgts = KNIGHT_ATTACKS[aSq]            & myOcc; break;
                case PAWN:   tgts = PAWN_ATTACKS[ci][aSq]          & myOcc; break;
                default:     break;
            }

            while (tgts) {
                const Square tSq = popLsb(tgts);
                const PieceType victim = pos.pieceAt[tSq].type;
                // Only trigger for significant material gains (> minor piece)
                if (victim == NO_PIECE_TYPE || MAT[victim] <= MAT[KNIGHT]) continue;
                if (see(pos, tSq, aSq) > MAT[KNIGHT]) return true;
            }
        }
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// qsearch — Quiescence search
// ═══════════════════════════════════════════════════════════════════════════════

int qsearch(Position& pos, int alpha, int beta, int ply) {
    // 50-move rule
    if (pos.halfClock >= 100) return 0;

    // TT probe (QS depth = QS_TT_DEPTH = -1)
    {
        int ttScore = 0;
        if (ttProbe(pos.zobristKey, QS_TT_DEPTH, alpha, beta, ttScore)) {
            return ttScore;
        }
    }

    // Stand-pat
    const int stand = evaluate(pos);
    if (stand >= beta) return beta;
    if (stand > alpha) alpha = stand;

    // Delta pruning threshold — queen value + buffer
    const int DELTA_MARGIN = MAT[QUEEN] + 50;

    // Generate captures + promotions only
    MoveGenContext ctx;
    {
        Square ttFrom = NO_SQUARE, ttTo = NO_SQUARE;
        PieceType ttPromo = NO_PIECE_TYPE;
        if (ttGetBest(pos.zobristKey, ttFrom, ttTo, ttPromo)) {
            // Bake the TT move so staged gen can skip it
            ctx.ttBest.from = ttFrom;
            ctx.ttBest.to   = ttTo;
            ctx.ttBest.promo = ttPromo;
        }
    }
    StagedMoveGen gen(pos, ctx, /*qsearch=*/true);

    int bestScore = stand;
    Move bestMv   = NULL_MOVE;
    int flag      = TT_UPPER;

    while (true) {
        const Move mv = gen.next();
        if (moveIsNull(mv)) break;

        // Delta pruning (not for promotions)
        if (!flagIsPromo(mv.flags) && mv.flags != EN_PASSANT) {
            const PieceType cap = (mv.flags == EN_PASSANT) ? PAWN : mv.capturedType;
            const int captureGain = (cap != NO_PIECE_TYPE) ? MAT[cap] : 0;
            if (stand + captureGain + DELTA_MARGIN < alpha) continue;
            // Tighter per-capture futility check
            if (stand + captureGain + 100 < alpha) continue;
        }

        // Skip clearly losing captures (SEE < -50)
        if ((mv.flags == CAPTURE || mv.flags == PROMO_CAPTURE) &&
            see(pos, mv.to, mv.from) < -50) continue;

        UndoRecord undo;
        pos.makeMove(mv, undo);
        nodeCount++;
        const int score = -qsearch(pos, -beta, -alpha, ply + 1);
        pos.unmakeMove(mv, undo);

        if (score > bestScore) { bestScore = score; bestMv = mv; }
        if (score >= beta) {
            ttStore(pos.zobristKey, QS_TT_DEPTH, score, TT_LOWER,
                    mv.from, mv.to, mv.promo);
            return beta;
        }
        if (score > alpha) { alpha = score; flag = TT_EXACT; }
    }

    ttStore(pos.zobristKey, QS_TT_DEPTH, bestScore, flag,
            moveIsNull(bestMv) ? NO_SQUARE : bestMv.from,
            moveIsNull(bestMv) ? NO_SQUARE : bestMv.to,
            moveIsNull(bestMv) ? NO_PIECE_TYPE : bestMv.promo);
    return alpha;
}

// ═══════════════════════════════════════════════════════════════════════════════
// alphaBeta — Negamax alpha-beta with full pruning and extension suite
// ═══════════════════════════════════════════════════════════════════════════════

// Constants mirroring JS exactly
static constexpr int LAZY_MARGIN       = 200;  // gap between lazy and full eval
static constexpr int PROBCUT_MARGIN    = 200;  // ProbCut beta margin
static constexpr int MC_TRIES          = 6;    // multi-cut: moves to try
static constexpr int MC_MIN            = 3;    // multi-cut: cutoffs needed
static constexpr int MC_RDEPTH         = 4;    // multi-cut: reduced depth
static constexpr int STABILITY_THRESH  = 5;    // ID: consecutive same-best depths
static constexpr double SOFT_TIME_FRAC = 0.62; // ID: soft deadline fraction
static constexpr double STAB_TIME_FRAC = 0.50; // ID: stability early-exit fraction
static constexpr int INSTABILITY_THRESH = 25;  // ID: score swing triggering extra time
static constexpr double INSTAB_BONUS   = 0.20; // ID: soft deadline extension fraction

// BASE_FUTILITY[depth] — futility margins for depths 1–6 (index 0 unused)
static constexpr int BASE_FUTILITY[7] = { 0, 100, 300, 500, 700, 900, 1100 };

// LMP_BASE[depth] — late move pruning quiet-move count thresholds (depths 1–8)
static constexpr int LMP_BASE[9] = { 0, 3, 5, 9, 14, 20, 27, 36, 46 };

int alphaBeta(Position& pos,
              int depth, int alpha, int beta,
              int ply, bool nullOk,
              const Move& prevMove,
              const Move& prevPrevMove,
              int contempt)
{
    // ── Time check (every 2048 nodes) ────────────────────────────────────────
    if ((++nodeCount & 2047) == 0) {
        if (timeNowMs() >= searchDeadlineMs) {
            searchAborted = true;
            return 0;
        }
    }
    if (searchAborted) return 0;

    // ── Mate distance pruning ─────────────────────────────────────────────────
    const int mateScore = MATE_VAL - ply;
    if (alpha < -mateScore) alpha = -mateScore;
    if (beta  >  mateScore) beta  =  mateScore;
    if (alpha >= beta) return alpha;

    // ── Threefold repetition ──────────────────────────────────────────────────
    if (ply > 0) {
        const int reps = repetitionCount(pos);
        if (reps >= 1) {
            return contemptScore(pos, ply, contempt);
        }
    }

    // ── TT probe ─────────────────────────────────────────────────────────────
    int ttScore = 0;
    const bool ttHit = ttProbe(pos.zobristKey, depth, alpha, beta, ttScore);
    if (ttHit && ply > 0) return ttScore;

    // Retrieve best move from TT for move ordering (may differ from ttHit)
    Move ttBest = NULL_MOVE;
    {
        Square ttFrom = NO_SQUARE, ttTo = NO_SQUARE;
        PieceType ttPromo = NO_PIECE_TYPE;
        if (ttGetBest(pos.zobristKey, ttFrom, ttTo, ttPromo)) {
            ttBest.from  = ttFrom;
            ttBest.to    = ttTo;
            ttBest.promo = ttPromo;
        }
    }

    // ── Horizon ───────────────────────────────────────────────────────────────
    if (depth <= 0) return qsearch(pos, alpha, beta, ply);

    // ── Static eval + improving heuristic ─────────────────────────────────────
    const bool inCheckNow = pos.inCheck(pos.turn);

    int rawStaticEval;
    if (inCheckNow) {
        rawStaticEval = -INF;
    } else if (ply > 0 && depth >= 1 && alpha + 1 >= beta) {
        // Non-PV node: try lazy eval guard
        const int lazyEst = evaluateLazy(pos);
        if (lazyEst >= beta + LAZY_MARGIN || lazyEst <= alpha - LAZY_MARGIN) {
            rawStaticEval = lazyEst;
        } else {
            rawStaticEval = evaluate(pos);
        }
    } else {
        // PV node or root — always full eval
        rawStaticEval = evaluate(pos);
    }

    if (ply < SEARCH_STACK_SIZE)
        pos.staticEvalStack[ply] = rawStaticEval;

    const bool improving = !inCheckNow && ply >= 2
        && rawStaticEval > pos.staticEvalStack[ply - 2];

    // ── Reverse futility pruning ──────────────────────────────────────────────
    if (depth >= 1 && depth <= 8 && !inCheckNow && ply > 0 &&
        alpha + 1 >= beta &&
        std::abs(beta) < MATE_VAL - 100) {
        const int rfpMargin = 120 * depth - (improving ? 40 : 0);
        if (rawStaticEval - rfpMargin >= beta)
            return rawStaticEval - rfpMargin;
    }

    // ── Null-move pruning ─────────────────────────────────────────────────────
    if (nullOk && depth >= 3 && !inCheckNow && ply > 0) {
        if (nonPawnMaterial(pos) > 600) {
            const int evalTerm = std::max(0, std::min(3,
                static_cast<int>((rawStaticEval - beta) / 200)));
            const int R = 3 + depth / 6 + evalTerm + (improving ? 0 : 1);

            // Save and make null move (flip turn, clear EP)
            UndoRecord nullUndo;
            nullUndo.enPassantSq    = pos.enPassantSq;
            nullUndo.castleRights   = pos.castleRights;
            nullUndo.halfClock      = pos.halfClock;
            nullUndo.zobristKey     = pos.zobristKey;
            nullUndo.pawnZobristKey = pos.pawnZobristKey;
            nullUndo.unmovedPawnSqs = pos.unmovedPawnSqs;
            nullUndo.stackLen       = pos.searchStackLen;

            if (pos.enPassantSq >= 0)
                pos.zobristKey ^= ZOBRIST_EP[pos.enPassantSq % 8];
            pos.enPassantSq = NO_SQUARE;
            pos.turn        = flipColor(pos.turn);
            pos.zobristKey ^= ZOBRIST_TURN;
            pos.halfClock++;

            // Push to search stack
            if (pos.searchStackLen < SEARCH_STACK_SIZE)
                pos.searchStack[pos.searchStackLen++] = pos.zobristKey;

            const int nullScore = -alphaBeta(pos, depth - 1 - R,
                                             -beta, -beta + 1,
                                             ply + 1, false,
                                             NULL_MOVE, NULL_MOVE,
                                             contempt);

            // Restore
            pos.searchStackLen  = nullUndo.stackLen;
            pos.turn            = flipColor(pos.turn);
            pos.enPassantSq     = nullUndo.enPassantSq;
            pos.castleRights    = nullUndo.castleRights;
            pos.halfClock       = nullUndo.halfClock;
            pos.zobristKey      = nullUndo.zobristKey;
            pos.pawnZobristKey  = nullUndo.pawnZobristKey;
            pos.unmovedPawnSqs  = nullUndo.unmovedPawnSqs;

            if (nullScore >= beta && std::abs(nullScore) < MATE_VAL - 100)
                return beta;
        }
    }

    // ── ProbCut ───────────────────────────────────────────────────────────────
    if (depth >= 5 && !inCheckNow && ply > 0 &&
        std::abs(beta) < MATE_VAL - 100 &&
        alpha + 1 >= beta) {  // non-PV only
        const int pcBeta  = beta + PROBCUT_MARGIN;
        const int pcDepth = depth - 4;

        // Generate all moves and try captures only, sorted MVV-LVA
        MoveList all;
        generateMoves(pos, pos.turn, false, all);

        for (int i = 0; i < all.size && !searchAborted; i++) {
            const Move& pcMv = all.moves[i];
            if (!flagIsCapture(pcMv.flags)) continue;
            if (see(pos, pcMv.to, pcMv.from) < -50) continue;

            UndoRecord pcUndo;
            pos.makeMove(pcMv, pcUndo);
            nodeCount++;

            // Qsearch pre-verification — much cheaper than full reduced search
            const int qScore = -qsearch(pos, -pcBeta, -pcBeta + 1, ply + 1);
            int pcScore = qScore;
            if (!searchAborted && qScore >= pcBeta) {
                pcScore = -alphaBeta(pos, pcDepth,
                                     -pcBeta, -pcBeta + 1,
                                     ply + 1, false,
                                     pcMv, prevMove,
                                     contempt);
            }
            pos.unmakeMove(pcMv, pcUndo);

            if (pcScore >= pcBeta) {
                ttStore(pos.zobristKey, depth, pcScore, TT_LOWER,
                        pcMv.from, pcMv.to, pcMv.promo);
                return beta;
            }
        }
    }

    // ── Razoring ─────────────────────────────────────────────────────────────
    if (depth == 1 && !inCheckNow) {
        if (rawStaticEval + 300 < alpha)
            return qsearch(pos, alpha, beta, ply);
    }

    // ── 50-move rule (after qsearch/razoring — same as JS) ───────────────────
    if (pos.halfClock >= 100) return 0;

    // ── Internal iterative deepening (IID) ───────────────────────────────────
    if (moveIsNull(ttBest) && depth >= 5 && ply > 0 && !inCheckNow) {
        alphaBeta(pos, depth - 3, alpha, beta, ply, false,
                  prevMove, prevPrevMove, contempt);
        // Re-read best move from TT after IID
        Square iidFrom = NO_SQUARE, iidTo = NO_SQUARE;
        PieceType iidPromo = NO_PIECE_TYPE;
        if (ttGetBest(pos.zobristKey, iidFrom, iidTo, iidPromo)) {
            ttBest.from  = iidFrom;
            ttBest.to    = iidTo;
            ttBest.promo = iidPromo;
        }
    }

    // ── Futility base (depths 1–6) ────────────────────────────────────────────
    const int improvingAdj  = improving ? -50 : 50;
    const bool futilityOn   = (depth >= 1 && depth <= 6 && !inCheckNow);
    const int futilityBase  = futilityOn
        ? (rawStaticEval + BASE_FUTILITY[depth] + improvingAdj)
        : INF;

    // ── LMP threshold ─────────────────────────────────────────────────────────
    const bool lmpActive = (depth >= 1 && depth <= 8 && !inCheckNow
                            && ply > 0 && alpha + 1 >= beta);
    const int lmpThreshold = lmpActive
        ? (improving ? LMP_BASE[std::min(depth, 8)]
                     : LMP_BASE[std::min(depth, 8)] / 2)
        : INT_MAX;

    // ── Multi-cut pruning ─────────────────────────────────────────────────────
    if (depth >= 8 && !inCheckNow && nullOk && ply > 0 &&
        alpha + 1 >= beta &&
        std::abs(beta) < MATE_VAL - 100) {
        // Try the first MC_TRIES moves at reduced depth
        MoveList mcAll;
        generateMoves(pos, pos.turn, false, mcAll);
        int cutCount = 0, triesDone = 0;
        for (int i = 0; i < mcAll.size && triesDone < MC_TRIES && !searchAborted; i++) {
            UndoRecord mcUndo;
            pos.makeMove(mcAll.moves[i], mcUndo);
            nodeCount++;
            const int mcScore = -alphaBeta(pos, depth - 1 - MC_RDEPTH,
                                           -beta, -beta + 1,
                                           ply + 1, false,
                                           mcAll.moves[i], prevMove,
                                           contempt);
            pos.unmakeMove(mcAll.moves[i], mcUndo);
            triesDone++;
            if (mcScore >= beta) {
                cutCount++;
                if (cutCount >= MC_MIN) return beta;
            }
        }
    }

    // ── Move generation + staged iteration ───────────────────────────────────
    MoveGenContext ctx;
    ctx.ttBest      = ttBest;
    ctx.killer1     = killerGet1(ply);
    ctx.killer2     = killerGet2(ply);
    ctx.ply         = ply;
    ctx.prevMove    = prevMove;
    ctx.prevPrevMove = prevPrevMove;
    // Countermove: the quiet move that refuted prevMove's (piece, to)
    // In this C++ translation countermoves are folded into killer2 via killerStore.
    // No separate countermove table is needed — history ordering covers this.

    StagedMoveGen gen(pos, ctx, /*qsearch=*/false);

    int  bestScore = -INF;
    Move bestMv    = NULL_MOVE;
    int  flag      = TT_UPPER;
    int  movesDone = 0;

    // Track quiet moves for bulk history update on beta cutoff
    Move quietsTried[256];
    int  quietsTriedCount = 0;

    // Track capture moves for bulk capture history update
    Move capturesTried[256];
    int  capturesTriedCount = 0;

    while (true) {
        if (searchAborted) return 0;

        const Move mv = gen.next();
        if (moveIsNull(mv)) break;

        const bool isCapture = flagIsCapture(mv.flags);
        const bool isPromo   = flagIsPromo(mv.flags);
        const bool isQuiet   = !isCapture && !isPromo;

        // ── Late Move Pruning ─────────────────────────────────────────────
        if (movesDone >= lmpThreshold && isQuiet && mv.flags != CASTLE) {
            continue;
        }

        // ── Futility pruning ──────────────────────────────────────────────
        if (futilityOn && isQuiet && mv.flags != CASTLE && movesDone > 0) {
            if (futilityBase < alpha) continue;
        }

        // ── SEE-based quiet move pruning ──────────────────────────────────
        if (depth <= 6 && !inCheckNow && ply > 0 && movesDone > 0
            && alpha + 1 >= beta
            && isQuiet && mv.flags != CASTLE) {
            if (see(pos, mv.to, mv.from) < -50 * depth) continue;
        }

        // ── History leaf pruning ──────────────────────────────────────────
        if (depth == 1 && !inCheckNow && ply > 0 && movesDone > 0
            && alpha + 1 >= beta && isQuiet && mv.flags != CASTLE) {
            const int hs   = histScore(pos.turn, mv);
            const int chs  = contHistScore(pos.turn, prevMove, mv, pos);
            const int chs2 = contHistScore2(pos.turn, prevPrevMove, mv, pos);
            if (hs + chs * 2 + chs2 < -2000) continue;
        }

        // ── Singular extension ────────────────────────────────────────────
        int extension = 0;
        const bool isTTMove = !moveIsNull(ttBest)
            && mv.from == ttBest.from && mv.to == ttBest.to;
        const bool timeOk = (timeNowMs() < searchDeadlineMs - 100);

        if (isTTMove && depth >= 8 && ply <= 4 && ply > 0 && timeOk) {
            int singTT = 0;
            if (ttProbe(pos.zobristKey, depth - 3, -INF, INF, singTT)) {
                const int singularBeta  = singTT - 60;
                const int singularDepth = depth / 2 - 1;
                int singularScore = -INF;
                int singNodes     = 0;
                const int singNodeLimit = 800;

                // Try all other moves at shallow depth
                MoveList singAll;
                generateMoves(pos, pos.turn, false, singAll);
                for (int si = 0; si < singAll.size && !searchAborted; si++) {
                    const Move& other = singAll.moves[si];
                    if (other.from == mv.from && other.to == mv.to) continue;
                    UndoRecord su;
                    pos.makeMove(other, su);
                    nodeCount++;
                    const int s = -alphaBeta(pos, singularDepth,
                                             -singularBeta - 1, -singularBeta,
                                             ply + 1, false,
                                             other, prevMove,
                                             contempt);
                    pos.unmakeMove(other, su);
                    singNodes++;
                    if (s > singularScore) singularScore = s;
                    if (singularScore >= singularBeta
                        || singNodes >= singNodeLimit
                        || searchAborted) break;
                }

                if (!searchAborted) {
                    if (singularScore < singularBeta) {
                        extension = 1;
                        // Double extension: singular + gives check
                        if (ply <= 3 && inCheckNow) extension = 2;
                    } else {
                        // Negative singular: multi-cut condition
                        if (singularScore >= beta && alpha + 1 >= beta)
                            return beta;  // hard multi-cut prune
                        extension = -1;  // soft: reduce this move
                    }
                }
            }
        }

        // ── Make the move ─────────────────────────────────────────────────
        UndoRecord undo;
        pos.makeMove(mv, undo);
        nodeCount++;

        // ── Check extension ───────────────────────────────────────────────
        if (extension == 0 && ply < MAX_PLY - 2 && pos.inCheck(pos.turn)) {
            extension = 1;
            // Double extension: check + rook/queen recapture with positive SEE
            if (ply <= 6 && !moveIsNull(prevMove)
                && flagIsCapture(mv.flags)
                && mv.to == prevMove.to) {
                const PieceType victimType = prevMove.attackerType;
                if (victimType == ROOK || victimType == QUEEN) {
                    if (see(pos, mv.to, mv.from) > 0) extension = 2;
                }
            }
        }

        // ── Threat extension ──────────────────────────────────────────────
        if (extension == 0 && ply <= 3 && depth >= 3 && !pos.inCheck(pos.turn)) {
            if (opponentHasWinningCapture(pos)) extension = 1;
        }

        // ── Recapture extension ───────────────────────────────────────────
        if (extension == 0 && extension < 2 && ply < MAX_PLY - 2
            && !moveIsNull(prevMove) && flagIsCapture(mv.flags)
            && mv.to == prevMove.to) {
            extension = 1;
        }

        // ── Passed pawn extension ─────────────────────────────────────────
        if (extension == 0 && ply < MAX_PLY - 2
            && mv.attackerType == PAWN) {
            // pos.turn has already flipped; mover was flipColor(pos.turn)
            const int movingCi = static_cast<int>(flipColor(pos.turn));
            const int destRow  = mv.to / 8;
            const bool onRank7 = (movingCi == WHITE && destRow == 1)  // white rank 7 = row 1
                              || (movingCi == BLACK && destRow == 6);  // black rank 7 = row 6
            if (onRank7) {
                const int enemyCi = 1 - movingCi;
                const Bitboard enemyPawns = pos.bb[enemyCi][PAWN];
                if ((PASSED_MASK[movingCi][mv.to] & enemyPawns) == 0)
                    extension = 1;
            }
        }

        // ── SEE pruning for losing captures ───────────────────────────────
        // (Applied post-makeMove in JS to match the undo/skip pattern)
        if (!pos.inCheck(pos.turn) && depth <= 6
            && (mv.flags == CAPTURE || mv.flags == PROMO_CAPTURE)
            && see(pos, mv.to, mv.from) < -MAT[PAWN] * depth) {
            pos.unmakeMove(mv, undo);
            movesDone++;
            continue;
        }

        // ── LMR (Late Move Reductions) ────────────────────────────────────
        int reduction = 0;
        if (extension == 0 && depth >= 3 && movesDone >= 4 && !pos.inCheck(pos.turn)) {
            if (isQuiet && mv.flags != CASTLE) {
                // Quiet LMR
                reduction = LMR_TABLE[std::min(depth, MAX_PLY - 1)]
                                     [std::min(movesDone, MAX_PLY - 1)];
                const int hs   = histScore(pos.turn, mv);
                const int chs  = contHistScore(pos.turn, prevMove, mv, pos);
                const int chs2 = contHistScore2(pos.turn, prevPrevMove, mv, pos);
                const int combinedHist = hs + chs * 2 + chs2;

                if (combinedHist > 5000)       reduction = std::max(0, reduction - 1);
                else if (combinedHist <= 0)    reduction = reduction + 1;

                if (improving)                 reduction = std::max(0, reduction - 1);
                else                           reduction = reduction + 1;

                // Cut node: extra reduction
                const bool isCutNode = (alpha + 1 >= beta) && moveIsNull(ttBest);
                if (isCutNode)                 reduction += 1;

            } else if (isCapture && movesDone >= 6) {
                // Capture LMR: reduce late poor-history captures by 1 ply
                if (capHistScore(pos.turn, mv) < -200) reduction = 1;
            }

            reduction = std::min(reduction, depth - 2);
        }

        // ── Search this move ──────────────────────────────────────────────
        int score;
        if (movesDone == 0) {
            // First move — full window (no null-window)
            score = -alphaBeta(pos,
                               depth - 1 + extension,
                               -beta, -alpha,
                               ply + 1, true,
                               mv, prevMove, contempt);
        } else {
            // Null-window search at reduced depth
            score = -alphaBeta(pos,
                               depth - 1 - reduction + extension,
                               -alpha - 1, -alpha,
                               ply + 1, true,
                               mv, prevMove, contempt);
            // Re-search at full depth if reduced search beats alpha
            if (!searchAborted && score > alpha && reduction > 0)
                score = -alphaBeta(pos,
                                   depth - 1 + extension,
                                   -alpha - 1, -alpha,
                                   ply + 1, true,
                                   mv, prevMove, contempt);
            // PVS: if inside window, re-search with full window
            if (!searchAborted && score > alpha && score < beta)
                score = -alphaBeta(pos,
                                   depth - 1 + extension,
                                   -beta, -alpha,
                                   ply + 1, true,
                                   mv, prevMove, contempt);
        }

        pos.unmakeMove(mv, undo);
        if (searchAborted) return 0;

        // Track moves tried (for history updates)
        if (isQuiet && mv.flags != CASTLE) {
            if (quietsTriedCount < 255) quietsTried[quietsTriedCount++] = mv;
        }
        if (isCapture) {
            if (capturesTriedCount < 255) capturesTried[capturesTriedCount++] = mv;
        }

        movesDone++;

        if (score > bestScore) {
            bestScore = score;
            bestMv    = mv;
            if (ply == 0) bestMoveRoot = mv;
        }
        if (score > alpha) {
            alpha = score;
            flag  = TT_EXACT;
        }
        if (score >= beta) {
            // Beta cutoff — update history tables
            if (isQuiet && mv.flags != CASTLE) {
                histBulkUpdate(pos.turn, mv,
                               quietsTried, quietsTriedCount,
                               ply, depth,
                               prevMove, prevPrevMove,
                               pos);
            }
            if (isCapture) {
                capHistBulkUpdate(pos.turn, mv,
                                  capturesTried, capturesTriedCount,
                                  depth);
            }

            ttStore(pos.zobristKey, depth, score, TT_LOWER,
                    mv.from, mv.to, mv.promo);
            return beta;
        }
    }

    // No legal moves?
    if (movesDone == 0) {
        return pos.inCheck(pos.turn) ? -(MATE_VAL - ply) : 0;
    }

    // ── Correction history update on exact score ──────────────────────────────
    if (flag == TT_EXACT && !inCheckNow && rawStaticEval != -INF) {
        corrHistUpdate(pos.turn, pos.pawnZobristKey,
                       bestScore - rawStaticEval);
    }

    ttStore(pos.zobristKey, depth, bestScore, flag,
            moveIsNull(bestMv) ? NO_SQUARE : bestMv.from,
            moveIsNull(bestMv) ? NO_SQUARE : bestMv.to,
            moveIsNull(bestMv) ? NO_PIECE_TYPE : bestMv.promo);
    return bestScore;
}

// ═══════════════════════════════════════════════════════════════════════════════
// iterativeDeepening — Top-level search driver
// ═══════════════════════════════════════════════════════════════════════════════

Move iterativeDeepening(Position& pos,
                        int maxDepth,
                        int moveTimeMs,
                        int contempt,
                        const SearchInfoCallback& onInfo)
{
    // ── Initialise search state ───────────────────────────────────────────────
    searchAborted    = false;
    searchStartMs    = timeNowMs();
    searchDeadlineMs = searchStartMs + static_cast<long long>(moveTimeMs);
    searchBudgetMs   = static_cast<long long>(moveTimeMs);
    nodeCount        = 0;
    bestMoveRoot     = NULL_MOVE;

    pos.searchStackLen = 0;
    pos.staticEvalStack.fill(-INF);

    // Clamp to safe depth ceiling
    const int safeMaxDepth = std::min(maxDepth, MAX_PLY - 1);

    // Clear per-search state (killers are position-specific)
    for (int i = 0; i < MAX_PLY; i++) {
        KILLERS[i][0] = NULL_MOVE;
        KILLERS[i][1] = NULL_MOVE;
    }

    // Bump TT age for this search (stale entries treated as lower priority)
    ttClear();  // increments ttAge by 1 (O(1), no memset)

    // ── Aspiration window state ───────────────────────────────────────────────
    // Staged widening sequence mirrors JS: 50 → 150 → 450 → INF
    static constexpr int ASPIRATION_WIDTHS[4] = { 50, 150, 450, INF };
    int bestScore      = 0;
    int aspirDelta     = ASPIRATION_WIDTHS[0];
    int aspirStage     = 0;

    // ── Best-move stability tracking ──────────────────────────────────────────
    int  stabilityCount = 0;
    Move prevBestMove   = NULL_MOVE;

    // ── Score instability tracking (Upgrade 4) ────────────────────────────────
    int prevBestScore = 0;
    // Compute a soft deadline (extended on instability)
    long long softDeadlineMs = searchStartMs
        + static_cast<long long>(moveTimeMs * SOFT_TIME_FRAC);

    // ── Iterative deepening loop ──────────────────────────────────────────────
    for (int depth = 1; depth <= safeMaxDepth; depth++) {
        if (searchAborted) break;

        // Soft time control: don't start a new depth we can't finish
        if (depth > 1) {
            const long long elapsed = timeNowMs() - searchStartMs;
            if (elapsed >= softDeadlineMs - searchStartMs) break;
        }

        // Aspiration windows (only for depth >= 4 and near-finite scores)
        int alpha, beta;
        if (depth >= 4
            && bestScore > -MATE_VAL + 100
            && bestScore <  MATE_VAL - 100) {
            alpha = bestScore - aspirDelta;
            beta  = bestScore + aspirDelta;
        } else {
            alpha = -INF;
            beta  =  INF;
        }

        int score = 0;

        // Aspiration re-search loop
        while (true) {
            score = alphaBeta(pos, depth, alpha, beta, 0, true,
                              NULL_MOVE, NULL_MOVE, contempt);

            if (searchAborted) {
                // Abort: reset aspiration so it doesn't carry into next depth
                aspirDelta = ASPIRATION_WIDTHS[0];
                aspirStage = 0;
                break;
            }

            if (score <= alpha) {
                // Failed low — widen downward
                aspirStage = std::min(aspirStage + 1, 3);
                aspirDelta = ASPIRATION_WIDTHS[aspirStage];
                alpha      = std::max(-INF, alpha - aspirDelta);
            } else if (score >= beta) {
                // Failed high — widen upward
                aspirStage = std::min(aspirStage + 1, 3);
                aspirDelta = ASPIRATION_WIDTHS[aspirStage];
                beta       = std::min(INF, beta + aspirDelta);
            } else {
                break;  // within window
            }
        }

        if (!searchAborted) {
            bestScore  = score;
            aspirDelta = ASPIRATION_WIDTHS[0];
            aspirStage = 0;

            // Emit info (UCI info line + internal info message)
            if (onInfo) {
                SearchInfo info;
                info.depth     = depth;
                info.score     = bestScore;
                info.isMate    = (std::abs(bestScore) >= MATE_VAL - MAX_PLY);
                info.nodes     = nodeCount;
                info.elapsedMs = static_cast<int>(timeNowMs() - searchStartMs);
                info.bestMove  = bestMoveRoot;
                onInfo(info);
            }
        }

        // Early exit on forced mate
        if (std::abs(bestScore) >= MATE_VAL - MAX_PLY) break;

        // ── Score instability detection (Upgrade 4) ───────────────────────
        if (!searchAborted && depth > 1) {
            const int swing = std::abs(bestScore - prevBestScore);
            if (swing > INSTABILITY_THRESH) {
                // Extend the soft deadline
                const long long extension = static_cast<long long>(
                    moveTimeMs * INSTAB_BONUS);
                softDeadlineMs = std::min(searchDeadlineMs,
                                          softDeadlineMs + extension);
            }
        }
        prevBestScore = bestScore;

        // ── Best-move stability early exit ────────────────────────────────
        if (!searchAborted && !moveIsNull(bestMoveRoot)) {
            const bool sameMove = !moveIsNull(prevBestMove)
                && bestMoveRoot.from  == prevBestMove.from
                && bestMoveRoot.to    == prevBestMove.to
                && bestMoveRoot.promo == prevBestMove.promo;

            if (sameMove) stabilityCount++;
            else          stabilityCount = 1;
            prevBestMove = bestMoveRoot;

            if (stabilityCount >= STABILITY_THRESH) {
                const long long elapsed = timeNowMs() - searchStartMs;
                if (elapsed >= static_cast<long long>(moveTimeMs * STAB_TIME_FRAC))
                    break;
            }
        }
    }

    // ── Fallback: if aborted before depth 1 completes ────────────────────────
    if (moveIsNull(bestMoveRoot)) {
        MoveList fallback;
        generateMoves(pos, pos.turn, false, fallback);
        if (fallback.size > 0) bestMoveRoot = fallback.moves[0];
    }

    return bestMoveRoot;
}
