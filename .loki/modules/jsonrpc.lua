-- jsonrpc.lua - JSON-RPC 2.0 over newline-delimited streams
--
-- Shared by mcp.lua (loki as server) and acp.lua (loki as client). One
-- message per line, as in the MCP stdio binding.

local M = {}

local json = loki.json

M.PARSE_ERROR = -32700
M.INVALID_REQUEST = -32600
M.METHOD_NOT_FOUND = -32601
M.INVALID_PARAMS = -32602
M.INTERNAL_ERROR = -32603

M.MAX_LINE = 64 * 1024 * 1024

-- Returns fn(chunk) that calls on_line(line) for each complete line. Chunks
-- are kept in a list until a newline arrives, so a large message costs one
-- concatenation rather than one per chunk. Calls on_overflow() and discards
-- the data if a line exceeds M.MAX_LINE.
function M.lines(on_line, on_overflow)
  local parts, size = {}, 0
  return function(chunk)
    local start = 1
    while true do
      local nl = chunk:find("\n", start, true)
      if not nl then break end
      parts[#parts + 1] = chunk:sub(start, nl - 1)
      local line = table.concat(parts)
      parts, size = {}, 0
      if line:find("%S") then on_line(line) end
      start = nl + 1
    end
    if start <= #chunk then
      parts[#parts + 1] = chunk:sub(start)
      size = size + #chunk - start + 1
      if size > M.MAX_LINE then
        parts, size = {}, 0
        if on_overflow then on_overflow() end
      end
    end
  end
end

-- Returned by a handler that will answer later through ctx.reply/ctx.fail.
M.ASYNC = setmetatable({}, { __tostring = function() return "jsonrpc.ASYNC" end })

-- A peer sends and receives on one stream.
--   write(s)   sends raw text
--   handlers   method -> function(params, ctx) returning
--              result | nil, {code=, message=, data=} | M.ASYNC
--   opts.before(method, params, ctx) may return nil, err to reject a request
--   before its handler runs (MCP version checks).
-- ctx has id, method, reply(result) and fail(err).
function M.peer(write, handlers, opts)
  opts = opts or {}
  local peer = { pending = {}, next_id = 0 }

  local function send(msg)
    msg.jsonrpc = "2.0"
    write(json.encode(msg) .. "\n")
  end

  local function error_object(err)
    if type(err) == "table" then
      return { code = err.code or M.INTERNAL_ERROR, message = err.message or "error", data = err.data }
    end
    return { code = M.INTERNAL_ERROR, message = tostring(err) }
  end

  function peer.request(method, params, cb)
    peer.next_id = peer.next_id + 1
    local id = peer.next_id
    peer.pending[id] = cb or false
    send({ id = id, method = method, params = params })
    return id
  end

  function peer.notify(method, params)
    send({ method = method, params = params })
  end

  -- Fail every outstanding request (the stream closed).
  function peer.close(reason)
    local pending = peer.pending
    peer.pending = {}
    for _, cb in pairs(pending) do
      if cb then cb(nil, { code = M.INTERNAL_ERROR, message = reason or "connection closed" }) end
    end
  end

  local function handle_request(msg)
    local id = msg.id
    local is_request = id ~= nil and id ~= json.null
    local answered = false
    local ctx = { id = id, method = msg.method }
    function ctx.reply(result)
      if answered or not is_request then return end
      answered = true
      if result == nil then result = json.null end
      send({ id = id, result = result })
    end
    function ctx.fail(err)
      if answered or not is_request then return end
      answered = true
      send({ id = id, error = error_object(err) })
    end

    local params = msg.params
    if opts.before then
      local ok, err = opts.before(msg.method, params, ctx)
      if not ok then return ctx.fail(err) end
    end
    local h = handlers[msg.method]
    if not h then
      return ctx.fail({ code = M.METHOD_NOT_FOUND, message = "method not found: " .. tostring(msg.method) })
    end
    local ok, result, err = pcall(h, params, ctx)
    if not ok then return ctx.fail({ code = M.INTERNAL_ERROR, message = tostring(result) }) end
    if result == M.ASYNC then return end
    if err then return ctx.fail(err) end
    ctx.reply(result)
  end

  local function handle_response(msg)
    local cb = peer.pending[msg.id]
    if cb == nil then return end
    peer.pending[msg.id] = nil
    if not cb then return end
    if msg.error ~= nil and msg.error ~= json.null then
      cb(nil, msg.error)
    else
      cb(msg.result)
    end
  end

  function peer.receive(line)
    local msg = json.decode(line)
    if type(msg) ~= "table" then
      return send({ id = json.null, error = { code = M.PARSE_ERROR, message = "parse error" } })
    end
    if type(msg.method) == "string" then
      handle_request(msg)
    elseif msg.id ~= nil and (msg.result ~= nil or msg.error ~= nil) then
      handle_response(msg)
    else
      send({ id = json.null, error = { code = M.INVALID_REQUEST, message = "invalid request" } })
    end
  end

  return peer
end

return M
