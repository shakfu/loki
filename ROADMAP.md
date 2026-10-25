# Loki Roadmap

This document separates behavior that exists in the repository today from work still planned. Status below is based on source and build configuration; it does not claim that tests have been run for this snapshot.

## Current State

Loki is a terminal text editor written in C, with embedded Lua and an optional async HTTP feature. CMake currently requires C11, POSIX support, Lua, libuv, and the vendored cmark and linenoise/Tree-sitter components. HTTP support is optional and enabled with `-DLOKI_ENABLE_HTTP=ON`.

The codebase includes:

- Editor sessions with an opaque public handle, editor model/view structures, and a JSON-RPC stdio harness.

- Terminal input and rendering, modal editing, multiple buffers/tabs, selection, line numbers, and word wrapping.

- Literal incremental search, basic current-line substitution, undo/redo, automatic indentation with electric dedent, and matching-bracket highlighting.

- Lua configuration and editor APIs, a REPL, Lua modules, and dynamic language registration.

- Syntax highlighting through built-in rules and Tree-sitter integrations, plus CommonMark rendering through cmark.

- An optional libcurl-based asynchronous HTTP client.

The repository has focused unit-test executables configured in CMake. The exact registered test set depends on build options; this roadmap does not assert a pass rate.

### Known gaps in existing features

- Line numbers are absolute only. Relative numbering, current-line number highlighting, and a command/config toggle are not implemented. `:set` currently supports word wrap only.

- Search is literal. It has no regex engine, search history, smart-case option, or complete Vim-style search command set. Substitution applies to the current line and lacks confirmation and whole-file operation.

- Mouse input is not implemented. `modal.c` has a click handler for `EVENT_MOUSE`, but the terminal layer neither enables mouse reporting nor produces mouse events.

- Lua configuration and modules are available, but there is no formal plugin discovery, metadata, lifecycle, or plugin-manager interface.

- Internal-host blocking for HTTP is checked at connect time, so it does not apply when an HTTP proxy resolves the destination. See `docs/security.md`.

## Planned Work

The following items are proposals and are not represented as completed features.

### Near term

#### Search improvements

- Add search history and repeat/previous-match commands.

- Support forward and backward search commands, including `n` and `N` navigation.

- Add smart-case behavior and document case handling.

- Extend substitution to ranges and the whole buffer, with optional confirmation.

- Evaluate regex support as a separate enhancement; keep literal search available.

#### Line number settings

- Add relative and hybrid number modes.

- Distinguish the current line number visually.

- Expose settings through a command and Lua/config API.

### Mid term

#### Split windows

- Add horizontal and vertical splits with independent viewports.

- Add split creation and window-navigation commands.

- Define how windows share buffers and how closing a buffer affects its windows.

#### Macro recording

- Record and replay named keystroke sequences.

- Support repeating the last macro and numeric repeat counts.

- Define a bounded storage and error policy for macro execution.

#### Git awareness

- Show branch and working-tree state in the status area.

- Display line change markers in the gutter.

- Navigate between diff hunks.

- Keep the first version read-only and avoid running shell commands with interpolated user input.

### Longer term

#### Lua plugin conventions

- Define a plugin directory and deterministic loading rules.

- Add optional metadata and lifecycle hooks.

- Add commands to list and enable/disable plugins if the configuration model needs them.

- Keep ordinary Lua modules usable without a plugin manager.

#### Mouse interaction

- Add terminal mouse-mode enable/disable with reliable cleanup, and parse SGR mouse reports into `EVENT_MOUSE`.

- Support left-click cursor positioning through the existing `modal.c` handler.

- Support drag selection and wheel scrolling.

- Preserve keyboard operation when mouse reporting is unavailable.

## Non-goals

The project intends to remain terminal-native and focused on editing. A GUI, built-in terminal, debugger, full project-management environment, and LSP client are outside the current scope. Tree-Sitter is already part of the current build for syntax highlighting, so it is not a non-goal; adding unrelated parser features should still be weighed against dependency and maintenance cost. A package manager is not currently planned; Lua modules should remain easy to load directly.

## Development Principles

- Keep modules focused and APIs explicit; split functionality into modules when that reduces coupling.

- Prefer existing Lua extension points when they can provide a maintainable feature.

- Make a feature optional at build time when its dependencies or runtime cost justify that choice; current build options do not make every module optional.

- Use the C11 and POSIX environment configured by CMake, and keep compiler warnings enabled (`-Wall -Wextra -pedantic`).

- Add focused tests for behavior changes, update user-facing documentation, and run the relevant configured checks before describing a change as verified.

## Contribution Areas

- Language definitions and themes under `.loki/`.

- Lua modules and editor utilities.

- Tests, documentation, and bug fixes.

- Core features that follow the module and API principles above.
