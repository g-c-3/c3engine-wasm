cmake_minimum_required(VERSION 3.20)
project(C3Engine VERSION 1.0.0 LANGUAGES CXX)

# ── Language standard ────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Source files ─────────────────────────────────────────────────────────────────
# Listed in dependency order (headers-first layers).
# Each .cpp has a matching .h except main.cpp (entry point only).
set(SOURCES
    src/bitboard.cpp     # uint64_t ops, attack tables, ray masks
    src/zobrist.cpp      # splitmix32 PRNG, Zobrist key arrays
    src/board.cpp        # Position state, make/unmake, isAttackedBy
    src/movegen.cpp      # variant-aware legal move generation
    src/tt.cpp           # dual-bucket transposition table + pawn hash
    src/history.cpp      # killer, butterfly, continuation, capture, correction history
    src/book.cpp         # opening book data + weighted-random lookup
    src/eval.cpp         # PSTs, all evaluation components, evaluate()
    src/search.cpp       # qsearch, alphaBeta, iterativeDeepening, time manager
    src/uci.cpp          # UCI parser, option state, output helpers
    src/main.cpp         # entry point — stdin/stdout loop + WASM exports
)

add_executable(c3engine ${SOURCES})
target_include_directories(c3engine PRIVATE src)

# ── Warnings (both builds) ───────────────────────────────────────────────────────
target_compile_options(c3engine PRIVATE
    -Wall
    -Wextra
    -Wshadow
    -Wno-unused-parameter   # generated tables trigger this harmlessly
)

# ════════════════════════════════════════════════════════════════════════════════
# WASM build  (cmake -DCMAKE_TOOLCHAIN_FILE=.../Emscripten.cmake)
# ════════════════════════════════════════════════════════════════════════════════
if(EMSCRIPTEN)

    set_target_properties(c3engine PROPERTIES
        OUTPUT_NAME "c3engine"
        SUFFIX ".js"              # Emscripten emits c3engine.js + c3engine.wasm
    )

    target_compile_options(c3engine PRIVATE
        -O3
        -DNDEBUG
        -fno-rtti               # no RTTI needed; saves ~10 KB
        -fno-exceptions         # no exceptions; engine uses error codes
    )

    # ── Emscripten linker flags ──────────────────────────────────────────────────
    # SHELL: prevents CMake from de-duplicating -s flags that look identical.
    target_link_options(c3engine PRIVATE
        -O3

        # Memory — start lean; ALLOW_MEMORY_GROWTH handles setoption Hash expansion.
        # C3Engine has no NNUE network, so initial footprint is small:
        #   16 MB TT (setoption Hash default) + ~8 MB code/stack/data = ~24 MB needed.
        # 32 MB gives comfortable headroom without pre-committing unnecessary RAM.
        # (Stockfish's larger figure includes a 40 MB NNUE network — not applicable here.)
        "SHELL:-s WASM=1"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s INITIAL_MEMORY=33554432"     # 32 MB  (16 MB TT + code/stack headroom)
        "SHELL:-s MAXIMUM_MEMORY=536870912"    # 512 MB (setoption Hash 512 ceiling)
        "SHELL:-s STACK_SIZE=1048576"          # 1 MB stack

        # Exported C functions (must be prefixed with _)
        "SHELL:-s EXPORTED_FUNCTIONS=_main,_c3_uci_command,_c3_stop"

        # Runtime helpers exposed to JS
        "SHELL:-s EXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8"

        # Web Worker environment — no window, no document
        "SHELL:-s ENVIRONMENT=worker"

        # Wrap the output in a factory function so multiple instances are possible
        "SHELL:-s MODULARIZE=1"
        "SHELL:-s EXPORT_NAME=C3Engine"

        # Keep the runtime alive after main() returns (UCI loop is infinite in native;
        # in WASM we return from main() and drive the loop via emscripten_set_main_loop)
        "SHELL:-s NO_EXIT_RUNTIME=1"

        # Separate .wasm file (do NOT use SINGLE_FILE — .wasm can be 1+ MB and
        # inlining it as base64 bloats the .js and prevents streaming compilation)
        "SHELL:-s SINGLE_FILE=0"

        # Assertions off in release; set to 1 locally when debugging memory issues
        "SHELL:-s ASSERTIONS=0"
    )

# ════════════════════════════════════════════════════════════════════════════════
# Native build  (cmake -DCMAKE_BUILD_TYPE=Release)
# ════════════════════════════════════════════════════════════════════════════════
else()

    target_compile_options(c3engine PRIVATE
        -O3
        -DNDEBUG
        -march=native           # use host CPU features (BMI2, POPCNT, etc.)
    )

    # Link pthreads on Linux for std::chrono on some toolchains
    find_package(Threads REQUIRED)
    target_link_libraries(c3engine PRIVATE Threads::Threads)

endif()

# ── Install (optional — useful for packaging the WASM pair) ─────────────────────
if(EMSCRIPTEN)
    install(FILES
        "${CMAKE_BINARY_DIR}/c3engine.js"
        "${CMAKE_BINARY_DIR}/c3engine.wasm"
        DESTINATION dist
    )
endif()
