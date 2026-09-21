#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import selectors
import shlex
import time
from pathlib import Path

from lib.py import bkg, cmd, defer, ksft_exit, ksft_pr, ksft_run, rand_port
from lib.py import wait_port_listen
from lib.py import KsftFailEx, KsftSkipEx
from lib.py import NetDrvEpEnv

SUITE_TIMEOUT = 900
# The remote endpoint probes its interface (XDP/zero-copy attach, ring
# query) before it listens; allow for slow NIC reconfiguration.
LISTEN_DEADLINE = 60
# 2 MiB pages for the unaligned-mode UMEMs of one endpoint (up to ~10 MiB
# with 9K frames) with headroom; both hosts must have them or those tests
# are skipped.
NR_HUGEPAGES = 32


def _management_addr(cfg):
    """Address the local RX connector uses to reach the remote TX listener."""
    addr = cfg.env.get("XSK_SYNC_ADDR")
    if addr:
        return addr
    if cfg.env.get("REMOTE_TYPE") == "ssh":
        # REMOTE_ARGS is the ssh destination ([user@]host); the control
        # channel rides the same management path as the ssh session.
        return cfg.remote.name.rpartition("@")[2]
    raise KsftSkipEx("XSK_SYNC_ADDR is required for this remote backend")


def _channel_counts(ifname, host=None):
    """Parse ethtool -l (works without --json).

    Returns the "Pre-set maximums" and "Current hardware settings" sections
    as two dicts keyed by rx/tx/combined.
    """
    result = cmd(f"ethtool -l {shlex.quote(ifname)}", host=host, fail=False)
    if result.ret:
        stderr = (result.stderr or "").strip()
        detail = f": {stderr}" if stderr else ""
        raise KsftSkipEx(f"cannot query channels on {ifname}{detail}")

    maxima = {}
    current = {}
    section = None
    for line in result.stdout.splitlines():
        if line.startswith("Pre-set maximums"):
            section = maxima
            continue
        if line.startswith("Current hardware settings"):
            section = current
            continue
        if section is None or ":" not in line:
            continue
        key, _, value = line.partition(":")
        key = key.strip().lower()
        value = value.strip()
        if key in ("rx", "tx", "combined") and value.isdigit():
            section[key] = int(value)
    return maxima, current


def _root_cmd(command, host=None, sudo=False, **kwargs):
    """Run a privileged command; the remote account may need sudo."""
    if sudo:
        # Non-interactive so a missing NOPASSWD rule fails instead of hanging.
        command = "sudo -n " + command
    return cmd(command, host=host, **kwargs)


def _check_remote_sudo(remote, remote_bin):
    """Fail early, with the sudoers line to add, if sudo -n is not covered.

    A non-root ssh account typically gets a NOPASSWD rule for a fixed set of
    commands. Everything run through _root_cmd() plus the endpoint binary
    must be in it, otherwise setup or cleanup dies half-way with a bare
    "a password is required".
    """
    paths = cmd("PATH=$PATH:/usr/sbin:/sbin command -v ethtool sysctl",
                host=remote).stdout.split()
    denied = []
    for path in paths + [str(remote_bin)]:
        probe = "--version" if path in paths else "--help"
        result = cmd(f"sudo -n {shlex.quote(path)} {probe}", host=remote,
                     fail=False)
        if result.ret and (result.stderr or "").startswith("sudo:"):
            denied.append(path)
    if denied:
        user = cmd("id -un", host=remote).stdout.strip()
        rule = f"{user} ALL=(root) NOPASSWD: " + ", ".join(denied)
        # sudoers.d is included last; a later generic rule with a password
        # would otherwise override the entry.
        raise KsftFailEx(f"remote sudo -n denied for {' '.join(denied)}; "
                         f"add to /etc/sudoers.d/ on {remote.name}: {rule}")


