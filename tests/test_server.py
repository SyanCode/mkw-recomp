"""The rooms and relay server (tools/server/mkw_server.py), driven over UDP on the loopback.

Why: handles, the permission to host, private rooms, invitations, the peer lists and the relay are the server's whole
job and need no game. Usage: uv run pytest tests/test_server.py
"""

import re
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
MAGIC = b"MKSV "


class Client:
    def __init__(self, port: int) -> None:
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind(("127.0.0.1", 0))
        self.s.settimeout(2)
        self.srv = ("127.0.0.1", port)
        self.token = ""
        self.events: list[str] = []

    def send(self, line: str) -> None:
        self.s.sendto(MAGIC + line.encode(), self.srv)

    def recv(self, prefix: str = "", timeout: float = 2) -> str:
        """The next control line starting with prefix; others are kept in events."""
        end = time.time() + timeout
        for i, e in enumerate(self.events):
            if e.startswith(prefix):
                return self.events.pop(i)
        while time.time() < end:
            self.s.settimeout(max(0.05, end - time.time()))
            try:
                d, _ = self.s.recvfrom(2048)
            except TimeoutError:
                break
            if d.startswith(MAGIC):
                line = d[len(MAGIC) :].decode()
                if line.startswith(prefix):
                    return line
                self.events.append(line)
        raise AssertionError(f"no {prefix!r} in {self.events}")

    def cmd(self, line: str, prefix: str = "") -> str:
        self.send(f"{self.token} {line}")
        return self.recv(prefix)

    def hello(self, handle: str, password: str = "") -> str:
        self.send(f"HELLO {handle} secret-{handle[1:]}-123 {password}".strip())
        r = self.recv()
        if r.startswith("OK"):
            self.token = r.split()[1]
        return r


@pytest.fixture
def server(tmp_path: Path):
    p = subprocess.Popen(
        [sys.executable, str(ROOT / "tools/server/mkw_server.py"), "--port", "0", "--state", str(tmp_path / "s.json"), "--hello-burst", "50"],
        stdout=subprocess.PIPE, text=True)  # fmt: skip
    lines = [p.stdout.readline() for _ in range(3)]
    info = {
        "port": int(re.search(r"udp \S+:(\d+)", lines[0]).group(1)),
        "admin": lines[1].split(": ")[1].strip(),
        "password": lines[2].split(": ")[1].strip(),
    }
    yield info
    p.terminate()
    p.wait()


def test_server(server) -> None:
    port, pw = server["port"], server["password"]
    a, b, c = Client(port), Client(port), Client(port)
    assert a.hello("@alice", "wrong").startswith("ERR wrong server password")
    assert a.hello("@alice", pw).split()[0] == "OK"
    assert a.hello("@alice", pw).startswith("OK")  # the same login again replaces the session
    assert b.hello("@bob", pw).startswith("OK")
    imp = Client(port)
    imp.send(f"HELLO @alice other-secret-1 {pw}")
    assert imp.recv().startswith("ERR that @handle belongs")

    # nobody may host until the admin says so
    assert a.cmd("CREATE fun public 8").startswith("ERR you may not host")
    assert a.cmd("ADMIN nope").startswith("ERR")
    assert a.cmd(f"ADMIN {server['admin']}").endswith("admin")
    code = a.cmd("CREATE fun public 8", "ROOM").split()[1]
    assert re.fullmatch(r"[A-Z2-9]{6}", code)
    assert b.cmd("CREATE mine public 4").startswith("ERR you may not host")
    assert a.cmd("GRANT @bob", "OK") == "OK"
    assert b.recv("YOU").endswith("host")
    assert a.cmd("CREATE perm public 4 persist", "ROOM")

    # join, peers, the room's secret
    ja = a.cmd(f"JOIN {code} 192.168.1.5:4000", "JOINED").split()
    jb = b.cmd(f"JOIN {code} 10.0.0.9:5000", "JOINED").split()
    assert ja[2] == "1" and jb[2] == "2" and ja[3] == jb[3] and len(ja[3]) == 20
    peers = a.recv("PEERS").split()
    for _ in range(3):
        if peers[2] == "2":
            break
        peers = a.recv("PEERS").split()
    assert peers[2] == "2" and ":@bob:127.0.0.1:" in peers[4] and peers[4].endswith(":10.0.0.9:5000")
    assert a.cmd("JOIN ZZZZZZ").startswith("ERR no such room")

    # a private room: the code is not enough
    priv = a.cmd("CREATE secret private 4", "ROOM").split()[1]
    assert c.hello("@carol", pw).startswith("OK")
    assert c.cmd(f"JOIN {priv}").startswith("ERR private room")
    assert a.cmd(f"JOIN {priv} 1.1.1.1:1", "JOINED")
    assert a.cmd("INVITE @carol", "OK") == "OK"
    assert c.recv("INVITED").split()[1:3] == ["@alice", priv]
    assert c.cmd(f"JOIN {priv} 2.2.2.2:2", "JOINED").split()[2] == "2"
    assert "secret" in c.cmd("LIST", "ROOMS")

    # the relay: a packet to slot 2 comes out of the server as from slot 1
    a.s.sendto(b"MKRL" + bytes([2]) + b"packet-a-to-c", a.srv)
    d = b""
    for _ in range(10):
        d, _ = c.s.recvfrom(2048)
        if d.startswith(b"MKRL"):
            break
    assert d == b"MKRL" + bytes([1]) + b"packet-a-to-c"

    # kicking and closing
    assert b.cmd("KICK @carol", "ERR").startswith("ERR not allowed")
    assert a.cmd("KICK @carol", "OK") == "OK" and c.recv("KICKED")
    assert a.cmd(f"CLOSE {code}", "OK") == "OK"
    assert b.cmd(f"JOIN {code}", "ERR").startswith("ERR no such room")
    assert b.cmd("PING", "PONG") == "PONG"
