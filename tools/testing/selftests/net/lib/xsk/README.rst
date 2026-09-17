.. SPDX-License-Identifier: GPL-2.0

==========
xskxceiver
==========

xskxceiver is the AF_XDP test suite shared by selftests/bpf (over a veth
pair, via ``test_xsk.sh`` and the ``ns_xsk_*`` prog_tests) and
drivers/net/hw (against a NIC, via ``xsk.py``). The engine in ``test_xsk.c`` runs a matrix of tests for every
supported mode (SKB, DRV, zero-copy) between a TX and an RX interface.

Modes of operation
==================

Every run consists of two processes, each owning one interface and one
AF_XDP socket set: a TX endpoint and an RX endpoint. They keep in step
over a control connection (``xsk_peer.c``) and both run the same test
matrix in the same order. The engine in ``test_xsk.c`` only ever drives
the interface its process owns; the other interface is a *shadow*
ifobject that carries the peer's capabilities.

Each process is started with its interface, its role and the control
address; the TX endpoint listens on it, the RX endpoint connects to it::

  # traffic generator
  xskxceiver -i eth0 --endpoint tx --peer 192.0.2.1 --peer-port 44661
  # device under test
  xskxceiver -i eth1 --endpoint rx --peer 192.0.2.1 --peer-port 44661

The endpoints may run on one host or two. The control connection must
not use the interface under test: its XDP program would consume the
control traffic. Both endpoints print the same TAP verdicts.

``test_xsk.sh`` runs the pair over a veth pair on one host with the
control connection on ``127.0.0.1:35989`` (``-p PORT`` changes the port),
TX in the background and RX in the foreground. The ``ns_xsk_*``
prog_tests do the same inside ``test_progs``: a forked child is the TX
endpoint, the parent the RX one, connected over loopback in the test
netns. Frames between veth
interfaces carry synthetic MACs the XDP programs rely on; a NIC only
accepts its own address, so unless the driver is ``veth`` the real MACs
exchanged at start-up are used and the XDP_SHARED_UMEM test is skipped.

Both endpoints on one interface is not supported: two AF_XDP sockets on
the same queue would have to share one UMEM across the processes, and a
netdev holds only one XDP program.

Running against hardware
========================

Quick start
-----------

Two hosts with the interfaces under test cabled back to back, and a
separate management network for ssh and the control connection. Kernels
with ``CONFIG_XDP_SOCKETS`` (see ``bpf/config``) and BTF on the build
host (``/sys/kernel/btf/vmlinux``; the BPF skeleton needs it, otherwise
xskxceiver is skipped with a warning).

1. Build the hw selftests on the local host (the DUT)::

     make -C tools/testing/selftests TARGETS=drivers/net/hw

   ``drivers/net/hw/xskxceiver`` is linked statically when the static
   libelf/zlib/zstd archives are installed, so the copy runs on a remote
   host with an older libc (see ``XSK_STATIC``).

2. Describe the setup in ``drivers/net/hw/net.config``::

     NETIF=eth0
     LOCAL_V4=192.168.100.1
     REMOTE_V4=192.168.100.2
     REMOTE_TYPE=ssh
     REMOTE_ARGS=user@remote.example.com
     # only when the ssh account is not root:
     XSK_REMOTE_SUDO=1
     XSK_REMOTE_BIN=/home/user/xskxceiver

   ``LOCAL_V4``/``REMOTE_V4`` (or the ``_V6`` pair) are required by the
   framework but xsk.py does not use them for traffic: the control
   connection goes to the ssh host, or to ``XSK_SYNC_ADDR``.

3. Prepare the remote host: passwordless ssh for the account, ``ethtool``
   and ``sysctl`` in the path, and, for a non-root account, this rule in
   ``/etc/sudoers.d/xskxceiver`` (matching ``XSK_REMOTE_BIN``)::

     user ALL=(root) NOPASSWD: /usr/sbin/ethtool, /usr/sbin/sysctl, /home/user/xskxceiver

   Nothing else is installed there; xsk.py copies the binary itself.

4. Run as root on the DUT::

     cd tools/testing/selftests/drivers/net/hw && sudo ./xsk.py

   The TAP verdicts of the RX endpoint are printed as they arrive; the
   remote TX output is shown prefixed with ``remote|``.

How it works
------------

``drivers/net/hw/xsk.py`` launches the split endpoints for the drv-net
framework: it copies xskxceiver to the remote host, starts the TX endpoint
there, runs the RX endpoint on the DUT and streams its TAP output.
Before the run it reserves hugepages on both hosts (needed by the
unaligned-mode tests) and reduces both interfaces to a single channel so
AF_XDP queue 0 receives all traffic; both settings are restored afterwards.

Environment
-----------

Besides the usual drv-net variables (``NETIF``, ``REMOTE_TYPE``,
``REMOTE_ARGS``) the following are recognized:

XSK_SYNC_ADDR
  Address the RX endpoint connects to. Defaults to the host part of the
  ssh destination in ``REMOTE_ARGS``; set it when that is an ssh alias.

XSK_REMOTE_BIN
  Fixed path on the remote host the binary is copied to. Without it the
  framework's ``deploy()`` picks a temporary name.

