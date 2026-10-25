-- AI Integration Module
-- Provides AI completion and explanation via OpenAI API
-- Requires OPENAI_API_KEY environment variable

local M = {}

-- Detect execution mode (editor or REPL)
local MODE = loki.get_lines and "editor" or "repl"

-- Internal: response handler for AI requests
local function response_handler(response)
    if not response then
        if MODE == "editor" then
            loki.status("Error: No response from AI")
        else
            print("Error: No response from AI")
        end
        return
    end

    -- Check for errors
    if response.error then
        if MODE == "editor" then
            loki.status("Error: " .. response.error)
        else
            print("Error: " .. response.error)
        end
        return
    end

    -- Check HTTP status
    if response.status ~= 200 then
        if MODE == "editor" then
            loki.status(string.format("HTTP error %d", response.status))
        else
            print(string.format("HTTP error %d", response.status))
        end
        return
    end

    local body = response.body
    if not body then
        if MODE == "editor" then
            loki.status("Error: Empty response body")
        else
            print("Error: Empty response body")
        end
        return
    end

    -- Check for API errors in JSON body
    local error_msg = body:match('"error"%s*:%s*{.-"message"%s*:%s*"(.-)"')
    if error_msg then
        if MODE == "editor" then
            loki.status("API Error: " .. error_msg)
        else
            print("API Error: " .. error_msg)
        end
        return
    end

    -- Parse OpenAI response format: {"choices":[{"message":{"content":"..."}}]}
    -- First try the nested format (OpenAI chat completions)
    local content = body:match('"message"%s*:%s*{.-"content"%s*:%s*"(.-[^\\])"')

    -- If that fails, try simple format: {"content":"..."}
    if not content then
        content = body:match('"content"%s*:%s*"(.-[^\\])"')
    end

    if content then
        -- Unescape JSON string
        content = content:gsub('\\n', '\n')
        content = content:gsub('\\t', '\t')
        content = content:gsub('\\"', '"')
        content = content:gsub('\\\\', '\\')

        -- Output response based on context
        if MODE == "editor" then
            -- Editor context: insert into buffer
            loki.insert_text("\n\n--- AI Response ---\n" .. content .. "\n---\n")
            loki.status("AI response inserted!")
        else
            -- REPL context: print to console
            print("\n--- AI Response ---")
            print(content)
            print("---\n")
        end
    else
        -- Show first 100 chars of response for debugging
        local preview = body:sub(1, math.min(100, #body))
        if MODE == "editor" then
            loki.status("Error parsing response: " .. preview)
        else
            print("Error parsing response: " .. preview)
        end
    end
end

-- Send entire buffer (or text) to OpenAI and insert response asynchronously
-- In editor: ai.complete() uses buffer content
-- In REPL: ai.complete("your prompt text") or ai.complete() prompts for input
function M.complete(text)
    local prompt

    if text then
        -- Explicit text provided
        prompt = text
    elseif MODE == "editor" then
        -- Editor context: get buffer content
        local lines = {}
        for i = 0, loki.get_lines() - 1 do
            table.insert(lines, loki.get_line(i))
        end
        prompt = table.concat(lines, "\n")
    else
        -- REPL context without text: error
        print("Usage: ai.complete('your prompt text')")
        return
    end

    -- Get API key from environment
    local api_key = os.getenv("OPENAI_API_KEY")
    if not api_key or api_key == "" then
        if MODE == "editor" then
            loki.status("Error: OPENAI_API_KEY environment variable not set")
        else
            print("Error: OPENAI_API_KEY environment variable not set")
        end
        return
    end

    -- Build JSON request body
    local json_body = string.format([[{
  "model": "gpt-4o-mini",
  "messages": [
    {"role": "user", "content": %s}
  ]
}]], string.format("%q", prompt))

    -- Set up headers
    local headers = {
        "Content-Type: application/json",
        "Authorization: Bearer " .. api_key
    }

    -- Make async HTTP request (non-blocking)
    local id, err = loki.async_http(
        "https://api.openai.com/v1/chat/completions",
        "POST",
        json_body,
        headers,
        "ai_response_handler"  -- Callback function
    )

    if not id then
        if MODE == "editor" then
            loki.status("Error: " .. err)
        else
            print("Error: " .. err)
        end
    elseif MODE == "editor" then
        loki.status("AI request sent... (will appear when ready)")
    else
        print("AI request sent... (will appear when ready)")
    end
end

-- Get code explanation from AI
-- In editor: ai.explain() uses buffer content
-- In REPL: ai.explain("code to explain")
function M.explain(code)
    if not code then
        if MODE == "editor" then
            -- Editor context: get buffer content
            local lines = {}
            for i = 0, loki.get_lines() - 1 do
                table.insert(lines, loki.get_line(i))
            end
            code = table.concat(lines, "\n")
        else
            -- REPL context without text: error
            print("Usage: ai.explain('code to explain')")
            return
        end
    end

    local api_key = os.getenv("OPENAI_API_KEY")
    if not api_key or api_key == "" then
        if MODE == "editor" then
            loki.status("Error: OPENAI_API_KEY not set")
        else
            print("Error: OPENAI_API_KEY not set")
        end
        return
    end

    local prompt = "Explain this code concisely:\n\n" .. code
    local json_body = string.format([[{
  "model": "gpt-4o-mini",
  "messages": [
    {"role": "user", "content": %s}
  ]
}]], string.format("%q", prompt))

    local headers = {
        "Content-Type: application/json",
        "Authorization: Bearer " .. api_key
    }

    local id, err = loki.async_http(
        "https://api.openai.com/v1/chat/completions",
        "POST",
        json_body,
        headers,
        "ai_response_handler"
    )

    if not id then
        if MODE == "editor" then
            loki.status("Error: " .. err)
        else
            print("Error: " .. err)
        end
    elseif MODE == "editor" then
        loki.status("Requesting code explanation...")
    else
        print("Requesting code explanation...")
    end
end

-- Export response handler to global scope (required for callback)
_G.ai_response_handler = response_handler

-- Register help for REPL
if loki.repl and loki.repl.register then
    if MODE == "editor" then
        loki.repl.register("ai.complete", "Send buffer to AI for completion")
        loki.repl.register("ai.explain", "Get code explanation from AI")
    else
        loki.repl.register("ai.complete", "Send text to AI for completion (requires OPENAI_API_KEY)")
        loki.repl.register("ai.explain", "Get code explanation from AI (requires OPENAI_API_KEY)")
    end
end

return M
