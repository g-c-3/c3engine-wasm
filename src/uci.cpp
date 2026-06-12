// ═══════════════════════════════════════════════════════════════════════════════
// uci.cpp — UCI protocol handler, option state, output helpers, WASM exports
//
// C3Engine — JS → C++ translation
//
// This file is the engine's front door. It owns:
//
//   • Option state     — uciContempt, uciHashMB, uciC3Mode
//   • emit()           — thread-safe stdout writer (locks cout for atomicity)
//   • uciIdent()       — print id + options + uciok
//   • handlePosition() — parse "position startpos|fen|c3 [moves …]"
//   • handleGo()       — parse "go depth|movetime|wtime/btime …", run search
//   • handleLine()     — dispatch one raw UCI line to the right handler
//   • uciLoop()        — stdin/stdout loop (native build)
//   • c3_uci_command() — WASM entry point (called by JS glue per UCI line)
//   • c3_stop()        — WASM stop entry point (called by JS glue on stop)
//
// ── JS → C++ translation notes ──────────────────────────────────────────────
//   JS self.postMessage({type:'uci', data: …})
//     → emit(line) — writes to stdout with '\n', flushed immediately.
//       In native mode stdout is the UCI pipe. In WASM mode Emscripten
//       intercepts stdout writes so the JS glue can forward them to the GUI.
//
//   JS self.onmessage dispatcher (msg.type === 'uci' | 'isready' | …)
//     → handleLine(raw) — splits on whitespace, dispatches by first token.
//
//   JS msg.type === 'position'  → handlePosition()
//   JS msg.type === 'uci_go'   → handleGo()
//   JS msg.type === 'uci'      → uciIdent()
//   JS msg.type === 'isready'  → emit("readyok")
//   JS msg.type === 'setoption'→ parseSetoption()
//   JS msg.type === 'ucinewgame' → fullReset()
//   JS msg.type === 'stop'     → stopSearch()
//   JS msg.type === 'quit'     → exit(0)
//
//   JS uciContempt (int, 0-100, default 25) → file-static uciContempt
//   JS uciHashSize (int, 1-512, default 16) → file-static uciHashMB
//   JS uciC3Mode   (bool, default false)    → file-static uciC3Mode
//
//   JS const STARTPOS_FEN = '…'           → constexpr STARTPOS_FEN[]
//
//   JS Math.floor(myTime/30) + Math.floor(myInc/2)
//     → same formula in allocTime()
//
//   JS mateIn = Math.ceil((MATE_VAL - |score|) / 2)
//     → same in formatScore()
//
//   JS info depth … score cp … nodes … nps … time … pv …
//     → formatInfo() / onSearchInfo()
//
// ── WASM / Native split ──────────────────────────────────────────────────────
//   Native build: main() (in main.cpp) calls uciLoop() which reads stdin
//   line-by-line and calls handleLine().
//
//   WASM build: main() calls initAll() then returns; the JS glue drives the
//   engine by calling c3_uci_command(line) and c3_stop() via ccall/cwrap.
//   Those two functions are the only EMSCRIPTEN_KEEPALIVE exports.
//   stdout in the WASM module is line-buffered by Emscripten; every emit()
//   call ends with '\n' which flushes to Module.print(), which the JS worker
//   wrapper intercepts and forwards to the GUI as UCI text.
//
// ── Thread safety ────────────────────────────────────────────────────────────
//   UCI is single-threaded: the search runs synchronously in the same thread
//   as the parser (Web Workers are single-threaded; native builds do not use
//   std::thread here). stopSearch() sets a bool that alphaBeta polls; it is
//   safe to call from a signal handler or a JS setTimeout.
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "uci.h"
#include "types.h"
#include "board.h"
#include "bitboard.h"
#include "zobrist.h"
#include "tt.h"
#include "history.h"
#include "book.h"
#include "eval.h"
#include "search.h"
#include "movegen.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>

#ifdef __EMSCRIPTEN__
#  include <emscripten.h>
#endif

