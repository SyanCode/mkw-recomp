// Peer-to-peer virtual LAN: see p2p.h for what it is for. This file is the whole protocol.
//
// Finding each other, with no server of ours:
//   The host asks a STUN server (RFC 5389 binding request, sent from the very socket the room uses, so the answer is
//   the NAT mapping the peers will hit) for its public address, and shows it with its local address and a room secret
//   as an invite code. A guest enters that code and sends HELLO to both addresses; a host behind a NAT that filters by
//   sender drops it, which is why the guest can hand its own code back and the host (p2p_add) sends first too: two
//   packets crossing is the whole of UDP hole punching. Two machines on one network meet on the local address.
//   The host then introduces every guest to every other one (TABLE, repeated every two seconds so a lost one does not
//   matter) and each pair punches by itself, so the room is a full mesh and survives the host's NAT but not a
//   symmetric NAT on either side of a pair (the mapping the host saw is not the one the other guest would hit).
// Wire format, big endian: "MKP2", type, sender's slot, payload length (2), payload, 16-byte HMAC-SHA256 tag over the
// rest keyed by the room secret. Types: 1 HELLO (guest to host: our nonce, local address), 2 WELCOME (your slot, then
// a TABLE), 3 PING / 4 PONG (a time the answer echoes: punching, keep-alive, round trip), 5 DATA (source port,
// destination port, the game's datagram), 6 TABLE (the players' addresses), 7 BYE.
// Server mode (role 3, tools/server/mkw_server.py): the same mesh, but the server hands out the slots, the room's secret and
// the players' addresses (PEERS), so a room is a short code, and relays the packets of a pair that cannot reach each other
// (MKRL frames: the room's own HMAC-tagged packet, wrapped; the receiver does not learn the server as the peer's address).
// A peer we have not heard from in 8 s is "not connected" and is punched at again every 400 ms; after 60 s it is
// dropped. Not done: encryption, replay protection, relaying through a third machine, IPv6 (the game uses IPv4 only).
#include "p2p.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET Sock;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#define sock_poll WSAPoll
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int Sock;
#define SOCK_BAD (-1)
#define sock_close close
#define sock_poll poll
#endif

#include "platform/platform.h"
#include "sha256.h"

void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

enum { T_HELLO = 1, T_WELCOME, T_PING, T_PONG, T_DATA, T_TABLE, T_BYE };
#define HDR 8
#define TAG 16
#define MAX_PACKET 1500
#define ENTRY 13               // slot, public ip, public port, local ip, local port
#define ALIVE_MS 8000
#define DROP_MS 60000
#define PUNCH_MS 400
#define KEEPALIVE_MS 2000
#define CODE_HOST 0xA1
#define CODE_GUEST 0xA2

typedef struct {
    uint32_t ip;     // host order
    uint16_t port;   // host order
} Ep;

typedef struct {
    int used, have_addr;
    char handle[24];               // server mode: the player's @handle
    uint64_t last_direct, last_relay_tx;   // last packet that came straight; last keep-alive sent through the server
    Ep cand[2];                    // public and local address, as announced
    struct sockaddr_in addr;       // where the last authentic packet came from: the way in
    uint32_t nonce;                // a guest's HELLO nonce (0: unknown)
    uint64_t last_rx, last_tx, last_punch;
    int rtt;
} Peer;

struct P2p {
    Sock s;
    uint16_t lport;
    Ep pub, loc;
    int pub_known, stun_done;
    volatile int stop;
    char stun[160];
    int role;                      // 0 none, 1 host, 2 guest
    uint8_t slot;                  // 0 until the host's WELCOME
    uint8_t key10[10], key[32];
    uint32_t nonce;
    Peer peers[P2P_MAX_PLAYERS + 1];
    int table_dirty;
    uint64_t last_hello, last_table;
    // server mode
    struct sockaddr_in srv;
    char srv_spec[160], handle[24], secret[72], password[72], token[24], srv_role[12], room_code[8];
    int srv_resolve, srv_set, srv_state;   // 0 off / 1 logging in / 2 logged in
    uint64_t srv_tx, srv_rx;
    char events[8][192];
    int ev_n;
    int block_direct;              // tests: drop what peers send straight, as a NAT that blocks them would
    PlatMutex lock;
    PlatThread *th;
    P2pDeliver cb;
    void *user;
};

// ---- bytes
static void put16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)(v >> 8), b[1] = (uint8_t)v; }
static void put32(uint8_t *b, uint32_t v) { put16(b, (uint16_t)(v >> 16)), put16(b + 2, (uint16_t)v); }
static uint16_t get16(const uint8_t *b) { return (uint16_t)(b[0] << 8 | b[1]); }
static uint32_t get32(const uint8_t *b) { return (uint32_t)get16(b) << 16 | get16(b + 2); }
static void put64(uint8_t *b, uint64_t v) { put32(b, (uint32_t)(v >> 32)), put32(b + 4, (uint32_t)v); }
static uint64_t get64(const uint8_t *b) { return (uint64_t)get32(b) << 32 | get32(b + 4); }

static void sa_make(struct sockaddr_in *a, Ep e) {
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_addr.s_addr = htonl(e.ip);
    a->sin_port = htons(e.port);
}
static Ep ep_of(const struct sockaddr_in *a) { return (Ep){ntohl(a->sin_addr.s_addr), ntohs(a->sin_port)}; }

