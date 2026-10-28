-- mcp.lua - MCP server: lets an agent read and edit loki's buffers
--
-- Serves both MCP eras: requests carrying a per-request protocol version
-- (2026-07-28) and the legacy initialize handshake (2025-11-25 and older).
-- The transport is a Unix socket; agents launch `loki --mcp-shim` as their
-- stdio server. Spec: https://modelcontextprotocol.io/specification
--
-- Edits go into buffers and are never saved: the user reviews them and
-- writes with :w.

local jsonrpc = require("jsonrpc")
local json = loki.json

local M = {}

M.MODERN = { "2026-07-28" }
M.LEGACY = { "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05" }
M.UNSUPPORTED_VERSION = -32022

local VERSION_KEY = "io.modelcontextprotocol/protocolVersion"
local SERVER_INFO = { name = "loki", version = "1" }

local function contains(list, v)
  for _, x in ipairs(list) do if x == v then return true end end
  return false
end

local function all_versions()
  local all = {}
  for _, v in ipairs(M.MODERN) do all[#all + 1] = v end
  for _, v in ipairs(M.LEGACY) do all[#all + 1] = v end
  return all
end

-- The absolute form of path if it resolves inside root (default M.root,
-- the project root), else nil. Shared with acp.lua.
function M.inside_root(path, root)
  root = root or M.root
  local p = type(path) == "string" and loki.abspath(path)
  if not p or not root then return nil end
  if p == root or p:sub(1, #root + 1) == root .. "/" then return p end
  return nil
end

local function buffer_for(path)
  local id = type(path) == "string" and loki.buffer_find(path)
  if not id then return nil, "not open in loki: " .. tostring(path) end
  return id
end

local function text_result(text, is_error)
  return { content = json.array({ { type = "text", text = text } }), isError = is_error or false }
end

local function schema(props, required)
  return {
    type = "object",
    properties = props or {},
    required = required and json.array(required) or nil,
    additionalProperties = false,
  }
end

local STR = { type = "string" }
local INT = { type = "integer", minimum = 1 }

-- Tools in a fixed order (the spec asks for deterministic listing).
M.tools = {
  {
    name = "list_buffers",
    description = "List buffers open in the loki editor: path, whether they have unsaved changes, cursor line and column.",
    inputSchema = schema(),
    run = function()
      local out = {}
      for _, b in ipairs(loki.buffers()) do
        out[#out + 1] = { path = b.path, name = b.name, dirty = b.dirty,
                          current = b.current, line = b.line, col = b.col }
      end
      return json.encode(json.array(out))
    end,
  },
  {
    name = "read_buffer",
    description = "Read an open buffer's text, including unsaved edits. Lines are 1-based and inclusive.",
    inputSchema = schema({ path = STR, start_line = INT, end_line = INT }, { "path" }),
    run = function(a)
      local id, err = buffer_for(a.path)
      if not id then return nil, err end
      return loki.buffer_text(id, a.start_line, a.end_line)
    end,
  },
  {
    name = "get_selection",
    description = "The visual selection in the current buffer, or null.",
    inputSchema = schema(),
    run = function()
      local s = loki.get_selection()
      if not s then return "null" end
      for _, b in ipairs(loki.buffers()) do
        if b.id == s.id then s.path = b.path end
      end
      s.id = nil
      return json.encode(s)
    end,
  },
  {
    name = "edit_buffer",
    description = "Replace old_string, which must occur exactly once, with new_string in an open buffer. "
      .. "The edit is one undo step and is not saved; the user reviews and saves it.",
    inputSchema = schema({ path = STR, old_string = STR, new_string = STR },
                         { "path", "old_string", "new_string" }),
    run = function(a)
      if type(a.old_string) ~= "string" or type(a.new_string) ~= "string" or a.old_string == "" then
        return nil, "old_string must be a non-empty string and new_string a string"
      end
      if not M.inside_root(a.path) then return nil, "path is outside the project: " .. tostring(a.path) end
      local id, err = buffer_for(a.path)
      if not id then return nil, err .. " (open it with open_file first)" end
      local text = loki.buffer_text(id)
      local s, e = text:find(a.old_string, 1, true)
      if not s then return nil, "old_string not found" end
      local n, at = 1, e + 1
      while true do
        local s2, e2 = text:find(a.old_string, at, true)
        if not s2 then break end
        n, at = n + 1, e2 + 1
      end
      if n > 1 then return nil, ("old_string matches %d times; include more context"):format(n) end
      local ok, rerr = loki.buffer_replace(id, s, e, a.new_string)
      if not ok then return nil, rerr end
      return "edited " .. a.path
    end,
  },
  {
    name = "open_file",
    description = "Open a file inside the project in loki and show it, optionally at a 1-based line.",
    inputSchema = schema({ path = STR, line = INT }, { "path" }),
    run = function(a)
      local p = M.inside_root(a.path)
      if not p then return nil, "path is outside the project: " .. tostring(a.path) end
      local id, err = loki.open(p, a.line)
      if not id then return nil, err end
      return "opened " .. p
    end,
  },
}

local by_name = {}
for _, t in ipairs(M.tools) do by_name[t.name] = t end

local function is_modern(params)
  return type(params) == "table" and type(params._meta) == "table"
     and params._meta[VERSION_KEY] ~= nil
end

local function complete(result, params)
  if is_modern(params) then result.resultType = "complete" end
  return result
end

M.handlers = {
  -- Legacy handshake: answer with the client's version when supported.
  initialize = function(p)
    local want = type(p) == "table" and p.protocolVersion
    return {
      protocolVersion = contains(M.LEGACY, want) and want or M.LEGACY[1],
      capabilities = { tools = {} },
      serverInfo = SERVER_INFO,
    }
  end,
  ["notifications/initialized"] = function() end,
  ["notifications/cancelled"] = function() end,
  ping = function(p) return complete({}, p) end,

  ["server/discover"] = function(p)
    return complete({
      supportedVersions = json.array(all_versions()),
      capabilities = { tools = {} },
      _meta = { ["io.modelcontextprotocol/serverInfo"] = SERVER_INFO },
    }, p)
  end,

  ["tools/list"] = function(p)
    local list = {}
    for _, t in ipairs(M.tools) do
      list[#list + 1] = { name = t.name, description = t.description, inputSchema = t.inputSchema }
    end
    return complete({ tools = json.array(list) }, p)
  end,

  ["tools/call"] = function(p)
    if type(p) ~= "table" or type(p.name) ~= "string" then
      return nil, { code = jsonrpc.INVALID_PARAMS, message = "tools/call needs a tool name" }
    end
    local tool = by_name[p.name]
    if not tool then
      return nil, { code = jsonrpc.INVALID_PARAMS, message = "unknown tool: " .. p.name }
    end
    local args = type(p.arguments) == "table" and p.arguments or {}
    local ok, text, err = pcall(tool.run, args)
    if not ok then return complete(text_result(tostring(text), true), p) end
    if text == nil then return complete(text_result(err or "failed", true), p) end
    if M.on_tool then M.on_tool(p.name, args) end
    return complete(text_result(text), p)
  end,
}

-- Reject a per-request version we do not serve, listing the ones we do.
local function check_version(_, params)
  if not is_modern(params) then return true end
  local v = params._meta[VERSION_KEY]
  if contains(M.MODERN, v) then return true end
  return nil, { code = M.UNSUPPORTED_VERSION, message = "Unsupported protocol version",
                data = { supported = json.array(all_versions()), requested = v } }
end

-- A peer for one connection; write(s) sends to the client.
function M.peer(write)
  return jsonrpc.peer(write, M.handlers, { before = check_version })
end

-- Start listening on path (default loki.socket_path()). Returns the path,
-- or nil, message. Idempotent.
function M.start(path)
  if M.server then return M.server:path() end
  M.root = loki.abspath(".")
  local peers = {}
  local server, err = loki.listen{
    path = path or loki.socket_path(),
    on_connect = function(conn)
      local peer = M.peer(function(s) conn:write(s) end)
      peers[conn] = jsonrpc.lines(peer.receive, function() conn:close() end)
    end,
    on_data = function(conn, chunk)
      local feed = peers[conn]
      if feed then feed(chunk) end
    end,
    on_close = function(conn) peers[conn] = nil end,
  }
  if not server then return nil, err end
  M.server = server
  return server:path()
end

function M.stop()
  if M.server then M.server:close() end
  M.server = nil
end

-- :mcp starts the server and shows how to connect an agent.
function M.register_command()
  if not loki.register_ex_command then return end
  loki.register_ex_command("mcp", function()
    local path, err = M.start()
    if not path then
      loki.status("mcp: " .. tostring(err))
      return false
    end
    loki.status("mcp: listening; agent command: loki --mcp-shim " .. path)
    return true
  end, "Start the MCP server for agents")
end

return M
