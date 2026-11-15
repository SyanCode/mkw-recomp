"""The peer-to-peer virtual LAN (runtime/sdk/ov/p2p.c) and the game's side of it (ov_p2p.c), over the loopback.

Why: online play between PCs has no server of ours to test against. tests/c/t_p2p.c runs a host and guests of a room in
one process (invite codes, join, the mesh, unicast and broadcast, a stranger, a forged packet, leaving);
tests/c/t_ov_p2p.c checks what ov_socket.c leans on (the relay sockets that stand for the room's players).
tests/c/t_p2p_srv.c runs the client in server mode against the real server (tools/server/mkw_server.py).
Usage: uv run pytest tests/test_p2p.py
"""

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
pytestmark = pytest.mark.skipif(os.name == "nt", reason="POSIX sockets and pthreads harness")

OV = ROOT / "runtime/sdk/ov"
SHA = ROOT / "runtime/core/base/sha256.c"


def build(tmp_path: Path, name: str, *sources: Path) -> Path:
    # gcc first: sha256.c asks for the SHA extension test clang 18 does not know
    cc = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
    if not cc:
        pytest.skip("no C compiler")
    exe = tmp_path / name
    inc = [f"-I{ROOT / 'runtime'}", f"-I{ROOT / 'runtime/core/base'}", f"-I{OV}", f"-I{ROOT / 'tests/c'}"]
    subprocess.run([cc, "-O1", "-Wall", *inc, *map(str, sources), str(SHA), "-lpthread", "-o", str(exe)], check=True)  # fmt: skip
    return exe


def run(exe: Path) -> None:
    out = subprocess.run([str(exe)], capture_output=True, text=True, check=False, timeout=120)
    assert out.returncode == 0 and " 0 failed" in out.stdout, out.stdout


def test_room_protocol(tmp_path: Path) -> None:
    run(build(tmp_path, "t_p2p", ROOT / "tests/c/t_p2p.c", OV / "p2p.c"))


def test_game_side_relay(tmp_path: Path) -> None:
    run(build(tmp_path, "t_ov_p2p", ROOT / "tests/c/t_ov_p2p.c", OV / "ov_p2p.c", OV / "p2p.c"))


def test_server_mode(tmp_path: Path) -> None:
    """The client against the real tools/server/mkw_server.py: logging in, a room by its code, the mesh, the relay."""
    exe = build(tmp_path, "t_p2p_srv", ROOT / "tests/c/t_p2p_srv.c", OV / "p2p.c")
    server = subprocess.Popen(
        [sys.executable, str(ROOT / "tools/server/mkw_server.py"), "--port", "0", "--hello-burst", "50",
         "--state", str(tmp_path / "state.json")],
        stdout=subprocess.PIPE, text=True)  # fmt: skip
    try:
        lines = [server.stdout.readline() for _ in range(3)]
        port = re.search(r"udp \S+:(\d+)", lines[0]).group(1)
        key, password = (ln.split(": ")[1].strip() for ln in lines[1:])
        out = subprocess.run([str(exe), port, password, key], capture_output=True, text=True, check=False, timeout=120)
        assert out.returncode == 0 and " 0 failed" in out.stdout, out.stdout
    finally:
        server.terminate()
        server.wait()