// ─── Constants ───────────────────────────────────────────────────────────────

// Standard chess starting position — used by "position startpos".
static constexpr const char* STARTPOS_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// Standard back-rank piece order for book-gate check (C3 variant detection).
// If the starting position doesn't have exactly this order on both back ranks,
// the opening book is skipped (Zobrist keys won't match).
static constexpr const char STD_BACK[8] = { 'r','n','b','q','k','b','n','r' };

// ─── Option state ─────────────────────────────────────────────────────────────
// File-static — owned here, never touched by search or eval.

static int  uciContempt = 25;    // ContemptValue spin 0-100, default 25 cp
static int  uciHashMB   = 16;    // Hash spin 1-512, default 16 MB
static bool uciC3Mode   = false; // C3Mode check — enables variant unmoved-pawn rules

// ─── Engine-wide Position object ──────────────────────────────────────────────
// A single Position, set up by handlePosition() and consumed by handleGo().
// Re-initialised on every "position" command and on ucinewgame.
static Position gPos;

// ─── emit ────────────────────────────────────────────────────────────────────
// Write one UCI output line to stdout and flush immediately.
// All UCI output goes through this function.
//
// In WASM, Emscripten routes stdout to Module.print() (set by the JS worker
// wrapper), which then posts the line to the GUI. The '\n' acts as the flush
// trigger because Emscripten's stdout is line-buffered.
//
// In native builds, flush after every line so GUI tools (Arena, Cute Chess,
// python-chess) see output without buffering delays.
static void emit(const std::string& line) {
    std::cout << line << '\n';
    std::cout.flush();
}

// ─── formatScore ─────────────────────────────────────────────────────────────
// Convert a centipawn score to a UCI score string.
//   • |score| >= MATE_VAL - MAX_PLY  →  "score mate ±N"
//   • otherwise                       →  "score cp N"
// mateIn: same formula as JS: ceil((MATE_VAL - |score|) / 2)
static std::string formatScore(int score) {
    if (std::abs(score) >= MATE_VAL - MAX_PLY) {
        int mateIn = static_cast<int>(
            std::ceil((MATE_VAL - std::abs(score)) / 2.0));
        if (score < 0) mateIn = -mateIn;
        return "score mate " + std::to_string(mateIn);
    }
    return "score cp " + std::to_string(score);
}

// ─── moveToUci ───────────────────────────────────────────────────────────────
// Format a Move as a UCI string: "e2e4", "e7e8q", "0000" for NULL_MOVE.
// Mirrors JS: sqName(mv.from) + sqName(mv.to) + (mv.promo || '')
static std::string moveToUci(const Move& mv) {
    if (moveIsNull(mv)) return "0000";
    std::string s = sqName(mv.from) + sqName(mv.to);
    if (flagIsPromo(mv.flags) && mv.promo != NO_PIECE_TYPE)
        s += PIECE_CHAR[mv.promo];
    return s;
}

// ─── fullReset ───────────────────────────────────────────────────────────────
// Reset all search-related state for a new game.
// Mirrors JS msg.type === 'ucinewgame' / 'cleartt' handler.
static void fullReset() {
    ttClear();         // bump TT age (O(1))
    historyClear();    // zero all history tables + killers
    pawnHashClear();   // reset pawn hash
    gPos = Position(); // reset board state
}

// ─── uciIdent ────────────────────────────────────────────────────────────────
// Respond to the "uci" command.
// Mirrors JS: postMessage id name / id author / option … / uciok.
static void uciIdent() {
    emit("id name C3Engine");
    emit("id author C3Engine Dev");
    // Declare supported options — must match parseSetoption() below.
    emit("option name C3Mode type check default false");
    emit("option name ContemptValue type spin default 25 min 0 max 100");
    emit("option name Hash type spin default 16 min 1 max 512");
    emit("uciok");
}

