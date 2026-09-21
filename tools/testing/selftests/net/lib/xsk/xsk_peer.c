// SPDX-License-Identifier: GPL-2.0
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../../kselftest.h"
#include "xsk_peer.h"

#define XSK_PEER_MAGIC        0x58534b50

enum xsk_peer_msg_type {
	XSK_PEER_CAPS = 1,
	XSK_PEER_START,
	XSK_PEER_PROGRESS,
	XSK_PEER_DONE,
	XSK_PEER_RESULT,
};

struct xsk_peer_msg {
	u32 magic;
	u32 type;
	union {
		struct xsk_caps caps;
		u32 arg[sizeof(struct xsk_caps) / sizeof(u32)];
	};
};

struct xsk_peer {
	int fd;
	u32 test_id;
	u32 step;
	bool peer_start;
	bool peer_done;
	bool peer_result;
	int peer_result_status;
	int cancel_status;
	bool broken;
};

static int io_full(int fd, void *buf, size_t len, bool write_op)
{
	ssize_t ret = write_op ? ksft_write_full(fd, buf, len) : ksft_read_full(fd, buf, len);

	if (ret < 0)
		return -errno;
	if ((size_t)ret != len)
		return -EPIPE;
	return 0;
}

static int peer_send(struct xsk_peer *peer, const struct xsk_peer_msg *msg)
{
	struct xsk_peer_msg wire = *msg;
	int err;
	u32 i;

	wire.magic = htonl(XSK_PEER_MAGIC);
	wire.type = htonl(msg->type);
	for (i = 0; i < ARRAY_SIZE(wire.arg); i++)
		wire.arg[i] = htonl(msg->arg[i]);
	err = io_full(peer->fd, &wire, sizeof(wire), true);
	if (err)
		peer->broken = true;
	return err;
}

static int peer_recv(struct xsk_peer *peer, struct xsk_peer_msg *msg)
{
	int err = io_full(peer->fd, msg, sizeof(*msg), false);
	u32 i;

	if (err) {
		peer->broken = true;
		return err;
	}
	msg->magic = ntohl(msg->magic);
	if (msg->magic != XSK_PEER_MAGIC) {
		peer->broken = true;
		return -EPROTO;
	}
	msg->type = ntohl(msg->type);
	for (i = 0; i < ARRAY_SIZE(msg->arg); i++)
		msg->arg[i] = ntohl(msg->arg[i]);
	return 0;
}

static int peer_handle(struct xsk_peer *peer, const struct xsk_peer_msg *msg,
		       bool test_finished)
{
	if (msg->arg[0] != peer->test_id)
		return -EPROTO;
	/*
	 * After local completion only the peer's RESULT matters: it may
	 * still be inside a step of this test before it sees our RESULT.
	 */
	if (msg->type != XSK_PEER_RESULT) {
		if (test_finished)
			return 0;
		if (msg->arg[1] != peer->step)
			return -EPROTO;
	}

	switch (msg->type) {
	case XSK_PEER_START:
		peer->peer_start = true;
		if (msg->arg[2] != TEST_PASS)
			peer->cancel_status = TEST_FAILURE;
		break;
	case XSK_PEER_PROGRESS:
		pkts_in_flight -= msg->arg[2];
		break;
	case XSK_PEER_DONE:
		peer->peer_done = true;
		if (msg->arg[2] != TEST_PASS)
			peer->cancel_status = TEST_FAILURE;
		break;
	case XSK_PEER_RESULT:
		peer->peer_result_status = (s32)msg->arg[1];
		peer->peer_result = true;
		if (peer->peer_result_status != TEST_PASS)
			peer->cancel_status = peer->peer_result_status;
		break;
	default:
		return -EPROTO;
	}
	return 0;
}

static int peer_recv_one(struct xsk_peer *peer, bool test_finished)
{
	struct xsk_peer_msg msg;
	int err;

	err = peer_recv(peer, &msg);
	if (!err)
		err = peer_handle(peer, &msg, test_finished);
	if (err)
		peer->broken = true;
	return err;
}

static int peer_wait(struct xsk_peer *peer, const bool *event)
{
	int err;

	while (!*event && !peer->peer_result) {
		err = peer_recv_one(peer, false);
		if (err)
			return err;
	}
	return 0;
}

static int peer_drain(struct xsk_peer *peer)
{
	struct pollfd pfd = { .fd = peer->fd, .events = POLLIN };
	int ret, err;

	for (;;) {
		ret = poll(&pfd, 1, 0);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (!ret)
			return 0;
		err = peer_recv_one(peer, false);
		if (err)
			return err;
	}
}

static int peer_cancelled(struct xsk_peer *peer)
{
	return peer->cancel_status ? -ECANCELED : 0;
}

/*
 * Announce our status for this step and block until the peer announces
 * the same step. If its RESULT arrives instead, it abandoned the test.
 */