// HMAC-SHA256 (RFC 2104) of the packet's header and payload, cut to 16 bytes
static void tag_of(const uint8_t key[32], const uint8_t *msg, size_t n, uint8_t out[TAG]) {
    uint8_t k[64] = {0}, ip[64], op[64], inner[32], full[32];
    memcpy(k, key, 32);
    for (int i = 0; i < 64; i++) ip[i] = k[i] ^ 0x36, op[i] = k[i] ^ 0x5c;
    Sha256 s;
    sha256_init(&s), sha256_update(&s, ip, 64), sha256_update(&s, msg, n), sha256_final(&s, inner);
    sha256_init(&s), sha256_update(&s, op, 64), sha256_update(&s, inner, 32), sha256_final(&s, full);
    memcpy(out, full, TAG);
}

// ---- invite codes: Crockford-style base32 without I, O, 0, 1, in groups of five, CRC-16 inside
static const char B32[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

static uint16_t crc16(const uint8_t *b, size_t n) {
    uint16_t c = 0xffff;
    while (n--) {
        c ^= (uint16_t)(*b++ << 8);
        for (int i = 0; i < 8; i++) c = (uint16_t)(c & 0x8000 ? c << 1 ^ 0x1021 : c << 1);
    }
    return c;
}

static void code_encode(const uint8_t *b, size_t n, char out[P2P_CODE_MAX]) {
    uint32_t acc = 0;
    int bits = 0, chars = 0, o = 0;
    for (size_t i = 0; i < n || bits > 0;) {
        if (bits < 5) {
            if (i < n) acc = acc << 8 | b[i++], bits += 8;
            else acc <<= 5 - bits, bits = 5;
        }
        if (chars && chars % 5 == 0) out[o++] = '-';
        out[o++] = B32[acc >> (bits - 5) & 31];
        bits -= 5, chars++;
    }
    out[o] = 0;
}

// the bytes of a code, or -1; at most max
static int code_decode(const char *s, uint8_t *out, int max) {
    uint32_t acc = 0;
    int bits = 0, n = 0;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c == '-' || c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        const char *p = c ? strchr(B32, c) : NULL;
        if (!p) return -1;
        acc = acc << 5 | (uint32_t)(p - B32), bits += 5;
        if (bits >= 8) {
            if (n >= max) return -1;
            out[n++] = (uint8_t)(acc >> (bits - 8)), bits -= 8;
        }
    }
    return n;
}

typedef struct {
    char kind;       // 'h' or 'g'
    Ep pub, loc;
    uint8_t key10[10];
} Code;

static int code_parse(const char *s, Code *c) {
    uint8_t b[32];
    int n = code_decode(s, b, sizeof b);
    int len = n > 0 && b[0] == CODE_HOST ? 25 : n > 0 && b[0] == CODE_GUEST ? 15 : 0;
    if (!len || n < len || get16(b + len - 2) != crc16(b, (size_t)len - 2)) return -1;
    c->kind = b[0] == CODE_HOST ? 'h' : 'g';
    c->pub = (Ep){get32(b + 1), get16(b + 9)};
    c->loc = (Ep){get32(b + 5), get16(b + 11)};
    if (c->kind == 'h') memcpy(c->key10, b + 13, 10);
    return 0;
}

int p2p_code_check(const char *code, char *kind) {
    Code c;
    if (code_parse(code, &c)) return -1;
    if (kind) *kind = c.kind;
    return 0;
}

static void key_derive(P2p *p) {
    Sha256 s;
    sha256_init(&s), sha256_update(&s, "mkw-p2p-v1", 10), sha256_update(&s, p->key10, 10), sha256_final(&s, p->key);
}

// ---- sending
static size_t build(P2p *p, uint8_t *b, int type, const uint8_t *pl, size_t n) {
    memcpy(b, "MKP2", 4);
    b[4] = (uint8_t)type, b[5] = p->slot;
    put16(b + 6, (uint16_t)n);
    if (n) memcpy(b + HDR, pl, n);
    tag_of(p->key, b, HDR + n, b + HDR + n);
    return HDR + n + TAG;
}

static void send_to(P2p *p, const struct sockaddr_in *to, int type, const uint8_t *pl, size_t n) {
    uint8_t b[MAX_PACKET];
    size_t len = build(p, b, type, pl, n);
    sendto(p->s, (const char *)b, (int)len, 0, (const struct sockaddr *)to, sizeof *to);
}

// through the server: "MKRL", the destination's slot, then the packet as it would have gone straight
static void send_relay(P2p *p, int slot, int type, const uint8_t *pl, size_t n) {
    uint8_t b[MAX_PACKET + 5];
    memcpy(b, "MKRL", 4);
    b[4] = (uint8_t)slot;
    size_t len = build(p, b + 5, type, pl, n);
    sendto(p->s, (const char *)b, (int)(5 + len), 0, (const struct sockaddr *)&p->srv, sizeof p->srv);
}

static int direct_ok(const Peer *q, uint64_t now) { return q->have_addr && now - q->last_direct < ALIVE_MS; }

static void send_ep(P2p *p, Ep e, int type, const uint8_t *pl, size_t n) {
    if (!e.ip || !e.port) return;
    struct sockaddr_in a;
    sa_make(&a, e);
    send_to(p, &a, type, pl, n);
}

static void ping(P2p *p, Peer *q, uint64_t now, int all) {
    uint8_t t[8];
    put64(t, now);
    if (q->have_addr) send_to(p, &q->addr, T_PING, t, 8);
    if (all || !q->have_addr)
        for (int i = 0; i < 2; i++) send_ep(p, q->cand[i], T_PING, t, 8);
    q->last_tx = now;
}

