// The game's sockets and the peer-to-peer room (ov_p2p.c); plain C types so the menu (C++) can call it too.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int ov_p2p(void);   // MKW_P2P set: the LAN lobby runs over a peer-to-peer virtual network (implies ov_lan)

// ---- for ov_socket.c and ov_net.c
uint32_t ov_p2p_vip(void);                  // our virtual address, host order; 0 outside a room
int ov_p2p_is_virtual(uint32_t host_order); // 1 for 100.77.x.x, the broadcast 255.255.255.255 and multicast: what a room carries
// One datagram of socket port sport to a virtual address (the broadcast ones and multicast groups: the room). len, or -1.
int ov_p2p_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, size_t len);
// recvfrom answered with 127.0.0.1:lport: 1 when that is one of our relay sockets, with the sender it stands for
int ov_p2p_from(uint16_t lport, uint32_t *vip, uint16_t *sport);

// ---- for the menu (settings_tabs.cpp)
enum { P2P_ST_NONE, P2P_ST_HOSTING, P2P_ST_JOINING, P2P_ST_JOINED };
typedef struct {
    int mode;          // P2P_ST_*: JOINING until the host answers
    int players;       // in the room, us included
    int connected;     // of them, heard from lately (us included)
    int worst_rtt_ms;   // the other players' worst round trip, -1 with none
    int code_ready;    // the invite code can be read: the public address is known
    int slot;          // our player number, 0 before JOINED
} OvP2pStatus;
int ov_p2p_host(void);                    // opens a room; 0
int ov_p2p_join(const char *host_code);   // 0, or -1 for a text that is not a room's code
int ov_p2p_add(const char *guest_code);   // host: take a guest's code too; 0, or -1
int ov_p2p_code(char out[64]);            // this machine's code to give away; 1 when there is one
void ov_p2p_leave(void);
void ov_p2p_status(OvP2pStatus *st);

#ifdef __cplusplus
}
#endif
