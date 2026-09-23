#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""AF_XDP zero-copy hardware tests with an SKB-mode XSK peer."""

import fnmatch
import getopt
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

from lib.py import (CmdExitFailure, EthtoolFamily, KsftFailEx, KsftNamedVariant,
                    KsftSkipEx, NetdevFamily, NetDrvEpEnv, bkg, cmd, defer,
                    ethtool, ip, ksft_exit, ksft_pr, ksft_run, ksft_variants,
                    rand_port, wait_port_listen)

CASE_TIMEOUT = 120
LISTEN_TIMEOUT = 30
NR_HUGEPAGES = 32
SKIP_CODE = 4


def _test_ids(binary, host=None):
    command = f"{shlex.quote(str(binary))} -l" if host else [str(binary), "-l"]
    result = cmd(command, host=host, fail=False)
    where = f" on {host.name}" if host else ""
    ids = {}
    for line in result.stdout.splitlines():
        match = re.fullmatch(r"(\d+): ([A-Z0-9_]+)", line)
        if match:
            ids[match[2]] = int(match[1])
    if ids:
        return ids
    detail = (result.stderr or result.stdout).strip()
    suffix = f": {detail}" if detail else ""
    if result.ret:
        raise KsftFailEx(f"xskxceiver -l failed{where} "
                         f"with exit code {result.ret}{suffix}")
    raise KsftFailEx(f"xskxceiver did not list tests{where}{suffix}")


def _hardware_cases(binary):
    result = cmd([str(binary), "--list-hw"], fail=False)
    cases = []
    labels = set()

    for line in result.stdout.splitlines():
        match = re.fullmatch(r"(\d+)\t([A-Z0-9_]+)\t(rx|tx)", line)
        if not match:
            continue
        test_id, name, direction = match.groups()
        label = f"{direction}_{name.lower()}"
        if label in labels:
            raise KsftFailEx(f"xskxceiver listed duplicate hardware case {label}")
        labels.add(label)
        cases.append((label, name, direction, int(test_id)))

    if cases:
        return cases
    detail = (result.stderr or result.stdout).strip()
    suffix = f": {detail}" if detail else ""
    if result.ret:
        raise KsftFailEx("xskxceiver --list-hw failed with exit code "
                         f"{result.ret}{suffix}")
    raise KsftFailEx(f"xskxceiver did not list hardware cases{suffix}")


def _management_addr(cfg):
    if cfg.env.get("XSK_SYNC_ADDR"):
        return cfg.env["XSK_SYNC_ADDR"]
    if cfg.env.get("REMOTE_TYPE") == "ssh":
        return cfg.remote.name.rpartition("@")[2]
    raise KsftSkipEx("XSK_SYNC_ADDR is required for this remote backend")


def _root_cmd(command, host=None, sudo=False, **kwargs):
    if sudo:
        command = "sudo -n " + command
    return cmd(command, host=host, **kwargs)


def _remote_binary(cfg, local_binary):
    binary = cfg.env.get("XSK_REMOTE_BIN")
    if not binary:
        return cfg.remote.deploy(local_binary)
    if cfg.env.get("REMOTE_TYPE") != "ssh":
        raise KsftSkipEx("XSK_REMOTE_BIN requires the SSH remote backend")
    if cfg.env.get("XSK_REMOTE_DEPLOY", "1") == "0":
        result = cmd(f"test -x {shlex.quote(binary)}", host=cfg.remote,
                     fail=False)
        if result.ret:
            raise KsftSkipEx(f"remote xskxceiver is not executable: {binary}")
        return binary
    upload = binary + ".upload"
    cmd(["scp", "-q", str(local_binary), f"{cfg.remote.name}:{upload}"],
        shell=False)
    cmd(f"mv -f {shlex.quote(upload)} {shlex.quote(binary)}",
        host=cfg.remote)
    return binary


def _remote_sudo(cfg):
    return cfg.env.get("XSK_REMOTE_SUDO", "").lower() not in ("", "0", "false")


def _mac(ifname, host=None):
    return cmd(f"cat /sys/class/net/{shlex.quote(ifname)}/address",
               host=host).stdout.strip()


