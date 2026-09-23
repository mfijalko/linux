// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2020 Intel Corporation. */

/*
 * AF_XDP per-endpoint test engine. The generic veth suite uses two of these
 * processes; the hardware suite uses one on the DUT and a Python UDP peer.
 * See net/lib/xsk/README.rst for topology and test setup.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/if_link.h>
#include <linux/if_ether.h>
#include <linux/mman.h>
#include <linux/netdev.h>
#include <linux/ethtool.h>
#include <linux/align.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <libgen.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/types.h>

#include "test_xsk.h"
#include "xsk_peer.h"
#include "xsk_xdp_progs.skel.h"
#include "xsk.h"
#include "xskxceiver.h"
#include <bpf/bpf.h>
#include <linux/filter.h>
#include "../../../kselftest.h"
#include "xsk_xdp_common.h"

static enum test_mode opt_mode = TEST_MODE_ALL;
static u32 opt_run_test = RUN_ALL_TESTS;
static enum xsk_endpoint_role opt_endpoint_role = XSK_ENDPOINT_NONE;
static bool opt_probe_zc;
static bool opt_hw;
static bool opt_listen;
static const char *opt_peer_host;
static const char *opt_peer_port;
static const char *opt_ifname;
static const char *opt_udp_src;
static const char *opt_udp_dst;
static const char *opt_peer_mac;
static u16 opt_udp_port;
static u32 opt_queue;
static u32 opt_max_frags;

enum {
	OPT_HW = 256,
	OPT_LIST_HW,
	OPT_LISTEN,
	OPT_UDP_SRC,
	OPT_UDP_DST,
	OPT_UDP_PORT,
	OPT_PEER_MAC,
	OPT_QUEUE,
	OPT_MAX_FRAGS,
};

static void __exit_with_error(int error, const char *file, const char *func, int line)
{
	ksft_test_result_fail("[%s:%s:%i]: ERROR: %d/\"%s\"\n", file, func, line,
			      error, strerror(error));
	ksft_exit_xfail();
}

#define exit_with_error(error) __exit_with_error(error, __FILE__, __func__, __LINE__)

static bool ifobj_zc_avail(struct ifobject *ifobject)
{
	size_t umem_sz = DEFAULT_UMEM_BUFFERS * XSK_UMEM__DEFAULT_FRAME_SIZE;
	int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
	struct xsk_socket_info *xsk;
	struct xsk_umem_info *umem;
	struct xdp_options options = {};
	socklen_t options_len = sizeof(options);
	bool zc_avail = false;
	void *bufs;
	int ret;

	bufs = mmap(NULL, umem_sz, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
	if (bufs == MAP_FAILED)
		exit_with_error(errno);

	umem = calloc(1, sizeof(struct xsk_umem_info));
	if (!umem) {
		munmap(bufs, umem_sz);
		exit_with_error(ENOMEM);
	}
	umem->frame_size = XSK_UMEM__DEFAULT_FRAME_SIZE;
	ret = xsk_configure_umem(ifobject, umem, bufs, umem_sz);
	if (ret)
		exit_with_error(-ret);

	xsk = calloc(1, sizeof(struct xsk_socket_info));
	if (!xsk)
		goto out;
	ifobject->bind_flags = XDP_USE_NEED_WAKEUP | XDP_ZEROCOPY;
	ifobject->rx_on = true;
	xsk->rxqsize = XSK_RING_CONS__DEFAULT_NUM_DESCS;
	ret = xsk_configure_socket(xsk, umem, ifobject, false);
	if (!ret && !getsockopt(xsk_socket__fd(xsk->xsk), SOL_XDP, XDP_OPTIONS,
				&options, &options_len))
		zc_avail = options.flags & XDP_OPTIONS_ZEROCOPY;

	xsk_socket__delete(xsk->xsk);
	free(xsk);
out:
	munmap(umem->buffer, umem_sz);
	xsk_umem__delete(umem->umem);
	free(umem);
	return zc_avail;
}

static struct option long_options[] = {
	{"interface", required_argument, 0, 'i'},
	{"busy-poll", no_argument, 0, 'b'},
	{"verbose", no_argument, 0, 'v'},
	{"mode", required_argument, 0, 'm'},
	{"list", no_argument, 0, 'l'},
	{"test", required_argument, 0, 't'},
	{"endpoint", required_argument, 0, 'e'},
	{"peer", required_argument, 0, 'p'},
	{"peer-port", required_argument, 0, 'P'},
	{"probe-zc", no_argument, 0, 'Z'},
	{"hw", no_argument, 0, OPT_HW},
	{"list-hw", no_argument, 0, OPT_LIST_HW},
	{"listen", no_argument, 0, OPT_LISTEN},
	{"udp-src", required_argument, 0, OPT_UDP_SRC},
	{"udp-dst", required_argument, 0, OPT_UDP_DST},
	{"udp-port", required_argument, 0, OPT_UDP_PORT},
	{"peer-mac", required_argument, 0, OPT_PEER_MAC},
	{"queue", required_argument, 0, OPT_QUEUE},
	{"max-frags", required_argument, 0, OPT_MAX_FRAGS},
	{"help", no_argument, 0, 'h'},
	{0, 0, 0, 0}
};

static void print_usage(char **argv)
{
	const char *str =
		"  Usage: xskxceiver -i IFACE -e rx|tx -p HOST -P PORT [OPTIONS]\n"
		"  Options:\n"
		"  -i, --interface      Interface this endpoint drives\n"
		"  -v, --verbose        Verbose output\n"
		"  -b, --busy-poll      Enable busy poll\n"
		"  -m, --mode           Run only mode skb, drv, or zc\n"
		"  -l, --list           List all available tests\n"
		"      --list-hw        List hardware cases and DUT directions\n"
		"  -t, --test           Run a specific test. Enter number from -l option.\n"
		"  -e, --endpoint       Role of this endpoint: rx or tx\n"
		"  -p, --peer           Control host: IPv4/IPv6 address or hostname\n"
		"  -P, --peer-port      Control TCP port (1-65535)\n"
		"      --listen         Listen instead of connecting (hardware peer)\n"
		"      --probe-zc       Check advertised and effective zero-copy on -i IFACE\n"
		"      --hw             One physical-link case, controlled by xsk.py\n"
		"      --udp-src IP     IPv4 source address for test packets\n"
		"      --udp-dst IP     IPv4 destination address for test packets\n"
		"      --udp-port PORT  UDP source and destination port\n"
		"      --peer-mac MAC   Remote interface MAC address\n"
		"      --queue N        AF_XDP queue to bind (default 0)\n"
		"      --max-frags N    Fragment limit selected by the hardware runner\n"
		"  -h, --help           Display this help and exit\n";

	ksft_print_msg(str, basename(argv[0]));
	ksft_exit_xfail();
}

static void bind_iface(struct ifobject *ifobj, const char *ifname, char **argv)
{
	size_t len = strlen(ifname);

	if (!len || len >= sizeof(ifobj->ifname))
		print_usage(argv);

	memcpy(ifobj->ifname, ifname, len + 1);
	ifobj->ifindex = if_nametoindex(ifobj->ifname);
	if (!ifobj->ifindex) {
		ksft_print_msg("Error: cannot resolve interface %s\n", ifobj->ifname);
		ksft_exit_fail();
	}

	if (init_iface(ifobj)) {
		ksft_print_msg("Error: cannot initialize interface %s\n", ifobj->ifname);
		ksft_exit_fail();
	}
	if (xsk_get_mtu(ifobj->ifname, &ifobj->mtu)) {
		ksft_print_msg("Error: cannot read MTU of interface %s\n", ifobj->ifname);
		ksft_exit_fail();
	}
}

static void print_tests(void)
{
	u32 i;

	printf("Tests:\n");
	for (i = 0; i < ARRAY_SIZE(tests); i++)
		printf("%u: %s\n", i, tests[i].name);
	for (i = ARRAY_SIZE(tests); i < ARRAY_SIZE(tests) + ARRAY_SIZE(ci_skip_tests); i++)
		printf("%u: %s\n", i, ci_skip_tests[i - ARRAY_SIZE(tests)].name);
}

static void print_hw_test(u32 id, const struct test_spec *test)
{
	if (test->hw_directions & XSK_HW_RX)
		printf("%u\t%s\trx\n", id, test->name);
	if (test->hw_directions & XSK_HW_TX)
		printf("%u\t%s\ttx\n", id, test->name);
}

static void print_hw_tests(void)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(tests); i++)
		print_hw_test(i, &tests[i]);
	for (i = 0; i < ARRAY_SIZE(ci_skip_tests); i++)
		print_hw_test(ARRAY_SIZE(tests) + i, &ci_skip_tests[i]);
}

static void parse_command_line(struct ifobject *ifobj_tx, struct ifobject *ifobj_rx, int argc,
			       char **argv)
{
	int option_index, c;
	unsigned long val;
	char *end;

	opterr = 0;

	for (;;) {
		c = getopt_long(argc, argv, "i:vbm:lt:e:p:P:Z", long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'i':
			opt_ifname = optarg;
			break;
		case 'v':
			opt_verbose = true;
			break;
		case 'b':
			ifobj_tx->busy_poll = true;
			ifobj_rx->busy_poll = true;
			break;
		case 'm':
			if (!strncmp("skb", optarg, strlen(optarg)))
				opt_mode = TEST_MODE_SKB;
			else if (!strncmp("drv", optarg, strlen(optarg)))
				opt_mode = TEST_MODE_DRV;
			else if (!strncmp("zc", optarg, strlen(optarg)))
				opt_mode = TEST_MODE_ZC;
			else
				print_usage(argv);
			break;
		case 'l':
			print_tests();
			ksft_exit_pass();
			break;
		case 't':
			errno = 0;
			opt_run_test = strtol(optarg, NULL, 0);
			if (errno)
				print_usage(argv);
			break;
		case 'e':
			if (!strcmp(optarg, "rx"))
				opt_endpoint_role = XSK_ENDPOINT_RX;
			else if (!strcmp(optarg, "tx"))
				opt_endpoint_role = XSK_ENDPOINT_TX;
			else
				print_usage(argv);
			break;
		case 'p':
			opt_peer_host = optarg;
			break;
		case 'P':
			errno = 0;
			val = strtoul(optarg, &end, 10);
			if (optarg[0] < '0' || optarg[0] > '9' || errno ||
			    *end || !val || val > UINT16_MAX)
				print_usage(argv);
			opt_peer_port = optarg;
			break;
		case 'Z':
			opt_probe_zc = true;
			break;
		case OPT_HW:
			opt_hw = true;
			break;
		case OPT_LIST_HW:
			print_hw_tests();
			exit(KSFT_PASS);
			break;
		case OPT_LISTEN:
			opt_listen = true;
			break;
		case OPT_UDP_SRC:
			opt_udp_src = optarg;
			break;
		case OPT_UDP_DST:
			opt_udp_dst = optarg;
			break;
		case OPT_UDP_PORT:
			errno = 0;
			val = strtoul(optarg, &end, 10);
			if (errno || *end || !val || val > UINT16_MAX)
				print_usage(argv);
			opt_udp_port = val;
			break;
		case OPT_PEER_MAC:
			opt_peer_mac = optarg;
			break;
		case OPT_QUEUE:
			errno = 0;
			val = strtoul(optarg, &end, 10);
			if (errno || *end || val > UINT32_MAX)
				print_usage(argv);
			opt_queue = val;
			break;
		case OPT_MAX_FRAGS:
			errno = 0;
			val = strtoul(optarg, &end, 10);
			if (errno || *end || !val || val > UINT16_MAX)
				print_usage(argv);
			opt_max_frags = val;
			break;
		case 'h':
		default:
			print_usage(argv);
		}
	}

	if (!opt_ifname || (!opt_probe_zc &&
	    (opt_endpoint_role == XSK_ENDPOINT_NONE || !opt_peer_host ||
	     !*opt_peer_host || !opt_peer_port ||
	     opt_run_test == RUN_ALL_TESTS || opt_mode == TEST_MODE_ALL)))
		print_usage(argv);
	if (opt_hw && (opt_run_test == RUN_ALL_TESTS ||
		       !opt_udp_src || !opt_udp_dst || !opt_udp_port || !opt_peer_mac))
		print_usage(argv);
	if (opt_listen && !opt_hw)
		print_usage(argv);
	if (opt_max_frags && !opt_hw)
		print_usage(argv);
}

static int parse_mac(const char *text, u8 mac[ETH_ALEN])
{
	unsigned int byte[ETH_ALEN];
	char tail;
	int i;

	if (sscanf(text, "%x:%x:%x:%x:%x:%x%c", &byte[0], &byte[1],
		   &byte[2], &byte[3], &byte[4], &byte[5], &tail) != ETH_ALEN)
		return -EINVAL;
	for (i = 0; i < ETH_ALEN; i++) {
		if (byte[i] > 0xff)
			return -EINVAL;
		mac[i] = byte[i];
	}
	return 0;
}

static void xsk_unload_xdp_programs(struct ifobject *ifobj)
{
	xsk_xdp_progs__destroy(ifobj->xdp_progs);
}

static void report_pkt_test(struct test_spec *test, int ret)
{
	switch (ret) {
	case TEST_PASS:
		ksft_test_result_pass("PASS: %s %s%s\n", mode_string(test), busy_poll_string(test),
				      test->name);
		break;
	case TEST_SKIP:
		ksft_test_result_skip("SKIP: %s %s%s\n", mode_string(test), busy_poll_string(test),
				      test->name);
		break;
	case TEST_FAILURE:
		ksft_test_result_fail("FAIL: %s %s%s\n", mode_string(test), busy_poll_string(test),
				      test->name);
		break;
	default:
		ksft_test_result_fail("FAIL: %s %s%s -- Unexpected returned value (%d)\n",
				      mode_string(test), busy_poll_string(test), test->name, ret);
	}
}

static int run_pkt_test(struct test_spec *test, struct xsk_peer *peer, u32 sync_id)
{
	int ret;

	xsk_peer_case_begin(peer, sync_id);
	ret = test->test_func(test);
	if (ret != TEST_PASS && ret != TEST_SKIP)
		ret = TEST_FAILURE;
	if (!opt_hw)
		report_pkt_test(test, ret);
	pkt_stream_restore_default(test);
	return ret;
}

static bool is_xdp_supported(int ifindex)
{
	int flags = XDP_FLAGS_DRV_MODE;

	LIBBPF_OPTS(bpf_link_create_opts, opts, .flags = flags);
	struct bpf_insn insns[2] = {
		BPF_MOV64_IMM(BPF_REG_0, XDP_PASS),
		BPF_EXIT_INSN()
	};
	int prog_fd, insn_cnt = ARRAY_SIZE(insns);
	int err;

	prog_fd = bpf_prog_load(BPF_PROG_TYPE_XDP, NULL, "GPL", insns, insn_cnt, NULL);
	if (prog_fd < 0)
		return false;

	err = bpf_xdp_attach(ifindex, prog_fd, flags, NULL);
	if (err) {
		close(prog_fd);
		return false;
	}

	bpf_xdp_detach(ifindex, flags, NULL);
	close(prog_fd);

	return true;
}

/* Probe the mode-gating capabilities: DRV, ZC and HW ring resizing. */
static u32 detect_ifobj_caps(struct ifobject *ifobj)
{
	char driver[32];

	/* veth delivers any destination MAC; a NIC only its own. */
	if (xsk_get_driver(ifobj->ifname, driver, sizeof(driver)) ||
	    strcmp(driver, "veth"))
		ifobj->caps.flags |= XSK_CAP_MAC_FILTER;

	if (opt_hw && opt_mode == TEST_MODE_ZC &&
	    ifobj_has_cap(ifobj, XSK_CAP_ZC_ADVERTISED)) {
		/* xsk.py performs the effective ZC probe once per suite. */
		ifobj->caps.flags |= XSK_CAP_DRV | XSK_CAP_ZC;
	} else if (!opt_hw && is_xdp_supported(ifobj->ifindex)) {
		ifobj->caps.flags |= XSK_CAP_DRV;
		if ((opt_mode == TEST_MODE_ZC || opt_probe_zc) &&
		    ifobj_zc_avail(ifobj))
			ifobj->caps.flags |= XSK_CAP_ZC;
	}

	if (!xsk_get_hw_ring_size(ifobj->ifname, &ifobj->ring)) {
		ifobj->caps.flags |= XSK_CAP_HW_RING;
		ifobj->caps.tx_max_pending = ifobj->ring.tx_max_pending;
		ifobj->set_ring.default_tx = ifobj->ring.tx_pending;
		ifobj->set_ring.default_rx = ifobj->ring.rx_pending;
	}

	return ifobj->caps.flags;
}

