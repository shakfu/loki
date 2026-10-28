# Agent Integration (design sketch)

**Status:** implemented 2026-10-28; the sketch below is kept as the design record. "As built" lists where the code differs.
**Companion:** [`architecture.md`](architecture.md), [`lua_async_http.md`](lua_async_http.md).

This sketch replaces `.loki/modules/ai.lua` (one hardcoded OpenAI call) with three layers:

1. `loki.spawn` and `loki.listen`: non-blocking child processes and Unix sockets, exposed to Lua.
2. An ACP client. loki drives an external agent (myra, Claude Code, Gemini CLI, ...).
3. An MCP server. An external agent reads and edits loki's buffers.

Layers 2 and 3 are Lua on top of layer 1. Layer 0 must come first.

## As built

- **Layer 0:** the idle hook does not call `uv_run` from inside `terminal_read_key`. `terminal_read_key_idle` is opt-in, and only `modal_process_keypress` uses it. The search prompt keeps pointers into row highlight data that a callback could free. After the idle read, the key goes to the buffer now shown, since a callback may have switched buffers.
- **`loki.json`:** `src/json.c` had to change first. `\uXXXX` decoded to `?`, and floats and large integers were truncated. It now decodes UTF-8 escapes, adds `JSON_DOUBLE`, and escapes object keys.
- **`loki.spawn`:**
  - The process methods are `pid()` and `running()`, not a `pid` field.
  - A failed spawn has pid 0 until its handles close. A kill before then would be `kill(0)` and hit the editor's process group, so every kill path checks the pid.
- **Undo:** `editor_replace_range` records a block delete plus a new `UNDO_INSERT_BLOCK` as one group.
- **MCP:**
  - The server serves both protocol eras: `2026-07-28` per-request `_meta` versions with `server/discover`, and the legacy `initialize` handshake.
  - `edit_buffer` edits only open buffers; `open_file` opens first.
  - `read_buffer` is not limited to the project root.
  - `loki.abspath` resolves missing files through their parent directory and rejects a final `..`.
- **ACP:**
  - `fs/write_text_file` saves to disk; the spec requires creating missing files, and the agent's own tools read the disk. On an open buffer, the write is one undo step and the buffer is saved. MCP `edit_buffer` never saves.
  - There is one `*agent*` scratch buffer, kept across restarts.
  - Permission prompts use `loki.choose`, which swallows other keys until answered.
- **myra:**
  - `myra_io` hooks carry text, tool, read, write and allow.
  - `myra_poll` drains stdin from curl's progress callback and the shell loop, so cancel works without threads.
  - `read` falls back to the disk when the client refuses a path.

## 0. Prerequisite: the main loop never wakes up

`loki_editor_main` (`src/editor.c`) loops through dispatch, refresh, and `editor_process_keypress`. `terminal_read_key` (`src/terminal.c:153`) keeps reading until it gets a byte. Each 100 ms `VTIME` timeout just repeats the read. Results:

- `async_queue_dispatch_all` runs only after a keypress.
- Nothing in `src/` calls `loki_http_poll`. Only `tests/test_http_security.c:563` does. No `LokiLangOps` registers `check_callbacks`. So `loki.async_http` callbacks never fire in the editor, and `ai.complete()` cannot insert a reply. This comes from reading the code; I have not reproduced it.
- `uv_run` is never called. `uv_default_loop()` exists but does not run.

**Minimal fix.** Add an idle hook in the `nread == 0` branch of `terminal_read_key`:

```c
/* terminal.c */
static void (*idle_hook)(void);
void terminal_set_idle_hook(void (*fn)(void)) { idle_hook = fn; }
...
if (nread == 0) {
    if (input_closed(fd)) return -1;
    if (idle_hook) idle_hook();
    continue;
}
```

```c
/* editor.c */
static void editor_idle(void) {
    editor_ctx_t *ctx = buffer_get_current();
    uv_run(uv_default_loop(), UV_RUN_NOWAIT);   /* spawn/listen I/O -> Lua callbacks */
#ifdef LOKI_ENABLE_HTTP
    loki_http_poll(ctx_L(ctx));
#endif
    if (async_queue_dispatch_all(NULL, ctx) > 0 || lua_dirty) editor_refresh_screen(ctx);
}
```

- Latency is up to 100 ms. That is fine for streamed agent text.
- The hook runs between keys, never during an escape sequence, because `read_seq_byte` is separate. Editor state is consistent at that point.
- `lua_dirty` is a flag set by any Lua API that mutates a buffer or the status line.
- Alternative: make `uv_run(UV_RUN_ONCE)` the loop, with a `uv_poll_t` on stdin. Latency drops to 0, but every key path has to be restructured. Defer this until 100 ms is a measured problem.