def _restore_mtu(ifname, before, host=None, sudo=False):
    actual = ip(f"link show dev {shlex.quote(ifname)}", json=True,
                host=host)[0]["mtu"]
    if actual == before:
        return
    _root_cmd(f"ip link set dev {shlex.quote(ifname)} mtu {before}",
              host=host, sudo=sudo)
    actual = ip(f"link show dev {shlex.quote(ifname)}", json=True,
                host=host)[0]["mtu"]
    if actual != before:
        raise KsftFailEx(f"MTU rollback requested {before}, got {actual}")


def _save_mtu(ifname, host=None, sudo=False, register=defer):
    before = ip(f"link show dev {shlex.quote(ifname)}", json=True,
                host=host)[0]["mtu"]
    register(_restore_mtu, ifname, before, host, sudo)


def _restore_hugepages(count, host=None, sudo=False):
    _root_cmd(f"sysctl -qw vm.nr_hugepages={count}", host=host, sudo=sudo)
    actual = int(cmd("sysctl -n vm.nr_hugepages", host=host).stdout)
    if actual != count:
        raise KsftFailEx(f"hugepage rollback requested {count}, got {actual}")


def _reserve_hugepages(host=None, sudo=False, register=defer):
    have = int(cmd("sysctl -n vm.nr_hugepages", host=host).stdout)
    if have >= NR_HUGEPAGES:
        return
    _root_cmd(f"sysctl -qw vm.nr_hugepages={NR_HUGEPAGES}", host=host,
              sudo=sudo, fail=False)
    got = int(cmd("sysctl -n vm.nr_hugepages", host=host).stdout)
    if got != have:
        register(_restore_hugepages, have, host, sudo)
    if got < NR_HUGEPAGES:
        raise KsftSkipEx(f"only {got}/{NR_HUGEPAGES} hugepages available")


def _restore_rss(ethnl, header, saved):
    ethnl.rss_set({"header": header, "indir": saved})
    actual = ethnl.rss_get({"header": header}).get("indir")
    if actual != saved:
        raise KsftFailEx("RSS indirection table was not restored exactly")


def _delete_rule(ifname, rule_id, host=None, sudo=False):
    _root_cmd(f"ethtool -N {shlex.quote(ifname)} delete {rule_id}",
              host=host, sudo=sudo)
    remaining = _root_cmd(f"ethtool -u {shlex.quote(ifname)}", host=host,
                          sudo=sudo, fail=False)
    if remaining.ret == 0 and rule_id in re.findall(r"Filter: (\d+)", remaining.stdout):
        raise KsftFailEx(f"ntuple rule {rule_id} remains after deletion")


def _feature_state(ifname, feature, host=None):
    args = f"-k {shlex.quote(ifname)}"
    try:
        return ethtool(args, json=True, host=host)[0][feature]
    except (CmdExitFailure, KeyError, IndexError, ValueError):
        output = ethtool(args, host=host).stdout

    match = re.search(rf"^{re.escape(feature)}:\s+(on|off)(.*)$", output,
                      re.MULTILINE)
    if not match:
        raise KsftSkipEx(f"ethtool does not report {feature} on {ifname}")
    return {"active": match[1] == "on", "fixed": "[fixed]" in match[2]}


def _disable_ntuple(ifname, host=None, sudo=False):
    _root_cmd(f"ethtool -K {shlex.quote(ifname)} ntuple-filters off",
              host=host, sudo=sudo)
    current = _feature_state(ifname, "ntuple-filters", host)
    if current["active"]:
        raise KsftFailEx("ntuple filters remained enabled after rollback")


