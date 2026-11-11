"""Run loki in a pty; /, n, N and Ctrl-F search must work in the terminal.

Usage: search.py <loki binary>
"""
import fcntl, os, pty, select, struct, sys, tempfile, termios, time

TEXT = "alpha\nbeta\ngamma beta\n"


class Pty:
    def __init__(self, loki, home):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ["HOME"] = home
            os.chdir(home)
            os.execv(loki, [loki, "words.txt"])
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        self.out = b""

    def expect(self, token, what, timeout=3.0):
        """Read until token appears in output written since the last expect."""
        end = time.time() + timeout
        while time.time() < end:
            if token in self.out:
                self.out = self.out[self.out.index(token) + len(token):]
                return
            if select.select([self.fd], [], [], 0.1)[0]:
                try:
                    data = os.read(self.fd, 65536)
                except OSError:
                    break
                self.out += data
                if b"\x1b[6n" in data:  # cursor query before the size is set
                    os.write(self.fd, b"\x1b[24;80R")
        self.close()
        sys.exit("%s: %r not drawn" % (what, token))

    def send(self, data):
        os.write(self.fd, data)

    def close(self):
        os.kill(self.pid, 9)
        os.waitpid(self.pid, 0)


def main(loki):
    loki = os.path.abspath(loki)  # the child chdirs before exec
    home = tempfile.mkdtemp()
    with open(os.path.join(home, "words.txt"), "w") as f:
        f.write(TEXT)
    p = Pty(loki, home)
    p.expect(b"words.txt", "startup")

    p.send(b"/")
    p.expect(b"Search:  (Use ESC/Arrows/Enter)", "slash opens the search prompt")
    p.send(b"beta")
    p.expect(b"Search: beta", "query typed into the prompt")
    p.send(b"\r")
    p.expect(b"2/3", "enter keeps the cursor on line 2")

    # A frame draws the status bar (line/total) before the message line.
    p.send(b"n")
    p.expect(b"3/3", "n moves to line 3")
    p.expect(b"/beta", "n reports the query")
    p.send(b"n")
    p.expect(b"2/3", "n wraps to line 2")
    p.expect(b"/beta (wrapped)", "n reports the wrap")
    p.send(b"N")
    p.expect(b"3/3", "N wraps back to line 3")
    p.expect(b"?beta (wrapped)", "N reports the wrap")

    p.send(b"\x06")  # Ctrl-F
    p.expect(b"Search:  (Use ESC/Arrows/Enter)", "Ctrl-F opens the search prompt")
    p.send(b"alp\x1b")
    p.expect(b"3/3", "ESC restores the cursor")
    p.close()


if __name__ == "__main__":
    main(sys.argv[1])
