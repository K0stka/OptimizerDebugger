# IFJ26 Optimizer Debugger

A standalone dev tool for the [IFJ26 compiler](../IFJ26): paste IR in a browser,
see the optimized IR produced by the real `optimize_ir()` from the repository —
automatically recompiled whenever the repository sources change.

The IFJ26 repository is **never modified**: its `src/*.c` files are only read
and compiled into a separate `ifj26-opt` binary owned by this project.

## Usage

    make run

Then open http://127.0.0.1:8080.

- The optimizer binary is rebuilt automatically whenever any file in
  `<repo>/src` or `opt_src/` is newer than the binary — edit the optimizer,
  save, press Optimize, and the fresh code runs. No server
  restart needed.
- Hot reload reuses unchanged object files; header edits rebuild all sources.
- The current IR is stored in the browser's local storage and survives reloads.
- Input edits automatically run the optimizer after a short pause.
- Web optimizer runs enable `--verbose` and show DEBUG output in the diagnostics drawer.
- Hover values, slots, block targets, or user-defined function names to highlight
  matching references across both panes. Local identifiers are scoped to their
  function. Built-in calls have distinct syntax coloring and no linked hover.
- Presets live in `presets/*.ir`.
- [Guide examples](presets/GUIDE.md) cover every step of
  [the optimizer tutorial](https://optimizer.krychlic.com/), with additional edge cases.

## The ifj26-opt tool (CLI)

The same binary the page uses is a normal CLI tool:

    build/release/ifj26-opt --plain < presets/01_constant_folding.ir

It reads textual IR on stdin and writes a JSON envelope (or plain IR with
`--plain`). `--no-opt` parses and prints without optimizing (round-trip
check), `--verbose` forwards the compiler's debug output to stderr.

## Configuration

Symbol highlighting checks: `node tests/highlighting.cjs`.

`config.txt`:

    repo = C:/path/to/IFJ26   # repository root containing src/
    port = 8080

CLI overrides: `--repo <path>`, `--port <n>`; `make opt` runs
`server --build-only` (compiles both flavors, no server).

Optimizer requests time out after 5 seconds; each compiler/linker command has a
60-second limit. Timed-out process trees are terminated and the UI reports an
error. Client socket reads and writes have 10-second timeouts.

Process timeout checks on Windows:

    clang -std=c17 -O2 -D_CRT_SECURE_NO_WARNINGS tests/process_timeouts.c -o build/process-timeouts.exe -lws2_32
    build/process-timeouts.exe

## The textual IR format

Defined by `opt_src/ir_text.c` (printer + parser over the repository's public
`ir.h` / `ir_helpers.h` API). Quick reference:

    .entrypoint main

    .func main : unit
    .param @n : int          # parameters precede local slots
    .local @i : int
    .entry 0                 # function entrypoint block (default 0)

    .block 0
      v0 : int = add int 2, int 3      # v<id> : <type> = <op> <operands>
      store @i v0                      # store @slot <operand>
      v1 : int = call helper, v0       # builtins by name, user fns by name
      call print, v1                   # unit-returning calls have no v<id>
      branch v1 ? 1 : 2                # exactly one terminator per block
    .block 1
      jump 0

Types: `int | double | string | bool | unit`. Operands: `v<id>`, `int 5`,
`double 1.5`, `string "..."` (escapes `\\ \" \n \t \r \xHH`), `bool true`,
`unit`. Value ids may have gaps (post-optimization output re-parses; ids are
preserved on parse).

## Layout

    Makefile        make / make run / make opt / make clean
    config.txt      repository path + port
    server.c        dev server: static files, staleness check, clang rebuild,
                    spawns ifj26-opt per request (Winsock/BSD sockets, no deps)
    opt_src/        ifj26-opt tool: ir_text.c (IR printer/parser),
                    ifj26_opt.c (main: stdin IR -> optimize_ir -> stdout JSON)
    presets/        sample IR programs (also the UI presets)
    static/         the single-page UI (vanilla JS, dark theme)