// ─── parseSetoption ──────────────────────────────────────────────────────────
// Parse "setoption name <Name> value <Value>" and apply the option.
// The full line (everything after the "setoption" token) is passed in.
// Mirrors JS msg.type === 'setoption' with nameMatch[1] / nameMatch[2].
//
// Unknown option names are silently ignored per the UCI specification.
static void parseSetoption(const std::string& rest) {
    // Split on "value" keyword — names and values may contain spaces.
    // "name Hash value 64" → name="Hash", val="64"
    // "name C3Mode value true" → name="C3Mode", val="true"
    const std::string NAME_KW  = "name ";
    const std::string VALUE_KW = " value ";

    auto namePos = rest.find(NAME_KW);
    if (namePos == std::string::npos) return;
    namePos += NAME_KW.size();

    auto valPos = rest.find(VALUE_KW, namePos);
    if (valPos == std::string::npos) return;

    std::string optName = rest.substr(namePos, valPos - namePos);
    std::string optVal  = rest.substr(valPos + VALUE_KW.size());

    // Trim whitespace from both ends.
    auto trim = [](std::string& s) {
        while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
        while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
    };
    trim(optName);
    trim(optVal);

    // Case-insensitive comparison helper.
    auto iequal = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
                return false;
        return true;
    };

    if (iequal(optName, "c3mode")) {
        // "true" enables variant unmoved-pawn rules; anything else disables.
        uciC3Mode = iequal(optVal, "true");

    } else if (iequal(optName, "contemptvalue")) {
        int v = std::atoi(optVal.c_str());
        uciContempt = std::max(0, std::min(100, v));

    } else if (iequal(optName, "hash")) {
        int mb = std::atoi(optVal.c_str());
        if (mb >= 1 && mb <= 512) {
            uciHashMB = mb;
            ttResize(mb); // reallocate TT immediately; existing entries lost
        }
    }
    // Unrecognised option names → silent ignore (UCI spec §3.2).
}

// ─── derivePawnSqs ───────────────────────────────────────────────────────────
// Parse the piece-placement field of a FEN and return a list of pawn squares
// that should be treated as unmoved (eligible for variant double-push).
//
//   • C3 mode  → every pawn in the FEN is unmoved (variant starting pos).
//   • Standard → only rank-2 white pawns and rank-7 black pawns are unmoved.
//
// Mirrors JS derivation of initUnmovedPawns in the position handler.
static std::vector<Square> derivePawnSqs(const std::string& fen, bool c3mode) {
    std::vector<Square> result;
    // Extract piece-placement field (first token).
    std::string placement = fen;
    auto sp = fen.find(' ');
    if (sp != std::string::npos) placement = fen.substr(0, sp);

    int row = 0, file = 0;
    for (char ch : placement) {
        if (ch == '/') { row++; file = 0; }
        else if (ch >= '1' && ch <= '8') { file += ch - '0'; }
        else {
            Color     color = (ch >= 'A' && ch <= 'Z') ? WHITE : BLACK;
            PieceType type  = charToPieceType(ch);
            if (type == PAWN) {
                int sq   = row * 8 + file;
                int rank = 8 - row;   // sqRank: rank8 = row0, rank1 = row7
                bool stdUnmoved = (color == WHITE && rank == 2) ||
                                  (color == BLACK && rank == 7);
                if (c3mode || stdUnmoved)
                    result.push_back(sq);
            }
            file++;
        }
    }
    return result;
}

// ─── isStandardBackRank ──────────────────────────────────────────────────────
// Return true if the position looks like a standard chess starting position
// (both back ranks match the canonical RNBQKBNR order).
// Used to gate opening book access — the book was generated from the standard
// start, so variant shuffled positions must bypass it.
// Mirrors JS const isStandardStart = … check in the 'go' handler.
static bool isStandardBackRank(const Position& pos) {
    // White back rank: row 7 = squares 56-63.
    // Black back rank: row 0 = squares 0-7.
    const PieceType expected[8] = {
        ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK
    };
    for (int f = 0; f < 8; f++) {
        // White: sq 56+f
        const Piece& wp = pos.pieceAt[56 + f];
        if (wp.color != WHITE || wp.type != expected[f]) return false;
        // Black: sq f
        const Piece& bp = pos.pieceAt[f];
        if (bp.color != BLACK || bp.type != expected[f]) return false;
    }
    return true;
}

