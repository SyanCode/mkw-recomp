// The peer-to-peer virtual LAN (runtime/sdk/ov/p2p.c): three instances in one process meet over the loopback, no STUN.
// Invite codes (round trip, damage), the join handshake, unicast and broadcast between guests, a stranger with another
// room's code, a forged packet, and leaving. The platform layer is replaced by pthreads here.
#include <arpa/inet.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "p2p.h"
#include "platform/platform.h"

#include "p2p_stubs.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct {
    volatile int n;
    uint32_t src;
    uint16_t sport, dport;
    char data[64];
} Inbox;
static void on_data(uint32_t src, uint16_t sport, uint16_t dport, const uint8_t *d, size_t len, void *user) {
    Inbox *in = user;
    in->src = src, in->sport = sport, in->dport = dport;
    snprintf(in->data, sizeof in->data, "%.*s", (int)len, (const char *)d);
    in->n++;
}
static int wait_for(int (*f)(void *), void *arg, int ms) {
    for (int i = 0; i < ms / 20; i++) {
        if (f(arg)) return 1;
        usleep(20000);
    }
    return f(arg);
}
typedef struct { P2p *p; char code[P2P_CODE_MAX]; } Code1;
#define NEWCODE(x) {.p = (x)}
static int has_code(void *a) { Code1 *c = a; return p2p_code(c->p, c->code); }
static int has_vip(void *a) { return p2p_vip(a) != 0; }
typedef struct { P2p *p; int want; } Want;
static int all_connected(void *a) {
    Want *w = a;
    P2pPlayer pl[P2P_MAX_PLAYERS];
    int n = p2p_players(w->p, pl, P2P_MAX_PLAYERS), up = 0;
    for (int i = 0; i < n; i++) up += pl[i].connected;
    return n == w->want && up == n;
}
static int got(void *a) { return ((Inbox *)a)->n > 0; }
static int got_still(void *a) { return !strcmp(((Inbox *)a)->data, "still"); }

