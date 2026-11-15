# tests/c

C sources the Python tests next door build with the compiler at hand and run.

| File | Built by | What it checks |
|---|---|---|
| `t_switch2.c` | `test_switch2.py` | The Switch 2 controllers' protocol (`runtime/input/switch2_proto.h`): from seedds/mkw-neo (MIT), with reports and replies recorded from a left and a right Joy-Con 2 |
| `t_game_dir.c` | `test_game_dir.py`, `test_game_dir_windows.py` | Settings, installation discovery and log-folder discovery with real files and single-thread platform stubs; Windows uses UTF-16 file APIs and UTF-8 conversion through `wmain`; no game, SDL or GPU |
| `t_p2p.c`, `p2p_stubs.h` | `test_p2p.py` | `runtime/sdk/ov/p2p.c`: a host and guests of one room in one process over the loopback (no STUN); the platform layer is replaced by pthreads in `p2p_stubs.h` |
| `t_ov_p2p.c` | `test_p2p.py` | `runtime/sdk/ov/ov_p2p.c`: the game's socket on the loopback receives the room's datagrams from relay sockets that `ov_p2p_from` names back as the players; sends to a player, the broadcast and a multicast group |