def _reserve_hugepages(host=None, sudo=False):
    """Raise vm.nr_hugepages to NR_HUGEPAGES and defer its restoration."""
    have = int(cmd("sysctl -n vm.nr_hugepages", host=host).stdout)
    if have >= NR_HUGEPAGES:
        return
    # sysctl exits 0 even when denied; judge by what the kernel granted.
    _root_cmd(f"sysctl -qw vm.nr_hugepages={NR_HUGEPAGES}", host=host,
              sudo=sudo, fail=False)
    got = int(cmd("sysctl -n vm.nr_hugepages", host=host).stdout)
    if got != have:
        defer(_root_cmd, f"sysctl -qw vm.nr_hugepages={have}", host=host,
              sudo=sudo)
    if got < NR_HUGEPAGES:
        ksft_pr(f"only {got}/{NR_HUGEPAGES} hugepages on "
                f"{host.name if host else 'local'}; unaligned tests skip")


def _isolate_queue_zero(ifname, host=None, sudo=False):
    """Pin traffic to queue 0 and defer restoration of the channel counts.

    A NIC found already at a single channel is most likely a leftover of an
    earlier run that never got to restore it; bring it back to the pre-set
    maximum afterwards instead.
    """
    maxima, counts = _channel_counts(ifname, host)
    keys = ("combined",) if counts.get("combined") else ("rx", "tx")
    if not any(counts.get(k) for k in keys):
        raise KsftSkipEx(f"no channels reported for {ifname}")

    single = all(counts[k] <= 1 for k in keys)
    restore = maxima if single else counts
    restore_args = " ".join(f"{k} {restore[k]}" for k in keys)
    if single:
        ksft_pr(f"{ifname}: already at a single channel, will restore "
                f"pre-set maximum ({restore_args})")
    else:
        args = " ".join(f"{k} 1" for k in keys)
        result = _root_cmd(f"ethtool -L {shlex.quote(ifname)} {args}",
                           host=host, sudo=sudo, fail=False)
        if result.ret:
            stderr = (result.stderr or "").strip()
            detail = f": {stderr}" if stderr else ""
            raise KsftSkipEx(f"cannot isolate queue 0 on {ifname}{detail}")
    defer(_root_cmd, f"ethtool -L {shlex.quote(ifname)} {restore_args}",
          host=host, sudo=sudo)


def _deploy_remote_bin(cfg, local_bin):
    """Ship the freshly built helper to the remote host.

    XSK_REMOTE_BIN names a fixed destination path (e.g. one covered by a
    sudoers NOPASSWD rule) instead of deploy()'s random temporary name.
    """
    remote_bin = cfg.env.get("XSK_REMOTE_BIN")
    if not remote_bin:
        return cfg.remote.deploy(local_bin)
    if cfg.env.get("REMOTE_TYPE") != "ssh":
        raise KsftSkipEx("XSK_REMOTE_BIN requires the ssh remote backend")
    # Upload next to the target and rename over it: a previous copy may
    # be root-owned (e.g. built with sudo) and thus not writable.
    tmp = remote_bin + ".upload"
    cmd(["scp", "-q", str(local_bin), f"{cfg.remote.name}:{tmp}"],
        shell=False)
    cmd(f"mv -f {shlex.quote(tmp)} {shlex.quote(remote_bin)}", host=cfg.remote)
    return remote_bin


def _endpoint_args(binary, ifname, role, sync_addr, sync_port, sudo=False):
    args = [str(binary), "-i", ifname,
            "--endpoint", role,
            "--peer", sync_addr,
            "--peer-port", str(sync_port)]
    return ["sudo", "-n"] + args if sudo else args


def _print_output(proc, line_pfx):
    if proc.stdout:
        ksft_pr(proc.stdout, line_pfx=line_pfx)
    if proc.stderr:
        ksft_pr(proc.stderr, line_pfx=line_pfx)


def _wait_remote_listen(remote, port, host):
    """Wait for the remote listener, bailing out if it exits first."""
    deadline = time.monotonic() + LISTEN_DEADLINE
    while True:
        try:
            wait_port_listen(port, host=host, deadline=1)
            return
        except Exception:
            if remote.proc.poll() is not None:
                raise KsftFailEx("remote endpoint exited before listening")
            if time.monotonic() > deadline:
                raise