// ─── handlePosition ──────────────────────────────────────────────────────────
// Parse and apply a "position" command, setting up gPos ready for handleGo().
//
// Formats accepted:
//   position startpos [moves <uci>...]
//   position fen <fen-string> [moves <uci>...]
//   position c3 <fen-string> [moves <uci>...]   ← variant mode
//
// For "c3" the caller supplies the variant starting FEN (randomised back rank).
// For "startpos" the standard FEN is used; uciC3Mode controls unmoved-pawn scope.
//
// Mirrors JS msg.type === 'position' handler in its entirety, including the
// derivation of initUnmovedPawns and the subsequent replayMoves() call.
static void handlePosition(const std::string& rest) {
    // Tokenise.
    std::istringstream ss(rest);
    std::vector<std::string> tokens;
    std::string tok;
    while (ss >> tok) tokens.push_back(tok);

    if (tokens.empty()) return;

    std::string fen;
    bool isC3 = uciC3Mode; // inherit global; overridden by explicit "c3" keyword
    size_t idx = 0;

    if (tokens[idx] == "startpos") {
        fen = STARTPOS_FEN;
        idx++;
    } else if (tokens[idx] == "c3") {
        // C3 mode: FEN tokens follow until "moves".
        isC3 = true;
        idx++;
        std::vector<std::string> fenParts;
        while (idx < tokens.size() && tokens[idx] != "moves")
            fenParts.push_back(tokens[idx++]);
        for (size_t i = 0; i < fenParts.size(); ++i)
            fen += (i ? " " : "") + fenParts[i];
    } else if (tokens[idx] == "fen") {
        idx++;
        std::vector<std::string> fenParts;
        while (idx < tokens.size() && tokens[idx] != "moves")
            fenParts.push_back(tokens[idx++]);
        for (size_t i = 0; i < fenParts.size(); ++i)
            fen += (i ? " " : "") + fenParts[i];
    } else {
        return; // unrecognised format
    }

    if (fen.empty()) return;

    // Collect UCI half-moves (everything after "moves" keyword).
    std::vector<std::string> uciMoves;
    if (idx < tokens.size() && tokens[idx] == "moves") {
        idx++;
        while (idx < tokens.size())
            uciMoves.push_back(tokens[idx++]);
    }

    // Derive unmoved-pawn squares from the FEN piece placement.
    // These are passed to initFromFen() so variant double-push works during
    // replayMoves() — the same order as the JS position handler.
    std::vector<Square> initPawnSqs = derivePawnSqs(fen, isC3);

    // Initialise board from FEN.
    gPos = Position();
    gPos.initFromFen(fen, initPawnSqs);

    // Replay moves and build full game history for repetition detection.
    // replayMoves() records gPos.zobristKey after each half-move into
    // gPos.gameHistory, exactly as the JS replayMoves() does.
    gPos.replayMoves(uciMoves);
}

// ─── allocTime ───────────────────────────────────────────────────────────────
// Compute a move-time budget (ms) from wtime/btime/winc/binc tokens.
// Formula mirrors JS: floor(myTime/30) + floor(myInc/2), clamped to >= 50ms.
static int allocTime(int myTime, int myInc) {
    return std::max(50, myTime / 30 + myInc / 2);
}

// ─── onSearchInfo ─────────────────────────────────────────────────────────────
// SearchInfoCallback passed into iterativeDeepening().
// Fires after every completed depth. Emits one UCI "info …" line.
// Mirrors JS uciInfoMode block + legacy 'info' postMessage in search().
static void onSearchInfo(const SearchInfo& info) {
    // Avoid division by zero in NPS calculation.
    const long long elapsedSafe = std::max(1, info.elapsedMs);
    const long long nps = (info.nodes * 1000LL) / elapsedSafe;

    std::string scoreStr = formatScore(info.score);
    std::string pvStr;
    if (!moveIsNull(info.bestMove))
        pvStr = " pv " + moveToUci(info.bestMove);

    // UCI info line — identical format to Stockfish / the JS engine's uciInfoMode output.
    emit("info depth "    + std::to_string(info.depth)        +
         " "              + scoreStr                           +
         " nodes "        + std::to_string(info.nodes)        +
         " nps "          + std::to_string(nps)               +
         " time "         + std::to_string(info.elapsedMs)    +
         pvStr);
}