def _steer_udp(ifname, port, queue, host=None, sudo=False, register=defer):
    ntuple = _feature_state(ifname, "ntuple-filters", host)
    if not ntuple["active"]:
        if ntuple["fixed"]:
            raise KsftSkipEx("NIC does not support ntuple filters")
        _root_cmd(f"ethtool -K {shlex.quote(ifname)} ntuple-filters on",
                  host=host, sudo=sudo)
        register(_disable_ntuple, ifname, host, sudo)
        ntuple = _feature_state(ifname, "ntuple-filters", host)
        if not ntuple["active"]:
            raise KsftSkipEx("NIC did not enable ntuple filters")

    rules_cmd = f"ethtool -u {shlex.quote(ifname)}"
    before = _root_cmd(rules_cmd, host=host, sudo=sudo, fail=False)
    if before.ret:
        raise KsftSkipEx(f"cannot read ntuple rules on {ifname}")
    old_ids = set(re.findall(r"Filter: (\d+)", before.stdout))
    try:
        output = _root_cmd(f"ethtool -N {shlex.quote(ifname)} flow-type udp4 "
                           f"dst-port {port} action {queue}", host=host,
                           sudo=sudo).stdout
    except CmdExitFailure as exc:
        raise KsftSkipEx(f"cannot steer UDP flow to queue {queue}: {exc}") from exc
    match = re.search(r"ID (\d+)", output)
    if not match:
        after = _root_cmd(rules_cmd, host=host, sudo=sudo, fail=False)
        new_ids = set(re.findall(r"Filter: (\d+)", after.stdout))
        added = new_ids - old_ids if after.ret == 0 else set()
        if len(added) != 1:
            raise KsftFailEx(f"cannot identify new ntuple rule: {output}")
        rule_id = added.pop()
    else:
        rule_id = match[1]
    register(_delete_rule, ifname, rule_id, host, sudo)


def _reserve_rx_queue(cfg, port, register=defer):
    """Move one RX queue out of default RSS and steer the test UDP flow."""
    header = {"dev-index": cfg.ifindex}
    channels = cfg.ethnl.channels_get({"header": header})
    count = (channels.get("combined-count") or 0) + (channels.get("rx-count") or 0)
    if count < 2:
        raise KsftSkipEx("RX queue isolation needs at least two channels")
    queue = count - 1

    try:
        rss = cfg.ethnl.rss_get({"header": header})
        saved = list(rss["indir"])
    except Exception as exc:
        raise KsftSkipEx(f"cannot read RSS indirection table: {exc}") from exc
    if queue in saved:
        updated = [0 if entry == queue else entry for entry in saved]
        try:
            cfg.ethnl.rss_set({"header": header, "indir": updated})
        except Exception as exc:
            raise KsftSkipEx(f"cannot reserve RX queue in RSS: {exc}") from exc
        register(_restore_rss, cfg.ethnl, header, saved)
        actual = cfg.ethnl.rss_get({"header": header}).get("indir")
        if actual != updated:
            raise KsftSkipEx("NIC did not remove the reserved queue from RSS")

    _steer_udp(cfg.ifname, port, queue, register=register)
    return queue


def _probe_zc(cfg, binary):
    if hasattr(cfg, "xsk_zc_ready"):
        if not cfg.xsk_zc_ready:
            raise KsftSkipEx("NIC does not advertise AF_XDP zero-copy")
        return
    result = cmd([str(binary), "-i", cfg.ifname, "--probe-zc"], fail=False)
    if result.ret == SKIP_CODE:
        cfg.xsk_zc_ready = False
        raise KsftSkipEx("NIC does not advertise AF_XDP zero-copy")
    if result.ret:
        raise KsftFailEx("NIC advertises zero-copy but XDP_OPTIONS_ZEROCOPY "
                         f"probe failed: {result.stdout} {result.stderr}")
    cfg.xsk_zc_ready = True


def _case_args(cfg, name):
    if name != "TOO_MANY_FRAGS":
        return []
    try:
        info = cfg.netnl.dev_get({"ifindex": cfg.ifindex})
    except Exception as exc:
        raise KsftSkipEx(f"cannot query DUT fragment limit: {exc}") from exc
    max_frags = info.get("xdp-zc-max-segs", 0)
    if max_frags <= 1:
        raise KsftSkipEx("DUT does not advertise multi-buffer zero-copy")
    return ["--max-frags", str(max_frags)]


def _xdp_attached(ifname, host=None):
    info = ip(f"-d link show dev {shlex.quote(ifname)}", json=True,
              host=host)[0]
    return (info.get("xdp") or {}).get("attached", "off")


def _ensure_xdp_free(ifname, host=None):
    attached = _xdp_attached(ifname, host)
    if attached not in ("off", "none"):
        raise KsftSkipEx(f"{ifname} already has an XDP program")


def _detach_test_xdp(ifname, host=None, sudo=False):
    if _xdp_attached(ifname, host) in ("off", "none"):
        return
    _root_cmd(f"ip link set dev {shlex.quote(ifname)} xdp off",
              host=host, sudo=sudo)
    if _xdp_attached(ifname, host) not in ("off", "none"):
        raise KsftFailEx(f"XDP program remains attached to {ifname}")