XSK_REMOTE_SUDO
  Set to ``1`` when the ssh account is not root. The remote endpoint and
  the remote ``ethtool``/``sysctl`` calls are prefixed with ``sudo -n``.
  The account needs a NOPASSWD rule for all three, e.g. in
  ``/etc/sudoers.d/xskxceiver`` (files there are parsed last, so a later
  ``%sudo ALL=(ALL:ALL) ALL`` rule does not override it)::

    user ALL=(root) NOPASSWD: /usr/sbin/ethtool, /usr/sbin/sysctl, /path/to/xskxceiver

  xsk.py probes the rule before touching anything and fails with the
  missing line otherwise.

XSK_STATIC
  Build-time knob. The hw Makefile links xskxceiver statically when the
  static libelf/zlib/zstd archives are available, so the copy works on a
  remote host with an older libc; ``XSK_STATIC=0`` or ``1`` overrides the
  probe.

Control protocol
================

Transport
---------

One TCP connection per run. The TX endpoint listens, the RX endpoint
connects; there are no retries or timeouts in C, the launcher starts the
listener first and bounds the run time. Records are fixed size::

  struct xsk_peer_msg {
          u32 magic;      /* 0x58534b50 "XSKP" */
          u32 type;       /* enum xsk_peer_msg_type */
          union {
                  struct xsk_caps caps;
                  u32 arg[sizeof(struct xsk_caps) / 4];
          };
  };

Every u32 is sent in network byte order; the MAC inside ``xsk_caps`` is
byte-swapped symmetrically on both ends and so survives. Records are read
and written whole with ``ksft_read_full()``/``ksft_write_full()``. A short
transfer, bad magic or unknown type marks the peer *broken* for good; the
engine turns that into TEST_FAILURE and xskxceiver ends the suite.

Step messages carry ``arg[0]`` = test id, ``arg[1]`` = step, ``arg[2]`` =
payload. The test id is not negotiated: both processes iterate the same
tables with the same capability set and count identically, and the
``(test_id, step)`` check on every received message asserts that.

Messages
--------

CAPS (once per run)
  Both sides send their ``struct xsk_caps`` (mode and feature flags,
  ring/frag limits, MAC), then each reads the other's. The received copy is
  stored in the local *shadow* ifobject standing in for the remote
  interface, the mode flags are ANDed so only mutually supported modes run,
  and the engine's ``ifobj_has_cap()`` checks on the shadow yield the same
  skips the remote side computes locally. ``XSK_CAP_MAC_FILTER`` selects
  the exchanged MACs over the synthetic ones.

START (per step, ``step_start``)
  Sent after RX preparation with its status; both sides wait for the
  other's START, so no packets move before both are ready. A START carrying
  TEST_FAILURE cancels the step on the receiver.

PROGRESS (RX to TX, ``rx_progress``)
  Flow control. ``pkts_in_flight`` is TX's sent count minus RX's consumed
  count and TX will not run more than a window ahead. RX reports how many
  packets it consumed; TX applies it when ``tx_sync`` drains the socket. RX also drains after sending, which is how
  it notices a DONE or RESULT from a failing TX instead of waiting for its
  own receive timeout.

DONE (per step, ``step_done``)
  Sent after validation with the step verdict; both sides wait for the
  other's DONE before tearing down sockets, so neither destroys its socket
  while the peer is still using it.

RESULT (once per test)
  Sent after ``test_func()`` returns with the local verdict, then the
  peer's RESULT is awaited. Step messages of the same test that the peer
  sent before seeing our RESULT are ignored at this point. The verdicts are
  merged identically on both sides: FAILURE if either failed, else SKIP if
  either skipped, else PASS. RESULT is the only message accepted in the
  middle of a START/DONE wait; it ends the test on the sender side, so the
  waiter is cancelled instead of blocking forever.

Receiving
---------

``peer_wait()`` blocks until a flag (START/DONE/RESULT seen) is set or the
peer's RESULT arrives. It is used where the engine has nothing else to do.
``peer_drain()`` polls with a zero timeout and consumes whatever is
already queued; it is used from the data path so a single-threaded
endpoint keeps moving frames while picking up PROGRESS, DONE and RESULT.

Lifecycle of one test
---------------------

::

  RX endpoint                              TX endpoint
  test_begin(id)                           test_begin(id)
    prepare rx (sockets, fq, xdp)
    START(id,1,PASS)  ------------------>  wait START
    wait START        <------------------  START(id,1,PASS)
    receive loop:                          send loop:
      PROGRESS(id,1,n) ----------------->    tx_sync(): pkts_in_flight -= n
      drain
    validate rx                            validate tx
    DONE(id,1,PASS)   ------------------>  wait DONE
    wait DONE         <------------------  DONE(id,1,PASS)
    [further steps repeat START..DONE]
  RESULT(id,res)      ------------------>  wait RESULT
  wait RESULT         <------------------  RESULT(id,res)
  report merged verdict                    report merged verdict

A failure makes the failing side's next START or DONE carry TEST_FAILURE,
or it goes straight to RESULT. The other side sees it in its next wait or
drain, gets -ECANCELED from ``xsk_peer_*()``, unwinds as TEST_FAILURE and
the RESULT exchange makes both print the same verdict. If a process dies, the
peer's read fails, the connection is marked broken and the suite ends.