def _run_streaming(args, line_pfx, timeout):
    """Run a local command, forwarding each stdout line as it is produced.

    xskxceiver prints one TAP line per test case and kselftest.h makes
    stdout line-buffered, so the per-test verdicts show up live instead
    of after the whole suite has finished.
    """
    proc = cmd(args, background=True, shell=False, fail=False)
    deadline = time.monotonic() + timeout
    with selectors.DefaultSelector() as sel:
        sel.register(proc.proc.stdout, selectors.EVENT_READ)
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not sel.select(remaining):
                proc.proc.kill()
                proc.process(terminate=False, fail=False)
                raise KsftFailEx(f"{args[0]} timed out after {timeout}s")
            line = proc.proc.stdout.readline()
            if not line:
                break
            ksft_pr(line.decode("utf-8").rstrip("\n"), line_pfx=line_pfx)
    proc.process(terminate=False, fail=False,
                 timeout=max(deadline - time.monotonic(), 1))
    proc.stdout = ""
    return proc


def test_xskxceiver(cfg):
    local_bin = Path(__file__).with_name("xskxceiver")
    if not local_bin.exists():
        raise KsftSkipEx("xskxceiver helper was not built")
    remote_bin = _deploy_remote_bin(cfg, local_bin)
    # The remote ssh account may not be root; XSK_REMOTE_SUDO=1 wraps the
    # remote setup and endpoint (which loads BPF and binds AF_XDP) in sudo.
    remote_sudo = cfg.env.get("XSK_REMOTE_SUDO", "") not in ("", "0")
    if remote_sudo:
        _check_remote_sudo(cfg.remote, remote_bin)

    _reserve_hugepages()
    _reserve_hugepages(cfg.remote, sudo=remote_sudo)
    _isolate_queue_zero(cfg.ifname)
    _isolate_queue_zero(cfg.remote_ifname, cfg.remote, sudo=remote_sudo)

    # The local host is the DUT (see README.rst) and runs the RX endpoint
    # under test; the remote host is the TX traffic generator.
    sync_addr = _management_addr(cfg)
    sync_port = rand_port()

    remote_args = _endpoint_args(remote_bin, cfg.remote_ifname, "tx",
                                 sync_addr, sync_port, sudo=remote_sudo)
    local_args = _endpoint_args(local_bin, cfg.ifname, "rx",
                                sync_addr, sync_port)
    # The remote backend takes a shell command; keep local arguments as argv.
    remote_cmd = shlex.join(remote_args)

    ksft_pr(f"remote: {remote_cmd}")
    ksft_pr(f"local:  {shlex.join(local_args)}")

    remote = bkg(remote_cmd, host=cfg.remote, exit_wait=True, fail=False)
    try:
        with remote:
            # Start the local RX connector only once the remote TX
            # endpoint is listening; the endpoints themselves do not retry.
            _wait_remote_listen(remote, sync_port, cfg.remote)
            result = _run_streaming(local_args, "local|", SUITE_TIMEOUT)
    except Exception:
        _print_output(remote, "remote|")
        raise

    # The local endpoint is the DUT under test and its per-test
    # PASS/FAIL/SKIP breakdown was streamed above; the remote side is
    # just a traffic generator (see README.rst), so its output is only
    # surfaced if something actually failed.
    if result.ret or remote.ret:
        _print_output(result, "local|")
        _print_output(remote, "remote|")

    if result.ret:
        raise KsftFailEx("xskxceiver endpoint suite failed "
                         f"(exit code {result.ret})")
    if remote.ret:
        raise KsftFailEx("xskxceiver remote endpoint failed "
                         f"(exit code {remote.ret})")


def main():
    with NetDrvEpEnv(__file__, nsim_test=False) as cfg:
        ksft_run([test_xskxceiver], args=(cfg,))
    ksft_exit()


if __name__ == "__main__":
    main()
