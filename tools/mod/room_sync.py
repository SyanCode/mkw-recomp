#!/usr/bin/env python3
"""Which mods a multiplayer room needs, and what the player must do to have exactly those.

Why: in a room everybody has to run the same mods or the game desynchronises. The room's creator names the mods it needs;
a player who joins compares them with the installed ones. Mods are told apart by a fingerprint of their content (the
hashes `import.py` wrote in romfs/manifest.tsv, the code patches, the switches), not by their folder name, so the same
mod under two names is the same mod and a different version of it is not. A missing mod is fetched from GameBanana (the
`source` in its mod.json, kept by `import.py --source`); mods a player has enabled that the room does not name are
switched off for the match. Nothing here changes a file: the start screen applies the plan.
Usage: uv run tools/mod/room_sync.py encode --game DIR --names a,b
       uv run tools/mod/room_sync.py plan --game DIR --enabled a,b --required "ENTRY ENTRY ..."
Output (tab-separated lines): `download MOD FILE TITLE`, `unavailable TITLE` (needed, not on GameBanana, not installed),
`enable NAME`, `disable NAME`, `need TITLE ok|enable|download|unavailable` (one per mod the room names, in its order),
`set a,b,c` (the MODS list for the match, in the room's order), `ready 0|1`.
A room's entry is `<GameBanana mod id>:<file id>:<12 hex of the fingerprint>:<title, percent-encoded>`; 0:0 means no source.
"""

import argparse
import hashlib
import json
import sys
from pathlib import Path
from urllib.parse import quote, unquote

MAX_REQUIRED = 8


def fingerprint(mod_dir: Path) -> str:
    """12 hex characters that change when the mod's files, code patches or switches do (not its name or title)."""
    h = hashlib.sha256()
    manifest = mod_dir / "romfs" / "manifest.tsv"
    if manifest.exists():
        for line in sorted(manifest.read_text(encoding="utf-8").splitlines()):
            rel, _size, sha, _orig = (line.split("\t") + ["", "", "", ""])[:4]
            h.update(f"r\t{rel}\t{sha}\n".encode())
    meta = {}
    try:
        meta = json.loads((mod_dir / "mod.json").read_text(encoding="utf-8"))
    except OSError, ValueError:
        pass
    for key in ("patches", "image", "switches"):
        h.update(f"{key}\t{json.dumps(meta.get(key), sort_keys=True)}\n".encode())
    return h.hexdigest()[:12]


def installed(game: Path) -> list[dict]:
    out = []
    mods = game / "mods"
    for d in sorted(p for p in mods.iterdir() if p.is_dir() and not p.name.startswith(".")) if mods.is_dir() else []:
        try:
            meta = json.loads((d / "mod.json").read_text(encoding="utf-8"))
        except OSError, ValueError:
            continue
        gb = (meta.get("source") or {}).get("gamebanana") or {}
        out.append({"name": d.name, "title": meta.get("title") or d.name, "mod": int(gb.get("mod", 0)),
                    "file": int(gb.get("file", 0)), "fp": fingerprint(d)})  # fmt: skip
    return out


def encode(game: Path, names: list[str]) -> str:
    by_name = {m["name"]: m for m in installed(game)}
    entries = []
    for n in names[:MAX_REQUIRED]:
        if n in by_name:
            m = by_name[n]
            entries.append(f"{m['mod']}:{m['file']}:{m['fp']}:{quote(m['title'][:24], safe='')}")
    return " ".join(entries)


def decode(line: str) -> list[dict]:
    out = []
    for e in line.split():
        parts = e.split(":", 3)
        if len(parts) == 4 and parts[0].isdigit() and parts[1].isdigit() and len(parts[2]) == 12:
            out.append({"mod": int(parts[0]), "file": int(parts[1]), "fp": parts[2], "title": unquote(parts[3])})
    return out[:MAX_REQUIRED]


def plan(inst: list[dict], enabled: list[str], required: list[dict]) -> list[tuple]:
    by_fp = {m["fp"]: m for m in inst}
    lines: list[tuple] = []
    chosen: list[str] = []
    ready = True
    for r in required:
        m = by_fp.get(r["fp"])
        if m:
            chosen.append(m["name"])
            if m["name"] not in enabled:
                lines.append(("enable", m["name"]))
            lines.append(("need", r["title"], "ok" if m["name"] in enabled else "enable"))
        elif r["mod"]:
            lines.append(("need", r["title"], "download"))
            lines.append(("download", r["mod"], r["file"], r["title"]))   # after its need line: the ids go onto it
            ready = False
        else:
            lines.append(("unavailable", r["title"]))
            lines.append(("need", r["title"], "unavailable"))
            ready = False
    for n in enabled:
        if n not in chosen:
            lines.append(("disable", n))
    lines.append(("set", ",".join(chosen)))
    lines.append(("ready", int(ready)))
    return lines


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("encode")
    e.add_argument("--game", type=Path, required=True)
    e.add_argument("--names", default="")
    p = sub.add_parser("plan")
    p.add_argument("--game", type=Path, required=True)
    p.add_argument("--enabled", default="")
    p.add_argument("--required", default="")
    a = ap.parse_args()
    if a.cmd == "encode":
        print(encode(a.game, [n for n in a.names.split(",") if n]))
        return 0
    for line in plan(installed(a.game), [n for n in a.enabled.split(",") if n], decode(a.required)):
        print("\t".join(str(x) for x in line))
    return 0


if __name__ == "__main__":
    sys.exit(main())