static int peer_step_sync(struct xsk_peer *peer, u32 type, u32 step,
			  int status, bool *event)
{
	struct xsk_peer_msg msg = {
		.type = type,
		.arg = { peer->test_id, step, status },
	};
	int err = peer_send(peer, &msg);

	if (!err)
		err = peer_wait(peer, event);
	if (err)
		return err;
	if (!*event && !peer->cancel_status)
		peer->cancel_status = TEST_FAILURE;
	return peer_cancelled(peer);
}

int xsk_peer_step_start(struct xsk_peer *peer, u32 step, int status)
{
	peer->step = step;
	peer->peer_start = false;
	peer->peer_done = false;
	return peer_step_sync(peer, XSK_PEER_START, step, status,
			      &peer->peer_start);
}

int xsk_peer_rx_progress(struct xsk_peer *peer, u32 pkts)
{
	struct xsk_peer_msg msg = {
		.type = XSK_PEER_PROGRESS,
		.arg = { peer->test_id, peer->step, pkts },
	};
	int err;

	if (pkts) {
		err = peer_send(peer, &msg);
		if (err)
			return err;
	}
	err = peer_drain(peer);
	return err ? err : peer_cancelled(peer);
}

int xsk_peer_tx_sync(struct xsk_peer *peer)
{
	int err = peer_drain(peer);

	return err ? err : peer_cancelled(peer);
}

int xsk_peer_step_done(struct xsk_peer *peer, u32 step, int status)
{
	return peer_step_sync(peer, XSK_PEER_DONE, step, status,
			      &peer->peer_done);
}

/*
 * The launcher starts the listener first, waits for the port before
 * starting the connector, and bounds the run time of both, so neither
 * side needs its own retry or timeout logic here.
 */
static int peer_open_tcp(const char *host, const char *port, bool listen_side)
{
	struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
	int fd = -1, lfd, saved = ECONNREFUSED, one = 1, err;
	struct addrinfo *res, *ai;

	err = getaddrinfo(host, port, &hints, &res);
	if (err)
		return -EINVAL;

	if (!listen_side)
		goto conn;

	for (ai = res; ai; ai = ai->ai_next) {
		lfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (lfd < 0)
			continue;
		setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		if (!bind(lfd, ai->ai_addr, ai->ai_addrlen) && !listen(lfd, 1)) {
			fd = accept(lfd, NULL, NULL);
			saved = errno;
			close(lfd);
			break;
		}
		saved = errno;
		close(lfd);
	}
	freeaddrinfo(res);
	return fd >= 0 ? fd : -saved;

conn:
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (!connect(fd, ai->ai_addr, ai->ai_addrlen))
			break;
		saved = errno;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	return fd >= 0 ? fd : -saved;
}

struct xsk_peer *xsk_peer_open(const char *host, const char *port,
			       bool listen_side)
{
	struct xsk_peer *peer;
	int fd = peer_open_tcp(host, port, listen_side);

	if (fd < 0) {
		errno = -fd;
		return NULL;
	}
	peer = calloc(1, sizeof(*peer));
	if (!peer) {
		close(fd);
		return NULL;
	}
	peer->fd = fd;
	return peer;
}

void xsk_peer_close(struct xsk_peer *peer)
{
	if (peer) {
		close(peer->fd);
		free(peer);
	}
}

bool xsk_peer_broken(const struct xsk_peer *peer)
{
	return peer && peer->broken;
}

int xsk_peer_exchange_caps(struct xsk_peer *peer, struct ifobject *owned,
			   struct ifobject *shadow)
{
	struct xsk_peer_msg msg = {
		.type = XSK_PEER_CAPS,
		.caps = owned->caps,
	};
	int err = peer_send(peer, &msg);

	if (err)
		return err;
	err = peer_recv(peer, &msg);
	if (err)
		return err;
	if (msg.type != XSK_PEER_CAPS)
		return -EPROTO;
	shadow->caps = msg.caps;
	return 0;
}

void xsk_peer_test_begin(struct xsk_peer *peer, u32 test_id)
{
	peer->test_id = test_id;
	peer->step = 0;
	peer->peer_result = false;
	peer->peer_result_status = TEST_PASS;
	peer->cancel_status = 0;
}

int xsk_peer_test_result(struct xsk_peer *peer, int result)
{
	struct xsk_peer_msg msg = { .type = XSK_PEER_RESULT };
	int err;

	if (peer->cancel_status == TEST_SKIP && result == TEST_FAILURE)
		result = TEST_SKIP;
	msg.arg[0] = peer->test_id;
	msg.arg[1] = result;

	err = peer_send(peer, &msg);
	if (err)
		return TEST_FAILURE;
	while (!peer->peer_result) {
		err = peer_recv_one(peer, true);
		if (err)
			return TEST_FAILURE;
	}
	if (result == TEST_FAILURE || peer->peer_result_status == TEST_FAILURE)
		return TEST_FAILURE;
	if (result == TEST_SKIP || peer->peer_result_status == TEST_SKIP)
		return TEST_SKIP;
	return TEST_PASS;
}