// ---- the table of players
static size_t table_write(P2p *p, uint8_t *out) {   // count, then ENTRY bytes per player
    size_t o = 1;
    int n = 0;
    for (int s = 1; s <= P2P_MAX_PLAYERS; s++) {
        Ep pub, loc;
        if (s == p->slot) pub = p->pub, loc = (Ep){p->loc.ip, p->lport};
        else if (p->peers[s].used) pub = p->peers[s].cand[0], loc = p->peers[s].cand[1];
        else continue;
        out[o] = (uint8_t)s;
        put32(out + o + 1, pub.ip), put16(out + o + 5, pub.port), put32(out + o + 7, loc.ip), put16(out + o + 11, loc.port);
        o += ENTRY, n++;
    }
    out[0] = (uint8_t)n;
    return o;
}

static void table_apply(P2p *p, const uint8_t *b, size_t len) {
    if (!len) return;
    size_t n = b[0];
    if (1 + n * ENTRY > len) return;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *e = b + 1 + i * ENTRY;
        int s = e[0];
        if (s < 1 || s > P2P_MAX_PLAYERS || s == p->slot || s == 1) continue;   // the host's address we have
        Peer *q = &p->peers[s];
        if (!q->used) memset(q, 0, sizeof *q), q->used = 1, q->rtt = -1;
        if (!q->have_addr) {
            q->cand[0] = (Ep){get32(e + 1), get16(e + 5)};
            q->cand[1] = (Ep){get32(e + 7), get16(e + 11)};
        }
    }
}

static Peer *peer_new(P2p *p, int slot) {
    Peer *q = &p->peers[slot];
    memset(q, 0, sizeof *q);
    q->used = 1, q->rtt = -1;
    return q;
}

static int slot_free(P2p *p) {
    for (int s = 2; s <= P2P_MAX_PLAYERS; s++)
        if (!p->peers[s].used) return s;
    return 0;
}

// ---- receiving
typedef struct {
    int deliver;
    uint32_t src;
    uint16_t sport, dport;
    const uint8_t *data;
    size_t len;
} Out;

// With the lock held; fills out when the game's callback has to run once it is released
static void handle(P2p *p, const uint8_t *b, size_t n, const struct sockaddr_in *from, int relayed, Out *out) {
    int type = b[4], s = b[5];
    size_t pl = get16(b + 6);
    const uint8_t *d = b + HDR;
    uint64_t now = plat_now_ms();
    if (p->role == 0) return;
    if (type == T_HELLO) {
        if (p->role != 1 || pl < 10 || relayed) return;
        uint32_t nonce = get32(d);
        Ep fe = ep_of(from), lcl = {get32(d + 4), get16(d + 8)};
        int slot = 0;
        for (int i = 2; i <= P2P_MAX_PLAYERS && !slot; i++) {
            Peer *q = &p->peers[i];
            if (q->used && ((q->nonce && q->nonce == nonce) ||
                            (!q->nonce && q->cand[0].ip == fe.ip && q->cand[0].port == fe.port))) slot = i;
        }
        if (!slot && !(slot = slot_free(p))) return;
        Peer *q = &p->peers[slot];
        if (!q->used) {
            peer_new(p, slot);
            rt_log("p2p: player %d joined from %u.%u.%u.%u:%u", slot, fe.ip >> 24, fe.ip >> 16 & 255,
                   fe.ip >> 8 & 255, fe.ip & 255, fe.port);
        }
        if (!q->have_addr || q->cand[0].ip != fe.ip || q->cand[0].port != fe.port) p->table_dirty = 1;
        q->nonce = nonce;
        q->cand[0] = fe;
        if (lcl.ip && lcl.port) q->cand[1] = lcl;
        q->addr = *from, q->have_addr = 1, q->last_rx = q->last_direct = now;
        uint8_t w[1 + 1 + P2P_MAX_PLAYERS * ENTRY];
        w[0] = (uint8_t)slot;
        size_t k = table_write(p, w + 1);
        send_to(p, from, T_WELCOME, w, 1 + k);
        return;
    }
    if (type == T_WELCOME) {
        if (p->role != 2 || relayed || s != 1 || pl < 2 || d[0] < 2 || d[0] > P2P_MAX_PLAYERS) return;
        if (!p->slot) rt_log("p2p: joined the room as player %d (100.77.0.%d)", d[0], d[0]);
        p->slot = d[0];
        Peer *h = &p->peers[1];
        h->addr = *from, h->have_addr = 1, h->last_rx = h->last_direct = now;
        table_apply(p, d + 1, pl - 1);
        return;
    }
    if (s < 1 || s > P2P_MAX_PLAYERS || s == p->slot || !p->slot) return;
    Peer *q = &p->peers[s];
    if (type == T_BYE) {   // the host leaving keeps the mesh: its slot stays, silent, and is punched at
        if (q->used && s != 1 && p->role != 3)   // (in a server's room the server's list says who is there) memset(q, 0, sizeof *q), p->table_dirty = 1;
        return;
    }
    if (!q->used && p->role == 2 && s != 1 && type != T_TABLE) peer_new(p, s)->cand[0] = ep_of(from);   // not yet in the table
    if (!q->used) return;
    q->last_rx = now;
    if (!relayed) q->addr = *from, q->have_addr = 1, q->last_direct = now;   // a relayed packet says nothing of the way in
    switch (type) {
    case T_PING:
        if (pl >= 8 && relayed) send_relay(p, s, T_PONG, d, 8);
        else if (pl >= 8) send_to(p, from, T_PONG, d, 8);
        break;
    case T_PONG:
        if (pl >= 8) {
            uint64_t t = get64(d);
            q->rtt = now >= t ? (int)(now - t) : 0;
        }
        break;
    case T_TABLE:
        if (s == 1) table_apply(p, d, pl);
        break;
    case T_DATA:
        if (pl >= 4) {
            out->deliver = 1, out->src = P2P_NET | (uint32_t)s, out->sport = get16(d), out->dport = get16(d + 2);
            out->data = d + 4, out->len = pl - 4;
        }
        break;
    default: break;
    }
    (void)n;
}

