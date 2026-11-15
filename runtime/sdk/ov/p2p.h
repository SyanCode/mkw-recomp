// Peer-to-peer virtual LAN (p2p.c): the game's LAN play (Pia over nn::socket) between PCs that are not on one network.
// Game-free: UDP sockets, the platform layer and SHA-256, so tests/c/t_p2p.c runs two instances over the loopback.
//
// A room is a virtual /24 (100.77.0.0, inside the RFC 6598 range ov_lan_only already accepts): the host is .1, each
// guest takes the next free slot, .255 is the broadcast. The game sends datagrams to those addresses; p2p_send carries
// them to the peer's real address and the deliver callback hands the ones that arrive back. Peers find each other
// without a server of ours: an invite code holds the host's public address (learnt from a STUN server), its local
// address, and a room secret. Every packet carries a 16-byte HMAC-SHA256 tag keyed by that secret, so only the people
// who got the code can inject traffic. Nothing is encrypted (the game's own traffic is not secret); the tag stops
// forgery, not eavesdropping.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define P2P_MAX_PLAYERS 24                 // slots 1..24 (the game's largest lobby)
#define P2P_NET 0x644D0000u                // 100.77.0.0 in host order
#define P2P_BROADCAST (P2P_NET | 0xffu)
#define P2P_CODE_MAX 64                    // room for a code, grouped, with its terminating zero

typedef struct P2p P2p;

// A datagram arrived from the virtual address src_vip (host order), sent from its port sport to our port dport. Runs on
// the p2p thread; keep it short.
typedef void (*P2pDeliver)(uint32_t src_vip, uint16_t sport, uint16_t dport, const uint8_t *data, size_t len, void *user);

typedef struct {
    uint32_t vip;       // virtual address, host order
    int rtt_ms;         // last round trip, -1 before the first answer
    int connected;      // 1 when heard from in the last few seconds
    int self;           // 1 for this machine
} P2pPlayer;

// Binds a UDP socket (port 0: any; a port you forwarded on the router: that port) and starts the thread. stun: "host:port"
// of a STUN server (several separated by commas: the first that answers), or NULL for none (a room then works on one
// local network only). local_ip: the address peers on our network should use ("192.168.1.20"), NULL or "" to find it.
// NULL on failure.
P2p *p2p_new(P2pDeliver cb, void *user, const char *stun, uint16_t port, const char *local_ip);
void p2p_free(P2p *p);   // tells the peers and stops the thread

// Becomes the host of a new room. 0 on success; the code follows with p2p_code once the public address is known.
int p2p_host(P2p *p);
// Joins the room of a host's code. 0 on success, -1 for a code that is malformed or not a room's.
int p2p_join(P2p *p, const char *host_code);
// Host: accepts a guest's own code, so both sides send first (needed behind a NAT that filters by sender). Guests: a
// guest's code is ignored; the host introduces everyone. 0 on success, -1 for a malformed code.
int p2p_add(P2p *p, const char *peer_code);
// This machine's invite code: the room's when hosting, else the guest's own, to hand to the host. 1 with the code
// written, 0 while the public address is still being looked up or no room was started.
int p2p_code(P2p *p, char out[P2P_CODE_MAX]);
// Ends the room: BYE to everyone, all peers dropped, the virtual address released.
void p2p_leave(P2p *p);

int p2p_role(P2p *p);                         // 0 no room, 1 host, 2 guest
uint16_t p2p_port(P2p *p);                    // the UDP port the room uses here (what to forward on a router)
uint32_t p2p_vip(P2p *p);                     // our virtual address in host order, 0 without one (not joined yet)
int p2p_players(P2p *p, P2pPlayer *out, int max);   // the room's players, us first; the count
// Sends one datagram to a virtual address (the broadcast and multicast ones go to everyone, and back to us as on a real
// LAN). Returns len, or -1 (no room, too long: the limit is P2P_MAX_DATA, or the peer is not reachable).
#define P2P_MAX_DATA 1400
int p2p_send(P2p *p, uint32_t dst_vip, uint16_t sport, uint16_t dport, const void *data, size_t len);

// The code's pieces, exposed for tests: 0 on a valid code of kind 'h' (host) or 'g' (guest), else -1.
int p2p_code_check(const char *code, char *kind);