def _wait_remote_listen(remote, port, host):
    deadline = time.monotonic() + LISTEN_TIMEOUT
    while time.monotonic() < deadline:
        try:
            wait_port_listen(port, host=host, deadline=1)
            return
        except Exception:
            if remote.proc.poll() is not None:
                raise KsftFailEx("remote XSK endpoint exited before listening")
    raise TimeoutError("remote XSK endpoint did not listen on the control port")


class _SuiteState:
    def __init__(self, cfg):
        self.cfg = cfg
        self.cleanups = []
        self.closed = False
        self.unaligned_skip = None

    def defer(self, func, *args, **kwargs):
        self.cleanups.append((func, args, kwargs))

    def close(self):
        if self.closed:
            return
        self.closed = True
        errors = []
        while self.cleanups:
            func, args, kwargs = self.cleanups.pop()
            try:
                func(*args, **kwargs)
            except Exception as exc:
                errors.append(exc)
        if errors:
            for exc in errors[1:]:
                ksft_pr(f"additional suite cleanup failure: {exc}")
            raise errors[0]


def _selected_cases(cases):
    opts, _ = getopt.getopt(sys.argv[1:], "hlt:T:")
    filters = [(opt == "-t", value) for opt, value in opts
               if opt in ("-t", "-T")]
    have_include = any(include for include, _ in filters)

    def enabled(label):
        full_name = f"test_xsk.{label}"
        for include, pattern in filters:
            if fnmatch.fnmatchcase(full_name, pattern):
                return include
        return not have_include

    return [case for case in cases if enabled(case[0])]


def _prepare_suite(cfg):
    state = _SuiteState(cfg)
    selected = cfg.xsk_selected

    try:
        cfg.require_ipver("4")
        state.sync_addr = _management_addr(cfg)
        state.binary = Path(__file__).with_name("xskxceiver")
        if not state.binary.exists():
            raise KsftSkipEx("xskxceiver helper was not built")
        state.remote_sudo = _remote_sudo(cfg)

        _ensure_xdp_free(cfg.ifname)
        _ensure_xdp_free(cfg.remote_ifname, cfg.remote)
        _probe_zc(cfg, state.binary)
        state.remote_bin = _remote_binary(cfg, state.binary)
        state.remote_ids = _test_ids(state.remote_bin, cfg.remote)

        _save_mtu(cfg.ifname, register=state.defer)
        _save_mtu(cfg.remote_ifname, cfg.remote, state.remote_sudo,
                  register=state.defer)

        if any("UNALIGNED" in name for _, name, _, _ in selected):
            try:
                _reserve_hugepages(register=state.defer)
                _reserve_hugepages(cfg.remote, state.remote_sudo,
                                   register=state.defer)
            except KsftSkipEx as exc:
                state.unaligned_skip = str(exc)

        state.udp_port = int(cfg.env.get("XSK_UDP_PORT", "42567"))
        if not 1 <= state.udp_port <= 65535:
            raise KsftFailEx("XSK_UDP_PORT must be from 1 to 65535")

        state.rx_queue = 0
        if any(direction == "rx" for _, _, direction, _ in selected):
            state.rx_queue = _reserve_rx_queue(cfg, state.udp_port,
                                               register=state.defer)
        if any(direction == "tx" for _, _, direction, _ in selected):
            _steer_udp(cfg.remote_ifname, state.udp_port, 0, cfg.remote,
                       state.remote_sudo, register=state.defer)

        state.defer(_detach_test_xdp, cfg.ifname)
        state.defer(_detach_test_xdp, cfg.remote_ifname, cfg.remote,
                    state.remote_sudo)
        state.local_mac = _mac(cfg.ifname)
        state.remote_mac = _mac(cfg.remote_ifname, cfg.remote)
    except Exception:
        state.close()
        raise

    return state


def _case_result(local, remote):
    if local.stdout:
        ksft_pr(local.stdout, line_pfx="dut|")
    if local.stderr:
        ksft_pr(local.stderr, line_pfx="dut|")
    if remote.stdout:
        ksft_pr(remote.stdout, line_pfx="remote|")
    if remote.stderr:
        ksft_pr(remote.stderr, line_pfx="remote|")
    if local.ret == SKIP_CODE or remote.ret == SKIP_CODE:
        raise KsftSkipEx("AF_XDP case unsupported by one endpoint")
    if local.ret or remote.ret:
        raise KsftFailEx(f"DUT exit {local.ret}, remote exit {remote.ret}")


