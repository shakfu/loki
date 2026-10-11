-- ==============================================================================
-- Loki Editor Configuration
-- ==============================================================================
--
-- This is the main configuration file for Loki editor.
-- It loads modular components and sets up your editing environment.
--
-- Loading priority:
--   1. .loki/init.lua (local, project-specific), only if the project is
--      trusted: run :trust once, which records it in ~/.loki/trusted
--   2. ~/.loki/init.lua (global, home directory)
--
-- If a trusted .loki/init.lua exists, the global config is NOT loaded.
--
-- ==============================================================================

-- Detect execution mode: "editor" or "repl"
MODE = loki.get_lines and "editor" or "repl"

-- Module path: loki adds ~/.loki/modules, and .loki/modules for a trusted
-- project, to package.path before this file runs.

-- ==============================================================================
-- Load Core Modules
-- ==============================================================================

-- Load language definitions (lazy loading - loads on file open)
languages = require("languages")  -- Global for REPL access
local ext_count = languages.init()

-- Load markdown and TODO highlighting module (for advanced markdown features)
local markdown = require("markdown")
markdown.install()

-- Load theme utilities
theme = require("theme")

-- ==============================================================================
-- Load Feature Modules (only in editor mode)
-- ==============================================================================

if MODE == "editor" then
    -- Editor utilities
    editor = require("editor")

    -- Backward compatibility aliases (deprecated)
    count_lines = editor.count_lines
    show_cursor = editor.cursor
    insert_timestamp = editor.timestamp
    first_line = editor.first_line
end

-- AI functions (available in both editor and REPL)
ai = require("ai")

-- Backward compatibility aliases (deprecated)
ai_complete = ai.complete
ai_explain = ai.explain

-- Test/demo functions
test = require("test")

-- Backward compatibility alias (deprecated)
test_http = test.http

-- ==============================================================================
-- Color Theme Configuration
-- ==============================================================================

-- Uncomment one of these to load a pre-made theme (only works in editor mode):
if MODE == "editor" then
    local ok, err = theme.load("nord")
    if not ok then
        loki.status("Theme error: " .. tostring(err))
    end
    -- theme.load("dracula")
    -- theme.load("monokai")
    -- theme.load("nord")
    -- theme.load("github-light")
end

-- Or define your own custom colors:
-- loki.set_theme({
--     normal = {r=200, g=200, b=200},
--     comment = {r=100, g=150, b=100},
--     keyword1 = {r=220, g=100, b=220},
--     keyword2 = {r=100, g=220, b=220},
--     string = {r=220, g=220, b=100},
--     number = {r=200, g=100, b=200},
--     match = {r=100, g=150, b=220}
-- })

-- ==============================================================================
-- Modal Editing (Optional - Vim-like modes)
-- ==============================================================================

-- Load modal module (vim-like editing enabled by default)
if MODE == "editor" then
    modal = require("modal")

    -- Modal editing starts in NORMAL mode by default (vim-like)
    -- To start in INSERT mode instead (traditional editor behavior):
    -- modal.disable()

    -- Available commands:
    -- modal.enable()  - Switch to normal mode (vim-like)
    -- modal.disable() - Switch to insert mode (traditional)
    -- modal.help()    - Show modal commands
end

-- ==============================================================================
-- Startup Message
-- ==============================================================================

if ext_count > 0 then
    loki.status(string.format("Loki initialized! %d extensions available (lazy loading).", ext_count))
else
    loki.status("Loki initialized! Press Ctrl-L to run commands.")
end

print("Loki configuration loaded successfully")
print("Available modules: editor, ai, test, theme, languages, markdown, modal")
print("")
print("Type :help in REPL for available commands")
print("Type modal.help() to see modal editing commands (vim-like)")
