#!/usr/bin/env python3
"""Rooms and relay server for mkw-recomp's online play: a Linux daemon that does NOT run the game.

What it does, so players find each other with a short code or an address instead of a 40-letter invite:
  - Handles: every player picks an @handle and a secret (the game makes both); the first HELLO of a handle claims it.
  - Rooms: made by the admin, by players the admin promoted (GRANT), or by anyone with --open-hosting. A room has a short
    code (K7QM3X), is public (listed) or private (invite by @handle, or the code to an invited player), up to 24 players.
  - Rendezvous: it tells each member the others' public and local addresses (the one it sees the HELLO come from is
    the NAT mapping, so it doubles as a STUN server), and the room's secret, so the players punch holes to each other.
  - Relay: when two players cannot reach each other directly (symmetric NAT) their packets go through the server,
    wrapped in MKRL frames. The room's packets stay HMAC-tagged by the room's secret, which the server knows (it made it).
Not done: encryption; a server is trusted with the room's secret.

Usage: uv run tools/server/mkw_server.py [--port 27500] [--mode friends|public] [--password PW] [--open-hosting]
       [--state state.json] [--max-rooms N] [--relay-pps N]
 friends (default): a server password is required to say HELLO (generated and printed when not given).
 public: no password; rooms and handles are limited and public rooms are listed to everybody.
The admin key (printed at start, kept in the state file) makes a session admin: `ADMIN <key>` from the game's menu.
Wire format (UDP, one socket): "MKSV " + a text line, "MKRL" + a slot + a room packet. Lines, client to server:
  HELLO @h secret [password] | <token> CREATE name public|private max [persist] | JOIN code lip:lport | LEAVE | LIST |
  INVITE @h | GRANT @h | REVOKE @h | KICK @h | CLOSE code | ADMIN key | PING | WHOAMI
Replies and events: OK token ip:port role | ROOM code | JOINED code slot keyhex name max | PEERS code n slot:@h:ip:port:lip:lport...
  | ROOMS n code:name:players:max:@host... | INVITED @from code name | KICKED code | ERR text | PONG | YOU @h role.
"""

import argparse
import asyncio
import hashlib
import hmac
import json
import os
import re
import secrets
import sys
import time
from pathlib import Path

MAGIC = b"MKSV "
RELAY = b"MKRL"
ALPHA = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
HANDLE = re.compile(r"^@[a-z0-9_]{2,20}$")
MAX_PLAYERS = 24
SESSION_TTL = 30.0


def now() -> float:
    return time.monotonic()


class Bucket:
    """A token bucket: rate per second, burst."""

    def __init__(self, rate: float, burst: float) -> None:
        self.rate, self.burst, self.tokens, self.t = rate, burst, burst, now()

    def take(self) -> bool:
        t = now()
        self.tokens = min(self.burst, self.tokens + (t - self.t) * self.rate)
        self.t = t
        if self.tokens >= 1:
            self.tokens -= 1
            return True
        return False


class Session:
    def __init__(self, addr, handle: str, role: str, relay_pps: float) -> None:
        self.addr, self.handle, self.role = addr, handle, role
        self.token = secrets.token_hex(8)
        self.room: Room | None = None
        self.slot = 0
        self.lip, self.lport = "0.0.0.0", 0
        self.last = now()
        self.ctl = Bucket(30, 60)
        self.relay = Bucket(relay_pps, relay_pps)


class Room:
    def __init__(self, code: str, name: str, public: bool, maxp: int, creator: str, persist: bool, key: bytes) -> None:
        self.code, self.name, self.public, self.max, self.creator, self.persist, self.key = (
            code,
            name,
            public,
            maxp,
            creator,
            persist,
            key,
        )
        self.members: dict[int, Session] = {}
        self.invited: set[str] = {creator}


class State:
    """What survives a restart: handles (salted secret hashes, roles), the admin key, the password, permanent rooms."""

    def __init__(self, path: Path | None) -> None:
        self.path = path
        self.data = {"handles": {}, "admin_key": "", "password": "", "rooms": {}}
        if path and path.exists():
            self.data.update(json.loads(path.read_text()))

    def save(self) -> None:
        if not self.path:
            return
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.data, indent=1))
        os.replace(tmp, self.path)


