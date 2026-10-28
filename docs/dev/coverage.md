# Test Coverage

**Status:** current as of 2026-10-28.  
**Last verified:** default build `ctest` — 23/23 passed; HTTP tree registers 25 tests (`test_http_security`, `test_http_simple` added).

This replaces the 2025-01 snapshot that referenced old filenames (`loki_lua.c`, separate `loki-repl`, “10 suites / 134 tests”). Counts below are **`TEST()` macros** in source, not a gcov line-coverage report. Automated line coverage is not wired in CI.

## How to run

```bash
make test                              # HTTP off (default)
make test-http                         # build/http with -DLOKI_ENABLE_HTTP=ON
ctest --test-dir build --output-on-failure
```

CI (`.github/workflows/ci.yml`): Ubuntu + macOS × Debug/Release × HTTP ON/OFF; trailing whitespace/tabs; Valgrind on `test_core`, `test_file_io`, `test_lua_api` only.

Framework: `tests/test_framework.h` (`TEST`, `ASSERT_*`). Suites live under `tests/loki/` (HTTP suites under `tests/`). Add a new suite by creating `tests/loki/test_foo.c` and appending `test_foo` to `LOKI_TESTS` in `CMakeLists.txt`.

## Snapshot

| Metric | Approx. |
|--------|---------|
| Editor C (`src/` + `include/`) | ~20K lines |
| Test C (`tests/`) | ~11.5K lines |
| CTest (HTTP off) | 23 executables |
| CTest (HTTP on) | 25 executables |
| `TEST()` cases (all suites including HTTP) | ~530 |

`test_linenoise_repl` is built but **not** registered with CTest (interactive).

## Suite inventory

Cases = number of `TEST(...)` definitions in that file (2026-10-28).

### Always built (`BUILD_TESTS=ON`)

| Suite | Cases | Focus |
|-------|------:|-------|
| `loki_version` | 1* | Smoke: `loki --version` |
| `test_core` | 11 | Init, status, cursor helpers |
| `test_buffers` | 16 | Multi-buffer tabs |
| `test_modal` | 26 | Modes and key dispatch |
| `test_terminal` | 25 | Raw mode / input helpers |
| `test_syntax` | 25 | Highlight rules / HLDB |
| `test_search` | 24 | Incremental search |
| `test_selection` | 32 | Selection + clipboard paths |
| `test_undo` | 27 | Undo/redo grouping |
| `test_indent` | 27 | Auto-indent / electric |
| `test_command` | 38 | Ex command parsing / builtins |
| `test_command_handlers` | 39 | Handler edge cases (shared with psnd) |
| `test_serialize` | 12 | Buffer serialize |
| `test_lua_api` | 14 | Lua bindings |
| `test_lua_modules` | 3 | Bundled `.loki/modules` against stubbed HTTP |
| `test_lang_registration` | 17 | `register_language` |
| `test_file_io` | 10 | Open/save / binary reject |
| `test_row_operations` | 29 | Row insert/delete |
| `test_async_queue` | 15 | Async event queue |
| `test_trust` | 6 | `:trust` list |
| `test_json` | 58 | JSON parser |
| `test_jsonrpc` | 42 | JSON-RPC harness |
| `test_bracket` | 8 | Matching-bracket highlight |

\* `loki_version` is a CTest command, not a `TEST()` file.

### HTTP builds only (`-DLOKI_ENABLE_HTTP=ON`)

| Suite | Cases | Focus |
|-------|------:|-------|
| `test_http_security` | 19 | URL/header/body/rate/SSRF limits |
| `test_http_simple` | 1 | Smoke request path |

## Coverage by area (qualitative)

| Area | Assessment |
|------|------------|
| JSON / JSON-RPC | Strong (`test_json`, `test_jsonrpc`) |
| Commands / modal / selection / undo / indent | Strong dedicated suites |
| Trust gating | Covered (`test_trust`) |
| HTTP security | Strong when HTTP enabled; skipped on default `make test` |
| Lua API / modules | Moderate; not every binding has a case |
| File I/O / core | Moderate |
| Terminal / treesitter / markdown render | Partial — unit tests exist for terminal; Tree-sitter and cmark paths are thinner |
| Full TUI / resize / REPL UI | Little automated coverage; rely on manual checks and Valgrind subset |
| `host.c` session render path vs direct `editor.c` loop | Mostly exercised via jsonrpc/session tests, not full VT100 |

Gaps worth closing next: ASan job in CI; Valgrind (or ASan) on `test_modal`, `test_undo`, `test_http_security`; more Lua binding and markdown/Tree-sitter cases. See also `docs/security.md` for local ASan/Valgrind recipes.

## Memory checking (local)

```bash
# Valgrind (as CI does for three binaries)
valgrind --leak-check=full --error-exitcode=1 ./build/test_core

# AddressSanitizer rebuild
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address -g -O1"
cmake --build build-asan
ASAN_OPTIONS=detect_leaks=1 ./build-asan/test_core
```

## Keeping this document honest

When adding suites, update the table and the “Last verified” line, or regenerate counts with:

```bash
rg -c '^TEST\(' tests/loki/*.c tests/test_http*.c
ctest --test-dir build -N
```

Do not treat this file as a substitute for `gcov`/`llvm-cov` until those are wired into the build.
