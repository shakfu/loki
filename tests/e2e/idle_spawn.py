"""Run loki in a pty; a child's output must reach the buffer with no keypress.

Usage: idle_spawn.py <loki binary>
"""
import fcntl, os, pty, select, struct, sys, tempfile, termios, time

INIT = """
loki.spawn{cmd = 'sh', args = {'-c', 'sleep 0.3; printf IDLE_OK'},
  on_stdout = function(_, s) loki.insert_text(s) end}
"""


def main(loki):
    home = tempfile.mkdtemp()
    os.mkdir(os.path.join(home, ".loki"))
    with open(os.path.join(home, ".loki", "init.lua"), "w") as f:
        f.write(INIT)
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["HOME"] = home
        os.chdir(home)
        os.execv(loki, [loki, "scratch.txt"])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    out, end = b"", time.time() + 5
    while time.time() < end and b"IDLE_OK" not in out:
        if select.select([fd], [], [], 0.1)[0]:
            try:
                data = os.read(fd, 65536)
            except OSError:
                break
            out += data
            if b"\x1b[6n" in data:  # cursor query before the size is set
                os.write(fd, b"\x1b[24;80R")
    os.kill(pid, 9)
    os.waitpid(pid, 0)
    if b"IDLE_OK" not in out:
        sys.exit("child output was not drawn without a keypress")


if __name__ == "__main__":
    main(sys.argv[1])