class Server(asyncio.DatagramProtocol):
    def __init__(self, args, state: State) -> None:
        self.args, self.st = args, state
        self.by_addr: dict = {}
        self.by_token: dict[str, Session] = {}
        self.by_handle: dict[str, Session] = {}
        self.rooms: dict[str, Room] = {}
        self.hello_buckets: dict[str, Bucket] = {}
        self.tp = None
        for code, r in state.data["rooms"].items():
            self.rooms[code] = Room(code, r["name"], r["public"], r["max"], r["creator"], True, bytes.fromhex(r["key"]))

    # ---- plumbing
    def connection_made(self, transport) -> None:
        self.tp = transport

    def say(self, addr, text: str) -> None:
        self.tp.sendto(MAGIC + text.encode(), addr)

    def datagram_received(self, data: bytes, addr) -> None:
        try:
            if data.startswith(RELAY) and len(data) > 6:
                self.relay(data, addr)
            elif data.startswith(MAGIC) and len(data) < 600:
                self.control(data[len(MAGIC) :].decode("utf-8", "replace").strip(), addr)
        except Exception as e:  # a bad datagram never stops the server
            print(f"error from {addr}: {e!r}", file=sys.stderr)

    # ---- control
    def control(self, line: str, addr) -> None:
        words = line.split()
        if not words:
            return
        if words[0] == "HELLO":
            return self.hello(words[1:], addr)
        s = self.by_token.get(words[0])
        if not s:
            return self.say(addr, "ERR no session: say HELLO")
        if s.addr != addr:  # the NAT gave the client another port: the token is the proof
            self.by_addr.pop(s.addr, None)
            s.addr = addr
            self.by_addr[addr] = s
        s.last = now()
        if not s.ctl.take():
            return self.say(addr, "ERR slow down")
        cmd, args = (words[1] if len(words) > 1 else "").upper(), words[2:]
        fn = getattr(self, "cmd_" + cmd.lower(), None)
        if not fn:
            return self.say(addr, "ERR unknown command")
        fn(s, args)

    def hello(self, a: list[str], addr) -> None:
        ip = addr[0]
        b = self.hello_buckets.setdefault(ip, Bucket(1, self.args.hello_burst))
        if not b.take():
            return self.say(addr, "ERR slow down")
        if len(a) < 2 or not HANDLE.match(a[0]) or len(a[1]) < 8 or len(a[1]) > 128:
            return self.say(addr, "ERR HELLO @handle secret(8+) [password]")
        if self.st.data["password"] and not (len(a) > 2 and hmac.compare_digest(a[2], self.st.data["password"])):
            return self.say(addr, "ERR wrong server password")
        h, secret = a[0], a[1]
        rec = self.st.data["handles"].get(h)
        salt = rec["salt"] if rec else secrets.token_hex(8)
        digest = hashlib.sha256((salt + secret).encode()).hexdigest()
        if rec and not hmac.compare_digest(rec["hash"], digest):
            return self.say(addr, "ERR that @handle belongs to someone else")
        if not rec:
            if len(self.st.data["handles"]) >= self.args.max_handles:
                return self.say(addr, "ERR the server has no room for new handles")
            rec = self.st.data["handles"][h] = {"salt": salt, "hash": digest, "role": "user"}
            self.st.save()
            print(f"new handle {h}")
        old = self.by_handle.get(h)
        if old:
            self.drop(old, "replaced by a new login")
        elif len(self.by_token) >= self.args.max_sessions:
            return self.say(addr, "ERR server full")
        s = Session(addr, h, rec["role"], self.args.relay_pps)
        self.by_addr[addr], self.by_token[s.token], self.by_handle[h] = s, s, s
        self.say(addr, f"OK {s.token} {addr[0]}:{addr[1]} {s.role}")

    def can_host(self, s: Session) -> bool:
        return s.role in ("admin", "host") or self.args.open_hosting

    def cmd_ping(self, s, a) -> None:
        self.say(s.addr, "PONG")

    def cmd_whoami(self, s, a) -> None:
        self.say(s.addr, f"YOU {s.handle} {s.role}")

    def cmd_admin(self, s, a) -> None:
        if not a or not hmac.compare_digest(a[0], self.st.data["admin_key"]):
            return self.say(s.addr, "ERR wrong admin key")
        self.set_role(s.handle, "admin")  # says "YOU @h admin" to the player

    def set_role(self, h: str, role: str) -> bool:
        rec = self.st.data["handles"].get(h)
        if not rec:
            return False
        rec["role"] = role
        self.st.save()
        if h in self.by_handle:
            self.by_handle[h].role = role
            self.say(self.by_handle[h].addr, f"YOU {h} {role}")
        return True

    def cmd_grant(self, s, a) -> None:
        if s.role != "admin" or not a or not self.set_role(a[0], "host"):
            return self.say(s.addr, "ERR admin only, and the @handle must have logged in once")
        self.say(s.addr, "OK")

    def cmd_revoke(self, s, a) -> None:
        rec = self.st.data["handles"].get(a[0]) if a else None
        if s.role != "admin" or not rec or rec["role"] == "admin" or not self.set_role(a[0], "user"):
            return self.say(s.addr, "ERR admin only")
        self.say(s.addr, "OK")

    def cmd_create(self, s, a) -> None:
        if not self.can_host(s):
            return self.say(s.addr, "ERR you may not host here: ask the admin for GRANT")
        if len(a) < 3 or not a[2].isdigit():
            return self.say(s.addr, "ERR CREATE name public|private max [persist]")
        persist = len(a) > 3 and a[3] == "persist"
        if persist and s.role != "admin":
            return self.say(s.addr, "ERR only the admin makes permanent rooms")
        if len(self.rooms) >= self.args.max_rooms:
            return self.say(s.addr, "ERR the server has too many rooms")
        if s.role != "admin" and sum(r.creator == s.handle for r in self.rooms.values()) >= self.args.rooms_per_player:
            return self.say(s.addr, "ERR you already have rooms open")
        code = "".join(secrets.choice(ALPHA) for _ in range(6))
        while code in self.rooms:
            code = "".join(secrets.choice(ALPHA) for _ in range(6))
        r = Room(
            code,
            re.sub(r"[^A-Za-z0-9_-]", "", a[0])[:24] or "room",
            a[1] == "public",
            max(2, min(MAX_PLAYERS, int(a[2]))),
            s.handle,
            persist,
            secrets.token_bytes(10),
        )
        self.rooms[code] = r
        if persist:
            self.st.data["rooms"][code] = {
                "name": r.name,
                "public": r.public,
                "max": r.max,
                "creator": r.creator,
                "key": r.key.hex(),
            }
            self.st.save()
        print(f"room {code} {r.name} by {s.handle}")
        self.say(s.addr, f"ROOM {code}")

    def cmd_join(self, s, a) -> None:
        r = self.rooms.get(a[0].upper()) if a else None
        if not r:
            return self.say(s.addr, "ERR no such room")
        if s.room is r:
            return self.announce(r)
        if not r.public and s.handle not in r.invited and s.role != "admin":
            return self.say(s.addr, "ERR private room: ask a member to INVITE you")
        slot = next((i for i in range(1, r.max + 1) if i not in r.members), 0)
        if not slot:
            return self.say(s.addr, "ERR room full")
        if s.room:
            self.leave(s)
        if len(a) > 1 and ":" in a[1]:
            s.lip, _, lp = a[1].rpartition(":")
            s.lport = int(lp) if lp.isdigit() else 0
        s.room, s.slot = r, slot
        r.members[slot] = s
        self.say(s.addr, f"JOINED {r.code} {slot} {r.key.hex()} {r.name} {r.max}")
        self.announce(r)

    def cmd_leave(self, s, a) -> None:
        self.leave(s)
        self.say(s.addr, "OK")

    def cmd_invite(self, s, a) -> None:
        t = self.by_handle.get(a[0]) if a else None
        if not s.room or not t:
            return self.say(s.addr, "ERR you must be in a room, and the @handle online")
        s.room.invited.add(t.handle)
        self.say(t.addr, f"INVITED {s.handle} {s.room.code} {s.room.name}")
        self.say(s.addr, "OK")

    def cmd_list(self, s, a) -> None:
        rows = [
            f"{r.code}:{r.name}:{len(r.members)}:{r.max}:{r.creator}"
            for r in self.rooms.values()
            if r.public or s.role == "admin" or s.handle in r.invited
        ]
        self.say(s.addr, f"ROOMS {len(rows)} " + " ".join(rows[:12]))

    def cmd_kick(self, s, a) -> None:
        t = self.by_handle.get(a[0]) if a else None
        r = t.room if t else None
        if not r or not (s.role == "admin" or r.creator == s.handle):
            return self.say(s.addr, "ERR not allowed")
        self.leave(t)
        r.invited.discard(t.handle)
        self.say(t.addr, f"KICKED {r.code}")
        self.say(s.addr, "OK")

    def cmd_close(self, s, a) -> None:
        r = self.rooms.get(a[0].upper()) if a else None
        if not r or not (s.role == "admin" or r.creator == s.handle):
            return self.say(s.addr, "ERR not allowed")
        for m in list(r.members.values()):
            self.leave(m)
            self.say(m.addr, f"KICKED {r.code}")
        self.rooms.pop(r.code, None)
        if self.st.data["rooms"].pop(r.code, None):
            self.st.save()
        self.say(s.addr, "OK")

    # ---- rooms
    def announce(self, r: Room) -> None:
        peers = " ".join(
            f"{m.slot}:{m.handle}:{m.addr[0]}:{m.addr[1]}:{m.lip}:{m.lport}"
            for m in sorted(r.members.values(), key=lambda m: m.slot)
        )
        for m in r.members.values():
            self.say(m.addr, f"PEERS {r.code} {len(r.members)} {peers}")

    def leave(self, s: Session) -> None:
        r, s.room, s.slot = s.room, None, 0
        if not r:
            return
        r.members = {k: v for k, v in r.members.items() if v is not s}
        if r.members:
            self.announce(r)
        elif not r.persist:
            self.rooms.pop(r.code, None)

    def drop(self, s: Session, why: str) -> None:
        self.leave(s)
        self.by_addr.pop(s.addr, None)
        self.by_token.pop(s.token, None)
        if self.by_handle.get(s.handle) is s:
            del self.by_handle[s.handle]
        print(f"{s.handle} gone: {why}")

    def relay(self, d: bytes, addr) -> None:
        s = self.by_addr.get(addr)
        if not s or not s.room or not s.relay.take():
            return
        dst = s.room.members.get(d[4])
        if dst and dst is not s:
            self.tp.sendto(RELAY + bytes([s.slot]) + d[5:], dst.addr)

    async def sweep(self) -> None:
        n = 0
        while True:
            await asyncio.sleep(1)
            n += 1
            for s in [s for s in self.by_token.values() if now() - s.last > SESSION_TTL]:
                self.drop(s, "silent for 30 s")
            if n % 3 == 0:
                for r in self.rooms.values():
                    if r.members:
                        self.announce(r)


