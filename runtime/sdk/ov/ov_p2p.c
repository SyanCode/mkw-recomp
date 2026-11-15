// MKW_P2P: the game's LAN lobby (Pia over nn::socket) over p2p.c's virtual network, so PCs far apart race as if they
// shared one. ov_socket.c keeps the game's sockets as ordinary host UDP sockets, but bound to the loopback, and hands
// anything addressed to the room (100.77.0.x, the broadcast, a multicast group) to ov_p2p_send. What p2p.c delivers
// comes back through a relay socket per (sender, sender's port): bound to the loopback, it sends to the game's
// socket from a port of its own, and RecvFrom turns that port back into the sender's virtual address (ov_p2p_from).
// That keeps Poll, blocking reads and eventfds exactly as they are for a real LAN. The relay table is fixed
// (NPROXY): a room has at most 24 players and Pia uses a port or two each; a full table recycles its oldest.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET RelaySock;
#define RELAY_BAD INVALID_SOCKET
#define relay_close closesocket
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int RelaySock;
#define RELAY_BAD (-1)
#define relay_close close
#endif

#include "cfg.h"
#include "ov_p2p.h"
#include "p2p.h"
#include "platform/platform.h"

void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   // host.h's, without the CPU headers

#define NPROXY 256

static PlatMutex lock = PLAT_MUTEX_INIT;
static P2p *room;
static int room_failed;
static struct {
    uint32_t vip;
    uint16_t sport, lport;
    RelaySock s;
    int used;
} relay[NPROXY];
static int relay_next;

int ov_p2p(void) { return cfg_bool("MKW_P2P"); }

int ov_p2p_is_virtual(uint32_t h) { return (h >> 16) == (P2P_NET >> 16) || h == 0xffffffffu || (h >> 28) == 0xe; }

// the relay socket for datagrams from (vip, sport), made on first use; call with the lock held
static int relay_get(uint32_t vip, uint16_t sport) {
    for (int i = 0; i < NPROXY; i++)
        if (relay[i].used && relay[i].vip == vip && relay[i].sport == sport) return i;
    int i = relay_next;
    for (int k = 0; k < NPROXY; k++)   // a free slot, else the next in turn
        if (!relay[(relay_next + k) % NPROXY].used) {
            i = (relay_next + k) % NPROXY;
            break;
        }
    relay_next = (i + 1) % NPROXY;
    if (relay[i].used) relay_close(relay[i].s), relay[i].used = 0;
    RelaySock s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET}, me;
    socklen_t l = sizeof me;
    a.sin_addr.s_addr = htonl(0x7f000001);
    if (s == RELAY_BAD) return -1;
    if (bind(s, (struct sockaddr *)&a, sizeof a) || getsockname(s, (struct sockaddr *)&me, &l)) return relay_close(s), -1;
#ifdef _WIN32
    DWORD no = 0, got;
    WSAIoctl(s, SIO_UDP_CONNRESET, &no, sizeof no, NULL, 0, &got, NULL, NULL);
#endif
    relay[i].vip = vip, relay[i].sport = sport, relay[i].lport = ntohs(me.sin_port), relay[i].s = s, relay[i].used = 1;
    return i;
}

// p2p.c's thread: a datagram of the room goes to the game's socket bound to dport
static void deliver(uint32_t src, uint16_t sport, uint16_t dport, const uint8_t *d, size_t n, void *user) {
    (void)user;
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(dport)};
    to.sin_addr.s_addr = htonl(0x7f000001);
    plat_mutex_lock(&lock);
    int i = relay_get(src, sport);
    if (i >= 0) sendto(relay[i].s, (const char *)d, (int)n, 0, (struct sockaddr *)&to, sizeof to);
    plat_mutex_unlock(&lock);
}

static P2p *get_room(void) {
    plat_mutex_lock(&lock);
    if (!room && !room_failed && ov_p2p()) {
        const char *stun = cfg_value("MKW_P2P_STUN"), *lip = cfg_value("MKW_P2P_LOCAL_IP");
        room = p2p_new(deliver, NULL, *stun ? stun : NULL, (uint16_t)cfg_int("MKW_P2P_PORT"), *lip ? lip : NULL);
        if (room) rt_log("p2p: UDP port %u, STUN %s", p2p_port(room), *stun ? stun : "off");
        else room_failed = 1, rt_log("p2p: cannot open the room's UDP socket");
    }
    P2p *r = room;
    plat_mutex_unlock(&lock);
    return r;
}

uint32_t ov_p2p_vip(void) { return room ? p2p_vip(room) : 0; }

int ov_p2p_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, size_t len) {
    P2p *r = room;
    if (!r) return -1;
    if (dst == 0xffffffffu || (dst >> 28) == 0xe) dst = P2P_BROADCAST;   // the room is the only network there is
    return p2p_send(r, dst, sport, dport, data, len);
}

int ov_p2p_from(uint16_t lport, uint32_t *vip, uint16_t *sport) {
    int ok = 0;
    plat_mutex_lock(&lock);
    for (int i = 0; i < NPROXY && !ok; i++)
        if (relay[i].used && relay[i].lport == lport) *vip = relay[i].vip, *sport = relay[i].sport, ok = 1;
    plat_mutex_unlock(&lock);
    return ok;
}

// ---- the menu's side
int ov_p2p_host(void) {
    P2p *r = get_room();
    return r ? p2p_host(r) : -1;
}
int ov_p2p_join(const char *code) {
    P2p *r = get_room();
    return r ? p2p_join(r, code) : -1;
}
int ov_p2p_add(const char *code) {
    P2p *r = get_room();
    return r ? p2p_add(r, code) : -1;
}
int ov_p2p_code(char out[64]) { return room ? p2p_code(room, out) : 0; }
void ov_p2p_leave(void) {
    if (room) p2p_leave(room);
}

void ov_p2p_status(OvP2pStatus *st) {
    memset(st, 0, sizeof *st);
    st->worst_rtt_ms = -1;
    P2pPlayer pl[P2P_MAX_PLAYERS];
    int n = room ? p2p_players(room, pl, P2P_MAX_PLAYERS) : 0;
    char code[P2P_CODE_MAX];
    uint32_t vip = room ? p2p_vip(room) : 0;
    st->players = n, st->slot = (int)(vip & 0xff), st->code_ready = room && p2p_code(room, code);
    for (int i = 0; i < n; i++) {
        st->connected += pl[i].connected;
        if (!pl[i].self && pl[i].rtt_ms > st->worst_rtt_ms) st->worst_rtt_ms = pl[i].rtt_ms;
    }
    int role = room ? p2p_role(room) : 0;
    st->mode = !role ? P2P_ST_NONE : role == 1 ? P2P_ST_HOSTING : vip ? P2P_ST_JOINED : P2P_ST_JOINING;
}