// ---- the server's lines (server mode); the lock is held
static void ev_push(P2p *p, const char *line) {
    if (p->ev_n == 8) memmove(p->events[0], p->events[1], 7 * sizeof p->events[0]), p->ev_n--;
    snprintf(p->events[p->ev_n++], sizeof p->events[0], "%s", line);
}

static void srv_say(P2p *p, const char *line) {
    char b[300];
    int n = snprintf(b, sizeof b, "MKSV %s", line);
    if (p->srv_set && n > 0) sendto(p->s, b, (int)n, 0, (const struct sockaddr *)&p->srv, sizeof p->srv);
}

static void srv_cmd(P2p *p, const char *line) {
    char b[280];
    snprintf(b, sizeof b, "%s %s", p->token, line);
    srv_say(p, b);
}

static int ep_parse(const char *t, Ep *e) {
    char ip[40];
    const char *c = strrchr(t, ':');
    struct in_addr a;
    if (!c || (size_t)(c - t) >= sizeof ip) return -1;
    memcpy(ip, t, (size_t)(c - t)), ip[c - t] = 0;
    if (inet_pton(AF_INET, ip, &a) != 1) return -1;
    *e = (Ep){ntohl(a.s_addr), (uint16_t)atoi(c + 1)};
    return 0;
}

static void srv_peers(P2p *p, char **w, int nw) {   // PEERS code n slot:@handle:ip:port:lip:lport ...
    int seen[P2P_MAX_PLAYERS + 1] = {0};
    for (int i = 3; i < nw; i++) {
        char *f[6];
        int nf = 0;
        for (char *c = w[i]; nf < 6;) {
            f[nf++] = c;
            if (!(c = strchr(c, ':'))) break;
            *c++ = 0;
        }
        int slot = atoi(f[0]);
        Ep pub, loc;
        if (nf < 6 || slot < 1 || slot > P2P_MAX_PLAYERS) continue;
        char pe[48], le[48];
        snprintf(pe, sizeof pe, "%s:%s", f[2], f[3]);
        snprintf(le, sizeof le, "%s:%s", f[4], f[5]);
        if (ep_parse(pe, &pub) || ep_parse(le, &loc)) continue;
        seen[slot] = 1;
        if (slot == p->slot) continue;
        Peer *q = &p->peers[slot];
        if (!q->used) peer_new(p, slot);
        q->cand[0] = pub, q->cand[1] = loc;
        snprintf(q->handle, sizeof q->handle, "%s", f[1]);
    }
    for (int sl = 1; sl <= P2P_MAX_PLAYERS; sl++)
        if (!seen[sl] && sl != p->slot) memset(&p->peers[sl], 0, sizeof p->peers[sl]);
}

static void srv_line(P2p *p, char *line) {
    char *w[40];
    int nw = 0;
    for (char *t = strtok(line, " \r\n"); t && nw < 40; t = strtok(NULL, " \r\n")) w[nw++] = t;
    if (!nw) return;
    p->srv_rx = plat_now_ms();
    if (!strcmp(w[0], "OK") && nw >= 4) {   // OK token ip:port role: logged in, and our address as the server sees it
        Ep seen;
        snprintf(p->token, sizeof p->token, "%s", w[1]);
        snprintf(p->srv_role, sizeof p->srv_role, "%s", w[3]);
        if (!ep_parse(w[2], &seen)) p->pub = seen, p->pub_known = 1, p->stun_done = 1;
        if (p->srv_state != 2) rt_log("p2p: logged in to the server as %s (%s), seen as %s", p->handle, w[3], w[2]);
        p->srv_state = 2;
    } else if (!strcmp(w[0], "YOU") && nw >= 3) {
        snprintf(p->srv_role, sizeof p->srv_role, "%s", w[2]);
        ev_push(p, "YOU");
    } else if (!strcmp(w[0], "ROOM") && nw >= 2) {   // a room we made: join it
        char b[96];
        snprintf(b, sizeof b, "JOIN %s %u.%u.%u.%u:%u", w[1], p->loc.ip >> 24, p->loc.ip >> 16 & 255, p->loc.ip >> 8 & 255,
                 p->loc.ip & 255, p->lport);
        srv_cmd(p, b);
    } else if (!strcmp(w[0], "JOINED") && nw >= 6 && strlen(w[3]) == 20) {
        for (int i = 0; i < 10; i++) {
            unsigned v;
            if (sscanf(w[3] + 2 * i, "%2x", &v) == 1) p->key10[i] = (uint8_t)v;
        }
        key_derive(p);
        memset(p->peers, 0, sizeof p->peers);
        p->role = 3, p->slot = (uint8_t)atoi(w[2]);
        snprintf(p->room_code, sizeof p->room_code, "%s", w[1]);
        rt_log("p2p: joined room %s (%s) as player %d (100.77.0.%d)", w[1], w[4], p->slot, p->slot);
        ev_push(p, "JOINED");
    } else if (!strcmp(w[0], "PEERS") && nw >= 3 && p->role == 3 && !strcmp(w[1], p->room_code)) {
        srv_peers(p, w, nw);
    } else if (!strcmp(w[0], "KICKED")) {
        memset(p->peers, 0, sizeof p->peers);
        p->role = 0, p->slot = 0, p->room_code[0] = 0;
        ev_push(p, "KICKED");
    } else if (!strcmp(w[0], "ERR") || !strcmp(w[0], "ROOMS") || !strcmp(w[0], "INVITED")) {
        char b[192] = "";
        for (int i = 0; i < nw; i++) {
            size_t l = strlen(b);
            snprintf(b + l, sizeof b - l, "%s%s", i ? " " : "", w[i]);
        }
        ev_push(p, b);
    }
}