// ─── handleGo ────────────────────────────────────────────────────────────────
// Parse a "go …" command and run the search on gPos.
//
// Supported sub-commands:
//   go depth <n>
//   go movetime <ms>
//   go wtime <ms> btime <ms> [winc <ms>] [binc <ms>] [movestogo <n>]
//   go infinite
//
// After the search completes, emit "bestmove <uci>".
// Mirrors JS msg.type === 'uci_go' handler.
static void handleGo(const std::string& rest) {
    std::istringstream ss(rest);
    std::vector<std::string> tokens;
    std::string tok;
    while (ss >> tok) tokens.push_back(tok);

    int maxDepth = MAX_PLY;
    int moveTime = 5000; // fallback: 5 seconds

    // Parse go parameters.
    int wtime = 0, btime = 0, winc = 0, binc = 0;
    bool hasTimeControl = false;
    bool infinite       = false;

    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string& t = tokens[i];

        if (t == "depth" && i + 1 < tokens.size()) {
            maxDepth = std::max(1, std::min(std::atoi(tokens[++i].c_str()), MAX_PLY));
            moveTime = 60'000; // no wall-clock limit when explicit depth given
        }
        else if (t == "movetime" && i + 1 < tokens.size()) {
            moveTime = std::max(1, std::atoi(tokens[++i].c_str()));
        }
        else if (t == "wtime" && i + 1 < tokens.size()) {
            wtime = std::max(0, std::atoi(tokens[++i].c_str()));
            hasTimeControl = true;
        }
        else if (t == "btime" && i + 1 < tokens.size()) {
            btime = std::max(0, std::atoi(tokens[++i].c_str()));
            hasTimeControl = true;
        }
        else if (t == "winc" && i + 1 < tokens.size()) {
            winc = std::max(0, std::atoi(tokens[++i].c_str()));
        }
        else if (t == "binc" && i + 1 < tokens.size()) {
            binc = std::max(0, std::atoi(tokens[++i].c_str()));
        }
        else if (t == "infinite") {
            infinite = true;
            moveTime = 300'000; // 5-minute ceiling for "infinite" searches
            maxDepth = MAX_PLY;
        }
        // movestogo is intentionally ignored here; the simple allocTime() formula
        // already handles reasonable time allocation without it.
    }

    // Apply time control formula if wtime/btime were given.
    if (hasTimeControl && !infinite) {
        const int myTime = (gPos.turn == WHITE) ? wtime : btime;
        const int myInc  = (gPos.turn == WHITE) ? winc  : binc;
        moveTime = allocTime(myTime, myInc);
    }

    // ── Opening book probe ────────────────────────────────────────────────────
    // Only try the book for standard-position games (C3 variant back ranks
    // produce Zobrist keys that never match the book).
    // Mirrors JS isStandardStart gate in the 'go' handler.
    if (!uciC3Mode && isStandardBackRank(gPos)) {
        Move bookMv = bookMove(gPos);
        if (!moveIsNull(bookMv)) {
            emit("bestmove " + moveToUci(bookMv));
            return;
        }
    }

    // ── Run search ────────────────────────────────────────────────────────────
    const Move best = iterativeDeepening(gPos, maxDepth, moveTime,
                                         uciContempt, onSearchInfo);

    // Emit bestmove — always required, even after a "stop" (UCI spec §3.2).
    emit("bestmove " + moveToUci(best));
}

