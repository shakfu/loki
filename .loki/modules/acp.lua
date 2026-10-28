-- acp.lua - ACP client: drive an external coding agent from loki
--
-- loki spawns the agent and speaks the Agent Client Protocol on its stdio
-- (https://agentclientprotocol.com). The agent reads files through loki, so
-- it sees unsaved edits; its writes update an open buffer as one undo step
-- and are saved, since the agent's own tools (tests, builds) read the disk.
-- Paths must lie inside the directory the agent started in.
--
--   :agent <prompt>    start the agent if needed and send a prompt
--   :agent cancel      cancel the current turn
--   :agent stop        stop the agent
--
-- Replies stream into the *agent* buffer. Permission requests appear on the
-- message line; press a number to choose, Esc to reject.

local jsonrpc = require("jsonrpc")
local mcp = require("mcp")
local json = loki.json

local M = {}

-- The agent command; override in init.lua, e.g. {"claude-code-acp"}.
M.command = { "myra", "--acp" }

-- Optional hook: on_stop(stopReason, err) after each turn.
M.on_stop = nil
M.PROTOCOL_VERSION = 1

local state  -- nil when no agent is running

local function status(msg) loki.status("agent: " .. msg) end

-- One *agent* buffer, kept across agent restarts.
local out_id

local function output_buffer()
  if not out_id or not loki.buffer_text(out_id) then
    out_id = loki.buffer_new("*agent*")
  end
  return out_id
end

local function append(text)
  local id = output_buffer()
  if not id then return end
  local t = loki.buffer_text(id)
  loki.buffer_replace(id, #t + 1, #t, text)
end

local function invalid(msg)
  return nil, { code = jsonrpc.INVALID_PARAMS, message = msg }
end

-- Lines line..line+limit-1 of text (1-based); whole text when both are nil.
local function slice(text, line, limit)
  if not line and not limit then return text end
  local out, n, first = {}, 0, line or 1
  for l in (text .. (text:sub(-1) == "\n" and "" or "\n")):gmatch("(.-)\n") do
    n = n + 1
    if n >= first and (not limit or n < first + limit) then out[#out + 1] = l .. "\n" end
  end
  return table.concat(out)
end

-- Show queued permission requests one at a time.
local function next_permission()
  local req = state and state.perms[1]
  if not req then return end
  local labels = {}
  for i, o in ipairs(req.options) do
    if i > 9 then break end
    labels[i] = o.name or o.optionId
  end
  local function answer(i)
    if state then table.remove(state.perms, 1) end
    local o = i and req.options[i]
    req.ctx.reply({ outcome = o and { outcome = "selected", optionId = o.optionId }
                                  or { outcome = "cancelled" } })
    next_permission()
  end
  -- Another module's question on screen: decline rather than wait unseen.
  if not loki.choose("agent: " .. req.title, labels, answer) then answer(nil) end
end

local handlers = {}

handlers["session/update"] = function(p)
  local u = type(p) == "table" and p.update
  if type(u) ~= "table" then return end
  local kind = u.sessionUpdate
  if kind == "agent_message_chunk" and type(u.content) == "table" and u.content.type == "text" then
    append(u.content.text)
  elseif kind == "tool_call" then
    append("\n[" .. tostring(u.title or u.toolCallId) .. "]\n")
    status(tostring(u.title or u.toolCallId))
  elseif kind == "tool_call_update" and u.status then
    status(tostring(u.title or u.toolCallId) .. " " .. tostring(u.status))
  end
end

handlers["fs/read_text_file"] = function(p)
  local path = type(p) == "table" and mcp.inside_root(p.path, state.root)
  if not path then return invalid("path is outside the project: " .. tostring(p and p.path)) end
  local id, text = loki.buffer_find(path), nil
  if id then
    text = loki.buffer_text(id) .. "\n"  -- as :w would write it
  else
    local f = io.open(path, "rb")
    if not f then return invalid("cannot read " .. path) end
    text = f:read("a")
    f:close()
  end
  return { content = slice(text, p.line, p.limit) }
end

handlers["fs/write_text_file"] = function(p)
  local path = type(p) == "table" and mcp.inside_root(p.path, state.root)
  if not path then return invalid("path is outside the project: " .. tostring(p and p.path)) end
  if type(p.content) ~= "string" then return invalid("content must be a string") end
  local id = loki.buffer_find(path)
  if id then
    local new = p.content:gsub("\n$", "")  -- the buffer adds it back on save
    local old = loki.buffer_text(id)
    local ok, err = loki.buffer_replace(id, 1, #old, new)
    if ok then ok, err = loki.buffer_save(id) end
    if not ok then return invalid(tostring(err)) end
  else
    local f, err = io.open(path, "wb")  -- the spec requires creating the file
    if not f then return invalid(tostring(err)) end
    f:write(p.content)
    f:close()
  end
  status("wrote " .. path)
  return {}
end

handlers["session/request_permission"] = function(p, ctx)
  if type(p) ~= "table" or type(p.options) ~= "table" or #p.options == 0 then
    return invalid("request_permission needs options")
  end
  local call = type(p.toolCall) == "table" and p.toolCall or {}
  state.perms[#state.perms + 1] = {
    ctx = ctx, options = p.options, title = tostring(call.title or call.toolCallId or "tool call"),
  }
  if #state.perms == 1 then next_permission() end
  return jsonrpc.ASYNC
end

local function mcp_servers()
  if not mcp.server then return json.array() end
  return json.array({ {
    name = "loki", command = loki.exepath(),
    args = json.array({ "--mcp-shim", mcp.server:path() }), env = json.array(),
  } })
end

local function send_prompt(text)
  local blocks = { { type = "text", text = text } }
  for _, b in ipairs(loki.buffers()) do
    if b.current and b.path then
      blocks[#blocks + 1] = { type = "resource_link", uri = "file://" .. b.path, name = b.name }
    end
  end
  append((loki.buffer_text(output_buffer()) == "" and "" or "\n") .. "> " .. text .. "\n")
  state.busy = true
  state.peer.request("session/prompt", { sessionId = state.session, prompt = json.array(blocks) },
    function(res, err)
      if not state then return end
      state.busy = false
      if res then status(tostring(res.stopReason)) else status("error: " .. tostring(err.message)) end
      if M.on_stop then M.on_stop(res and res.stopReason, err) end
    end)
end

local function fail(msg)
  status(msg)
  M.stop()
end

-- Start the agent (cmd defaults to M.command). Returns true, or nil, message.
function M.start(cmd)
  if state then return true end
  cmd = cmd or M.command
  local root = loki.abspath(".")
  state = { root = root, perms = {}, queued = {}, busy = false }
  local feed
  local proc, err = loki.spawn{
    cmd = cmd[1], args = { table.unpack(cmd, 2) }, cwd = root,
    on_stdout = function(_, s) feed(s) end,
    on_stderr = function(_, s) if state then state.stderr = s end end,
    on_exit = function(p, code, sig)
      local s = state
      if not s or s.proc ~= p then return end
      state = nil
      s.peer.close("agent exited")
      local why = sig and ("signal " .. sig) or ("status " .. tostring(code))
      local tail = s.stderr and s.stderr:match("([^\n]+)\n*$")
      status("exited (" .. why .. ")" .. (tail and ": " .. tail or ""))
    end,
  }
  if not proc then
    state = nil
    return nil, err
  end
  local peer = jsonrpc.peer(function(s) proc:write(s) end, handlers)
  feed = jsonrpc.lines(peer.receive, function() proc:kill() end)
  state.proc, state.peer = proc, peer
  output_buffer()

  peer.request("initialize", {
    protocolVersion = M.PROTOCOL_VERSION,
    clientCapabilities = { fs = { readTextFile = true, writeTextFile = true }, terminal = false },
    clientInfo = { name = "loki", version = "1" },
  }, function(res, ierr)
    if not state then return end
    if not res then return fail("initialize failed: " .. tostring(ierr.message)) end
    if res.protocolVersion ~= M.PROTOCOL_VERSION then
      return fail("unsupported ACP version " .. tostring(res.protocolVersion))
    end
    peer.request("session/new", { cwd = root, mcpServers = mcp_servers() }, function(sres, serr)
      if not state then return end
      if not sres then return fail("session/new failed: " .. tostring(serr.message)) end
      state.session = sres.sessionId
      local queued = state.queued
      state.queued = {}
      if queued[1] then send_prompt(table.concat(queued, "\n\n")) end
    end)
  end)
  return true
end

-- Send a prompt, starting the agent first if needed.
function M.ask(text)
  local ok, err = M.start()
  if not ok then return nil, err end
  if not state.session then
    state.queued[#state.queued + 1] = text
    status("starting...")
    return true
  end
  if state.busy then return nil, "busy; :agent cancel first" end
  send_prompt(text)
  status("working; replies in *agent* (Ctrl-X n)")
  return true
end

-- Cancel the current turn. Queued permission requests are answered with
-- "cancelled", as the spec requires.
function M.cancel()
  if not state or not state.session then return end
  state.peer.notify("session/cancel", { sessionId = state.session })
  local perms = state.perms
  state.perms = {}
  for _, req in ipairs(perms) do req.ctx.reply({ outcome = { outcome = "cancelled" } }) end
  if perms[1] then loki.choose_cancel() end  -- the first one is on screen
end

function M.stop()
  local s = state
  if not s then return end
  state = nil
  s.proc:close_stdin()
  s.proc:kill("TERM")
  s.peer.close("agent stopped")
end

function M.running() return state ~= nil end

M.handlers = handlers

function M.register_command()
  if not loki.register_ex_command then return end
  loki.register_ex_command("agent", function(args)
    args = (args or ""):match("^%s*(.-)%s*$")
    if args == "stop" then M.stop(); status("stopped"); return true end
    if args == "cancel" then M.cancel(); status("cancel sent"); return true end
    if args == "" then
      local ok, err = M.start()
      if not ok then status(tostring(err)) end
      return ok ~= nil
    end
    local ok, err = M.ask(args)
    if not ok then status(tostring(err)) end
    return ok ~= nil
  end, "Agent: :agent <prompt> | cancel | stop")
end

return M