def _run_case(state, name, direction, test_id):
    cfg = state.cfg
    if name not in state.remote_ids:
        raise KsftSkipEx(f"remote xskxceiver lacks test {name}")
    if "UNALIGNED" in name and state.unaligned_skip:
        raise KsftSkipEx(state.unaligned_skip)

    queue = state.rx_queue if direction == "rx" else 0
    packet_src = (cfg.remote_addr_v["4"] if direction == "rx"
                  else cfg.addr_v["4"])
    packet_dst = (cfg.addr_v["4"] if direction == "rx"
                  else cfg.remote_addr_v["4"])
    case_args = _case_args(cfg, name)
    control_port = rand_port()
    remote_role = "tx" if direction == "rx" else "rx"
    remote_args = [str(state.remote_bin), "-i", cfg.remote_ifname,
                   "--hw", "--listen", "-m", "skb",
                   "-t", str(state.remote_ids[name]), "-e", remote_role,
                   "-p", "0.0.0.0", "-P", str(control_port),
                   "--udp-src", packet_src, "--udp-dst", packet_dst,
                   "--udp-port", str(state.udp_port),
                   "--peer-mac", state.local_mac, "--queue", "0"] + case_args
    if state.remote_sudo:
        remote_args = ["sudo", "-n"] + remote_args
    local_args = [str(state.binary), "-i", cfg.ifname, "--hw", "-m", "zc",
                  "-t", str(test_id), "-e", direction,
                  "-p", state.sync_addr, "-P", str(control_port),
                  "--udp-src", packet_src, "--udp-dst", packet_dst,
                  "--udp-port", str(state.udp_port),
                  "--peer-mac", state.remote_mac,
                  "--queue", str(queue)] + case_args

    local = None
    remote = bkg(shlex.join(remote_args), host=cfg.remote, exit_wait=True,
                 fail=False)
    try:
        _wait_remote_listen(remote, control_port, cfg.remote)
        local = cmd(local_args, background=True, shell=False, fail=False)
        try:
            local.process(terminate=False, fail=False, timeout=CASE_TIMEOUT)
        except subprocess.TimeoutExpired as exc:
            local.proc.kill()
            local.process(terminate=False, fail=False)
            raise KsftFailEx(f"DUT case timed out after {CASE_TIMEOUT}s") from exc
        remote.process(terminate=False, fail=False, timeout=10)
    finally:
        if remote.ret is None:
            remote.process(terminate=True, fail=False)
    _case_result(local, remote)


def test_xsk(cfg, label, name, direction, test_id):
    if hasattr(cfg, "xsk_setup_error"):
        exc_type, message = cfg.xsk_setup_error
        raise exc_type(message)
    if not hasattr(cfg, "xsk_state"):
        try:
            cfg.xsk_state = _prepare_suite(cfg)
        except (KsftFailEx, KsftSkipEx) as exc:
            cfg.xsk_setup_error = (type(exc), str(exc))
            raise

    try:
        _run_case(cfg.xsk_state, name, direction, test_id)
    finally:
        if label == cfg.xsk_selected[-1][0]:
            cfg.xsk_state.close()


def main():
    binary = Path(__file__).with_name("xskxceiver")
    if not binary.exists():
        raise KsftSkipEx("xskxceiver helper was not built")
    cases = _hardware_cases(binary)
    variants = ksft_variants([
        KsftNamedVariant(label, label, name, direction, test_id)
        for label, name, direction, test_id in cases
    ])(test_xsk)

    if any(arg in ("-h", "-l") for arg in sys.argv[1:]):
        ksft_run(cases=[variants])
        return
    with NetDrvEpEnv(__file__, nsim_test=False) as cfg:
        cfg.ethnl = EthtoolFamily()
        cfg.netnl = NetdevFamily()
        cfg.xsk_selected = _selected_cases(cases)
        try:
            ksft_run(cases=[variants], args=(cfg,))
        finally:
            if hasattr(cfg, "xsk_state"):
                cfg.xsk_state.close()
    ksft_exit()


if __name__ == "__main__":
    main()