## 1. `loki.spawn` and `loki.listen`

### Lua API

```lua
local p = loki.spawn{
  cmd    = "myra",                 -- resolved via PATH
  args   = {"--acp"},
  cwd    = loki.project_root(),    -- default "."
  env    = {LOKI_MCP_SOCK = sock}, -- merged over environ
  on_stdout = function(p, chunk) end,  -- raw bytes, any boundaries
  on_stderr = function(p, chunk) end,
  on_exit   = function(p, code, signal) end,
}
p:write(s)       -- queued; returns false once stdin is closed
p:close_stdin()
p:kill("TERM")   -- default TERM
p.pid

local srv = loki.listen(path, function(conn)   -- Unix socket, mode 0600
  conn.on_data  = function(conn, chunk) end
  conn.on_close = function(conn) end
  conn:write(s); conn:close()
end)
srv:close()

-- Lua helper, not C: wraps an on_data callback and calls fn per '\n'-terminated line.
local lines = require("lines")
```

`spawn` and `listen` return the same stream object (`write`, `close`, data and close callbacks). One C type backs both: a `uv_stream_t` wrapper.

### C sketch (`src/spawn.c`, about 300 lines)

```c
typedef struct {
    uv_process_t proc;
    uv_pipe_t in, out, err;
    lua_State *L;
    int self_ref, on_stdout, on_stderr, on_exit;  /* luaL_ref into registry */
    int open_handles;                             /* free when 0 */
} loki_proc;

static void on_read(uv_stream_t *s, ssize_t n, const uv_buf_t *buf) {
    loki_proc *p = s->data;
    if (n > 0) call_ref(p->L, s == (uv_stream_t *)&p->out ? p->on_stdout : p->on_stderr,
                        p->self_ref, buf->base, n);   /* lua_pcall; error -> status line */
    else if (n < 0) uv_close((uv_handle_t *)s, on_handle_closed);
    free(buf->base);
}

static int lua_loki_spawn(lua_State *L) {
    /* parse table; build argv/env; */
    uv_process_options_t o = {
        .file = cmd, .args = argv, .env = envp, .cwd = cwd,
        .exit_cb = on_exit,
        .flags = UV_PROCESS_DETACHED,     /* setsid: child has no controlling tty */
        .stdio_count = 3, .stdio = (uv_stdio_container_t[]){
            {UV_CREATE_PIPE | UV_READABLE_PIPE, {.stream = (uv_stream_t *)&p->in}},
            {UV_CREATE_PIPE | UV_WRITABLE_PIPE, {.stream = (uv_stream_t *)&p->out}},
            {UV_CREATE_PIPE | UV_WRITABLE_PIPE, {.stream = (uv_stream_t *)&p->err}},
        },
    };
    int rc = uv_spawn(uv_default_loop(), &p->proc, &o);
    ...
}
```

Decisions:

- **Callbacks are registry refs, not global names.** `async_http` takes a global function name, so it cannot carry closures. libuv callbacks run inside `uv_run` on the main thread, so they can call Lua directly. `async_queue` is not needed.
- **`UV_PROCESS_DETACHED` (new session).** The child cannot open `/dev/tty`. So `myra --permissions ask` refuses instead of fighting loki's raw-mode terminal; see myra README, "A call that would ask is refused when there is no terminal". A detached child is not killed with loki, so `editor_atexit` sends SIGTERM to every live process.
- **Limits:** at most 8 children, and at most 16 MB queued per stream on `write`. Past the cap, `write` returns false.
- **Trust:** this adds no new capability. Project Lua already runs only after `:trust`, and it can already call `io.popen` and `os.execute`.
- **JSON:** Lua needs a codec. Expose `src/json.c` as `loki.json.encode` and `loki.json.decode` rather than vendoring a Lua library. Check first that `json_parse` handles everything ACP needs: nested objects and arrays, `\uXXXX` escapes, and 64-bit ids.

## 2. ACP client (`.loki/modules/acp.lua`)

