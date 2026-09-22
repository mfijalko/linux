.. SPDX-License-Identifier: GPL-2.0

==========
xskxceiver
==========

The AF_XDP engine in ``net/lib/xsk`` is built by ``selftests/net``. The
generic ``net/test_xsk.sh`` test runs SKB and DRV modes over veth.

Generic veth test
=================

Two ``xskxceiver`` processes own the TX and RX veth interfaces. The TX
process listens on a TCP control port and the RX process connects. The
shell wrapper starts a fresh process pair for each mode and case. A small
control channel reports readiness, packet progress, and aborts. For example::

  cd tools/testing/selftests/net
  sudo ./test_xsk.sh

The shell wrapper creates the veth pair. ``-m skb|drv`` selects a mode,
``-t N`` selects the test number shown by ``xskxceiver -l``, and ``-p N``
selects a fixed control port instead of the default random port. The generic
frame format is unchanged.

Hardware zero-copy test
=======================

``xskxceiver --list-hw`` describes the hardware cases and whether the DUT is
the RX or TX endpoint. ``drivers/net/hw/xsk.py`` discovers that matrix and owns
the TAP results.
It starts one ``xskxceiver`` process on each host for each case. The DUT
runs in ZC mode; the remote runs in SKB mode and does not need zero-copy.
RX and TX cases are named separately in TAP. The hardware runner checks
the DUT's advertised ZC capability and verifies
``XDP_OPTIONS_ZEROCOPY`` on the bound AF_XDP socket. A device without
advertised ZC skips; an advertised device that fails to bind or run the
baseline case fails.

The remote XSK endpoint listens on a TCP control port and the DUT connects.
The small per-case channel reports readiness, packet progress, and aborts.
It has no capabilities or verdict handshake. ``xsk.py`` chooses the case,
sets the timeout, and reports the result from both process exit codes.

Both XSK endpoints generate and validate raw IPv4/UDP frames with one fixed
UDP port, so ntuple rules can steer the test flow. They do not use AF_INET
sockets. On DUT RX cases ``xsk.py`` reserves the last RX queue outside RSS
and steers test traffic to it. On DUT TX cases it steers traffic to queue 0
on the remote receiver. The XDP programs pass unrelated traffic through.
The runner restores the original RSS table, ntuple setting, and huge-page
count on the affected hosts when the selected run ends. Each endpoint
restores its MTU and any changed ring sizes before it exits. The runner does
not change channel counts. A one-channel DUT skips RX cases because it has
no queue to reserve outside RSS.

Setup
-----

Build the hardware selftests on the DUT::

  make -C tools/testing/selftests/drivers/net/hw xskxceiver

Configure the usual driver test variables in
``tools/testing/selftests/drivers/net/hw/net.config`` or the environment::

  NETIF=eth0
  LOCAL_V4=192.0.2.1
  REMOTE_V4=192.0.2.2
  REMOTE_TYPE=ssh
  REMOTE_ARGS=user@remote.example.com
  XSK_REMOTE_BIN=/path/to/remote/xskxceiver
  XSK_REMOTE_SUDO=1

Run as root on the DUT::

  cd tools/testing/selftests/drivers/net/hw
  sudo ./xsk.py -t test_xsk.rx_send_receive

``./xsk.py -l`` lists the case names. Omit ``-t`` to run the full hardware
matrix. Set the variables in ``net.config`` when using ``sudo`` so they are
available to the test process.

The remote host needs AF_XDP in SKB mode and BPF. ``xsk.py`` copies the
DUT's ``xskxceiver`` binary to the remote, so both hosts must have
compatible architectures. ``XSK_REMOTE_BIN`` chooses a fixed destination
path for that copy. Set ``XSK_REMOTE_DEPLOY=0`` to use a binary built on
the remote instead. ``XSK_REMOTE_SUDO=1`` runs the remote endpoint and
setup commands through passwordless ``sudo -n``; omit it when SSH logs in
as root. The DUT needs ntuple and RSS support for RX cases, while the
remote needs ntuple support for DUT TX cases. The test uses
``NetDrvEpEnv`` and the networking selftest Python helpers for setup and
rollback.

``XSK_SYNC_ADDR`` overrides the SSH host name used by the DUT to connect
to the remote control listener. ``XSK_UDP_PORT`` changes the fixed test
UDP port (default 42567). The remote listens on all addresses for control.

The hardware list covers baseline traffic, 2K frames, poll, headroom,
invalid TX descriptors, TX invalid-descriptor statistics, metadata, 9K,
and unaligned traffic. For ``TOO_MANY_FRAGS``, the runner reads the DUT's
``xdp-zc-max-segs`` value and passes it to both endpoint processes so the
SKB peer validates the same packet stream without a capabilities exchange.