static int from_server(const P2p *p, const struct sockaddr_in *from) {
    return p->srv_set && from->sin_addr.s_addr == p->srv.sin_addr.s_addr && from->sin_port == p->srv.sin_port;
}

static void on_packet(P2p *p, const uint8_t *b, size_t n, const struct sockaddr_in *from) {
    int relayed = 0;
    if (n > 5 && !memcmp(b, "MKSV ", 5)) {   // the server's text
        char line[400];
        if (n - 5 >= sizeof line) return;
        memcpy(line, b + 5, n - 5), line[n - 5] = 0;
        plat_mutex_lock(&p->lock);
        if (from_server(p, from)) srv_line(p, line);
        plat_mutex_unlock(&p->lock);
        return;
    }
    if (n > 5 && !memcmp(b, "MKRL", 4)) {   // a room packet the server relayed: "MKRL", the sender's slot, the packet
        plat_mutex_lock(&p->lock);
        int ok = from_server(p, from);
        plat_mutex_unlock(&p->lock);
        if (!ok || !b[4]) return;
        relayed = b[4], b += 5, n -= 5;
    }
    if (n < HDR + TAG || memcmp(b, "MKP2", 4)) return;
    size_t pl = get16(b + 6);
    if (HDR + pl + TAG != n) return;
    Out out = {0};
    plat_mutex_lock(&p->lock);
    uint8_t want[TAG], diff = 0;
    tag_of(p->key, b, HDR + pl, want);
    for (int i = 0; i < TAG; i++) diff |= (uint8_t)(want[i] ^ b[HDR + pl + i]);
    if (!diff && (!relayed || b[5] == relayed) && !(p->block_direct && !relayed)) handle(p, b, n, from, relayed, &out);
    plat_mutex_unlock(&p->lock);
    if (out.deliver) p->cb(out.src, out.sport, out.dport, out.data, out.len, p->user);
}

// ---- the clock: HELLO until welcomed, punching, keep-alive, the table
static void tick(P2p *p) {
    uint64_t now = plat_now_ms();
    plat_mutex_lock(&p->lock);
    if (p->srv_set) {   // the server: log in until it answers, then a keep-alive that also holds the NAT's mapping open
        if (p->srv_state == 2 && now - p->srv_rx > 20000) p->srv_state = 1;
        if (p->srv_state != 2 && now - p->srv_tx >= 1000) {
            char b[240];
            snprintf(b, sizeof b, "HELLO %s %s %s", p->handle, p->secret, p->password);
            srv_say(p, b);
            p->srv_state = 1, p->srv_tx = now;
        } else if (p->srv_state == 2 && now - p->srv_tx >= 5000) {
            srv_cmd(p, "PING");
            p->srv_tx = now;
        }
    }
    if (p->role) {
        if (p->role == 2 && !p->slot && now - p->last_hello >= 500) {
            uint8_t h[10];
            put32(h, p->nonce), put32(h + 4, p->loc.ip), put16(h + 8, p->lport);
            for (int i = 0; i < 2; i++) send_ep(p, p->peers[1].cand[i], T_HELLO, h, 10);
            p->last_hello = now;
        }
        for (int s = 1; s <= P2P_MAX_PLAYERS && p->slot; s++) {
            Peer *q = &p->peers[s];
            if (!q->used || s == p->slot) continue;
            int alive = direct_ok(q, now);
            if (alive) {
                if (now - q->last_tx >= KEEPALIVE_MS) ping(p, q, now, 0);
            } else {
                if (now - q->last_punch >= PUNCH_MS) {
                    ping(p, q, now, 1);
                    q->last_punch = now;
                }
                if (p->role == 3 && p->srv_state == 2 && now - q->last_relay_tx >= KEEPALIVE_MS) {   // through the server
                    uint8_t t[8];
                    put64(t, now);
                    send_relay(p, s, T_PING, t, 8);
                    q->last_relay_tx = now;
                }
            }
            if (q->have_addr && now - q->last_rx > DROP_MS && s != 1 && p->role != 3) {
                rt_log("p2p: player %d dropped (silent for a minute)", s);
                memset(q, 0, sizeof *q);
                p->table_dirty = 1;
            }
        }
        if (p->role == 1 && (p->table_dirty || now - p->last_table >= 2000)) {
            uint8_t t[1 + P2P_MAX_PLAYERS * ENTRY];
            size_t k = table_write(p, t);
            for (int s = 2; s <= P2P_MAX_PLAYERS; s++)
                if (p->peers[s].used && p->peers[s].have_addr) send_to(p, &p->peers[s].addr, T_TABLE, t, k);
            p->table_dirty = 0, p->last_table = now;
        }
    }
    plat_mutex_unlock(&p->lock);
}