// ─── handleLine ──────────────────────────────────────────────────────────────
// Dispatch one raw UCI input line to the correct handler.
// Mirrors JS self.onmessage dispatcher (msg.type switch).
//
// Lines with leading/trailing whitespace are handled; empty lines are ignored.
void handleLine(const std::string& rawLine) {
    // Trim leading whitespace.
    size_t start = rawLine.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return; // blank line
    const std::string line = rawLine.substr(start);

    // Extract the first token (command word).
    size_t spacePos = line.find_first_of(" \t");
    std::string cmd  = (spacePos == std::string::npos) ? line : line.substr(0, spacePos);
    std::string rest = (spacePos == std::string::npos) ? ""   : line.substr(spacePos + 1);

    // ── UCI command dispatch ──────────────────────────────────────────────────

    if (cmd == "uci") {
        // Engine identification: id lines + options + uciok.
        uciIdent();

    } else if (cmd == "isready") {
        // Synchronisation check — respond immediately; no deferred init needed.
        emit("readyok");

    } else if (cmd == "setoption") {
        // "setoption name <N> value <V>"
        // rest = "name Hash value 64"
        parseSetoption(rest);

    } else if (cmd == "ucinewgame") {
        // Reset all game state for a new game.
        // Mirrors JS msg.type === 'ucinewgame' / 'cleartt' handler.
        fullReset();

    } else if (cmd == "position") {
        // "position startpos [moves …]" or "position fen … [moves …]"
        handlePosition(rest);

    } else if (cmd == "go") {
        // "go depth N" / "go movetime N" / "go wtime N btime N …"
        handleGo(rest);

    } else if (cmd == "stop") {
        // Abort any running search. The search polls searchAborted every 2048
        // nodes and will emit "bestmove" itself when it unwinds.
        // In the synchronous (single-threaded) model this only matters if
        // "stop" arrives mid-search in the WASM async path.
        stopSearch();

    } else if (cmd == "quit") {
        // Terminate the engine process.
        stopSearch();
        std::exit(0);

    }
    // All unrecognised commands are silently ignored (UCI spec §3.1).
}

// ─── uciLoop ─────────────────────────────────────────────────────────────────
// Blocking stdin → handleLine() loop for the native build.
// Reads lines until EOF or "quit".
// Called from main() in main.cpp (native build only).
void uciLoop() {
    std::string line;
    while (std::getline(std::cin, line)) {
        handleLine(line);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// WASM entry points
//
// These two functions are exported via EMSCRIPTEN_KEEPALIVE and declared in
// CMakeLists.txt under EXPORTED_FUNCTIONS (_c3_uci_command, _c3_stop).
//
// The JS glue (c3engine.js wrapper) calls them via cwrap/ccall:
//
//   const c3_uci_command = Module.cwrap('c3_uci_command', null, ['string']);
//   const c3_stop        = Module.cwrap('c3_stop',        null, []);
//
//   // Forward every UCI line from the GUI to the engine:
//   c3_uci_command("position startpos moves e2e4");
//   c3_uci_command("go movetime 3000");
//
//   // Abort a running search:
//   c3_stop();
//
// In the WASM build main() (in main.cpp) runs initAll() and returns; these
// two exported C functions drive the engine for the rest of its lifetime.
// They are only compiled when targeting Emscripten.
// ═══════════════════════════════════════════════════════════════════════════════

#ifdef __EMSCRIPTEN__

extern "C" {

// c3_uci_command — receive one UCI line from the JS worker wrapper and
// process it synchronously. The engine calls emit() for any output, which
// Emscripten routes to Module.print() → the JS wrapper's stdout handler.
EMSCRIPTEN_KEEPALIVE
void c3_uci_command(const char* line) {
    if (line) handleLine(std::string(line));
}

// c3_stop — abort any running search.
// The JS glue calls this when the user clicks "stop" or the GUI sends "stop"
// over the UCI pipe. The search polls searchAborted every 2048 nodes, so
// the abort takes effect within a few microseconds.
EMSCRIPTEN_KEEPALIVE
void c3_stop() {
    stopSearch();
}

} // extern "C"

#endif // __EMSCRIPTEN__
