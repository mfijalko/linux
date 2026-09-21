// SPDX-License-Identifier: GPL-2.0
#include <net/if.h>
#include <stdarg.h>
#include <sys/prctl.h>
#include <sys/wait.h>

#include "network_helpers.h"
#include "test_progs.h"
#include "test_xsk.h"
#include "xsk_peer.h"
#include "xsk_xdp_progs.skel.h"

#define VETH_RX "veth0"
#define VETH_TX "veth1"
#define MTU	1500
/* Control connection between the two endpoints, inside the test netns. */
#define PEER_HOST "127.0.0.1"
#define PEER_PORT "35989"
#define PEER_CONNECT_RETRIES 50

int setup_veth(bool busy_poll)
{
	SYS(fail,
	"ip link add %s numtxqueues 4 numrxqueues 4 type veth peer name %s numtxqueues 4 numrxqueues 4",
	VETH_RX, VETH_TX);
	SYS(fail, "sysctl -wq net.ipv6.conf.%s.disable_ipv6=1", VETH_RX);
	SYS(fail, "sysctl -wq net.ipv6.conf.%s.disable_ipv6=1", VETH_TX);

	if (busy_poll) {
		SYS(fail, "echo 2 > /sys/class/net/%s/napi_defer_hard_irqs", VETH_RX);
		SYS(fail, "echo 200000 > /sys/class/net/%s/gro_flush_timeout", VETH_RX);
		SYS(fail, "echo 2 > /sys/class/net/%s/napi_defer_hard_irqs", VETH_TX);
		SYS(fail, "echo 200000 > /sys/class/net/%s/gro_flush_timeout", VETH_TX);
	}

	SYS(fail, "ip link set %s mtu %d", VETH_RX, MTU);
	SYS(fail, "ip link set %s mtu %d", VETH_TX, MTU);
	SYS(fail, "ip link set %s up", VETH_RX);
	SYS(fail, "ip link set %s up", VETH_TX);
	SYS(fail, "ip link set lo up");

	return 0;

fail:
	return -1;
}

void delete_veth(void)
{
	SYS_NOFAIL("ip link del %s", VETH_RX);
	SYS_NOFAIL("ip link del %s", VETH_TX);
}

int configure_ifobj(struct ifobject *tx, struct ifobject *rx)
{
	rx->ifindex = if_nametoindex(VETH_RX);
	if (!ASSERT_OK_FD(rx->ifindex, "get RX ifindex"))
		return -1;

	tx->ifindex = if_nametoindex(VETH_TX);
	if (!ASSERT_OK_FD(tx->ifindex, "get TX ifindex"))
		return -1;

	return 0;
}

/* The TX child may not have reached listen() yet when the parent connects. */
static struct xsk_peer *connect_peer(void)
{
	struct xsk_peer *peer;
	int i;

	for (i = 0; i < PEER_CONNECT_RETRIES; i++) {
		peer = xsk_peer_open(PEER_HOST, PEER_PORT, false);
		if (peer || errno != ECONNREFUSED)
			return peer;
		usleep(100000);
	}
	return NULL;
}

/*
 * Each endpoint runs in its own process, like the two xskxceiver
 * invocations of test_xsk.sh: the child drives TX and listens on the
 * control port, the parent drives RX and connects to it. Only the parent's
 * verdict counts; the child's outcome reaches it over the channel.
 */
static int run_test_endpoints(struct test_spec *test, struct ifobject *ifobj_tx,
			      struct ifobject *ifobj_rx, enum test_mode mode,
			      const struct test_spec *test_to_run)
{
	struct ifobject *owned, *shadow;
	struct xsk_peer *peer;
	int ret, err;
	pid_t pid;

	pid = fork();
	if (!ASSERT_GE(pid, 0, "fork"))
		return TEST_FAILURE;
	if (!pid) {
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		owned = ifobj_tx;
		shadow = ifobj_rx;
		peer = xsk_peer_open(PEER_HOST, PEER_PORT, true);
	} else {
		owned = ifobj_rx;
		shadow = ifobj_tx;
		peer = connect_peer();
	}

	ret = TEST_FAILURE;
	if (!pid ? !peer : !ASSERT_OK_PTR(peer, "control connection"))
		goto out;

	err = init_iface(owned);
	err = err ?: xsk_load_xdp_programs(shadow);
	err = err ?: xsk_peer_exchange_caps(peer, owned, shadow);
	if (err)
		goto out;

	xsk_set_endpoint(peer, owned);
	test_init(test, ifobj_tx, ifobj_rx, mode, test_to_run);
	xsk_peer_test_begin(peer, 0);
	ret = test->test_func(test);
	ret = xsk_peer_test_result(peer, ret);
	pkt_stream_restore_default(test);

	if (ifobj_has_cap(owned, XSK_CAP_HW_RING))
		hw_ring_size_reset(owned);
	xsk_set_endpoint(NULL, NULL);
out:
	xsk_peer_close(peer);
	if (!pid)
		exit(ret == TEST_FAILURE);
	if (waitpid(pid, &err, 0) < 0 || !WIFEXITED(err) || WEXITSTATUS(err))
		ret = TEST_FAILURE;
	return ret;
}

