"""The peer-to-peer virtual LAN (runtime/sdk/ov/p2p.c) and the game's side of it (ov_p2p.c), over the loopback.

Why: online play between PCs has no server of ours to test against. tests/c/t_p2p.c runs a host and guests of a room in
one process (invite codes, join, the mesh, unicast and broadcast, a stranger, a forged packet, leaving);
tests/c/t_ov_p2p.c checks what ov_socket.c leans on (the relay sockets that stand for the room's players).
Usage: uv run pytest tests/test_p2p.py
"""

import os
import shutil
import subprocess
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
