// The peer-to-peer client in server mode (runtime/sdk/ov/p2p.c) against the real tools/server/mkw_server.py, started by
// test_p2p.py: logging in, a room made and joined by its short code, a mesh of three over the loopback, and a second
// room whose players cannot reach each other directly (the test drops what peers send straight) so every packet
// goes through the server's relay. Usage: t_p2p_srv PORT PASSWORD ADMIN_KEY
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>

#include "p2p.h"
#include "p2p_stubs.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct { volatile int n; uint32_t src; uint16_t sport, dport; char data[32]; } Inbox;
static void on_data(uint32_t src, uint16_t sport, uint16_t dport, const uint8_t *d, size_t len, void *user) {
    Inbox *in = user;
    in->src = src, in->sport = sport, in->dport = dport;
    snprintf(in->data, sizeof in->data, "%.*s", (int)len, (const char *)d);
    in->n++;
}
static int wait_until(int (*f)(P2p *), P2p *p, int ms) {
    for (int i = 0; i < ms / 20 && !f(p); i++) usleep(20000);
    return f(p);
}
static int logged_in(P2p *p) { P2pServer s; p2p_server_status(p, &s); return s.state == 2; }
static int in_room(P2p *p) { P2pServer s; p2p_server_status(p, &s); return s.room[0] && p2p_vip(p); }
static int mesh3(P2p *p) {
    P2pPlayer pl[P2P_MAX_PLAYERS];
    int n = p2p_players(p, pl, P2P_MAX_PLAYERS), up = 0;
    for (int i = 0; i < n; i++) up += pl[i].connected;
    return n == 3 && up == 3;
}
static int wait_n(Inbox *in, int n) {
    for (int i = 0; i < 150 && in->n < n; i++) usleep(20000);
    return in->n >= n;
}

static P2p *client(Inbox *in, int port, const char *handle, const char *pw) {
    P2p *p = p2p_new(on_data, in, NULL, 0, "127.0.0.1");
    char spec[40], secret[40];
    snprintf(spec, sizeof spec, "127.0.0.1:%d", port);
    snprintf(secret, sizeof secret, "secret-%s-xyz", handle + 1);
    CHECK(p && p2p_server_connect(p, spec, handle, secret, pw) == 0 && wait_until(logged_in, p, 4000));
    return p;
}

static void room_test(int port, const char *pw, const char *admin, int blocked, const char *tag) {
    Inbox ia = {0}, ib = {0}, ic = {0};
    char h[3][24], line[64], code[16] = "";
    for (int i = 0; i < 3; i++) snprintf(h[i], sizeof h[i], "@%s%c", tag, 'a' + i);
    P2p *a = client(&ia, port, h[0], pw), *b = client(&ib, port, h[1], pw), *c = client(&ic, port, h[2], pw);
    if (blocked) p2p_debug_block_direct(a, 1), p2p_debug_block_direct(b, 1), p2p_debug_block_direct(c, 1);

    snprintf(line, sizeof line, "ADMIN %s", admin);
    CHECK(p2p_server_command(a, line) == 0);
    usleep(150000);
    CHECK(p2p_server_command(a, "CREATE fun public 4") == 0 && wait_until(in_room, a, 4000));
    P2pServer st;
    p2p_server_status(a, &st);
    snprintf(code, sizeof code, "%s", st.room);
    CHECK(strlen(code) == 6 && !strcmp(st.role, "admin") && p2p_vip(a) == (P2P_NET | 1));
    CHECK(p2p_server_join(b, "ZZZZZZ") == 0);   // no such room: an ERR line, no room
    usleep(150000);
    char ev[192];
    int err = 0;
    while (p2p_server_poll(b, ev)) err |= !strncmp(ev, "ERR no such room", 16);
    CHECK(err && !p2p_vip(b));
    CHECK(p2p_server_join(b, code) == 0 && p2p_server_join(c, code) == 0);
    CHECK(wait_until(in_room, b, 4000) && wait_until(in_room, c, 4000));
    CHECK(p2p_vip(b) == (P2P_NET | 2) && p2p_vip(c) == (P2P_NET | 3));
    CHECK(wait_until(mesh3, a, 8000) && wait_until(mesh3, b, 8000) && wait_until(mesh3, c, 8000));
    P2pPlayer pl[P2P_MAX_PLAYERS];
    int n = p2p_players(a, pl, P2P_MAX_PLAYERS), named = 0;
    for (int i = 0; i < n; i++) named += pl[i].handle && !strcmp(pl[i].handle, h[(pl[i].vip & 0xff) - 1]);
    CHECK(named == 3);

    // the room's required mods: named by its creator, seen by the joiners
    char mods[704];
    CHECK(p2p_server_command(a, "MODS 11:22:0123456789ab:Timer 0:0:ba9876543210:Local") == 0);
    for (int i = 0; i < 100; i++) { p2p_room_mods(c, mods); if (mods[0]) break; usleep(50000); }
    CHECK(p2p_room_mods(c, mods) == 1 && !strcmp(mods, "11:22:0123456789ab:Timer 0:0:ba9876543210:Local"));
    CHECK(p2p_server_command(b, "MODS 1:2:0123456789ab:X") == 0);   // not the creator: ERR, nothing changes
    usleep(150000);
    p2p_room_mods(b, mods);
    CHECK(strstr(mods, "Timer") != NULL);
    CHECK(p2p_send(b, P2P_NET | 3, 11, 22, "b->c", 4) == 4 && wait_n(&ic, 1) && ic.src == (P2P_NET | 2) && !strcmp(ic.data, "b->c"));
    CHECK(ia.n == 0);
    CHECK(p2p_send(c, P2P_BROADCAST, 33, 44, "all", 3) == 3 && wait_n(&ia, 1) && wait_n(&ib, 1) && ia.src == (P2P_NET | 3));
    CHECK(ic.n == 2);   // the sender hears its own broadcast, as on a LAN
    P2pPlayer q[P2P_MAX_PLAYERS];
    p2p_players(a, q, P2P_MAX_PLAYERS);
    CHECK(q[1].rtt_ms >= 0);

    // a late joiner by autojoin: the login, then the room, with its mods, with no command from the player
    Inbox id = {0};
    P2p *d = client(&id, port, h[0][0] == '@' ? "@autojoiner" : "@x", pw);
    p2p_server_autojoin(d, code);
    for (int i = 0; i < 100 && !in_room(d); i++) usleep(50000);
    CHECK(in_room(d) && p2p_room_mods(d, mods) == 1);
    p2p_free(d);

    CHECK(p2p_server_command(c, "LEAVE") == 0);   // leaving the room through the server: the others drop it
    p2p_leave(c);
    for (int i = 0; i < 100 && p2p_players(a, q, P2P_MAX_PLAYERS) != 2; i++) usleep(50000);
    CHECK(p2p_players(a, q, P2P_MAX_PLAYERS) == 2);
    p2p_free(a), p2p_free(b), p2p_free(c);
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    int port = atoi(argv[1]);
    room_test(port, argv[2], argv[3], 0, "direct");
    room_test(port, argv[2], argv[3], 1, "relay");
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
