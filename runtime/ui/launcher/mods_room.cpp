// The mods of a multiplayer room: see mods_room.h. tools/mod/room_sync.py does the comparing in a child process, as
// mods.cpp runs import.py; its tab-separated lines are read here by a thread of ours.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/launcher/launcher_setup.h"
#include "ui/launcher/mods.h"
#include "ui/launcher/mods_room.h"

extern "C" {
#include "cfg.h"
#include "platform/platform.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

namespace {

SDL_Mutex *g_lock;
RoomPlan g_plan;      // what the last finished (or running) plan job gives
char g_encoded[704];  // the last encode job's entries
int g_encoded_new;
int g_job;            // 1 plan, 2 encode; one job at a time
char g_wanted[704];   // the entries a plan was asked for (also while it runs)

struct Job {
    int kind;
    SDL_Process *proc;
    RoomPlan plan;
    char encoded[704];
};

void line_plan(RoomPlan &p, char *line) {
    char *f[5] = {};
    int n = 0;
    for (char *c = line; n < 5;) {
        f[n++] = c;
        if (!(c = strchr(c, '\t'))) break;
        *c++ = 0;
    }
    if (!strcmp(f[0], "need") && n >= 3 && p.n < ROOM_MODS_MAX) {
        RoomMod &m = p.need[p.n++];
        snprintf(m.title, sizeof m.title, "%s", f[1]);
        snprintf(m.state, sizeof m.state, "%s", f[2]);
    } else if (!strcmp(f[0], "download") && n >= 4) {   // the ids go to the "need" row of that title
        for (int i = 0; i < p.n; i++)
            if (!strcmp(p.need[i].title, f[3])) p.need[i].mod = atoi(f[1]), p.need[i].file = atoi(f[2]);
    } else if (!strcmp(f[0], "disable")) {
        p.ndisable++;
    } else if (!strcmp(f[0], "set") && n >= 2) {
        snprintf(p.set, sizeof p.set, "%s", f[1]);
    } else if (!strcmp(f[0], "ready") && n >= 2) {
        p.ready = atoi(f[1]);
    }
}

// the child's stdout, line by line; the result is published when it ends
int SDLCALL reader(void *arg) {
    Job *j = (Job *)arg;
    SDL_IOStream *out = SDL_GetProcessOutput(j->proc);
    char line[1024];
    size_t n = 0;
    char c;
    for (;;) {
        if (SDL_ReadIO(out, &c, 1) != 1) {
            if (SDL_GetIOStatus(out) != SDL_IO_STATUS_NOT_READY) break;
            SDL_Delay(20);
            continue;
        }
        if (c == '\r') continue;
        if (c != '\n') {
            if (n + 1 < sizeof line) line[n++] = c;
            continue;
        }
        line[n] = 0, n = 0;
        if (j->kind == 1) line_plan(j->plan, line);
        else snprintf(j->encoded, sizeof j->encoded, "%s", line);
    }
    int code = -1;
    SDL_WaitProcess(j->proc, true, &code);
    SDL_DestroyProcess(j->proc);
    SDL_LockMutex(g_lock);
    if (j->kind == 1) {
        g_plan = j->plan;
        g_plan.state = code == 0 ? 2 : -1;
        snprintf(g_plan.required, sizeof g_plan.required, "%s", g_wanted);
    } else {
        snprintf(g_encoded, sizeof g_encoded, "%s", j->encoded);
        g_encoded_new = code == 0;
    }
    g_job = 0;
    SDL_UnlockMutex(g_lock);
    rt_log("mods: room sync (%s) ended with code %d", j->kind == 1 ? "plan" : "encode", code);
    delete j;
    return 0;
}

// room_sync.py <cmd> with args; false when a job runs or the tools are missing
bool run(const char *exe_dir, int kind, const char *const *rest) {
    if (!g_lock) g_lock = SDL_CreateMutex();
    char python[1200], script[1200];
    if (!setup_tool(exe_dir, "mod/room_sync.py", python, script, sizeof python)) return false;
    const char *args[24] = {python, "-I", script};
    int na = 3;
    for (int i = 0; rest[i] && na < 22; i++) args[na++] = rest[i];
    args[na] = nullptr;
    Job *j = new Job();
    j->kind = kind;
    j->proc = SDL_CreateProcess(args, true);
    if (!j->proc) {
        delete j;
        return false;
    }
    SDL_DetachThread(SDL_CreateThread(reader, "mods-room", j));
    return true;
}

void enabled_csv(char *out, size_t cap) {
    const char *m = cfg_get("MKW_MODS");
    snprintf(out, cap, "%s", m ? m : "");
}

}  // namespace

void mods_room_plan_start(const char *exe_dir, const char *required) {
    if (!g_lock) g_lock = SDL_CreateMutex();
    SDL_LockMutex(g_lock);
    bool same = !strcmp(g_wanted, required) && g_plan.state != 0;
    bool busy = g_job != 0;
    SDL_UnlockMutex(g_lock);
    if (same || busy) return;
    if (!*required) {   // a room with no mods: nothing to compare
        SDL_LockMutex(g_lock);
        g_plan = RoomPlan();
        g_plan.state = 2, g_plan.ready = 1;
        snprintf(g_wanted, sizeof g_wanted, "%s", required);
        SDL_UnlockMutex(g_lock);
        return;
    }
    char enabled[4096], game[PLAT_PATH_MAX];
    enabled_csv(enabled, sizeof enabled);
    snprintf(game, sizeof game, "%s", cfg_game_dir());
    const char *rest[] = {"plan", "--game", game, "--enabled", enabled, "--required", required, nullptr};
    SDL_LockMutex(g_lock);
    snprintf(g_wanted, sizeof g_wanted, "%s", required);
    g_plan.state = 1;
    g_job = 1;
    SDL_UnlockMutex(g_lock);
    if (!run(exe_dir, 1, rest)) {
        SDL_LockMutex(g_lock);
        g_plan.state = -1, g_job = 0;
        SDL_UnlockMutex(g_lock);
    }
}

void mods_room_plan_forget(void) {
    if (!g_lock) return;
    SDL_LockMutex(g_lock);
    if (!g_job) g_wanted[0] = 0, g_plan.state = 0;
    SDL_UnlockMutex(g_lock);
}

RoomPlan mods_room_plan(void) {
    if (!g_lock) g_lock = SDL_CreateMutex();
    SDL_LockMutex(g_lock);
    RoomPlan p = g_plan;
    SDL_UnlockMutex(g_lock);
    return p;
}

void mods_room_encode_start(const char *exe_dir, const char *names_csv) {
    if (!g_lock) g_lock = SDL_CreateMutex();
    SDL_LockMutex(g_lock);
    bool busy = g_job != 0;
    if (!busy) g_job = 2;
    SDL_UnlockMutex(g_lock);
    if (busy) return;
    char game[PLAT_PATH_MAX];
    snprintf(game, sizeof game, "%s", cfg_game_dir());
    const char *rest[] = {"encode", "--game", game, "--names", names_csv, nullptr};
    if (!run(exe_dir, 2, rest)) {
        SDL_LockMutex(g_lock);
        g_job = 0;
        SDL_UnlockMutex(g_lock);
    }
}

int mods_room_encoded(char *out, int cap) {
    if (!g_lock) return 0;
    SDL_LockMutex(g_lock);
    int ready = g_encoded_new;
    if (ready) snprintf(out, (size_t)cap, "%s", g_encoded), g_encoded_new = 0;
    SDL_UnlockMutex(g_lock);
    return ready;
}

int mods_room_active(void) {
    const char *b = cfg_get("MKW_MODS_BACKUP");
    return b && *b;
}

// "-" stands for the player having no mod on: an empty setting would read as no backup
void mods_room_apply(void) {
    RoomPlan p = mods_room_plan();
    if (p.state != 2 || !p.ready) return;
    if (!mods_room_active()) {
        const char *own = cfg_get("MKW_MODS");
        cfg_set("MKW_MODS_BACKUP", own && *own ? own : "-");
    }
    mods_set_list(p.set);
    cfg_save();
    rt_log("mods: the room's mods are on for the match: %s", p.set[0] ? p.set : "(none)");
}

void mods_room_restore(void) {
    const char *b = cfg_get("MKW_MODS_BACKUP");
    if (!b || !*b) return;
    char own[4096];
    snprintf(own, sizeof own, "%s", !strcmp(b, "-") ? "" : b);
    cfg_set("MKW_MODS_BACKUP", "");
    mods_set_list(own);
    cfg_save();
    rt_log("mods: the player's own mods are back: %s", own[0] ? own : "(none)");
}
