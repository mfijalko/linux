// SPDX-License-Identifier: GPL-2.0
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../../kselftest.h"
#include "xsk_peer.h"

#define XSK_PEER_MAGIC        0x58534b50

/* One case per connection; the launcher owns the verdict. */
enum xsk_peer_msg_type {
	XSK_PEER_READY = 1,
	XSK_PEER_PROGRESS,
	XSK_PEER_ABORT,
};

struct xsk_peer_msg {
	u32 magic;
	u32 type;
	u32 arg[7];
};

_Static_assert(sizeof(struct xsk_peer_msg) == 9 * sizeof(u32),
	       "control record size changed");

struct xsk_peer {
	int fd;
	u32 test_id;
	u32 step;
	u32 ready_step;
	bool ready_seen;
	bool closed;
	bool broken;
};

static int io_full(int fd, void *buf, size_t len, bool write_op)
{
	size_t done = 0;
	ssize_t ret;

	while (done < len) {
		if (write_op)
			ret = send(fd, (char *)buf + done, len - done, MSG_NOSIGNAL);
		else
			ret = read(fd, (char *)buf + done, len - done);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret < 0)
			return -errno;
		if (!ret)
			return -EPIPE;
		done += ret;
	}
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

static int peer_recv_one(struct xsk_peer *peer)
{
	struct xsk_peer_msg msg;
	int err = peer_recv(peer, &msg);

	if (err == -EPIPE) {
		peer->closed = true;
		return err;
	}
	if (err)
		goto bad;
	if (msg.arg[0] != peer->test_id) {
		err = -EPROTO;
		goto bad;
	}
	switch (msg.type) {
	case XSK_PEER_READY:
		if (msg.arg[1] < peer->step || msg.arg[1] > peer->step + 1) {
			err = -EPROTO;
			goto bad;
		}
		peer->ready_step = msg.arg[1];
		peer->ready_seen = true;
		break;
	case XSK_PEER_PROGRESS:
		if (msg.arg[1] != peer->step && msg.arg[1] + 1 != peer->step) {
			err = -EPROTO;
			goto bad;
		}
		pkts_in_flight -= msg.arg[2];
		break;
	case XSK_PEER_ABORT:
		return -ECANCELED;
	default:
		err = -EPROTO;
		goto bad;
	}
	return 0;
bad:
	peer->broken = true;
	return err;
}

static int peer_drain(struct xsk_peer *peer)
{
	struct pollfd pfd = { .fd = peer->fd, .events = POLLIN };
	int ret, err;

	if (peer->closed)
		return 0;
	for (;;) {
		ret = poll(&pfd, 1, 0);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (!ret)
			return 0;
		err = peer_recv_one(peer);
		if (err == -EPIPE)
			return 0;
		if (err)
			return err;
	}
}

void xsk_peer_case_begin(struct xsk_peer *peer, u32 test_id)
{
	peer->test_id = test_id;
	peer->step = 0;
}

int xsk_peer_ready(struct xsk_peer *peer, u32 step)
{
	struct xsk_peer_msg msg = {
		.type = XSK_PEER_READY,
		.arg = { peer->test_id, step },
	};
	int err;

	peer->step = step;
	err = peer_send(peer, &msg);
	if (err)
		return err;
	while (!peer->ready_seen || peer->ready_step != step) {
		err = peer_recv_one(peer);
		if (err)
			return err;
	}
	peer->ready_seen = false;
	return 0;
}

void xsk_peer_abort(struct xsk_peer *peer, u32 step)
{
	struct xsk_peer_msg msg = {
		.type = XSK_PEER_ABORT,
		.arg = { peer->test_id, step },
	};

	if (peer && !peer->closed && !peer->broken)
		peer_send(peer, &msg);
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
	return peer_drain(peer);
}

int xsk_peer_tx_sync(struct xsk_peer *peer)
{
	return peer_drain(peer);
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
	int one = 1;

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
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
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
