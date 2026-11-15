# tools/server

A rooms and relay server for the online play of `MKW_P2P=1`. It runs on Linux (any OS with Python 3.11) and does **not**
run the game: it only introduces players to each other and relays the packets of two players whose routers block a direct
connection. Standard library only.

| File | What it is | Usage |
|---|---|---|
| `mkw_server.py` | The daemon: handles (`@name`), rooms with a short code, the invitations, the admin's rights, the relay | `uv run tools/server/mkw_server.py [--port 27500] [--mode friends\|public] [--password PW] [--open-hosting]` |

## Starting a server

```bash
uv run tools/server/mkw_server.py --port 27500
```

It prints its **admin key** and, in friends mode, the **server password**, and keeps both in `mkw-server.json`
(`--state` moves it). Open UDP port 27500 on the machine's firewall and router. Players put the machine's address in
Settings > General > Online play > Server address.

| Mode | Who gets in | Limits (defaults) |
|---|---|---|
| `friends` (default) | Whoever has the server password (generated when `--password` is not given) | 64 sessions, 16 rooms, 200 handles, 600 relayed packets a second per player |
| `public` | Anyone; `--password` is optional | 200 sessions, 40 rooms, 2000 handles, 300 relayed packets a second per player |

`--max-sessions`, `--max-rooms`, `--max-handles`, `--rooms-per-player` and `--relay-pps` change them.

## Who may do what

- **@handle**: a name (`@` then 2 to 20 of `a-z 0-9 _`). The first login claims it with a secret the game made and keeps in
  `mkw.ini`; nobody else can log in as it. Handles and roles are saved in the state file.
- **admin**: a player who sent the admin key (the menu's *Become admin*). Makes permanent rooms, allows other players to
  host (`GRANT @name`, undone with `REVOKE`), kicks players and closes any room.
- **host**: a player the admin allowed. Makes rooms, up to `--rooms-per-player` (2), and kicks from them.
- **user**: joins rooms. With `--open-hosting` everybody may make rooms: the server is open to whoever should host their
  own games.
- A **public** room is listed and joined with its code. A **private** one is joined with the code *and* an invitation
  (`INVITE @name` by a member); the admin enters any room.

## A room's required mods

The room's creator (or the admin) names the mods every player must run, up to 8: `MODS <entry> ...` from inside the room, one
entry per mod as `tools/mod/room_sync.py` writes it (`<GameBanana mod id>:<file id>:<12 hex fingerprint>:<title>`). The server
only stores and forwards them (to everyone joining, and again every few seconds), never reads one, and `LIST` shows how many a
room has. The players compare them with what they have installed; the start screen's Online tab does the rest (see the README).

## What the server sees and can do

It sees every player's public address and the room's secret (it makes it and hands it to the members), so it can read and
forge a room's packets: run it only for people who trust its owner. Nothing is encrypted, only tagged. A relayed packet is
the room's own tagged packet inside an `MKRL` frame; the server never changes it.

The wire format is in the head of `mkw_server.py`; `tests/test_server.py` drives it over UDP and `tests/c/t_p2p_srv.c`
runs the game's client against it.
