-- Example Module Template
-- Copy this file to create your own custom module

local M = {}

-- Detect execution mode (editor or REPL)
local MODE = loki.get_lines and "editor" or "repl"

-- Example function 1: Simple status message
function M.hello()
    if MODE == "editor" then
        loki.status("Hello from custom module!")
    else
        print("Hello from custom module!")
    end
end

-- Example function 2: Working with buffer content (editor only)
function M.word_count()
    if MODE ~= "editor" then
        print("This function only works in editor mode")
        return
    end

    local total_words = 0
    for i = 0, loki.get_lines() - 1 do
        local line = loki.get_line(i)
        if line then
            -- Count words by splitting on whitespace
            for word in line:gmatch("%S+") do
                total_words = total_words + 1
            end
        end
    end

    loki.status(string.format("Total words: %d", total_words))
end

-- Example function 3: Using async HTTP
function M.fetch_quote()
    local id, err = loki.async_http(
        "https://api.quotable.io/random",
        "GET",
        nil,
        {},
        "example_quote_handler"
    )

    if not id then
        if MODE == "editor" then
            loki.status("Error: " .. err)
        else
            print("Error: " .. err)
        end
    elseif MODE == "editor" then
        loki.status("Fetching random quote...")
    else
        print("Fetching random quote...")
    end
end

-- Callback handler for HTTP request (must be global)
function _G.example_quote_handler(response)
    if response.error then
        if MODE == "editor" then
            loki.status("Error: " .. response.error)
        else
            print("Error: " .. response.error)
        end
    elseif response.body then
        local content = response.body:match('"content"%s*:%s*"(.-[^\\])"')
        if content then
            -- Unescape JSON
            content = content:gsub('\\n', '\n'):gsub('\\"', '"')

            if MODE == "editor" then
                loki.insert_text("\n" .. content .. "\n")
                loki.status("Quote inserted!")
            else
                print("\n" .. content)
            end
        end
    end
end

-- Example function 4: Helper utility
function M.uppercase_line()
    if MODE ~= "editor" then
        print("This function only works in editor mode")
        return
    end

    local row, col = loki.get_cursor()
    local line = loki.get_line(row)

    if line then
        local upper = line:upper()
        -- Note: This is simplified - real implementation would need
        -- to delete the line and insert new one
        loki.status("Current line: " .. upper)
    end
end

-- Register functions with REPL help system
if loki.repl and loki.repl.register then
    loki.repl.register("example.hello", "Display a hello message")
    loki.repl.register("example.word_count", "Count words in buffer (editor only)")
    loki.repl.register("example.fetch_quote", "Fetch random quote via HTTP")
    loki.repl.register("example.uppercase_line", "Show current line in uppercase (editor only)")
end

-- Return module table
return M