static const struct test_spec *test_by_id(u32 id)
{
	if (id < ARRAY_SIZE(tests))
		return &tests[id];
	return &ci_skip_tests[id - ARRAY_SIZE(tests)];
}

static bool mode_supported(enum test_mode mode, u32 caps)
{
	if (mode == TEST_MODE_SKB)
		return true;
	if (mode == TEST_MODE_DRV)
		return caps & XSK_CAP_DRV;
	return caps & XSK_CAP_ZC;
}

static u32 count_supported_modes(u32 caps)
{
	u32 i, modes = 0;

	for (i = TEST_MODE_SKB; i <= TEST_MODE_ZC; i++)
		if ((opt_mode == TEST_MODE_ALL || opt_mode == i) && mode_supported(i, caps))
			modes++;

	return modes;
}

static void cleanup_iface(struct ifobject *ifobj)
{
	/* A peer shadow has a skeleton but no bound interface. */
	if (ifobj->ifindex) {
		if (ifobj_has_cap(ifobj, XSK_CAP_HW_RING))
			hw_ring_size_reset(ifobj);
		if (ifobj->xdp_prog)
			xsk_detach_xdp_program(ifobj->ifindex,
					       ifobj->mode == TEST_MODE_SKB ?
					       XDP_FLAGS_SKB_MODE : XDP_FLAGS_DRV_MODE);
	}
	/* Skip the libbpf error pointer left by a failed open_and_load(). */
	if (ifobj->xdp_progs && !libbpf_get_error(ifobj->xdp_progs))
		xsk_unload_xdp_programs(ifobj);
}