Spec: [agentclientprotocol.com](https://agentclientprotocol.com/protocol/overview). JSON-RPC 2.0 over the agent's stdio. Each message is one line; I assume newline framing, as in MCP stdio, but the overview page does not state it. All paths are absolute, and lines are 1-based.

### Session flow

```
loki -> agent  initialize        {protocolVersion: 1,
                                  clientCapabilities: {fs: {readTextFile: true, writeTextFile: true}}}
loki -> agent  session/new       {cwd: <abs root>, mcpServers: [<loki MCP shim, see 3>]}
               <- {sessionId}
loki -> agent  session/prompt    {sessionId, prompt: [{type:"text", text:...},
                                                      {type:"resource", ...selection...}]}
agent -> loki  session/update    agent_message_chunk | tool_call | tool_call_update | plan
agent -> loki  fs/read_text_file    {sessionId, path, line?, limit?}  <- {content}
agent -> loki  fs/write_text_file   {sessionId, path, content}         <- null
agent -> loki  session/request_permission {sessionId, toolCall, options[]}
               <- {outcome: {outcome: "selected", optionId}} | {outcome: {outcome: "cancelled"}}
               <- {stopReason}   (response to session/prompt)
loki -> agent  session/cancel    (notification; on ESC or :agent cancel)
```

Shapes for `request_permission`, `tool_call` and `session/new` come from the ACP [tool-calls](https://agentclientprotocol.com/protocol/tool-calls) and [session-setup](https://agentclientprotocol.com/protocol/session-setup) pages. The overview did not show the `initialize` request shape. `clientCapabilities` above is my assumption; check it against the spec.

### Lua skeleton

```lua
local M, pending, next_id = {}, {}, 0

local function send(p, msg) p:write(loki.json.encode(msg) .. "\n") end

local function request(p, method, params, cb)
  next_id = next_id + 1; pending[next_id] = cb
  send(p, {jsonrpc = "2.0", id = next_id, method = method, params = params})
end

local handlers = {}   -- agent -> loki requests and notifications

function handlers["session/update"](s, u)
  local k = u.update.sessionUpdate
  if k == "agent_message_chunk" then s.out:append(u.update.content.text)
  elseif k == "tool_call" or k == "tool_call_update" then loki.status(u.update.title or u.update.status)
  end
end

function handlers["fs/read_text_file"](s, a)  return {content = buffer_or_disk_read(s, a.path, a.line, a.limit)} end
function handlers["fs/write_text_file"](s, a) buffer_or_disk_write(s, a.path, a.content); return loki.json.null end

function handlers["session/request_permission"](s, a, reply)   -- async: reply later
  loki.choose(a.toolCall.title or a.toolCall.toolCallId, a.options, function(opt)
    reply(opt and {outcome = {outcome = "selected", optionId = opt.optionId}}
              or  {outcome = {outcome = "cancelled"}})
  end)
end
```

### Editor integration

| ACP event | loki behaviour |
|-|-|
| `agent_message_chunk` | Append to an `*agent*` buffer. |
| `tool_call` / `tool_call_update` | Status line. Diff content can go into the `*agent*` buffer. |
| `fs/read_text_file` | Return the open buffer's text, including unsaved edits. Otherwise read from disk. |
| `fs/write_text_file` | If the file is open: replace the buffer as one undo group, mark it dirty, do not save. Otherwise write to disk. |
| `request_permission` | Choice prompt in the status line, using `loki.choose`. |
| `ESC` in `*agent*` | `session/cancel`. |

Commands: `:agent start [cmd]`, `:agent ask <text>`, `:agent send-selection`, `:agent cancel`, `:agent stop`. Agent command line comes from `loki.config.agent = {"myra", "--acp"}`.

### New Lua APIs needed

The current API has `get_line`, `get_lines`, `insert_text` and `status`, all on the current buffer.

- `loki.buffers()` returns `{id, path, dirty}`. `loki.buffer(id)` gives `get_lines`, `set_text` (one undo group) and `path`.
- `loki.open(path, line)` opens a path in a new or existing tab.
- `loki.new_scratch(name)` creates an unnamed, never-saved buffer, for `*agent*`.
- `loki.choose(prompt, options, cb)` asks the user a question without blocking.
- `loki.get_selection()`.

### Trust boundary

The agent is semi-trusted. It runs model output.

- `fs/*` paths: `realpath`, then require a prefix match on the project root. Reject anything else, including symlink escapes. ACP does not require this; loki should.
- `fs/write_text_file` on an open buffer never saves to disk. The user reviews it and runs `:w`.
- The agent still has its own `shell` tool. ACP cannot confine that. myra's `--permissions` and the agent's own sandbox are the only limits.

### myra side: `--acp` mode

- A stdin JSON-RPC loop replaces the REPL. Streamed text goes out as `agent_message_chunk` instead of stdout. stdout must carry only protocol messages, so all `myra_note` output goes to stderr.
- `myra_run_tool` gets an I/O vtable: `read` and `write` go through `fs/*` when the client advertises them; otherwise they touch the disk. `edit` becomes read, replace, write over the same vtable. Diffs are reported in `tool_call_update`.
- `--permissions ask` becomes `session/request_permission` instead of `/dev/tty`.
- `myra_ask` is synchronous. While a tool waits for a client response, read stdin and queue unrelated messages. `session/cancel` sets a flag that the tool loop and the curl progress callback check. No threads are needed.
- `scripts/agent.py` needs the same mode, or `e2e-py` diverges. See `docs/dev/divergences.md`.

## 3. MCP server in loki

Spec: [MCP transports](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports).

### Transport

The stdio transport assumes the client launches the server. loki owns its own stdio, which is the tty, so loki cannot be that server. There are two options:

| Option | Cost | Notes |
|-|-|-|
| **Unix socket plus stdio shim** (recommended) | `loki.listen`, plus a `loki --mcp-shim SOCK` mode of about 40 lines that copies stdin to the socket and the socket to stdout | Owner-only `0600` socket in `$XDG_RUNTIME_DIR/loki/<pid>.sock`. No HTTP server, no Origin or DNS-rebinding exposure. Every MCP client supports stdio. |
| Streamable HTTP on 127.0.0.1 | An HTTP/1.1 and SSE server in C | The spec requires `Origin` validation and recommends auth ("Servers MUST validate the Origin header"). Any local process can reach it. More code and more attack surface. |

Discovery:

- ACP path: `session/new.mcpServers = [{name: "loki", command: "<abs path to loki>", args: ["--mcp-shim", sock], env: []}]`. The agent gets editor tools with no user configuration.
- Agent in another pane: loki exports `LOKI_MCP_SOCK` to its children. Otherwise the shim picks the newest socket whose owning loki has the same cwd. Or the user passes the path to `claude mcp add loki -- loki --mcp-shim <sock>`.

### Methods

`initialize` (echo the client's `protocolVersion` if supported; capabilities `{tools: {}}`), `notifications/initialized`, `ping`, `tools/list`, `tools/call`. Leave out resources and prompts (YAGNI).

### Tools

| Tool | Input | Result |
|-|-|-|
| `buffers` | none | `[{path, dirty, lang, current}]` |
| `read_buffer` | `path`, `start_line?`, `end_line?` | text, including unsaved edits |
| `selection` | none | `{path, start, end, text}` or empty |
| `cursor` | none | `{path, line, col}` |
| `edit_buffer` | `path`, `old_string`, `new_string` | Replaces one unique match as one undo group and marks the buffer dirty. Same semantics as myra's `edit`. |
| `open` | `path`, `line?` | Opens a tab. |

No `eval` or `run_lua` tool. It would run agent-chosen code inside the editor and bypass `:trust`. `edit_buffer` is limited to the project root, like ACP `fs/*`.

### Lua skeleton (`.loki/modules/mcp.lua`)

```lua
local tools = {
  buffers     = {schema = {type = "object"}, fn = function() return loki.buffers() end},
  read_buffer = {schema = {...}, fn = function(a) return table.concat(loki.buffer_by_path(a.path):get_lines(a.start_line, a.end_line), "\n") end},
  ...
}

local methods = {
  initialize   = function(p) return {protocolVersion = p.protocolVersion,
                   capabilities = {tools = {}}, serverInfo = {name = "loki", version = loki.version}} end,
  ping         = function() return {} end,
  ["tools/list"] = function() ... end,
  ["tools/call"] = function(p)
    local ok, r = pcall(tools[p.name].fn, p.arguments)
    return {content = {{type = "text", text = ok and loki.json.encode(r) or tostring(r)}}, isError = not ok}
  end,
}

loki.listen(sock, function(conn) conn.on_data = lines(function(l) dispatch(conn, methods, l) end) end)
```

ACP and MCP share the `dispatch(conn, methods, line)` JSON-RPC core: id matching, error objects `-32601` and `-32602`, notifications with no reply. Write it once as `jsonrpc.lua`.

## Order of work

1. Layer 0 idle hook, plus a test that an async callback fires with no keypress. This also fixes `async_http`.
2. `loki.spawn` and `loki.json`, with tests: echo child, exit code, kill, callback error goes to the status line.
3. Buffer Lua APIs (`buffers`, `buffer(id)`, `choose`, `new_scratch`).
4. `jsonrpc.lua`, then the MCP server and `--mcp-shim`. This is the smallest end-to-end result: an agent in another pane sees unsaved buffers.
5. ACP client in loki, tested against a mock agent script.
6. myra `--acp`.
7. Remove `ai.lua`. Decide whether `LOKI_ENABLE_HTTP` stays.

## Open questions

- Edits to a file that is not open: write to disk, or open it as a dirty buffer? Opening keeps every agent change reviewable, but many edits produce many tabs.
- A single `*agent*` buffer, or one per session?
- Is newline framing in ACP stdio confirmed? Check before writing the reader.
