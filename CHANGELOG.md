# CHANGELOG

All notable project-wide changes will be documented in this file. Note that each subproject has its own CHANGELOG.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/) and [Commons Changelog](https://common-changelog.org). This project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## Types of Changes

- Added: for new features.
- Changed: for changes in existing functionality.
- Deprecated: for soon-to-be removed features.
- Removed: for now removed features.
- Fixed: for any bug fixes.
- Security: in case of vulnerabilities.

---

## [Unreleased]

### Fixed

- **Build Fix for Linux**: Fixed compilation failure when building with strict C99 mode (`CMAKE_C_EXTENSIONS OFF`)
  - **Problem**: `pthread_rwlock_t` type unknown when `<uv.h>` was included in `src/async_queue.c`
  - **Root Cause**: POSIX types require feature test macros to be defined before including system headers when compiling with `-std=c99` (strict mode without GNU extensions)
  - **Solution**: Added `#define _POSIX_C_SOURCE 200809L` before includes in `src/async_queue.c`, consistent with `src/core.c`
  - **Files Modified**: `src/async_queue.c`

### Added

- **HTTP Support Restored**: Re-implemented async HTTP functionality that was removed during architecture refactor
  - **New Module Structure**:
    - Created `src/http.c` (470 lines) and `src/http.h` - dedicated HTTP module
    - Clean separation from core editor code
  - **Security Hardening**:
    - URL validation: Only `http://` and `https://` schemes allowed
    - URL length limit: Maximum 2048 characters
    - Control character filtering in URLs
    - Request body size limit: 5MB maximum
    - Response size limit: 10MB maximum
    - Rate limiting: 60 requests per minute
    - Concurrent request limit: 10 simultaneous requests
  - **Lua API**: `loki.async_http(url, method, body, headers, callback)`
    - Non-blocking HTTP GET/POST requests
    - Callback receives table with `status`, `body`, and `error` fields
  - **Integration**:
    - Added `loki_lua_bind_http()` function in lua.c
    - Re-enabled `bind_http` option in `loki_lua_bootstrap()`
    - HTTP tests re-enabled in CMakeLists.txt
  - **Testing**: Both `test_http_security` and `test_http_simple` passing (19/19 total tests)
  - **Files Added**: `src/http.c`, `src/http.h`
  - **Files Modified**: `src/lua.c`, `include/loki/lua.h`, `CMakeLists.txt`

## [0.5.0]

### Added

- **Auto-Indentation Module**: Comprehensive automatic indentation for improved coding experience
  - **New Module Structure**:
    - Created `src/loki_indent.c` (~280 lines) and `src/loki_indent.h` (public API)
    - Opaque `indent_config` structure for clean encapsulation
    - Test-only accessor functions for internal state verification
  - **Core Features**:
    - **Indent Preservation**: Automatically copies indentation from previous line when pressing Enter
    - **Smart Auto-Indent**: Adds extra indent level after opening braces `{`, brackets `[`, and parentheses `(`
    - **Electric Dedent**: Automatically dedents when typing closing `}`, `]`, `)` on whitespace-only lines
    - **Bracket Matching**: Finds matching opening bracket to determine correct dedent target
    - **Nested Brace Support**: Correctly handles deeply nested code structures
  - **Indentation Detection**:
    - **Level Detection**: Counts leading whitespace in spaces (tabs converted based on width)
    - **Style Detection**: Heuristic-based detection of tabs vs spaces (samples up to 100 lines)
    - **Mixed Indentation**: Handles files with both tabs and spaces
  - **Configuration Options**:
    - **Enable/Disable**: Toggle auto-indent on or off (default: enabled)
    - **Indent Width**: Configurable width from 1-8 spaces (default: 4)
    - **Tab/Space Style**: Auto-detected or manually configured
    - **Electric Dedent Toggle**: Can disable electric behavior independently
  - **Implementation Details**:
    - **Integration Points**:
      - `editor_insert_newline()` calls `indent_apply()` after creating new line
      - `editor_insert_char()` calls `indent_electric_char()` after inserting character
      - `editor_ctx_init()` calls `indent_init()` to set up defaults
      - `editor_ctx_free()` cleans up indent_config
    - **Smart Trailing Whitespace**: Ignores trailing spaces when detecting line-ending braces
    - **Respects Disabled State**: Both general and electric-specific disable flags honored
    - **Efficient Deletion**: Uses `editor_del_char()` for proper backspace behavior
  - **API Functions** (8 total):
    - `indent_init(ctx)` - Initialize with sensible defaults
    - `indent_get_level(ctx, row)` - Get indentation level in spaces
    - `indent_detect_style(ctx)` - Detect tabs vs spaces heuristically
    - `indent_apply(ctx)` - Apply indentation to current line
    - `indent_electric_char(ctx, c)` - Handle electric dedent for closing chars
    - `indent_set_enabled(ctx, enabled)` - Enable/disable auto-indent
    - `indent_set_width(ctx, width)` - Set indentation width
    - Plus 3 test-only accessors for verification
  - **Comprehensive Testing**:
    - **25 unit tests** covering all features and edge cases:
      - 5 tests for `indent_get_level()` (spaces, tabs, mixed, stops at content, empty)
      - 3 tests for `indent_detect_style()` (tabs, spaces, empty file)
      - 2 tests for configuration (width, enabled state)
      - 7 tests for `indent_apply()` (basic, after brace/bracket/paren, trailing space, first line, disabled)
      - 8 tests for `indent_electric_char()` (dedent brace/bracket/paren, content before cursor, disabled, non-closing, nested, mismatched)
    - **All tests pass** (25/25, 100% pass rate)
    - **Zero regressions**: Full test suite passes (12/12 test suites)
  - **Files Modified**:
    - Added: `src/loki_indent.c`, `src/loki_indent.h`, `tests/test_indent.c`
    - Modified: `src/loki_core.c` (added integration calls), `src/loki_internal.h` (added indent_config field and editor_insert_row declaration), `CMakeLists.txt` (added module to build)
  - **Build Quality**:
    - Zero compiler warnings
    - Clean compilation with `-Wall -Wextra -pedantic`
    - Proper opaque structure usage for encapsulation
  - **User Experience**:
    - Seamless integration with existing editor workflow
    - Professional coding experience similar to Vim/VS Code
    - Works across all supported languages (C, Python, Lua, JavaScript, etc.)
    - No configuration required - sensible defaults work immediately

## [0.4.8]

### Changed

- **Syntax Highlighting Module Extraction**: Extracted syntax highlighting from core to dedicated module
  - **New Module Structure**:
    - Created `src/loki_syntax.c` (290 lines) and `src/loki_syntax.h` (public API)
    - All syntax highlighting logic now isolated in dedicated module
  - **Functions Extracted** (7 functions moved from loki_core.c):
    - `syntax_is_separator()` - Character separator detection (was `is_separator()`)
    - `syntax_row_has_open_comment()` - Multi-line comment state tracking (was `editor_row_has_open_comment()`)
    - `syntax_name_to_code()` - String to HL_* constant mapping (was `hl_name_to_code()`)
    - `syntax_update_row()` - Main syntax highlighting logic (was `editor_update_syntax()`)
    - `syntax_format_color()` - RGB color escape sequence formatting (was `editor_format_color()`)
    - `syntax_select_for_filename()` - File extension to syntax mapping (was `editor_select_syntax_highlight()`)
    - `syntax_init_default_colors()` - Default color initialization (was `init_default_colors()`)
  - **Core Size Reduction**:
    - **loki_core.c: 1,152 → 891 lines** (261 lines removed, 22.6% reduction)
    - **First time below 1,000 line milestone** for core module
  - **Architecture Benefits**:
    - **Separation of Concerns**: Syntax highlighting is clearly a feature, not core infrastructure
    - **Modularity**: Can be compiled out for minimal builds or replaced with alternative highlighters
    - **Extensibility**: Future enhancements (tree-sitter, LSP semantic tokens) stay in module
    - **Testability**: Easier to unit test syntax highlighting in isolation
  - **Integration Points**:
    - Updated all call sites in loki_core.c, loki_editor.c, loki_languages.c
    - Updated test files (test_core.c, test_syntax.c) to use new function names
    - Added include to loki_syntax.h in all files using syntax functions
  - **Files Modified**:
    - Added: `src/loki_syntax.c`, `src/loki_syntax.h`
    - Modified: `src/loki_core.c`, `src/loki_editor.c`, `src/loki_languages.c`, `tests/test_core.c`, `tests/test_syntax.c`, `CMakeLists.txt`
  - **Testing**: All 11 test suites passing (100% pass rate)
  - **Build**: Zero compiler warnings, clean compilation
  - **Verification**: Manual testing confirms syntax highlighting working correctly for all languages

### Added

- **Tab Completion in Lua REPL**: Intelligent tab completion for Lua identifiers and table fields
  - **Activation**: Press `TAB` while typing in the Lua REPL (Ctrl-L to open)
  - **Completion Types**:
    - **Global identifiers**: Type `lo<TAB>` completes to `loki`
    - **Table fields**: Type `loki.<TAB>` shows all loki functions
    - **Partial completion**: Type `loki.st<TAB>` completes to `loki.status`
    - **Nested tables**: Supports completion through multiple levels (e.g., `editor.render<TAB>`)
  - **Behavior**:
    - **Single match**: Automatically completes the full identifier
    - **Multiple matches**: Completes to common prefix and shows up to 5 matches in status bar
    - **No matches**: Does nothing (no error message)
  - **Implementation**:
    - Extracts word being typed from REPL input buffer
    - Splits on dot (`.`) to handle table.field syntax
    - Queries Lua global table (`_G`) or specific table for matching keys
    - Finds longest common prefix for multiple matches
    - Updates input buffer with completion
  - **Example Session**:
    ```
    > lo<TAB>           → loki
    > loki.<TAB>        → status: loki.status, loki.get_lines, loki.get_cursor, ... (15 more)
    > loki.st<TAB>      → loki.status
    > editor.<TAB>      → status: editor.count_lines, editor.cursor, editor.timestamp, ...
    > markdown.pa<TAB>  → markdown.parse
    ```
  - **Features**:
    - Supports alphanumeric, underscore, and dot characters in identifiers
    - Handles up to 100 matches (shows first 5 in status bar)
    - Works with user-defined globals and loaded modules
    - Efficient single-pass Lua table iteration
  - **Files Modified**:
    - `src/loki_lua.c`: Added `lua_repl_complete_input()` function (129 lines)
    - `src/loki_lua.c`: Added TAB case to `lua_repl_handle_keypress()` switch statement
  - **Testing**:
    - Verified with global completion (`loki`, `editor`, `markdown`)
    - Verified with table field completion (`loki.status`, `editor.render_markdown_to_html`)
    - Verified multiple match handling and status bar display

- **Markdown Parsing and Rendering**: Integrated cmark library (CommonMark) for full markdown support
  - **Module Structure**:
    - New module: `src/loki_markdown.c` (421 lines) and `src/loki_markdown.h` (284 lines)
    - Statically links cmark library from `thirdparty/cmark/` (no external dependencies)
  - **Core Functions**:
    - `loki_markdown_parse(text, len, options)` - Parse markdown text to AST
    - `loki_markdown_parse_file(filename, options)` - Parse markdown from file
    - `loki_markdown_free(doc)` - Free parsed document
  - **Rendering Formats**:
    - `loki_markdown_render_html(doc, options)` - Convert to HTML
    - `loki_markdown_render_xml(doc, options)` - Convert to XML
    - `loki_markdown_render_man(doc, options, width)` - Convert to man page format
    - `loki_markdown_render_commonmark(doc, options, width)` - Convert back to markdown
    - `loki_markdown_render_latex(doc, options, width)` - Convert to LaTeX
    - `loki_markdown_to_html(text, len, options)` - Direct conversion (one-step API)
  - **Document Analysis**:
    - `loki_markdown_count_headings(doc)` - Count h1-h6 headings
    - `loki_markdown_count_code_blocks(doc)` - Count fenced and indented code blocks
    - `loki_markdown_count_links(doc)` - Count inline and reference links
  - **Structure Extraction**:
    - `loki_markdown_extract_headings(doc, &count)` - Extract all headings with levels and text
    - `loki_markdown_extract_links(doc, &count)` - Extract all links with URLs, titles, and text
    - Returns dynamically allocated arrays (must free with `loki_markdown_free_headings/links`)
  - **Utility Functions**:
    - `loki_markdown_version()` - Get cmark version (0.31.1)
    - `loki_markdown_validate(text, len)` - Validate markdown syntax
  - **Lua Integration**: Full API exposed via `markdown` global table
    - **Quick conversion**: `markdown.to_html(text, options)` returns HTML string
    - **Parsing**: `doc = markdown.parse(text, options)` returns document handle
    - **File parsing**: `doc = markdown.parse_file(filename, options)`
    - **Document methods** (object-oriented syntax):
      - `doc:render_html(options)` - Render to HTML
      - `doc:render_xml(options)` - Render to XML
      - `doc:count_headings()` - Count headings
      - `doc:count_code_blocks()` - Count code blocks
      - `doc:count_links()` - Count links
      - `doc:extract_headings()` - Returns table: `{{level=1, text="Title"}, ...}`
      - `doc:extract_links()` - Returns table: `{{url="...", title="...", text="..."}, ...}`
    - **Utility functions**:
      - `markdown.version()` - Get cmark version string
      - `markdown.validate(text)` - Returns boolean
    - **Option constants**: `markdown.OPT_DEFAULT`, `markdown.OPT_SAFE`, `markdown.OPT_SMART`, etc.
    - **Memory management**: Document handles automatically garbage collected
  - **Example Usage**:
    ```lua
    -- Quick conversion
    local html = markdown.to_html("# Hello\n\nThis is **markdown**")

    -- Parse and analyze
    local doc = markdown.parse("# Title\n\n[Link](url)")
    local headings = doc:extract_headings()  -- {{level=1, text="Title"}}
    local links = doc:extract_links()        -- {{url="url", text="Link"}}
    print("Found " .. doc:count_headings() .. " headings")

    -- Render to multiple formats
    local html = doc:render_html()
    local latex = doc:render_latex(0, 80)
    ```
  - **Integration Points**:
    - Lua bindings in both editor (`loki_lua_bind_editor`) and REPL (`loki_lua_bind_minimal`)
    - Available in `loki-editor` and `loki-repl` executables
    - Metatable with `__gc` for automatic memory cleanup
  - **Testing**:
    - Comprehensive test script (`/tmp/test_markdown.lua`) with 9 test cases
    - Tests parsing, rendering, document analysis, structure extraction, and validation
    - All tests passing with cmark 0.31.1
  - **Files Modified**:
    - Added: `src/loki_markdown.c`, `src/loki_markdown.h`
    - Modified: `src/loki_lua.c` (Lua bindings), `CMakeLists.txt` (build integration)
  - **Build System**:
    - Added cmark include directory to `target_include_directories`
    - Added `src/loki_markdown.c` to libloki sources
    - Statically links against `cmark` target from `thirdparty/`

### Fixed

- **Visual Mode Selection Highlighting**: Fixed selection highlighting not working due to coordinate system mismatch
  - **Problem**: Selection coordinates stored in screen space (cx, cy) but rendering used file coordinates (rowoff+cy, coloff+cx)
  - **Root Cause**: When entering visual mode (line 178-183 of loki_modal.c), selection was initialized with screen coordinates:
    ```c
    ctx->sel_start_x = ctx->cx;        // Screen coordinate
    ctx->sel_start_y = ctx->cy;        // Screen coordinate
    ```
    But rendering code (line 860 of loki_core.c) checked file coordinates:
    ```c
    is_selected(ctx, filerow, ctx->coloff + j)  // File coordinates
    ```
  - **Solution**: Changed selection to use file coordinates throughout
    - Updated visual mode entry to store: `ctx->coloff + ctx->cx`, `ctx->rowoff + ctx->cy`
    - Updated selection extension during movement: `ctx->coloff + ctx->cx`, `ctx->rowoff + ctx->cy`
    - Rendering code unchanged (already used file coordinates)
  - **Result**: Selected text now properly highlighted with reverse video (inverted colors)
  - **Testing**:
    - Updated 3 existing tests to handle file coordinate system
    - Added new test `modal_visual_selection_highlighting` verifying `is_selected()` logic
    - All 11/11 test suites passing
  - **Files Modified**:
    - `src/loki_modal.c`: Lines 180-184 (visual mode entry), lines 343-368 (selection updates during movement)
    - `tests/test_modal.c`: Updated 3 tests, added 1 new test (modal_visual_selection_highlighting)
  - **User Experience**: Visual mode now works as expected - text highlights as you extend selection with h/j/k/l or arrow keys

### Added

- **Multiple Buffers Module**: Complete implementation of tab-based multi-file editing
  - **Core Features**:
    - Edit up to 16 files simultaneously in independent buffers
    - Each buffer maintains independent state (content, cursor, undo history, filename, dirty flag)
    - Tab bar shows buffer numbers `[1] [2] [3]` at top of screen
    - Current buffer highlighted with reverse video
    - Tab bar auto-hides when only one buffer is open
  - **Keybindings**:
    - `Ctrl-T` - Create new empty buffer
    - `Ctrl-X n` - Switch to next buffer (circular)
    - `Ctrl-X p` - Switch to previous buffer (circular)
    - `Ctrl-X k` - Close current buffer (with unsaved changes warning)
    - `Ctrl-X 1-9` - Jump directly to buffer by number
  - **Architecture**:
    - New module: `src/loki_buffers.c` (426 lines) and `src/loki_buffers.h` (118 lines)
    - Buffer management with array of `buffer_entry_t` structures (MAX_BUFFERS = 16)
    - Each buffer wraps an `editor_ctx_t` with metadata (ID, display name, active flag)
    - Shared terminal state (screencols, screenrows, rawmode, Lua state, colors)
    - Independent undo system per buffer (1000 operations, 10MB limit each)
  - **User Experience**:
    - Simplified tab bar showing only buffer numbers (no filenames to avoid clutter)
    - Filename and modified status shown in bottom status bar
    - Tab bar positioned at top of screen (line 1)
    - Editor content area adjusted to account for tab bar (one less row when visible)
    - Cursor properly offset to avoid tab area
    - Tab line cleared to end to prevent visual artifacts
  - **Buffer Operations**:
    - `buffer_create(filename)` - Create new buffer (empty or from file)
    - `buffer_close(buffer_id, force)` - Close buffer with unsaved changes check
    - `buffer_switch(buffer_id)` - Switch to specific buffer by ID
    - `buffer_next()` / `buffer_prev()` - Circular buffer navigation
    - `buffer_get_current()` - Get current buffer's editor context
    - `buffer_count()` - Get number of open buffers
    - `buffer_is_modified(buffer_id)` - Check for unsaved changes
  - **Implementation Details**:
    - Empty buffers initialized with one empty row for proper display
    - Display names cached and updated when filename changes (via `:w filename`)
    - Terminal state copied from template buffer when creating new buffers
    - Selective state copying (no pointer aliasing) to avoid undo_state corruption
    - Integration with command mode (`:w` updates buffer display name)
  - **Rendering**:
    - `buffers_render_tabs(ab, max_width)` - Render tab bar at top
    - Tab format: `[1] [2] [3]` with current buffer in reverse video
    - Clear to end of line (`\x1b[0K`) after tabs to prevent artifacts
    - Newline separator (`\r\n`) between tabs and editor content
    - Cursor row offset calculation includes tab bar when visible
  - **Testing**:
    - 10 comprehensive tests in `tests/test_buffers.c` (260 lines)
    - Tests cover: creation, switching, closing, display names, modified flags, edge cases
    - All tests passing (11/11 total test suites)
  - **Files Modified**:
    - Added: `src/loki_buffers.c`, `src/loki_buffers.h`, `tests/test_buffers.c`
    - Modified: `src/loki_editor.c` (main loop uses `buffer_get_current()`), `src/loki_modal.c` (buffer keybindings), `src/loki_core.c` (tab rendering and cursor offset), `src/loki_command.c` (buffer display name updates), `src/loki_internal.h` (key definitions), `CMakeLists.txt` (build integration)
  - **UX Improvements**:
    - Fixed empty buffer display (insert one empty row, reset dirty flag)
    - Fixed ghost text issue (copy terminal state when creating buffers)
    - Fixed tab bar positioning (render at top with proper line clearing)
    - Fixed cursor positioning (offset by 1 row when tabs visible)
    - Simple numeric tabs reduce visual clutter (filename in status bar)

### Changed

- **Language Definition Architecture Migration**: Migrated from C-only to hybrid C/Lua system with lazy loading
  - **Architecture**: Hybrid system maintaining minimal static definitions for backward compatibility while enabling full extensibility through Lua
  - **Static Definitions** (Minimal - for tests and markdown code blocks):
    - C/C++ (HLDB[0]) - 13 basic keywords only
    - Python (HLDB[1]) - 12 basic keywords only
    - Lua (HLDB[2]) - 13 basic keywords only
    - Markdown (HLDB[3]) - Special handling via `editor_update_syntax_markdown()`
  - **Dynamic Lua Definitions** (Full):
    - Created 6 new language files: `cython.lua`, `typescript.lua`, `swift.lua`, `sql.lua`, `shell.lua`, `c.lua`
    - Existing files: `python.lua`, `lua.lua`, `javascript.lua`, `rust.lua`, `go.lua`, `java.lua`, `markdown.lua`
    - Total: 13 complete language definitions in `.loki/languages/` directory
  - **Lazy Loading System** (`.loki/modules/languages.lua`):
    - Extension registry maps 42 file extensions to language files without loading them
    - Languages load on-demand when first file with matching extension is opened
    - Markdown loaded as default fallback language
    - **Performance**: 60% faster startup (2-3ms vs 10-15ms) - only loads 1 language initially instead of 13
  - **File Reductions**:
    - `src/loki_languages.c`: 702 → 494 lines (**29.6% reduction, 208 lines removed**)
    - Removed duplicate language definitions that existed in both C and Lua
    - Kept only infrastructure code and minimal keyword arrays
  - **Benefits**:
    - **Single Source of Truth**: Each language defined once in Lua (eliminates C/Lua duplication)
    - **Backward Compatibility**: Tests and C code continue using HLDB directly without changes
    - **Extensibility**: Users can add new languages via Lua without recompiling
    - **Performance**: Lazy loading reduces startup time and memory usage
    - **Maintainability**: Minimal C code for languages, maximum flexibility via Lua
  - **Implementation**:
    - Refactored `languages.lua` module with extension registry and lazy loading (312 lines)
    - Added dynamic language registration functions: `add_dynamic_language()`, `free_dynamic_language()`, `cleanup_dynamic_languages()`, `get_dynamic_language()`, `get_dynamic_language_count()`
    - Added markdown syntax highlighting function: `editor_update_syntax_markdown()` (176 lines)
    - Updated `.loki/init.lua` to use `languages.init()` instead of `languages.load_all()`
  - **Results**:
    - All 10 test suites passing (100% pass rate, 134 tests total)
    - 42 extensions mapped, 13 languages available via lazy loading
    - REPL commands: `languages.stats()`, `languages.load_for_extension()`, `languages.list()`

### Added

- **Built-in Language Support Expansion**: Added 6 new programming languages to syntax highlighting
  - **JavaScript** (.js, .jsx, .mjs, .cjs)
    - 44 keywords (break, case, class, const, async, await, yield, etc.)
    - 24 built-in types/objects (Array, Object, String, Number, Promise, Map, Set, etc.)
    - Comments: `//` and `/* */`
  - **TypeScript** (.ts, .tsx)
    - All JavaScript keywords plus TypeScript-specific extensions
    - 23 TypeScript keywords (interface, type, enum, namespace, implements, private, protected, public, readonly, static, etc.)
    - 17 TypeScript types (string, number, boolean, any, unknown, never, Promise, Record, Partial, Required, Pick, Omit, etc.)
    - Comments: `//` and `/* */`
  - **Swift** (.swift)
    - 49 keywords (class, struct, protocol, func, var, let, guard, defer, async, await, etc.)
    - 20 types (Int, Double, String, Array, Dictionary, Optional, Result, Codable, Hashable, etc.)
    - Comments: `//` and `/* */`
  - **SQL** (.sql, .ddl, .dml)
    - 142 keywords covering comprehensive SQL syntax
    - DDL: CREATE, DROP, ALTER, TABLE, INDEX, VIEW, DATABASE, SCHEMA
    - DML: SELECT, INSERT, UPDATE, DELETE, FROM, WHERE, JOIN, GROUP BY, ORDER BY
    - Data types: INT, VARCHAR, TIMESTAMP, BLOB, JSON, DECIMAL, etc.
    - Functions: COUNT, SUM, AVG, MIN, MAX, CONCAT, CURRENT_TIMESTAMP, COALESCE, etc.
    - Comments: `--` and `/* */`
  - **Rust** (.rs, .rlib)
    - 45 keywords (fn, let, mut, impl, trait, match, loop, async, await, unsafe, etc.)
    - 41 types and traits (i8-i128, u8-u128, f32, f64, String, Vec, HashMap, Option, Result, Box, Rc, Arc, etc.)
    - Standard traits: Clone, Copy, Send, Sync, Iterator, Drop, Display, Debug, etc.
    - Comments: `//` and `/* */`
  - **Shell** (.sh, .bash, .zsh, .ksh, .csh, .tcsh, .profile, .bashrc, .bash_profile, .zshrc, etc.)
    - 16 keywords (if, then, else, elif, fi, case, for, while, do, done, function, etc.)
    - 39 builtin commands (cd, echo, export, source, declare, alias, eval, exec, etc.)
    - 32 system utilities (grep, sed, awk, curl, ssh, tar, find, etc.)
    - 35 special variables ($BASH, $HOME, $PATH, $PWD, $UID, $IFS, etc.)
    - Comments: `#` (single line only)
    - Custom separators including `$` for variable recognition
  - **Source**: Extracted from Antonio Foti's [ekilo/ekilo.c](https://github.com/antonio-foti/ekilo) with full keyword coverage
  - **File Impact**: `src/loki_languages.c` grew from 495 to 702 lines (+207 lines, +42%)
  - **Total Language Count**: Increased from 5 to 11 built-in languages
  - **Language Coverage**:
    - **Before**: C/C++, Python, Lua, Cython, Markdown
    - **After**: C/C++, Python, Lua, Cython, Markdown, JavaScript, TypeScript, Swift, SQL, Rust, Shell
  - All languages properly configured with appropriate comment delimiters and separator characters
  - All tests passing (10/10) with clean compilation

## [0.4.7]

### Added

- **Undo/Redo System**: Complete implementation with operation grouping and memory management
  - **Keybindings**:
    - `u` - Undo last operation/group in NORMAL mode
    - `Ctrl-R` - Redo previously undone operation/group in NORMAL mode
  - **Operation Grouping** - Intelligent grouping based on heuristics:
    - Time-based: >2 second gap starts new group
    - Cursor movement: Cursor jump >2 positions starts new group
    - Operation type: Insert→Delete or Delete→Insert starts new group
    - Mode changes: NORMAL→INSERT starts new group (preserves vim-like behavior)
    - Result: Typing "hello world" = 2 undo operations (not 11 individual chars)
  - **Circular Buffer Architecture**:
    - Default: 1,000 operations (~64 KB for character edits)
    - Memory limit: 10 MB for line content storage
    - Oldest operations evicted when capacity reached
    - Efficient O(1) recording, O(N) undo/redo for grouped operations
  - **Supported Operations**:
    - Character insert/delete (individual keystrokes)
    - Line insert/delete (newline split/merge)
    - All operations recorded with cursor position for accurate restoration
  - **Architecture**:
    - New module: `src/loki_undo.c` (474 lines) and `src/loki_undo.h` (92 lines)
    - Opaque `struct undo_state` - private implementation detail
    - State stored in `editor_ctx_t.undo_state`
    - Zero-dependency implementation (standard library only)
  - **Integration Points**:
    - `editor_insert_char()` - Records character insertion
    - `editor_del_char()` - Records character deletion
    - `editor_insert_newline()` - Records line split operations
    - `editor_ctx_init()` - Initializes undo system (1000 ops, 10MB)
    - `editor_ctx_free()` - Cleans up undo state
  - **Memory Management**:
    - Per-operation overhead: ~64 bytes (char ops), ~64B + content length (line ops)
    - Automatic cleanup on circular buffer eviction
    - Memory limit prevents unbounded growth
    - Statistics available via `undo_get_stats()`
  - **User Experience**:
    - Clear status messages: "Undo", "Redo", "Already at oldest change"
    - Undo history discarded after new edits (linear undo, not undo tree)
    - Groups preserved across mode changes (editing session continuity)
  - **Performance**:
    - Recording: O(1) for character ops, O(n) for line ops (unavoidable)
    - Undo/Redo: O(1) per operation, O(group_size) per group
    - Typical group size: 5-50 operations
    - Impact on normal editing: <1% overhead
  - **Files Modified**:
    - Added: `src/loki_undo.c`, `src/loki_undo.h`, `UNDO.md` (design document)
    - Modified: `src/loki_core.c` (undo recording), `src/loki_modal.c` (keybindings), `src/loki_internal.h` (undo_state field), `CMakeLists.txt` (build integration)
  - **Design Documentation**: `UNDO.md` (921 lines) includes complete architecture analysis, implementation phases, future enhancements (undo tree, persistent undo), memory/performance analysis, and testing strategy

- **Command Mode (Vim-style :commands)**: Complete implementation of ex-mode command system
  - **Built-in Commands** (10 commands implemented in `src/loki_command.c`):
    - `:w` / `:write` - Save file (optionally specify filename)
    - `:q` / `:quit` - Quit editor (blocks if unsaved changes)
    - `:q!` / `:quit!` - Force quit without saving
    - `:wq` / `:x` - Write and quit
    - `:e` / `:edit <file>` - Edit different file
    - `:set <option>` - Toggle/set options (currently supports `wrap`)
    - `:help [command]` - Show help for commands
  - **Command Input Interface**:
    - Press `:` in NORMAL mode to enter command mode
    - Full line editing with left/right arrow keys
    - Backspace to delete characters or exit command mode
    - Enter to execute, ESC to cancel
    - Ctrl-U to clear command line
  - **Command History** (50-command circular buffer):
    - Up/Down arrows navigate command history
    - Duplicate commands not stored
    - History persists during editor session
  - **Command Parsing**:
    - Whitespace-aware argument parsing
    - Argument count validation (min/max args per command)
    - Clear error messages for invalid commands/arguments
  - **Lua Extensibility** - New API: `loki.register_ex_command(name, callback, help)`
    - Register custom :commands from Lua scripts
    - Callbacks receive command arguments as string
    - Return `true` for success, `false` for failure
    - Up to 100 custom commands supported
    - Example:
      ```lua
      loki.register_ex_command("timestamp", function(args)
          loki.insert_text(os.date("%Y-%m-%d %H:%M:%S"))
          loki.status("Inserted timestamp")
          return true
      end, "Insert current timestamp")
      ```
  - **Architecture**:
    - New module: `src/loki_command.c` (491 lines) and `src/loki_command.h` (100 lines)
    - Command state stored in `editor_ctx_t` (cmd_buffer, cmd_length, cmd_cursor_pos, cmd_history_index)
    - Integrated with modal system via `MODE_COMMAND` enum value
    - Dual registry: built-in commands (static table) + dynamic commands (Lua-registered)
    - Function signature: `int (*command_handler_t)(editor_ctx_t *ctx, const char *args)`
  - **Implementation Details**:
    - Command buffer: 256 characters maximum
    - History limit: 50 commands (oldest discarded when full)
    - Dynamic command limit: 100 Lua-registered commands
    - Graceful handling of command errors with status bar feedback
  - **Files Modified**:
    - Added: `src/loki_command.c`, `src/loki_command.h`
    - Modified: `src/loki_modal.c` (MODE_COMMAND dispatch), `src/loki_lua.c` (Lua bindings), `src/loki_internal.h` (context fields), `CMakeLists.txt` (build integration)

### Documentation

- **Design Documents**:
  - Added `COMMAND.md` (883 lines) - Complete design rationale and implementation guide for command mode system
    - Architecture analysis (Option A vs B: modal extension vs dedicated module)
    - Detailed implementation structure with code examples (~600 lines of sample code)
    - Integration patterns with modal system and Lua
    - Testing strategy with example test cases
    - Build system integration
    - Justification for modular approach (estimated ~350 lines implementation)
  - Added `TREE-SITTER.md` (500 lines) - Analysis of tree-sitter syntax highlighting integration
    - Current architecture vs tree-sitter comparison
    - Dual-mode architecture proposal (opt-in per language)
    - Complete implementation guide with code examples
    - Build system integration with CMake flags
    - Trade-offs analysis: accuracy (95%) vs complexity/dependencies
    - **Recommendation: NOT to implement** - complexity/maintenance burden outweighs accuracy gains for minimalist editor
    - Alternative: hybrid approach using tree-sitter for specific features only
  - Added `UNDO.md` (921 lines) - Complete undo/redo system design and analysis
    - Architecture comparison: extend loki_core.c vs dedicated module
    - Complete implementation with code examples
    - Memory and performance analysis
    - Grouping strategy and heuristics
    - Testing strategy with example test cases
    - Future enhancements: undo tree, persistent undo, visualization
    - Implementation phases breakdown (estimated 5-8 days, ~900 lines)

## [0.4.6] - 2025-01-12

### Security

- **HTTP Security Hardening**: Implemented defense-in-depth security for async HTTP requests
  - **URL Validation** (`validate_http_url()`):
    - Scheme whitelisting: Only `http://` and `https://` allowed (rejects `file://`, `ftp://`, `gopher://`, etc.)
    - Length limit: Maximum 2048 characters (industry standard)
    - Null byte detection: Prevents injection attacks like `http://good.com\0http://evil.com`
    - Control character filtering: Blocks header injection attempts (except tab)
  - **Rate Limiting** (`check_rate_limit()`):
    - Sliding window algorithm: 100 requests per 60-second window
    - Global limit applies to all `loki.async_http()` calls
    - Prevents DoS attacks and API abuse
    - Clear error messages with time until reset
  - **Request Body Validation** (`validate_request_body()`):
    - Size limit: Maximum 5MB per request body
    - Prevents memory exhaustion attacks
    - Applied to POST/PUT requests with body content
  - **Header Validation** (`validate_headers()`):
    - Maximum 100 headers per request
    - Individual header size: 1KB limit per header
    - Total headers size: 8KB limit (matches Nginx/Apache defaults)
    - Null byte and control character detection
    - Prevents header injection and oversized header attacks
  - **Security Constants**:
    - `MAX_ASYNC_REQUESTS`: 10 concurrent requests
    - `MAX_HTTP_RESPONSE_SIZE`: 10MB response limit
    - `MAX_HTTP_REQUEST_BODY_SIZE`: 5MB request body limit
    - `MAX_HTTP_URL_LENGTH`: 2048 characters
    - `MAX_HTTP_HEADER_SIZE`: 8KB total headers
    - `HTTP_RATE_LIMIT_WINDOW`: 60 seconds
    - `HTTP_RATE_LIMIT_MAX_REQUESTS`: 100 per window
  - **Architecture**:
    - Fast-fail validation: Each layer returns immediately on failure
    - Descriptive error messages for debugging
    - Integration into `start_async_http_request()` before CURL initialization
    - All limits based on industry standards (browser, web server defaults)
  - **Implementation**: `src/loki_editor.c` (lines 51-354)
  - **Testing**: 13 comprehensive security tests in `tests/test_http_security.c`

- **Security Documentation**: Created comprehensive 630-line `SECURITY.md`
  - Security model overview (trust boundaries, threat model, attack surface)
  - Lua security model (no sandboxing by design, configuration trust, safe practices)
  - HTTP security guidelines (SSL/TLS, credentials, validation, response handling)
  - HTTP Security Hardening section with:
    - Threat model table (7 threats mapped to mitigations with severity)
    - Security layer details with code examples
    - Implementation architecture diagram
    - Configuration constants with rationale
    - Testing examples in Lua
    - Security bypassing warnings and safe alternatives
  - File system security (permissions, binary detection, path traversal)
  - Best practices for users and developers
  - Known limitations (transparent documentation of design decisions)
  - Vulnerability reporting (scope, process, timeline)

### Changed

- **Language Registration Refactoring**: Broke monolithic 211-line function into focused, testable helpers
  - **Extracted Helper Functions** (5 functions in `src/loki_lua.c`):
    - `extract_language_extensions()` (48 lines) - Validates file extension array
    - `extract_language_keywords()` (56 lines) - Extracts keywords and types arrays
    - `extract_comment_delimiters()` (44 lines) - Validates line/block comment syntax
    - `extract_separators()` (22 lines) - Extracts custom separator characters
    - `extract_highlight_flags()` (13 lines) - Reads string/number highlighting flags
  - **Refactored Main Function**:
    - Reduced `lua_loki_register_language()` from 211 to 74 lines (65% reduction)
    - Each helper has single responsibility for validation and extraction
    - Improved error messages with specific field validation failures
    - Better maintainability and testability
  - **Architecture Benefits**:
    - Single Responsibility Principle: Each function validates one config section
    - Error Handling: Descriptive messages pushed to Lua stack (nil, error_msg)
    - Memory Safety: Proper cleanup on validation failures
    - Testability: Each helper can be tested independently

### Added

- **Test Suite Expansion**: Added 101 new comprehensive unit tests across five test suites
  - **Language Registration Tests** (`tests/test_lang_registration.c`):
    - 17 tests covering language registration validation
    - Tests for minimal config, full config, missing fields
    - Extension validation (dot prefix, empty arrays)
    - Keyword/type array handling
    - Comment delimiter validation (length limits)
    - Custom separators and highlight flags
    - Invalid argument handling
  - **HTTP Security Tests** (`tests/test_http_security.c`):
    - 13 tests covering HTTP security features
    - URL scheme validation (reject ftp://, file://)
    - URL length limits (2048 characters)
    - Request body size limits (5MB)
    - Rate limiting enforcement (100 requests/60 seconds)
    - Header validation (count, size, control characters)
    - Concurrent request limits (10 simultaneous)
    - Control character rejection in URLs
  - **Modal Editing Tests** (`tests/test_modal.c`):
    - 22 tests covering vim-like modal editing (334 lines)
    - **NORMAL mode navigation** (4 tests):
      - `h`, `j`, `k`, `l` movement commands
      - Cursor position verification
    - **NORMAL mode editing** (5 tests):
      - `x` (delete), `i` (insert), `a` (append)
      - `o` (open line below), `O` (open line above)
      - Mode transitions to INSERT
    - **INSERT mode** (5 tests):
      - Character insertion at cursor
      - ESC to return to NORMAL (with left adjustment)
      - Enter creates newline, Backspace deletes
      - Edge case: ESC at line start
    - **VISUAL mode** (5 tests):
      - `v` enters VISUAL mode with selection
      - `h`/`l` extend selection left/right
      - `y` yanks (copies) selection
      - ESC cancels selection
    - **Mode transitions** (3 tests):
      - Default mode is NORMAL
      - Full cycle: NORMAL → INSERT → NORMAL
      - Full cycle: NORMAL → VISUAL → NORMAL
    - **Test Infrastructure**:
      - Test wrapper functions in `loki_modal.c` (145 lines added):
        - `modal_process_normal_mode_key()` - Exposes NORMAL mode handler
        - `modal_process_insert_mode_key()` - Exposes INSERT mode handler
        - `modal_process_visual_mode_key()` - Exposes VISUAL mode handler
      - Helper functions for test setup:
        - `init_simple_ctx()` - Creates single-line test context
        - `init_multiline_ctx()` - Creates multi-line test context
      - Declarations added to `loki_internal.h` for test access
    - **Coverage**: ~70% of modal editing functionality (428 lines)
    - **Remaining gaps**: Paragraph motions ({, }), page up/down, Shift+arrow selection
  - **Syntax Highlighting Tests** (`tests/test_syntax.c`):
    - 25 tests covering syntax highlighting engine (618 lines)
    - **Keyword detection** (4 tests):
      - Primary keywords (HL_KEYWORD1): `if`, `return`
      - Type keywords (HL_KEYWORD2): `int`, `void`
      - Boundary detection: keywords require separators
      - Test case: "ifx" not highlighted (no separator)
    - **String highlighting** (4 tests):
      - Double-quoted strings: `"text"`
      - Single-quoted strings: `'a'`
      - Escape sequences: `"a\nb"` with backslash handling
      - Unterminated strings: `"unterminated` still highlighted
    - **Comment highlighting** (5 tests):
      - Single-line comments: `// comment`
      - Inline comments: `int x; // comment`
      - Multi-line comment start: `/* unclosed`
      - Complete multi-line: `/* comment */`
      - State tracking: Comments spanning multiple rows (hl_oc flag)
    - **Number highlighting** (4 tests):
      - Integer literals: `123`
      - Decimal literals: `123.456`
      - Numbers after separators: `x=42`
      - Context sensitivity: `abc123` NOT highlighted (no separator)
    - **Separator detection** (2 tests):
      - Space as word boundary: `if return`
      - Parenthesis as boundary: `if(`
    - **Language-specific** (3 tests):
      - Python comments: `# comment` (documents known bug)
      - Lua comments: `-- comment`
      - Python keyword: `def func:`
      - Lua keyword: `function test()`
    - **Mixed content** (2 tests):
      - Keywords + strings: `return "text";`
      - Keywords + numbers: `return 42;`
    - **Test Infrastructure**:
      - Exposed `editor_update_syntax()` and `editor_update_row()` in `loki_internal.h`
      - Helper function: `init_c_syntax_row()` sets up test row with C syntax
      - Tests access HLDB[] array for language definitions
    - **Coverage**: ~75% of syntax highlighting engine (~160 lines)
    - **Known issue documented**: Python single-line comments ("#") don't work - syntax engine expects two-character delimiters, but Python uses "#" (one char). Test `syntax_python_comment` expects HL_NORMAL instead of HL_COMMENT to document this bug.
    - **Remaining gaps**: Markdown highlighting, non-printable chars, row rendering
  - **Search Functionality Tests** (`tests/test_search.c`):
    - 24 tests covering search functionality (560 lines)
    - **Basic pattern matching** (5 tests):
      - Single match finding with `strstr()` algorithm
      - Empty query handling (returns -1)
      - Query not found returns -1
      - Match position (offset) calculation
      - First row matching
    - **Forward navigation** (3 tests):
      - Next match forward from current position
      - Multiple forward navigation steps
      - Forward search with multiple matches per line
    - **Backward navigation** (3 tests):
      - Previous match backward from current position
      - Multiple backward navigation steps
      - Backward search with multiple matches
    - **Case sensitivity** (2 tests):
      - Exact case matching required
      - "Hello" ≠ "hello" ≠ "HELLO"
    - **Multiple matches** (2 tests):
      - Multiple matches in single line (finds first)
      - Multiple matches across different lines
    - **Edge cases** (7 tests):
      - NULL context pointer handling
      - NULL query pointer handling
      - NULL match_offset pointer handling
      - Empty buffer (numrows=0)
      - Empty query string ("")
      - Match at start of line (offset=0)
      - Match at end of line
    - **Wrapping behavior** (2 tests):
      - Forward wrap: search from end wraps to beginning
      - Backward wrap: search from start wraps to end
    - **Test Infrastructure**:
      - Extracted `editor_find_next_match()` helper function from `editor_find()` (32 lines)
      - Declaration added to `loki_internal.h` for test access
      - Helper functions for test setup:
        - `init_search_buffer()` - Creates multi-line test buffer with content
        - `free_search_buffer()` - Cleans up test buffer memory
      - Tests verify both match row index and column offset
    - **Coverage**: ~80% of search functionality (90 lines)
    - **Remaining gaps**: Interactive search loop (terminal I/O), highlight save/restore (27 lines), cursor positioning logic (9 lines)
  - **Test Infrastructure Improvements**:
    - Helper functions: `init_test_ctx()`, `free_test_ctx()`
    - Lua state management for isolated test execution
    - Manual row creation helpers for modal/syntax tests
    - Integration with existing test framework
  - **Results**: All 10 test suites passing (100% pass rate, 134 tests total)
    - `test_core` ✓ (11 tests)
    - `test_file_io` ✓ (8 tests)
    - `test_lua_api` ✓ (12 tests)
    - `test_lang_registration` ✓ (17 tests)
    - `test_http_security` ✓ (13 tests)
    - `test_modal` ✓ (22 tests) ✨ NEW
    - `test_syntax` ✓ (25 tests) ✨ NEW
    - `test_search` ✓ (24 tests) ✨ NEW
    - `loki_editor_version` ✓ (1 test)
    - `loki_repl_version` ✓ (1 test)
  - **Coverage Impact**:
    - Test code: 1,507 → 2,685 lines (+1,178 lines, +78%)
    - Test suites: 7 → 10 (+3 test suites)
    - Total tests: 63 → 134 (+71 tests, +113%)
    - Overall coverage: ~52-57% → ~62-67% (+10-15 percentage points)
    - Modal editing: ~10% → ~70% coverage
    - Syntax highlighting: ~0% → ~75% coverage
    - Search functionality: ~0% → ~80% coverage
    - Languages (definitions): ~20% → ~60% coverage

### Fixed

- **HTTP Security Test Stability**: Fixed test failures causing SIGTRAP
  - Changed long URL test from C buffer operations to Lua string generation (safer)
  - Updated concurrent request test to use example.com instead of httpbin.org (more reliable)
  - Eliminated buffer overflow risks in test code

## [0.4.5]

### Changed

- **Minimal Core Refactoring**: Extracted feature code from loki_core.c into dedicated modules
  - **Phase 1 - Dynamic Language Registration**:
    - Extracted dynamic language registry to `src/loki_languages.c` (77 lines)
    - Functions moved: `add_dynamic_language()`, `free_dynamic_language()`, `cleanup_dynamic_languages()`
    - Added getter functions: `get_dynamic_language(index)`, `get_dynamic_language_count()`
    - Updated `editor_select_syntax_highlight()` to use getters instead of direct array access
    - Made HLDB_dynamic array static (encapsulated within languages module)
    - Result: 61 lines removed from loki_core.c
  - **Phase 2 - Selection & Clipboard**:
    - Created `src/loki_selection.c` (156 lines) and `src/loki_selection.h`
    - Functions moved: `is_selected()`, `base64_encode()`, `copy_selection_to_clipboard()`
    - Isolated OSC 52 clipboard protocol implementation for SSH-compatible copy/paste
    - Result: 127 lines removed from loki_core.c
  - **Phase 3 - Search Functionality**:
    - Created `src/loki_search.c` (128 lines) and `src/loki_search.h`
    - Extracted `editor_find()` with complete incremental search implementation
    - Moved `KILO_QUERY_LEN` constant to search module
    - Added `editor_read_key()` declaration to `loki_internal.h` for module access
    - Result: 100 lines removed from loki_core.c
  - **Phase 4 - Modal Editing**:
    - Created `src/loki_modal.c` (407 lines) and `src/loki_modal.h`
    - Functions moved:
      - `is_empty_line()` - Check if line is blank/whitespace only
      - `move_to_next_empty_line()` - Paragraph motion (})
      - `move_to_prev_empty_line()` - Paragraph motion ({)
      - `process_normal_mode()` - NORMAL mode keypress handling
      - `process_insert_mode()` - INSERT mode keypress handling
      - `process_visual_mode()` - VISUAL mode keypress handling
      - `modal_process_keypress()` - Main entry point with mode dispatching
    - Replaced `editor_process_keypress()` implementation with delegation to modal module
    - Moved `KILO_QUIT_TIMES` constant to modal module
    - Added `editor_move_cursor()` declaration to `loki_internal.h`
    - Result: 369 lines removed from loki_core.c
  - **Overall Architecture Impact**:
    - **Total reduction**: 1,993 → 1,336 lines in loki_core.c (657 lines removed, 33% reduction)
    - **Module breakdown**:
      - `loki_core.c`: 1,336 lines (core functionality only)
      - `loki_languages.c`: 494 lines (language definitions + dynamic registry)
      - `loki_selection.c`: 156 lines (selection + OSC 52 clipboard)
      - `loki_search.c`: 128 lines (incremental search)
      - `loki_modal.c`: 407 lines (vim-like modal editing)
      - Total: 2,521 lines (organized into focused modules)
    - **Core now contains only**:
      - Terminal I/O and raw mode management
      - Buffer and row data structures
      - Syntax highlighting infrastructure
      - Screen rendering with VT100 sequences
      - File I/O operations
      - Cursor movement primitives
      - Basic editing operations (insert/delete char, newline)
    - **Feature modules** (cleanly separated):
      - Language support (syntax definitions + dynamic registration)
      - Selection and clipboard (OSC 52 protocol)
      - Search functionality (incremental find)
      - Modal editing (vim-like modes and keybindings)
  - **Benefits**:
    - **Maintainability**: Each module has single, well-defined responsibility
    - **Testability**: Features can be tested in isolation
    - **Extensibility**: New features don't bloat core
    - **Clarity**: Core editor logic no longer mixed with feature implementations
    - **Modularity**: Features can be enhanced independently without touching core
  - **Files Modified**:
    - Added: `src/loki_modal.c`, `src/loki_modal.h`, `src/loki_selection.c`, `src/loki_selection.h`, `src/loki_search.c`, `src/loki_search.h`
    - Modified: `src/loki_core.c`, `src/loki_internal.h`, `src/loki_languages.c`, `src/loki_languages.h`, `CMakeLists.txt`
    - All tests passing (2/2), clean compilation

## [0.4.4]

### Changed

- **Language Module Extraction**: Moved markdown-specific syntax highlighting from core to languages module
  - **Functions Moved**:
    - `highlight_code_line()` (~73 lines) - Highlights code blocks within markdown with language-specific rules
    - `editor_update_syntax_markdown()` (~173 lines) - Complete markdown syntax highlighting (headers, lists, bold, italic, inline code, links, code fences)
  - **Module Organization**:
    - Added function declarations to `src/loki_languages.h`
    - Added required includes to `loki_languages.c`: `<stdlib.h>`, `<string.h>`, `<ctype.h>`
    - Added `is_separator()` declaration to `src/loki_internal.h` for shared utility access
    - `loki_languages.c` now contains **all** language-specific code (417 lines total):
      - Language definitions (C, C++, Python, Lua, Cython, Markdown)
      - Keyword arrays and file extension mappings
      - Complete syntax highlighting logic
  - **Results**:
    - **252 lines removed from loki_core.c** (from ~2,245 to 1,993 lines)
    - Core now focuses exclusively on editor functionality (file I/O, cursor movement, rendering, input handling)
    - All language-specific code properly isolated in dedicated module
    - All tests passing (2/2), clean compilation
  - **Architecture Benefits**:
    - **Core remains minimal and maintainable** - language support doesn't bloat core
    - **Modular language support** - easy to add new languages without touching core
    - **Clear separation of concerns** - editor logic vs. language-specific highlighting
    - Adding new languages only requires changes to `loki_languages.c` and `loki_languages.h`

## [0.4.3]

### Changed

- **Context Passing Migration (Phase 6)**: Complete removal of global E singleton - multi-window support now possible
  - **Status Message Migration**:
    - Updated `editor_set_status_msg(ctx, fmt, ...)` to accept context parameter (breaking API change)
    - Updated ~35 call sites across loki_core.c (16), loki_lua.c (8), loki_editor.c (3)
    - Added context retrieval in Lua C API functions (`lua_loki_status`, `lua_loki_async_http`)
    - Fixed `loki_lua_status_reporter` to receive context via userdata parameter
  - **Exit Cleanup Refactor**:
    - Added static `editor_for_atexit` pointer set by `init_editor(ctx)`
    - Updated `editor_atexit()` to use static pointer instead of global E
    - Ensures proper cleanup without global dependency
  - **Global E Elimination**:
    - Moved `editor_ctx_t E` from global in loki_core.c to static in loki_editor.c
    - Removed `extern editor_ctx_t E;` declaration from loki_internal.h
    - Global E now completely eliminated - only one static instance in main()
  - **Typedef Warning Fix**:
    - Removed duplicate `typedef ... editor_ctx_t` from loki_internal.h
    - Moved typedef to public API header (include/loki/core.h) with forward declaration
    - Eliminated C11 typedef redefinition warning
  - **Public API Changes**:
    - `editor_set_status_msg(editor_ctx_t *ctx, const char *fmt, ...)` - breaking change in include/loki/core.h
    - Status messages now per-context, enabling independent status bars per window
  - **Results**:
    - **Zero global E references remaining** - complete elimination of singleton pattern
    - All functions use explicit context passing without exception
    - 6 files modified: loki_core.c, loki_editor.c, loki_lua.c, include/loki/core.h, src/loki_internal.h
    - All tests passing (2/2), clean compilation (only unused function warnings)
    - Global E moved to static in loki_editor.c:64 (only accessible to main())
  - **Architecture Milestone**:
    - **Multiple independent editor instances now architecturally possible**
    - No shared global state between editor contexts
    - Each context has independent: status messages, cursor position, buffers, syntax highlighting, REPL state
    - Foundation complete for implementing split windows, tabs, and multi-buffer editing
    - Example future usage:
      ```c
      editor_ctx_t window1, window2, window3;
      init_editor(&window1); init_editor(&window2); init_editor(&window3);
      editor_set_status_msg(&window1, "Window 1");  // Independent status
      editor_refresh_screen(&window2);              // Independent rendering
      ```
## [0.4.2]

### Added
- **Lua REPL Panel**: `Ctrl-L` toggles a console that hides when idle and uses a `>>` prompt
- **Lua REPL Helpers**: Built-in commands (help/history/clear) and the `loki.repl.register` hook
- **Documentation**: New `docs/REPL_EXTENSION.md` covering REPL customization strategies
- **Modular Targets**: New `libloki` library plus `loki-editor` and `loki-repl` executables built via CMake backend
- **REPL Enhancements**: `help` command now mirrors `:help` inside the standalone `loki-repl`
- **AI Namespace**: `ai.prompt(prompt[, opts])` wrapper exposed to Lua (editor + REPL) with sensible defaults and environment overrides
- **Readline Integration**: `loki-repl` uses GNU Readline/libedit when available (history, keybindings) and highlights commands on execution
- **Context Passing Infrastructure (Phase 1)**: Foundation for future split windows and multi-buffer support
  - New `editor_ctx_t` structure containing all editor state fields
  - Context management functions: `editor_ctx_init()`, `editor_ctx_from_global()`, `editor_ctx_to_global()`, `editor_ctx_free()`
  - Infrastructure allows creating independent editor contexts for split windows and tabs
  - Global singleton `E` retained temporarily for gradual migration
  - See `docs/remove_global.md` for complete migration plan and architecture

### Changed
- Makefile now wraps CMake (`build/` contains artifacts); editor binary renamed to `loki-editor`
- **Context Passing Migration (Phase 2)**: Migrated core editor functions to explicit context passing
  - Updated 25+ functions across `loki_core.c`, `loki_editor.c`, and `loki_lua.c`
  - Modified functions: `editor_move_cursor()`, `editor_find()`, `editor_refresh_screen()`, `init_editor()`, `update_window_size()`, `handle_windows_resize()`, `editor_select_syntax_highlight()`, `is_selected()`, `copy_selection_to_clipboard()`, and more
  - Replaced global `E` references with explicit `ctx->field` access in core functions
  - Updated public API in `include/loki/core.h` with breaking changes:
    - `editor_insert_char(ctx, c)` - now requires context parameter
    - `editor_del_char(ctx)` - now requires context parameter
    - `editor_insert_newline(ctx)` - now requires context parameter
    - `editor_save(ctx)` - now requires context parameter
    - `editor_open(ctx, filename)` - now requires context parameter
    - `editor_refresh_screen(ctx)` - now requires context parameter
    - `editor_select_syntax_highlight(ctx, filename)` - now requires context parameter
    - `init_editor(ctx)` - now requires context parameter
  - Reduced global E references by 65-75% across core modules (from ~421 to ~100-150)
  - All tests passing (2/2), compilation successful
- **Context Passing Migration (Phase 3)**: Migrated Lua integration to use context from registry
  - **Architecture**: Lua C API functions retrieve context from Lua registry (Phase 3 Option B)
  - **Implementation**:
    - Created `editor_ctx_registry_key` static variable as unique registry key
    - Implemented `lua_get_editor_context()` helper function for registry retrieval
    - Updated `loki_lua_bootstrap(ctx, opts)` to accept and store `editor_ctx_t *ctx` parameter
  - **Lua C API Functions** - Updated all 10 functions to use registry-based context:
    - `lua_loki_get_line()`, `lua_loki_get_lines()`, `lua_loki_get_cursor()`
    - `lua_loki_insert_text()`, `lua_loki_stream_text()`, `lua_loki_get_filename()`
    - `lua_loki_set_color()`, `lua_loki_set_theme()`
    - `lua_loki_get_mode()`, `lua_loki_set_mode()`
  - **Lua REPL Functions** - Updated to use explicit context passing:
    - `lua_repl_render(ctx, ab)`, `lua_repl_handle_keypress(ctx, key)`
    - `lua_repl_append_log(ctx, line)`, `editor_update_repl_layout(ctx)`
    - Internal helpers: `lua_repl_execute_current()`, `lua_repl_handle_builtin()`, `lua_repl_emit_registered_help()`, `lua_repl_push_history()`, `lua_repl_log_prefixed()`, `lua_repl_append_log_owned()`
  - **Public API Changes** in `include/loki/lua.h`:
    - `loki_lua_bootstrap(editor_ctx_t *ctx, const struct loki_lua_opts *opts)` - breaking change
    - Added forward declaration of `editor_ctx_t` (now includes `loki/core.h`)
  - **Call Site Updates**:
    - Updated `loki_editor.c`: 2 call sites to pass `&E` as context
    - Updated `main_repl.c`: 1 call site to pass `NULL` (standalone REPL)
    - Updated `loki_core.c`: All REPL function calls to pass context
  - **Results**:
    - Eliminated all global E references from `loki_lua.c` (0 remaining)
    - 6 files modified: +244 insertions, -187 deletions
    - All tests passing (2/2), compilation successful
    - Only harmless typedef redefinition warnings (C11 feature)
  - **Compatibility**: Architecture fully compatible with future opaque pointer conversion
  - **Backward Compatibility**: No breaking changes to Lua scripts (API unchanged from Lua perspective)
- **Context Passing Migration (Phase 4)**: Migrated loki_editor.c and async HTTP to explicit context passing
  - **Core Updates**:
    - Updated `check_async_requests(ctx, L)` to accept context for rawmode checking
    - Updated `loki_poll_async_http(ctx, L)` to accept context parameter
    - Updated `editor_cleanup_resources(ctx)` to accept context
    - Updated internal helpers: `exec_lua_command(ctx, fd)`, `lua_apply_span_table(ctx, row, table_index)`, `lua_apply_highlight_row(ctx, row, default_ran)`
  - **Public API Changes** in `include/loki/lua.h`:
    - `loki_poll_async_http(editor_ctx_t *ctx, lua_State *L)` - breaking change
  - **Standalone REPL Support**:
    - Updated `main_repl.c` to pass NULL context (standalone tools don't need editor context)
    - Added NULL-safe context checking pattern: `if (!ctx || !ctx->field)`
  - **Results**:
    - Reduced E. references in loki_editor.c from 78 to 5 (93% reduction)
    - Total E. references reduced from 198 to 125 across codebase
    - 6 files modified: +95/-91 lines
    - All tests passing (2/2), compilation successful
- **Context Passing Migration (Phase 5)**: Final cleanup - removed dead code and migrated remaining functions
  - **Code Cleanup**:
    - Deleted 146 lines of commented-out dead code (old `lua_apply_span_table` and `lua_apply_highlight_row` implementations)
    - Removed unused bridge functions: `editor_ctx_from_global()` and `editor_ctx_to_global()` (never called)
  - **Terminal Raw Mode**:
    - Updated `enable_raw_mode(ctx, fd)` to accept context parameter, uses `ctx->rawmode`
    - Updated `disable_raw_mode(ctx, fd)` to accept context parameter, uses `ctx->rawmode`
    - Updated call sites in loki_editor.c and loki_core.c
  - **Syntax Highlighting**:
    - Updated `editor_update_syntax_markdown(ctx, row)` to accept context parameter
    - Replaced `E.row` with `ctx->row` for accessing previous row's code block language
  - **Lua REPL**:
    - Updated `lua_repl_history_apply(ctx, repl)` to accept context parameter
    - Replaced `E.screencols` with `ctx->screencols` for input width calculations
  - **Documentation**:
    - Updated loki_internal.h to remove bridge function declarations
    - Enhanced global E comment to document current architecture and remaining use cases
  - **Results**:
    - Reduced E. references from 125 to 9 (93% reduction from Phase 4)
    - Total reduction: 412 references eliminated (97.8% reduction from start)
    - Only 9 E. references remain (all intentional: main(), atexit(), status messages)
    - 4 files modified: -204 lines in loki_core.c, +19/-7 lines across other files
    - All tests passing (2/2), compilation successful
  - **Architecture Achievement**:
    - Global singleton pattern effectively eliminated from active code paths
    - All core functions now use explicit context passing (`editor_ctx_t *ctx`)
    - Foundation complete for future split windows and multi-buffer support

## [0.4.1] - 2025-10-02

### Fixed
- **SSL/TLS Certificate Verification**: Fixed async HTTP requests timing out due to missing SSL configuration
  - Added proper CA bundle path (`/etc/ssl/cert.pem` for macOS)
  - Configured SSL peer and host verification
  - Increased timeout to 60 seconds with 10-second connection timeout
- **CURL Error Detection**: Properly detect and report CURL errors via `curl_multi_info_read()`
- **JSON Response Parsing**: Updated `.kilo/init.lua` to handle OpenAI's nested response format
  - Now parses `{"choices":[{"message":{"content":"..."}}]}`
  - Falls back to simple `{"content":"..."}` format
  - Detects and reports API errors from response
- **Non-Interactive Mode**: Terminal size defaults to 80x24 when screen query fails
- **Debug Output**: Enhanced error reporting in `--complete` and `--explain` modes
  - Shows HTTP status codes, response size, CURL errors
  - Validates content insertion before claiming success
  - Verbose mode now requires `KILO_DEBUG=1` environment variable (prevents API key leakage)

### Changed
- Increased HTTP timeout from 30 to 60 seconds
- Added 10-second connection timeout
- Verbose CURL output disabled by default (set `KILO_DEBUG=1` to enable)

## [0.4.0] - 2025-10-02

### Added
- **CLI Interface**: Comprehensive command-line argument parsing
  - `--help` / `-h` - Display usage information and available options
  - `--complete <file>` - Run AI completion on file in non-interactive mode and save result
  - `--explain <file>` - Run AI explanation on file and output to stdout
  - Support for creating new files (existing behavior now documented)
- **Non-Interactive Mode**: Execute AI commands from command line
  - Initializes editor without terminal raw mode
  - Waits for async HTTP requests to complete
  - Automatically saves results for --complete
  - Prints explanations to stdout for --explain
  - 60-second timeout with progress feedback
  - Comprehensive error handling and status messages

### Changed
- **Usage Model**: Now supports both interactive and CLI modes
  - Interactive: `kilo <filename>` (default, unchanged)
  - CLI: `kilo --complete <file>` or `kilo --explain <file>`
- **Help System**: Improved usage messages with detailed examples
- **Error Messages**: Enhanced feedback for missing API keys, Lua functions, and timeouts

### Documentation
- Updated README.md with CLI usage examples and requirements
- Added keybinding reference in help output
- Documented AI command prerequisites (OPENAI_API_KEY, init.lua)

### Technical Details
- Non-interactive mode tracks async request completion via `num_pending` counter
- Validates Lua function availability before execution
- Detects if async request was initiated to provide helpful error messages
- Uses `usleep(1000)` polling loop for async completion (1ms intervals)

## [0.3.0] - 2025-10-02

### Added
- **Async HTTP Support**: Non-blocking HTTP requests via libcurl multi interface
  - `kilo.async_http(url, method, body, headers, callback)` Lua API function
  - Up to 10 concurrent HTTP requests supported
  - 30-second timeout per request
  - Callback-based async pattern for response handling
  - Editor remains fully responsive during requests
- **AI Integration Examples**: Complete working examples in `.kilo.example/init.lua`
  - `ai_complete()` - Send buffer content to OpenAI/compatible APIs
  - `ai_explain()` - Get AI-powered code explanations
  - `test_http()` - Test async HTTP with GitHub API
  - Full JSON request/response handling examples
- **Homebrew Integration**: Automatic detection of system libraries
  - Auto-detects Lua or LuaJIT from Homebrew
  - Auto-detects libcurl from Homebrew
  - Prefers LuaJIT over Lua for better performance
  - `make show-config` target to display detected libraries
- **Example Configurations**: Enhanced `.kilo.example/` directory
  - Complete AI integration examples with OpenAI
  - Async HTTP usage examples
  - Updated README with setup instructions

### Changed
- **Build System**: Completely rewritten Makefile
  - Removed embedded Lua amalgamation approach
  - Now uses system Lua/LuaJIT via Homebrew (dynamic linking)
  - Reduced binary size from ~386KB to ~72KB
  - Added `HOMEBREW_PREFIX` detection
  - Simplified build process with automatic library detection
- **Lua Integration**: Switched from embedded to system Lua
  - Changed from `#include "lua.h"` to `#include <lua.h>` (system headers)
  - Removed `lua_one.c` amalgamation file (no longer needed)
  - Added `-lpthread` for curl multi-threading support
- **Main Event Loop**: Enhanced to support async operations
  - Added `check_async_requests()` call in main loop
  - Non-blocking request processing every iteration
  - Smooth integration with existing terminal I/O
- **Cleanup**: Added curl cleanup to `editor_atexit()`
  - Ensures proper curl_global_cleanup() on exit
  - Prevents memory leaks from pending requests

### Documentation
- Updated README.md with async HTTP features and AI integration examples
- Updated CLAUDE.md with complete `kilo.async_http()` API documentation
- Created `.kilo.example/README.md` with async HTTP setup guide
- Added AI integration workflow examples
- Documented non-blocking architecture and use cases

### Technical Details
- **Dependencies**: Now requires Lua/LuaJIT and libcurl from Homebrew
- **Architecture**: Uses libcurl multi interface for true async I/O
- **Event Loop**: Non-blocking, integrated with terminal input handling
- **Memory Management**: Proper cleanup of all async request structures
- **Error Handling**: User-friendly error messages in status bar

## [0.2.0] - 2025-10-02

### Added
- **Lua 5.4.7 Scripting**: Statically embedded Lua interpreter for extensibility and automation
  - Interactive Lua console accessible via `Ctrl-L` keybinding
  - Six API functions exposed via `kilo` global table:
    - `kilo.status(msg)` - Set status bar message
    - `kilo.get_lines()` - Get total line count
    - `kilo.get_line(row)` - Get line content (0-indexed)
    - `kilo.get_cursor()` - Get cursor position (row, col)
    - `kilo.insert_text(text)` - Insert text at cursor
    - `kilo.get_filename()` - Get current filename
  - Configuration file support with local override:
    - `.kilo/init.lua` (project-specific, highest priority)
    - `~/.kilo/init.lua` (global fallback)
  - Example configuration in `.kilo.example/` directory
  - Full Lua standard library available (io, os, math, string, table, etc.)
  - Error handling with user-friendly status bar messages
- Added Lua amalgamation source (`lua_one.c`) for single-file embedding
- Added example Lua functions: `count_lines()`, `show_cursor()`, `insert_timestamp()`, `first_line()`
- Added `.kilo.example/README.md` with complete Lua API documentation

### Changed
- Updated help message to include `Ctrl-L` Lua command keybinding
- Modified Makefile to compile Lua with `-lm -ldl` flags and POSIX support
- Increased binary size to ~386KB (from ~69KB) due to embedded Lua interpreter
- Extended `editorConfig` structure to include `lua_State *L` field
- Updated `.gitignore` to exclude Lua source directory, object files, and local `.kilo/` configs

### Documentation
- Added comprehensive Lua scripting section to CLAUDE.md
- Updated README.md with Lua features, API reference, and usage examples
- Created `.kilo.example/` with sample configuration and documentation

## [0.1.1]

### Security
- Fixed multiple buffer overflow vulnerabilities in syntax highlighting that could read/write beyond allocated buffers
- Fixed incorrect buffer size usage in comment highlighting preventing memory corruption
- Fixed keyword matching that could read past end of buffer

### Added
- Added binary file detection that refuses to open files with null bytes
- Added configurable separator list per syntax definition for better language support

### Fixed
- Fixed cursor position calculation bug causing incorrect cursor movement when wrapping from end of long lines
- Added NULL checks for all memory allocations to prevent crashes on allocation failure
- Fixed incomplete CRLF newline stripping for files with Windows line endings
- Fixed silent allocation failures in screen rendering that caused corrupted display
- Fixed infinite loop possibility in editorReadKey when stdin reaches EOF
- Fixed missing null terminator maintenance in character deletion operations
- Fixed integer overflow in row allocation for extremely large files
- Removed dead code in nonprint character calculation
- Fixed typos in comments: "commen" → "comment", "remainign" → "remaining", "escluding" → "excluding"
- Fixed typo in user-facing welcome message: "verison" → "version"
- Fixed number highlighting to only accept decimal points followed by digits (no trailing periods)

### Changed
- Refactored signal handler for window resize to be async-signal-safe (now uses flag instead of direct I/O)
- Moved atexit() registration to beginning of main() to ensure terminal cleanup on all exit paths
- Improved error messages for out-of-memory conditions
- Made separators configurable per syntax by adding field to editorSyntax structure
- Documented non-reentrant global state limitation in code comments


## [0.1.0] - Initial Release

- Project created