static int run_test_suite(struct test_spec *test, struct ifobject *ifobj_tx,
			  struct ifobject *ifobj_rx, u32 caps, struct xsk_peer *peer)
{
	const size_t total_tests = ARRAY_SIZE(tests) + ARRAY_SIZE(ci_skip_tests);
	u32 i, j, nb_tests, modes, failed_tests = 0, sync_id = 0;
	bool skipped = false;
	int ret;

	nb_tests = opt_run_test == RUN_ALL_TESTS ? total_tests : 1;
	modes = count_supported_modes(caps);
	/* Line-buffer stdout so verdicts reach a capturing launcher live. */
	if (!opt_hw) {
		ksft_print_header();
		ksft_set_plan(modes * nb_tests);
	}

	for (i = TEST_MODE_SKB; i <= TEST_MODE_ZC; i++) {
		if (opt_mode != TEST_MODE_ALL && opt_mode != i)
			continue;
		if (!mode_supported(i, caps))
			continue;
		for (j = 0; j < total_tests; j++) {
			const struct test_spec *spec;

			if (opt_run_test != RUN_ALL_TESTS && opt_run_test != j)
				continue;
			spec = test_by_id(j);
			test_init(test, ifobj_tx, ifobj_rx, i, spec);
			ret = run_pkt_test(test, peer, sync_id++);
			if (ret == TEST_FAILURE)
				failed_tests++;
			else if (ret == TEST_SKIP)
				skipped = true;
			if (xsk_peer_broken(peer))
				return TEST_FAILURE;
			usleep(USLEEP_MAX);
		}
	}
	if (failed_tests)
		return TEST_FAILURE;
	return nb_tests == 1 && modes == 1 && skipped ? TEST_SKIP : TEST_PASS;
}