async def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=27500)
    ap.add_argument("--mode", choices=["friends", "public"], default="friends")
    ap.add_argument("--password", help="server password (friends mode: generated when missing)")
    ap.add_argument(
        "--open-hosting", action="store_true", help="anyone may create rooms, not only the admin and who they GRANT"
    )
    ap.add_argument("--state", type=Path, default=Path("mkw-server.json"))
    ap.add_argument("--max-sessions", type=int)
    ap.add_argument("--max-rooms", type=int)
    ap.add_argument("--hello-burst", type=int, default=5, help="logins an address may make at once, then one a second")
    ap.add_argument("--rooms-per-player", type=int, default=2)
    ap.add_argument("--max-handles", type=int)
    ap.add_argument(
        "--relay-pps", type=float, help="relayed packets per second a player may send (default 600 friends, 300 public)"
    )
    args = ap.parse_args()
    pub = args.mode == "public"
    args.max_sessions = args.max_sessions or (200 if pub else 64)
    args.max_rooms = args.max_rooms or (40 if pub else 16)
    args.max_handles = args.max_handles or (2000 if pub else 200)
    args.relay_pps = args.relay_pps or (300 if pub else 600)
    st = State(args.state)
    if not st.data["admin_key"]:
        st.data["admin_key"] = secrets.token_urlsafe(12)
    if pub:
        st.data["password"] = args.password or ""
    else:
        st.data["password"] = args.password or st.data["password"] or secrets.token_urlsafe(9)
    st.save()
    srv = Server(args, st)
    loop = asyncio.get_running_loop()
    tp, _ = await loop.create_datagram_endpoint(lambda: srv, local_addr=(args.host, args.port))
    port = tp.get_extra_info("sockname")[1]
    print(f"listening on udp {args.host}:{port} ({args.mode} mode)", flush=True)
    print(f"admin key: {st.data['admin_key']}", flush=True)
    if st.data["password"]:
        print(f"server password: {st.data['password']}", flush=True)
    await srv.sweep()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