// ---- STUN (RFC 5389): one binding request from the room's own socket
static int stun_try(P2p *p, const char *spec, Ep *mapped) {
    char host[160], *colon;
    snprintf(host, sizeof host, "%s", spec);
    uint16_t port = 3478;
    if ((colon = strrchr(host, ':'))) *colon = 0, port = (uint16_t)atoi(colon + 1);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) || !res) return -1;
    struct sockaddr_in to = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    to.sin_port = htons(port);
    uint8_t req[20] = {0, 1, 0, 0, 0x21, 0x12, 0xa4, 0x42}, rsp[MAX_PACKET];
    plat_random(req + 8, 12);
    for (int attempt = 0; attempt < 3 && !p->stop; attempt++) {
        sendto(p->s, (const char *)req, sizeof req, 0, (struct sockaddr *)&to, sizeof to);
        uint64_t until = plat_now_ms() + 700;
        while (!p->stop && plat_now_ms() < until) {
            struct pollfd pf = {.fd = p->s, .events = POLLIN};
            if (sock_poll(&pf, 1, 100) <= 0) continue;
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = (int)recvfrom(p->s, (char *)rsp, sizeof rsp, 0, (struct sockaddr *)&from, &fl);
            if (n < 20) continue;
            if (rsp[0] != 0x01 || rsp[1] != 0x01 || memcmp(rsp + 4, req + 4, 16)) {
                on_packet(p, rsp, (size_t)n, &from);   // a peer's packet that came in meanwhile
                continue;
            }
            for (int o = 20; o + 4 <= n && o + 4 + get16(rsp + o + 2) <= n; o += 4 + ((get16(rsp + o + 2) + 3) & ~3)) {
                int type = get16(rsp + o), len = get16(rsp + o + 2);
                const uint8_t *v = rsp + o + 4;
                if (len >= 8 && v[1] == 1 && (type == 0x0020 || type == 0x0001)) {
                    uint32_t x = type == 0x0020 ? 0x2112a442u : 0;
                    *mapped = (Ep){get32(v + 4) ^ x, (uint16_t)(get16(v + 2) ^ (x >> 16))};
                    return 0;
                }
            }
        }
    }
    return -1;
}

// the address a datagram to the internet would leave from: a connected UDP socket sends nothing
static uint32_t find_local_ip(void) {
    Sock s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == SOCK_BAD) return 0;
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(9)}, me;
    to.sin_addr.s_addr = htonl(0xc0000201u);   // 192.0.2.1, TEST-NET-1: never routed
    socklen_t l = sizeof me;
    uint32_t ip = 0;
    if (!connect(s, (struct sockaddr *)&to, sizeof to) && !getsockname(s, (struct sockaddr *)&me, &l)) ip = ntohl(me.sin_addr.s_addr);
    sock_close(s);
    return ip;
}

// the server's address from its "host:port" (27500 without a port); on this thread, DNS can take a while
static void srv_resolve(P2p *p) {
    char spec[160], *colon;
    plat_mutex_lock(&p->lock);
    snprintf(spec, sizeof spec, "%s", p->srv_spec);
    p->srv_resolve = 0;
    plat_mutex_unlock(&p->lock);
    uint16_t port = 27500;
    if ((colon = strrchr(spec, ':'))) *colon = 0, port = (uint16_t)atoi(colon + 1);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *res = NULL;
    int bad = getaddrinfo(spec, NULL, &hints, &res) || !res;
    plat_mutex_lock(&p->lock);
    if (bad) {
        char e[192];
        snprintf(e, sizeof e, "ERR cannot find the server %.80s", spec);
        ev_push(p, e);
    } else {
        p->srv = *(struct sockaddr_in *)res->ai_addr;
        p->srv.sin_port = htons(port);
        p->srv_set = 1, p->srv_state = 0, p->srv_tx = 0;
    }
    plat_mutex_unlock(&p->lock);
    if (res) freeaddrinfo(res);
}

static void run(void *arg) {
    P2p *p = arg;
    int found = 0;
    Ep mapped = {0, 0};
    char list[160];
    snprintf(list, sizeof list, "%s", p->stun);
    for (char *tok = list; tok && *tok && !found && !p->stop;) {   // "host:port,host:port": the first that answers
        char *comma = strchr(tok, ',');
        if (comma) *comma = 0;
        found = !stun_try(p, tok, &mapped);
        tok = comma ? comma + 1 : NULL;
    }
    plat_mutex_lock(&p->lock);
    p->pub = found ? mapped : (Ep){p->loc.ip, p->lport};   // without an answer: a room for one local network
    p->pub_known = found, p->stun_done = 1;
    plat_mutex_unlock(&p->lock);
    if (found) rt_log("p2p: public address %u.%u.%u.%u:%u", p->pub.ip >> 24, p->pub.ip >> 16 & 255, p->pub.ip >> 8 & 255, p->pub.ip & 255, p->pub.port);
    else if (p->stun[0] && !p->stop) rt_log("p2p: no STUN answer: the room works on the local network only");
    uint8_t b[MAX_PACKET];
    while (!p->stop) {
        if (p->srv_resolve) srv_resolve(p);
        struct pollfd pf = {.fd = p->s, .events = POLLIN};
        if (sock_poll(&pf, 1, 40) > 0)
            for (int i = 0; i < 64; i++) {
                struct sockaddr_in from;
                socklen_t fl = sizeof from;
                struct pollfd more = {.fd = p->s, .events = POLLIN};
                if (i && sock_poll(&more, 1, 0) <= 0) break;
                int n = (int)recvfrom(p->s, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &fl);
                if (n > 0) on_packet(p, b, (size_t)n, &from);
            }
        tick(p);
    }
}

