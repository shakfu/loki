"""Run loki in a pty; Ctrl-\\ opens the terminal pane, a shell command's
output is drawn, Ctrl-\\ [ scrolls it, and Ctrl-\\ returns focus so Ctrl-Q quits the editor.

Usage: term_pane.py <loki binary>
"""
import fcntl, os, pty, select, struct, sys, tempfile, termios, time


def pump(fd, out, until, secs=5):
    end = time.time() + secs
    while time.time() < end and until not in out:
        if select.select([fd], [], [], 0.1)[0]:
            try:
                data = os.read(fd, 65536)
            except OSError:
                break
            out += data
            if b"\x1b[6n" in data:  # cursor query before the size is set
                os.write(fd, b"\x1b[24;80R")
    return out


def main(loki):
    home = tempfile.mkdtemp()
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["HOME"] = home
        os.environ["SHELL"] = "/bin/sh"
        os.chdir(home)
        os.execv(loki, [loki, "scratch.txt"])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    out = pump(fd, b"", b"NORMAL")
    os.write(fd, b"\x1c")
    out = pump(fd, out, b"Ctrl-\\ to leave")
    # Arithmetic so the typed echo cannot match: only the shell prints 42.
    os.write(fd, b"echo TERM_$((6*7))\r")
    out = pump(fd, out, b"TERM_42")
    ok = b"TERM_42" in out
    # Scroll mode through the real key path: Ctrl-\ [ then k, q.
    os.write(fd, b"seq 1 60\r")
    out = pump(fd, out, b"\r\n60")
    os.write(fd, b"\x1c[")
    out = pump(fd, out, b"SCROLL")
    os.write(fd, b"k")
    out = pump(fd, out, b"[scroll -1]")
    scrolled = b"[scroll -1]" in out
    os.write(fd, b"q")
    out = pump(fd, out, b"to leave", 1)
    os.write(fd, b"\x1c")
    out = pump(fd, out, b"Ctrl-\\ to focus")
    os.write(fd, b"\x11")
    status = None
    end = time.time() + 5
    while time.time() < end and status is None:
        pump(fd, b"", b"\x00", 0.1)
        got, st = os.waitpid(pid, os.WNOHANG)
        if got == pid:
            status = st
    if status is None:
        os.kill(pid, 9)
        os.waitpid(pid, 0)
    if not ok:
        sys.exit("shell output was not drawn in the pane")
    if not scrolled:
        sys.exit("Ctrl-\\ [ then k did not scroll the pane")
    if status is None:
        sys.exit("Ctrl-Q did not quit after leaving the pane")


if __name__ == "__main__":
    main(sys.argv[1])
