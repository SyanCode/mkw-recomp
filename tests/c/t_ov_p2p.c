// The game side of the peer-to-peer room (runtime/sdk/ov/ov_p2p.c): what ov_socket.c leans on. This process is the
// host with the game's socket bound on the loopback; a second p2p.c instance plays another PC. A datagram from it
// reaches the game's socket through a relay socket that ov_p2p_from names back as the player; what the game sends to
// the player's address, to the broadcast and to a multicast group reaches it. The menu's status follows the room.
#include <arpa/inet.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ov_p2p.h"
#include "p2p.h"
#include "p2p_stubs.h"

int cfg_bool(const char *n) { return !strcmp(n, "MKW_P2P"); }
const char *cfg_value(const char *n) { return !strcmp(n, "MKW_P2P_LOCAL_IP") ? "127.0.0.1" : ""; }   // no STUN: the loopback
int cfg_int(const char *n) { (void)n; return 0; }
void cfg_set(const char *n, const char *v) { (void)n, (void)v; }   // the server rows are not under test here

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct { volatile int n; uint32_t src; uint16_t sport, dport; char data[32]; } Inbox;
static void on_data(uint32_t src, uint16_t sport, uint16_t dport, const uint8_t *d, size_t len, void *user) {
    Inbox *in = user;
    in->src = src, in->sport = sport, in->dport = dport;
    snprintf(in->data, sizeof in->data, "%.*s", (int)len, (const char *)d);
    in->n++;
}
static int wait_n(Inbox *in, int n) {
    for (int i = 0; i < 150 && in->n < n; i++) usleep(20000);
    return in->n >= n;
}
static int wait_vip(P2p *p) {
    for (int i = 0; i < 150 && !p2p_vip(p); i++) usleep(20000);
    return p2p_vip(p) != 0;
}

int main(void) {
    OvP2pStatus st;
    ov_p2p_status(&st);
    CHECK(st.mode == P2P_ST_NONE && st.players == 0 && ov_p2p_vip() == 0 && !ov_p2p_code(NULL));
    CHECK(ov_p2p_send(P2P_NET | 2, 1, 2, "x", 1) == -1);   // no room, nowhere to go

    CHECK(ov_p2p_host() == 0 && ov_p2p_vip() == (P2P_NET | 1));
    char code[64];
    for (int i = 0; i < 150 && !ov_p2p_code(code); i++) usleep(20000);
    CHECK(ov_p2p_code(code) && p2p_code_check(code, NULL) == 0);
    ov_p2p_status(&st);
    CHECK(st.mode == P2P_ST_HOSTING && st.players == 1 && st.slot == 1 && st.code_ready);

    // the game's socket: bound to the loopback, as ov_socket.c binds it
    int g = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in me = {.sin_family = AF_INET}, from;
    me.sin_addr.s_addr = htonl(0x7f000001);
    socklen_t l = sizeof me;
    CHECK(!bind(g, (struct sockaddr *)&me, sizeof me) && !getsockname(g, (struct sockaddr *)&me, &l));
    uint16_t gport = ntohs(me.sin_port);

    Inbox pc = {0};
    P2p *other = p2p_new(on_data, &pc, NULL, 0, "127.0.0.1");
    CHECK(other && p2p_join(other, code) == 0 && wait_vip(other));
    uint32_t vo = p2p_vip(other);
    CHECK(vo == (P2P_NET | 2));
    ov_p2p_status(&st);
    CHECK(st.players == 2 && st.mode == P2P_ST_HOSTING);

    // the other PC to the game: arrives from a relay socket that stands for (player 2, port 4242)
    for (int i = 0; i < 100 && !ov_p2p_vip(); i++) usleep(1000);
    CHECK(p2p_send(other, P2P_NET | 1, 4242, gport, "hello", 5) == 5);
    struct pollfd pf = {.fd = g, .events = POLLIN};
    char buf[64];
    CHECK(poll(&pf, 1, 3000) == 1);
    l = sizeof from;
    ssize_t n = recvfrom(g, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &l);
    CHECK(n == 5 && !memcmp(buf, "hello", 5) && ntohl(from.sin_addr.s_addr) == 0x7f000001);
    uint32_t vip = 0;
    uint16_t sport = 0;
    CHECK(ov_p2p_from(ntohs(from.sin_port), &vip, &sport) && vip == vo && sport == 4242);
    CHECK(!ov_p2p_from(gport, &vip, &sport));   // the game's own socket is not a relay
    struct sockaddr_in first = from;
    CHECK(p2p_send(other, P2P_NET | 1, 4242, gport, "again", 5) == 5 && poll(&pf, 1, 3000) == 1);
    l = sizeof from;
    recvfrom(g, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &l);
    CHECK(from.sin_port == first.sin_port);   // the same sender and port keep one relay socket: a stable "address"

    // the game to the other PC: unicast, broadcast and a multicast group; the others never hear their own
    CHECK(ov_p2p_send(vo, gport, 7777, "to-you", 6) == 6 && wait_n(&pc, 1) && pc.src == (P2P_NET | 1) && pc.sport == gport && pc.dport == 7777 && !strcmp(pc.data, "to-you"));
    CHECK(ov_p2p_send(0xffffffffu, gport, 7778, "all", 3) == 3 && wait_n(&pc, 2) && pc.dport == 7778);
    CHECK(ov_p2p_send(0xe0000fc3u, gport, 7779, "grp", 3) == 3 && wait_n(&pc, 3) && pc.dport == 7779);
    // ... and a broadcast also comes back to the sender, as on a LAN: through a relay socket for (player 1, gport)
    CHECK(ov_p2p_send(0xffffffffu, gport, gport, "self", 4) == 4 && poll(&pf, 1, 3000) == 1);
    l = sizeof from;
    n = recvfrom(g, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &l);
    CHECK(n > 0 && ov_p2p_from(ntohs(from.sin_port), &vip, &sport) && vip == (P2P_NET | 1) && sport == gport);

    CHECK(ov_p2p_is_virtual(P2P_NET | 9) && ov_p2p_is_virtual(0xffffffffu) && ov_p2p_is_virtual(0xe0000001u) &&
          !ov_p2p_is_virtual(0xc0a80101u) && !ov_p2p_is_virtual(0x7f000001u));
    CHECK(ov_p2p_join("junk") == -1 && ov_p2p_add("junk") == -1);
    ov_p2p_leave();
    ov_p2p_status(&st);
    CHECK(st.mode == P2P_ST_NONE && ov_p2p_vip() == 0);
    p2p_free(other);
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
