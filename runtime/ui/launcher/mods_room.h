// The mods of a multiplayer room (ui/launcher/mods_room.cpp): which ones the room's server names, what the player lacks,
// and putting the player's own mods aside for the match. The comparison is tools/mod/room_sync.py's, run on a thread.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum { ROOM_MODS_MAX = 8 };

typedef struct {
    char title[48];
    char state[12];   // "ok" (installed and on), "enable" (installed, off), "download" (on GameBanana), "unavailable"
    int mod, file;    // GameBanana's ids when state is "download"
} RoomMod;

typedef struct {
    int state;        // 0 nothing asked, 1 working, 2 plan ready, -1 failed
    int ready;        // every mod the room names is installed: the match can start with them
    int n;
    RoomMod need[ROOM_MODS_MAX];
    int ndisable;     // the player's own mods that go off for the match
    char set[1024];   // the MODS list for the match
    char required[704];   // the room's entries this plan is for
} RoomPlan;

// Compares the room's entries (the server's ROOMMODS, "" for none) with the installed mods, on a thread. Asking again
// for the same entries does nothing; mods_room_plan_forget makes the next ask run (after an install).
void mods_room_plan_start(const char *exe_dir, const char *required);
void mods_room_plan_forget(void);
RoomPlan mods_room_plan(void);   // a copy, any time

// The room's creator: the entries for these mod names (comma-separated), on a thread. mods_room_encoded copies the result
// once it is ready and returns 1 (then 0 until the next one).
void mods_room_encode_start(const char *exe_dir, const char *names_csv);
int mods_room_encoded(char *out, int cap);

// The player's own mods are put aside (MODS_BACKUP) and the plan's set is on, or put back.
int mods_room_active(void);
void mods_room_apply(void);
void mods_room_restore(void);

#ifdef __cplusplus
}
#endif