static void test_xsk(const struct test_spec *test_to_run, enum test_mode mode)
{
	u32 max_frags, umem_tailroom, cache_line_size;
	struct ifobject *ifobj_tx, *ifobj_rx;
	struct test_spec test;
	int ret;

	ifobj_tx = ifobject_create();
	if (!ASSERT_OK_PTR(ifobj_tx, "create ifobj_tx"))
		return;

	ifobj_rx = ifobject_create();
	if (!ASSERT_OK_PTR(ifobj_rx, "create ifobj_rx"))
		goto delete_tx;

	if (!ASSERT_OK(configure_ifobj(ifobj_tx, ifobj_rx), "conigure ifobj"))
		goto delete_rx;

	ret = get_hw_ring_size(ifobj_tx->ifname, &ifobj_tx->ring);
	if (!ret) {
		ifobj_tx->caps.flags |= XSK_CAP_HW_RING;
		ifobj_tx->caps.tx_max_pending = ifobj_tx->ring.tx_max_pending;
		ifobj_tx->set_ring.default_tx = ifobj_tx->ring.tx_pending;
		ifobj_tx->set_ring.default_rx = ifobj_tx->ring.rx_pending;
	}

	cache_line_size = read_procfs_val(SMP_CACHE_BYTES_PATH);
	if (!cache_line_size)
		cache_line_size = 64;

	max_frags = read_procfs_val(MAX_SKB_FRAGS_PATH);
	if (!max_frags)
		max_frags = 17;

	ifobj_tx->caps.max_skb_frags = max_frags;
	ifobj_rx->caps.max_skb_frags = max_frags;

	/* 48 bytes is a part of skb_shared_info w/o frags array;
	 * 16 bytes is sizeof(skb_frag_t)
	 */
	umem_tailroom = ALIGN(48 + (max_frags * 16), cache_line_size);
	ifobj_tx->caps.umem_tailroom = umem_tailroom;
	ifobj_rx->caps.umem_tailroom = umem_tailroom;

	test.tx_pkt_stream_default = pkt_stream_generate(DEFAULT_PKT_CNT, MIN_PKT_SIZE);
	if (!ASSERT_OK_PTR(test.tx_pkt_stream_default, "TX pkt generation"))
		goto delete_rx;
	test.rx_pkt_stream_default = pkt_stream_generate(DEFAULT_PKT_CNT, MIN_PKT_SIZE);
	if (!ASSERT_OK_PTR(test.rx_pkt_stream_default, "RX pkt generation"))
		goto delete_rx;

	ret = run_test_endpoints(&test, ifobj_tx, ifobj_rx, mode, test_to_run);
	if (ret != TEST_SKIP)
		ASSERT_OK(ret, "Run test");

	pkt_stream_delete(test.tx_pkt_stream_default);
	pkt_stream_delete(test.rx_pkt_stream_default);
	xsk_xdp_progs__destroy(ifobj_tx->xdp_progs);
	xsk_xdp_progs__destroy(ifobj_rx->xdp_progs);

delete_rx:
	ifobject_delete(ifobj_rx);
delete_tx:
	ifobject_delete(ifobj_tx);
}

void test_ns_xsk_skb(void)
{
	int i;

	if (!ASSERT_OK(setup_veth(false), "setup veth"))
		return;

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		if (test__start_subtest(tests[i].name))
			test_xsk(&tests[i], TEST_MODE_SKB);
	}

	delete_veth();
}

void test_ns_xsk_drv(void)
{
	int i;

	if (!ASSERT_OK(setup_veth(false), "setup veth"))
		return;

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		if (test__start_subtest(tests[i].name))
			test_xsk(&tests[i], TEST_MODE_DRV);
	}

	delete_veth();
}