// ---- API
P2p *p2p_new(P2pDeliver cb, void *user, const char *stun, uint16_t port, const char *local_ip) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return NULL;
#endif
    P2p *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->cb = cb, p->user = user;
    if (stun) snprintf(p->stun, sizeof p->stun, "%s", stun);
    p->s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)}, me;
    socklen_t l = sizeof me;
    if (p->s == SOCK_BAD || bind(p->s, (struct sockaddr *)&a, sizeof a) || getsockname(p->s, (struct sockaddr *)&me, &l)) {
        if (p->s != SOCK_BAD) sock_close(p->s);
        free(p);
        return NULL;
    }
#ifdef _WIN32
    DWORD no = 0, got;
    WSAIoctl(p->s, SIO_UDP_CONNRESET, &no, sizeof no, NULL, 0, &got, NULL, NULL);   // an ICMP reset is not an error here
#endif
    p->lport = ntohs(me.sin_port);
    struct in_addr oa;   // given: a PC with several adapters, or a test on the loopback
    p->loc = (Ep){local_ip && inet_pton(AF_INET, local_ip, &oa) == 1 ? ntohl(oa.s_addr) : find_local_ip(), p->lport};
    plat_random(&p->nonce, sizeof p->nonce);
    if (!p->nonce) p->nonce = 1;
    key_derive(p);   // no room yet: handle() ignores every packet until p2p_host or p2p_join
    p->th = plat_thread_new(run, p, "p2p");
    if (!p->th) {
        sock_close(p->s);
        free(p);
        return NULL;
    }
    return p;
}

void p2p_leave(P2p *p) {
    plat_mutex_lock(&p->lock);
    if (p->role && p->slot)
        for (int s = 1; s <= P2P_MAX_PLAYERS; s++)
            if (p->peers[s].used && p->peers[s].have_addr) send_to(p, &p->peers[s].addr, T_BYE, NULL, 0);
    if (p->role == 3 && p->srv_state == 2) srv_cmd(p, "LEAVE");
    p->room_code[0] = 0;
    memset(p->peers, 0, sizeof p->peers);
    p->role = 0, p->slot = 0, p->table_dirty = 0;
    plat_mutex_unlock(&p->lock);
}

void p2p_free(P2p *p) {
    if (!p) return;
    p2p_leave(p);
    p->stop = 1;
    plat_thread_join(p->th);
    sock_close(p->s);
    free(p);
}

int p2p_host(P2p *p) {
    p2p_leave(p);
    plat_mutex_lock(&p->lock);
    plat_random(p->key10, sizeof p->key10);
    key_derive(p);
    p->role = 1, p->slot = 1;
    plat_mutex_unlock(&p->lock);
    rt_log("p2p: room opened, you are player 1 (100.77.0.1)");
    return 0;
}

int p2p_join(P2p *p, const char *host_code) {
    Code c;
    if (code_parse(host_code, &c) || c.kind != 'h') return -1;
    p2p_leave(p);
    plat_mutex_lock(&p->lock);
    memcpy(p->key10, c.key10, sizeof p->key10);
    key_derive(p);
    p->role = 2, p->slot = 0;
    Peer *h = peer_new(p, 1);
    h->cand[0] = c.pub, h->cand[1] = c.loc;
    p->last_hello = 0;
    plat_mutex_unlock(&p->lock);
    return 0;
}

int p2p_add(P2p *p, const char *peer_code) {
    Code c;
    if (code_parse(peer_code, &c) || c.kind != 'g') return -1;
    plat_mutex_lock(&p->lock);
    if (p->role == 1) {
        int slot = 0;
        for (int s = 2; s <= P2P_MAX_PLAYERS && !slot; s++)
            if (p->peers[s].used && p->peers[s].cand[0].ip == c.pub.ip && p->peers[s].cand[0].port == c.pub.port) slot = s;
        if (!slot && (slot = slot_free(p))) {
            Peer *q = peer_new(p, slot);
            q->cand[0] = c.pub, q->cand[1] = c.loc;
            p->table_dirty = 1;
        }
    }
    plat_mutex_unlock(&p->lock);
    return 0;
}

int p2p_code(P2p *p, char out[P2P_CODE_MAX]) {
    uint8_t b[25];
    int ok = 0;
    plat_mutex_lock(&p->lock);
    if (p->role && p->stun_done) {
        size_t n = p->role == 1 ? 23 : 13;
        b[0] = p->role == 1 ? CODE_HOST : CODE_GUEST;
        put32(b + 1, p->pub.ip), put32(b + 5, p->loc.ip), put16(b + 9, p->pub.port), put16(b + 11, p->lport);
        if (p->role == 1) memcpy(b + 13, p->key10, 10);
        put16(b + n, crc16(b, n));
        code_encode(b, n + 2, out);
        ok = 1;
    }
    plat_mutex_unlock(&p->lock);
    return ok;
}

int p2p_role(P2p *p) {
    plat_mutex_lock(&p->lock);
    int r = p->role;
    plat_mutex_unlock(&p->lock);
    return r;
}

uint16_t p2p_port(P2p *p) { return p->lport; }

uint32_t p2p_vip(P2p *p) {
    plat_mutex_lock(&p->lock);
    uint32_t v = p->role && p->slot ? P2P_NET | p->slot : 0;
    plat_mutex_unlock(&p->lock);
    return v;
}

