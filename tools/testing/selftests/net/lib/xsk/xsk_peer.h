/* SPDX-License-Identifier: GPL-2.0 */
#ifndef XSK_PEER_H_
#define XSK_PEER_H_

#include "test_xsk.h"

struct xsk_peer;

struct xsk_peer *xsk_peer_open(const char *host, const char *port,
			       bool listen);
void xsk_peer_close(struct xsk_peer *peer);
bool xsk_peer_broken(const struct xsk_peer *peer);

int xsk_peer_exchange_caps(struct xsk_peer *peer, struct ifobject *owned,
			   struct ifobject *shadow);

void xsk_peer_test_begin(struct xsk_peer *peer, u32 test_id);
int xsk_peer_step_start(struct xsk_peer *peer, u32 step, int status);
int xsk_peer_rx_progress(struct xsk_peer *peer, u32 pkts);
int xsk_peer_tx_sync(struct xsk_peer *peer);
int xsk_peer_step_done(struct xsk_peer *peer, u32 step, int status);
int xsk_peer_test_result(struct xsk_peer *peer, int result);

#endif /* XSK_PEER_H_ */