/* Connect the two endpoints without negotiating the remote NIC's capabilities. */
static int setup_peer(struct ifobject *owned, struct ifobject *shadow,
		      struct xsk_peer **peer)
{
	/* Unattached skeleton copy; the engine only attaches the owned side. */
	if (xsk_load_xdp_programs(shadow))
		return TEST_FAILURE;

	/* Generic TX listens; Python chooses the listener for hardware cases. */
	*peer = xsk_peer_open(opt_peer_host, opt_peer_port,
			      opt_hw ? opt_listen :
			      opt_endpoint_role == XSK_ENDPOINT_TX);
	if (!*peer) {
		ksft_print_msg("Failed to connect XSK peer: %s\n", strerror(errno));
		return TEST_FAILURE;
	}

	if (opt_hw) {
		shadow->caps = owned->caps;
		if (parse_mac(opt_peer_mac, shadow->caps.mac))
			return TEST_FAILURE;
	} else {
		/* Both generic endpoints are veth devices in the same test setup. */
		shadow->caps = owned->caps;
	}

	xsk_set_endpoint(*peer, owned, opt_hw);
	return TEST_PASS;
}

int main(int argc, char **argv)
{
	const size_t total_tests = ARRAY_SIZE(tests) + ARRAY_SIZE(ci_skip_tests);
	u32 cache_line_size, max_frags, umem_tailroom;
	struct ifobject *ifobj_tx, *ifobj_rx;
	struct ifobject *endpoint_ifobj;
	struct ifobject *shadow_ifobj;
	struct xsk_peer *peer = NULL;
	struct test_spec test = {};
	int initial_mtu = -1;
	int ret = TEST_FAILURE;
	u32 caps;

	/* Use libbpf 1.0 API mode */
	libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

	ifobj_tx = ifobject_create();
	if (!ifobj_tx)
		exit_with_error(ENOMEM);
	ifobj_rx = ifobject_create();
	if (!ifobj_rx)
		exit_with_error(ENOMEM);

	setlocale(LC_ALL, "");
	parse_command_line(ifobj_tx, ifobj_rx, argc, argv);

	cache_line_size = read_procfs_val(SMP_CACHE_BYTES_PATH);
	if (!cache_line_size) {
		ksft_print_msg("Can't get SMP_CACHE_BYTES from system, using default (64)\n");
		cache_line_size = 64;
	}

	max_frags = read_procfs_val(MAX_SKB_FRAGS_PATH);
	if (!max_frags) {
		ksft_print_msg("Can't get MAX_SKB_FRAGS from system, using default (17)\n");
		max_frags = 17;
	}
	ifobj_tx->caps.max_skb_frags = max_frags;
	ifobj_rx->caps.max_skb_frags = max_frags;

	/* 48 bytes is a part of skb_shared_info w/o frags array;
	 * 16 bytes is sizeof(skb_frag_t)
	 */
	umem_tailroom = ALIGN(48 + (max_frags * 16), cache_line_size);
	ifobj_tx->caps.umem_tailroom = umem_tailroom;
	ifobj_rx->caps.umem_tailroom = umem_tailroom;

	xsk_set_max_frags(opt_max_frags);
	if (opt_hw && xsk_set_udp_packet_format(opt_udp_src, opt_udp_dst,
					       opt_udp_port))
		print_usage(argv);

	if (opt_run_test != RUN_ALL_TESTS && opt_run_test >= total_tests) {
		ksft_print_msg("Error: test %u does not exist.\n", opt_run_test);
		ksft_exit_xfail();
	}

	if (opt_endpoint_role == XSK_ENDPOINT_TX) {
		endpoint_ifobj = ifobj_tx;
		shadow_ifobj = ifobj_rx;
	} else {
		endpoint_ifobj = ifobj_rx;
		shadow_ifobj = ifobj_tx;
	}
	bind_iface(endpoint_ifobj, opt_ifname, argv);
	initial_mtu = endpoint_ifobj->mtu;
	endpoint_ifobj->queue_id = opt_queue;
	if (opt_hw) {
		endpoint_ifobj->xdp_progs->bss->hw_mode = true;
		endpoint_ifobj->xdp_progs->bss->hw_queue = opt_queue;
		endpoint_ifobj->xdp_progs->bss->hw_port = opt_udp_port;
	}
	caps = detect_ifobj_caps(endpoint_ifobj);

	if (opt_probe_zc) {
		if (!ifobj_has_cap(endpoint_ifobj, XSK_CAP_ZC_ADVERTISED))
			ret = TEST_SKIP;
		else if (!(caps & XSK_CAP_ZC))
			ret = TEST_FAILURE;
		else
			ret = TEST_PASS;
		goto out;
	}

	test.tx_pkt_stream_default = pkt_stream_generate(DEFAULT_PKT_CNT, MIN_PKT_SIZE);
	test.rx_pkt_stream_default = pkt_stream_generate(DEFAULT_PKT_CNT, MIN_PKT_SIZE);
	if (!test.tx_pkt_stream_default || !test.rx_pkt_stream_default)
		goto out;

	if (setup_peer(endpoint_ifobj, shadow_ifobj, &peer))
		goto out;

	if (opt_mode != TEST_MODE_ALL && !count_supported_modes(caps)) {
		if (opt_mode == TEST_MODE_DRV)
			ksft_print_msg("XDP_DRV mode not supported.\n");
		else
			ksft_print_msg("zero-copy mode not supported.\n");
		ret = TEST_SKIP;
		goto out;
	}

	ret = run_test_suite(&test, ifobj_tx, ifobj_rx, caps, peer);

out:
	xsk_set_endpoint(NULL, NULL, false);
	cleanup_iface(ifobj_tx);
	cleanup_iface(ifobj_rx);
	if (initial_mtu >= 0 && endpoint_ifobj->mtu != initial_mtu) {
		int err = xsk_set_mtu(endpoint_ifobj->ifindex, initial_mtu);

		if (err) {
			ksft_print_msg("Error restoring MTU on %s: %d (%s)\n",
				       endpoint_ifobj->ifname, err, strerror(-err));
			ret = TEST_FAILURE;
		}
	}
	xsk_peer_close(peer);
	pkt_stream_delete(test.tx_pkt_stream_default);
	pkt_stream_delete(test.rx_pkt_stream_default);
	ifobject_delete(ifobj_tx);
	ifobject_delete(ifobj_rx);

	if (ret == TEST_SKIP && !opt_hw && ksft_test_num()) {
		ksft_print_cnts();
		return KSFT_SKIP;
	}
	if (ret == TEST_SKIP)
		ksft_exit_skip("zero-copy not advertised or test is unsupported\n");
	if (ret)
		ksft_exit_fail();
	ksft_exit_pass();
}