int p2p_players(P2p *p, P2pPlayer *out, int max) {
    int n = 0;
    uint64_t now = plat_now_ms();
    plat_mutex_lock(&p->lock);
    if (p->role && p->slot) {
        for (int s = 1; s <= P2P_MAX_PLAYERS && n < max; s++) {
            Peer *q = &p->peers[s];
            if (s == p->slot) out[n++] = (P2pPlayer){P2P_NET | (uint32_t)s, 0, 1, 1, p->handle};
            else if (q->used) out[n++] = (P2pPlayer){P2P_NET | (uint32_t)s, q->rtt, q->last_rx && now - q->last_rx < ALIVE_MS, 0, q->handle};
        }
        for (int i = 1; i < n; i++)   // us first
            if (out[i].self) {
                P2pPlayer t = out[i];
                memmove(out + 1, out, (size_t)i * sizeof *out);
                out[0] = t;
            }
    }
    plat_mutex_unlock(&p->lock);
    return n;
}

int p2p_send(P2p *p, uint32_t dst, uint16_t sport, uint16_t dport, const void *data, size_t len) {
    if (!p || len > P2P_MAX_DATA) return -1;
    uint8_t pl[4 + P2P_MAX_DATA];
    put16(pl, sport), put16(pl + 2, dport);
    memcpy(pl + 4, data, len);
    int to_self = 0, sent = 0, rc = -1;
    uint32_t me;
    plat_mutex_lock(&p->lock);
    if (p->role && p->slot) {
        me = P2P_NET | p->slot;
        int bcast = dst == P2P_BROADCAST || (dst >> 28) == 0xe;   // a multicast group is the room too
        if (bcast || dst == me) to_self = 1;
        uint64_t now = plat_now_ms();
        for (int s = 1; s <= P2P_MAX_PLAYERS; s++) {
            Peer *q = &p->peers[s];
            if (s == p->slot || !q->used || (!bcast && dst != (P2P_NET | (uint32_t)s))) continue;
            if (direct_ok(q, now)) send_to(p, &q->addr, T_DATA, pl, 4 + len), sent++;
            else if (p->role == 3 && p->srv_state == 2) send_relay(p, s, T_DATA, pl, 4 + len), sent++;   // no way in yet
        }
        rc = bcast || to_self || sent ? (int)len : -1;
    } else me = 0;
    plat_mutex_unlock(&p->lock);
    if (to_self) p->cb(me, sport, dport, data, len, p->user);
    return rc;
}

// ---- server mode
int p2p_server_connect(P2p *p, const char *spec, const char *handle, const char *secret, const char *password) {
    if (!spec || !*spec || !handle || handle[0] != '@' || strlen(handle) >= sizeof p->handle || !secret ||
        strlen(secret) < 8 || strlen(secret) >= sizeof p->secret || strchr(spec, ' ') || strchr(handle, ' '))
        return -1;
    plat_mutex_lock(&p->lock);
    snprintf(p->srv_spec, sizeof p->srv_spec, "%s", spec);
    snprintf(p->handle, sizeof p->handle, "%s", handle);
    snprintf(p->secret, sizeof p->secret, "%s", secret);
    snprintf(p->password, sizeof p->password, "%s", password ? password : "");
    p->srv_set = 0, p->srv_state = 0, p->srv_resolve = 1;
    plat_mutex_unlock(&p->lock);
    return 0;
}

void p2p_server_disconnect(P2p *p) {
    p2p_leave(p);
    plat_mutex_lock(&p->lock);
    p->srv_set = p->srv_resolve = p->srv_state = 0;
    plat_mutex_unlock(&p->lock);
}

int p2p_server_command(P2p *p, const char *line) {
    int ok = 0;
    plat_mutex_lock(&p->lock);
    if (p->srv_state == 2 && strlen(line) < 200) srv_cmd(p, line), ok = 1;
    plat_mutex_unlock(&p->lock);
    return ok ? 0 : -1;
}

int p2p_server_join(P2p *p, const char *code) {
    char b[120];
    if (!code || strlen(code) > 12 || strchr(code, ' ')) return -1;
    plat_mutex_lock(&p->lock);
    snprintf(b, sizeof b, "JOIN %s %u.%u.%u.%u:%u", code, p->loc.ip >> 24, p->loc.ip >> 16 & 255, p->loc.ip >> 8 & 255,
             p->loc.ip & 255, p->lport);
    int ok = p->srv_state == 2;
    if (ok) srv_cmd(p, b);
    plat_mutex_unlock(&p->lock);
    return ok ? 0 : -1;
}

int p2p_server_poll(P2p *p, char out[192]) {
    int ok = 0;
    plat_mutex_lock(&p->lock);
    if (p->ev_n) {
        snprintf(out, 192, "%s", p->events[0]);
        memmove(p->events[0], p->events[1], (size_t)(p->ev_n - 1) * sizeof p->events[0]);
        p->ev_n--, ok = 1;
    }
    plat_mutex_unlock(&p->lock);
    return ok;
}

void p2p_server_status(P2p *p, P2pServer *st) {
    plat_mutex_lock(&p->lock);
    st->state = p->srv_set ? p->srv_state : 0;
    snprintf(st->role, sizeof st->role, "%s", p->srv_role);
    snprintf(st->handle, sizeof st->handle, "%s", p->handle);
    snprintf(st->room, sizeof st->room, "%s", p->room_code);
    plat_mutex_unlock(&p->lock);
}

void p2p_debug_block_direct(P2p *p, int on) { p->block_direct = on; }
