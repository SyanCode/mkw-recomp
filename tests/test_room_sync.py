"""Which mods a multiplayer room needs (tools/mod/room_sync.py): the fingerprint ignores names and tells versions
apart, a room's entries survive the server's line, and the plan downloads, enables and disables what it must.
Usage: uv run pytest tests/test_room_sync.py
"""

import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("room_sync", ROOT / "tools/mod/room_sync.py")
rs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rs)


def make(game: Path, name: str, files: dict[str, str], source=None, title=None, switches=None) -> None:
    d = game / "mods" / name
    (d / "romfs").mkdir(parents=True)
    rows = [f"{rel}\t1\t{sha}\t-\n" for rel, sha in files.items()]
    (d / "romfs" / "manifest.tsv").write_text("".join(rows))
    meta = {"name": name, "title": title or name, "switches": switches or {}}
    if source:
        meta["source"] = {"gamebanana": {"mod": source[0], "file": source[1]}}
    (d / "mod.json").write_text(json.dumps(meta))


def test_fingerprint_and_entries(tmp_path: Path) -> None:
    make(tmp_path, "Timer", {"a": "aa", "b": "bb"}, (11, 22), "Race Timer")
    make(tmp_path, "TimerCopy", {"b": "bb", "a": "aa"}, None, "Same files, other name")
    make(tmp_path, "TimerV2", {"a": "aa", "b": "cc"}, (11, 23))
    fps = {m["name"]: m["fp"] for m in rs.installed(tmp_path)}
    assert fps["Timer"] == fps["TimerCopy"] != fps["TimerV2"]
    line = rs.encode(tmp_path, ["Timer", "Nope"])
    (got,) = rs.decode(line)
    assert got == {"mod": 11, "file": 22, "fp": fps["Timer"], "title": "Race Timer"}
    assert rs.decode("garbage 1:2:short:x 3:4:" + "a" * 12 + ":Sp%20ace")[0]["title"] == "Sp ace"


def test_plan(tmp_path: Path) -> None:
    make(tmp_path, "Timer", {"a": "aa"}, (11, 22))
    make(tmp_path, "Skin", {"s": "ss"}, (33, 44))
    make(tmp_path, "Mine", {"m": "mm"}, (55, 66))
    inst = rs.installed(tmp_path)
    fp = {m["name"]: m["fp"] for m in inst}
    host = rs.decode(f"11:22:{fp['Timer']}:Timer 99:98:{'b' * 12}:New 0:0:{'c' * 12}:Local")
    out = rs.plan(inst, ["Skin", "Mine"], host)
    assert ("enable", "Timer") in out and ("download", 99, 98, "New") in out and ("unavailable", "Local") in out
    assert ("disable", "Skin") in out and ("disable", "Mine") in out  # not the room's: off for the match
    assert ("need", "Timer", "enable") in out and ("need", "New", "download") in out
    assert ("need", "Local", "unavailable") in out
    assert out[-2:] == [("set", "Timer"), ("ready", 0)]
    # everything there: ready, with the room's order
    ok = rs.plan(inst, ["Timer", "Mine"], rs.decode(f"33:44:{fp['Skin']}:Skin 11:22:{fp['Timer']}:Timer"))
    assert ok[-2:] == [("set", "Skin,Timer"), ("ready", 1)] and ("disable", "Mine") in ok


def test_import_keeps_the_source(tmp_path: Path) -> None:
    """An install from GameBanana records where it came from (import.py --source), which is what a room's player downloads."""
    import subprocess
    import sys

    game, romfs, folder = tmp_path / "game", tmp_path / "romfs", tmp_path / "Timer"
    (game / "mods").mkdir(parents=True)
    romfs.mkdir()
    (folder / "romfs" / "Layout").mkdir(parents=True)
    (folder / "romfs" / "Layout" / "a.bin").write_bytes(b"mod file")
    run = [sys.executable, "-I", str(ROOT / "tools/mod/import.py"), "--game", str(game), "--romfs", str(romfs),
           "--build-id", "0123456789abcdef", "--folder", str(folder)]  # fmt: skip
    out = subprocess.run([*run, "--source", "gb:11:22"], capture_output=True, text=True, check=False)
    assert out.returncode == 0, out.stdout + out.stderr
    meta = json.loads((game / "mods/Timer/mod.json").read_text())
    assert meta["source"] == {"gamebanana": {"mod": 11, "file": 22}}
    (mod,) = rs.installed(game)
    assert (mod["mod"], mod["file"]) == (11, 22)
    # a hand-made import has no source: a room can still name it, but nobody can download it
    out = subprocess.run([*run, "--name", "Mine"], capture_output=True, text=True, check=False)
    assert out.returncode == 0 and "source" not in json.loads((game / "mods/Mine/mod.json").read_text())
