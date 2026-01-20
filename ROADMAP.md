# Loki Roadmap

This roadmap outlines future development for Loki, organized around the **modular architecture principle**: keep core components focused on essential infrastructure while adding capabilities through feature modules.

## Current Status (v0.5.x)

**Architecture:**
- Session-based design with opaque `EditorSession` handles
- Model/View separation (`EditorModel` for document state, `EditorView` for presentation)
- 19 test suites, 100% pass rate

**Core Modules:**

| Module | Description |
|--------|-------------|
| `core.c` | Buffer/row management, file I/O |
| `terminal.c` | Terminal I/O and raw mode |
| `renderer.c` | Screen rendering (VT100) |
| `buffers.c` | Multi-buffer management |
| `modal.c` | Vim-like modal editing |
| `selection.c` | Text selection, OSC 52 clipboard |
| `search.c` | Incremental search |
| `syntax.c` | Syntax highlighting infrastructure |
| `languages.c` | Language definitions |
| `command.c` | Ex-style command mode |
| `undo.c` | Undo/redo with operation grouping |
| `indent.c` | Smart auto-indentation |
| `http.c` | Async HTTP with security hardening |
| `lua.c` | Lua C API bindings |
| `session.c` | Session management |
| `editor.c` | Main editor loop |

**Recently Completed:**

- Session-based architecture refactor
- HTTP module restored with security hardening (URL validation, rate limiting, size limits)
- Async HTTP client for AI/API integration
- Undo/Redo system with circular buffer
- Multiple buffers (tabs) with Ctrl-T/Ctrl-X navigation
- Modal editing with vim-like motions
- Auto-indentation with electric dedent
- CommonMark markdown support via cmark
- Tab completion in Lua REPL
- Dynamic language registration system

## Philosophy

**Modular Architecture Principle:**

- Core remains focused on essential editor infrastructure
- Features implemented as separate, composable modules
- Each module has single, well-defined responsibility
- Clear API boundaries between components
- Testable in isolation with zero tolerance for test failures

**Module Design Guidelines:**

- Self-contained with minimal coupling
- Optional (can be compiled out via CMake if not needed)
- No direct dependencies on other feature modules
- Comprehensive error handling and bounds checking

---

## Near-Term Improvements (v0.6.x)

### 1. Line Numbers Module

**Impact:** Navigation and reference
**Complexity:** Low (~120-150 lines)

**Implementation:**

- Optional gutter display with configurable width
- Relative/absolute number modes (`:set relativenumber`)
- Highlight current line number
- Toggle via command or config

---

### 2. Search Enhancements

**Impact:** More powerful text finding
**Complexity:** Medium (~150-200 lines)

**Planned additions:**

- POSIX regex support via `<regex.h>`
- Replace functionality: `:%s/find/replace/gc`
- Search history with up/down arrows
- Case sensitivity toggle (smart-case)
- Vim-style search commands: `/`, `?`, `n`, `N`, `*`, `#`

---

### 3. Bracket Matching Visualization

**Impact:** Visual pairing feedback
**Complexity:** Low (~80-100 lines)

**Implementation:**

- Highlight matching `()`, `{}`, `[]` when cursor on bracket
- Show match in status if off-screen
- Support multi-line matching

---

## Mid-Term Improvements (v0.7.x)

### 4. Split Windows

**Impact:** View multiple locations/files simultaneously
**Complexity:** High (~350-450 lines)

**Implementation:**

- Horizontal/vertical splits (vim-style)
- Each window has independent viewport into buffer
- Commands: `:split`, `:vsplit`, `Ctrl-W s/v`
- Window navigation: `Ctrl-W hjkl`

---

### 5. Macro Recording

**Impact:** Automate repetitive edits
**Complexity:** Medium (~180-220 lines)

**Implementation:**

- Record keystroke sequences: `q{a-z}` to record, `q` to stop
- Replay: `@{a-z}`, `@@` repeat last
- Count prefix: `10@a` repeats macro 10 times
- Up to 26 named registers

---

### 6. Git Integration (Phase 1)

**Impact:** VCS awareness in editor
**Complexity:** Medium (~300 lines)

**Phase 1 - Read-only awareness:**

- Git status in status line (branch, dirty state)
- Diff markers in gutter (+/-/~ for add/delete/modify)
- Jump to next/prev hunk (`]c` / `[c`)

---

## Long-Term Vision (v0.8.x+)

### 7. Plugin System Formalization

**Impact:** User extensibility
**Complexity:** Low (~150 lines additional)

**Current state:** Lua already embedded with extensive API

**Enhancements needed:**

- Formalize plugin discovery (scan `.loki/plugins/`)
- Plugin metadata (`name`, `version`, `author`)
- Plugin lifecycle hooks (`on_load`, `on_save`, `on_exit`)
- Plugin manager commands (`:PluginList`, `:PluginEnable`)

---

### 8. Mouse Support

**Complexity:** Low (~100 lines)

- Enable SGR mouse mode
- Click to position cursor
- Drag to select text
- Scroll wheel support

---

## Non-Goals

Things we explicitly **won't** add (preserves minimalist identity):

- **GUI version** - Terminal-native is core identity
- **Complex project management** - Use external tools
- **Debugger integration** - Use gdb/lldb directly
- **Built-in terminal** - Use tmux/screen
- **LSP client** - Too complex for minimalist editor
- **Tree-sitter** - Overkill for current use cases
- **Package manager** - Lua plugins are just files

---

## Quality Gates

Before adding any feature:

1. Does it require core changes? If yes, strong justification needed
2. Can it be a separate module? If yes, make it optional
3. Can it be done in Lua? If yes, document as example instead

**All changes must:**

- Pass all tests (`make test`)
- Have no compiler warnings (`-Wall -Wextra -pedantic`)
- Include documentation updates

---

## Contributing

**Ways to contribute:**

1. Language definitions - Add to `.loki/languages/`
2. Themes - Add to `.loki/themes/`
3. Lua modules - Editor utilities, AI integrations
4. Core modules - New C modules following architecture
5. Bug fixes and documentation

**Module contribution guidelines:**

- Must be self-contained with clear API
- Must include unit tests
- Must be optional (can compile out)
- Must follow coding style (C99, bounds checking)
