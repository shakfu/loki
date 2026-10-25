-- Test/Demo Module
-- Provides test functions to verify async HTTP and other features

local M = {}

-- Internal: handler for HTTP test
local function http_handler(response)
    local MODE = loki.get_lines and "editor" or "repl"
    if not response.error and response.body then
        if MODE == "editor" then
            loki.insert_text("\n\nGitHub Zen: " .. response.body .. "\n")
            loki.status("HTTP test successful!")
        else
            print("\nGitHub Zen: " .. response.body)
            print("HTTP test successful!")
        end
    else
        local reason = response.error or "empty response"
        if MODE == "editor" then
            loki.status("HTTP test failed: " .. reason)
        else
            print("HTTP test failed: " .. reason)
        end
    end
end

-- Test the async HTTP with a simple GET request
function M.http()
    local id, err = loki.async_http(
        "https://api.github.com/zen",
        "GET",
        nil,
        {"User-Agent: loki-editor"},
        "test_http_handler"
    )
    local MODE = loki.get_lines and "editor" or "repl"
    if not id then
        if MODE == "editor" then
            loki.status("HTTP test failed: " .. err)
        else
            print("HTTP test failed: " .. err)
        end
    elseif MODE == "editor" then
        loki.status("Testing HTTP...")
    else
        print("Testing HTTP...")
    end
end

-- Export handler to global scope (required for callback)
_G.test_http_handler = http_handler

-- Register help for REPL
if loki.repl and loki.repl.register then
    loki.repl.register("test.http", "Test async HTTP with GitHub API")
end

return M
