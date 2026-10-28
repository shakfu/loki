"""Run loki in a pty with the MCP server on; read an unsaved buffer through
`loki --mcp-shim` from a separate process.

Usage: mcp_shim.py <loki binary> <modules dir>
"""
import fcntl, json, os, pty, select, struct, subprocess, sys, tempfile, termios, time

INIT = """
package.path = %r .. '/?.lua;' .. package.path
assert(require('mcp').start(%r))
loki.insert_text('UNSAVED')
"""


def main(loki, modules):
    home = tempfile.mkdtemp()
    os.mkdir(os.path.join(home, ".loki"))
    run = os.path.join(home, "run")
    os.mkdir(run, 0o700)
    sock = os.path.join(run, "e2e.sock")
    with open(os.path.join(home, ".loki", "init.lua"), "w") as f:
        f.write(INIT % (modules, sock))
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["HOME"] = home
        os.chdir(home)
        os.execv(loki, [loki, "scratch.txt"])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    try:
        end = time.time() + 5
        while not os.path.exists(sock) and time.time() < end:
            if select.select([fd], [], [], 0.05)[0]:
                os.read(fd, 65536)
        meta = {"io.modelcontextprotocol/protocolVersion": "2026-07-28"}
        req = {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {
            "name": "read_buffer",
            "arguments": {"path": os.path.join(home, "scratch.txt")},
            "_meta": meta}}
        shim = subprocess.Popen([loki, "--mcp-shim", sock], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE)
        shim.stdin.write((json.dumps(req) + "\n").encode())
        shim.stdin.flush()
        # The editor answers from its idle hook; keep draining its screen.
        line, end = b"", time.time() + 5
        while not line.endswith(b"\n") and time.time() < end:
            r = select.select([fd, shim.stdout], [], [], 0.05)[0]
            if fd in r:
                os.read(fd, 65536)
            if shim.stdout in r:
                line += os.read(shim.stdout.fileno(), 65536)
        shim.stdin.close()
        shim.wait(timeout=5)
        reply = json.loads(line)
        text = reply["result"]["content"][0]["text"]
        if text != "UNSAVED":
            sys.exit("unexpected reply: %r" % reply)
    finally:
        os.kill(pid, 9)
        os.waitpid(pid, 0)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