int main(void) {
    Inbox ia = {0}, ib = {0}, ic = {0}, id = {0};
    P2p *a = p2p_new(on_data, &ia, NULL, 0, "127.0.0.1"), *b = p2p_new(on_data, &ib, NULL, 0, "127.0.0.1"), *c = p2p_new(on_data, &ic, NULL, 0, "127.0.0.1"),
        *d = p2p_new(on_data, &id, NULL, 0, "127.0.0.1");
    CHECK(a && b && c && d);

    // codes: the host's, a damaged one, a guest's, kinds
    CHECK(p2p_host(a) == 0);
    Code1 ca = NEWCODE(a), cb = NEWCODE(b), cc = NEWCODE(c);
    CHECK(wait_for(has_code, &ca, 2000));
    char kind = 0;
    CHECK(strlen(ca.code) == 47 && p2p_code_check(ca.code, &kind) == 0 && kind == 'h');
    char bad[P2P_CODE_MAX];
    snprintf(bad, sizeof bad, "%s", ca.code);
    bad[3] = bad[3] == 'A' ? 'B' : 'A';
    CHECK(p2p_code_check(bad, NULL) == -1);
    CHECK(p2p_code_check("", NULL) == -1 && p2p_code_check("hello world", NULL) == -1);
    char lower[P2P_CODE_MAX];
    for (int i = 0; ca.code[i]; i++) lower[i] = (char)(ca.code[i] >= 'A' && ca.code[i] <= 'Z' ? ca.code[i] + 32 : ca.code[i]);
    lower[strlen(ca.code)] = 0;
    CHECK(p2p_code_check(lower, NULL) == 0);   // lower case and the dashes are as good as the printed code
    CHECK(p2p_vip(a) == (P2P_NET | 1));

    // B and C join; each hands its own code back to the host
    CHECK(p2p_join(b, "not a code") == -1 && p2p_join(b, ca.code) == 0 && p2p_join(c, ca.code) == 0);
    CHECK(wait_for(has_vip, b, 3000) && wait_for(has_vip, c, 3000));
    CHECK(wait_for(has_code, &cb, 2000) && p2p_code_check(cb.code, &kind) == 0 && kind == 'g');
    CHECK(p2p_join(b, cb.code) == -1 && p2p_add(a, ca.code) == -1);   // a guest's code is not a room, a room's is not a guest
    CHECK(p2p_add(a, cb.code) == 0 && p2p_add(a, cb.code) == 0);       // twice: the same slot
    wait_for(has_code, &cc, 2000);
    CHECK(p2p_add(a, cc.code) == 0);
    uint32_t vb = p2p_vip(b), vc = p2p_vip(c);
    CHECK((vb == (P2P_NET | 2) && vc == (P2P_NET | 3)) || (vb == (P2P_NET | 3) && vc == (P2P_NET | 2)));

    // the mesh: every pair hears every other, B and C learnt each other from the host
    Want wa = {a, 3}, wb = {b, 3}, wc = {c, 3};
    CHECK(wait_for(all_connected, &wa, 6000) && wait_for(all_connected, &wb, 6000) && wait_for(all_connected, &wc, 6000));
    P2pPlayer pl[P2P_MAX_PLAYERS];
    CHECK(p2p_players(b, pl, P2P_MAX_PLAYERS) == 3 && pl[0].self && pl[0].vip == vb);

    // unicast B -> C, then broadcast from A: B, C and A itself get it, as on a real LAN
    CHECK(p2p_send(b, vc, 1111, 2222, "to-c", 4) == 4);
    CHECK(wait_for(got, &ic, 2000) && ic.src == vb && ic.sport == 1111 && ic.dport == 2222 && !strcmp(ic.data, "to-c"));
    CHECK(ia.n == 0 && ib.n == 0);   // nobody else heard it
    CHECK(p2p_send(a, P2P_BROADCAST, 7, 8, "all", 3) == 3);
    CHECK(wait_for(got, &ia, 2000) && wait_for(got, &ib, 2000) && ic.n == 2 && !strcmp(ib.data, "all") && ib.src == (P2P_NET | 1));
    CHECK(ia.src == (P2P_NET | 1));
    CHECK(p2p_send(a, 0xe0000001u, 7, 8, "mc", 2) == 2);   // a multicast group is the room too
    CHECK(p2p_send(a, P2P_NET | 77, 1, 2, "x", 1) == -1);   // nobody there
    CHECK(p2p_send(a, vb, 1, 2, "x", P2P_MAX_DATA + 1) == -1);

    // a stranger with another room's code, and a forged packet, change nothing
    P2p *e = p2p_new(on_data, &id, NULL, 0, "127.0.0.1");
    CHECK(p2p_host(e) == 0);
    Code1 ce = NEWCODE(e);
    CHECK(wait_for(has_code, &ce, 2000));
    CHECK(p2p_join(d, ce.code) == 0);   // d joins room e, not a's
    CHECK(wait_for(has_vip, d, 3000));
    int before = ib.n + ic.n;
    p2p_send(d, vb, 1, 2, "intruder", 8);   // d's room has no player at that address
    usleep(300000);
    CHECK(ib.n + ic.n == before);
    // a DATA packet claiming to be player 2, with no valid tag, aimed at A's own port
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(p2p_port(a))};
    to.sin_addr.s_addr = htonl(0x7f000001);
    uint8_t raw[8 + 4 + 3 + 16] = {'M', 'K', 'P', '2', 5, 2, 0, 7, 0, 1, 0, 2, 'b', 'a', 'd'};
    int ia_before = ia.n;
    CHECK(sendto(fd, raw, sizeof raw, 0, (struct sockaddr *)&to, sizeof to) == (ssize_t)sizeof raw);
    usleep(300000);
    CHECK(ia.n == ia_before);
    close(fd);

    // C leaves: the others see it go quiet and the room keeps working
    p2p_leave(c);
    CHECK(p2p_vip(c) == 0 && p2p_send(c, vb, 1, 2, "x", 1) == -1);
    usleep(200000);
    CHECK(p2p_send(a, vb, 3, 4, "still", 5) == 5);
    CHECK(wait_for(got_still, &ib, 2000));

    p2p_free(a), p2p_free(b), p2p_free(c), p2p_free(d), p2p_free(e);
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
