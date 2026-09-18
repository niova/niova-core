#!/usr/bin/env python3
"""System-test runner for niova-core.

Discovers shell test scripts in category subdirectories of this directory
(test/system-tests/<category>/<name>.sh); scripts under lib/ are templates
reached only by reference and are not run directly.  Each script carries a
small comment header that declares what the test needs:

  # REQUIRE_ENV: MINIO_ENDPOINT   SKIP the test unless every named var is set
  # TIMEOUT: 60                   optional per-script timeout override (default 600)
  # EXPECT: fail                  under --selftest, the test must fail to pass (default pass)

Any directive value may reference the runner's environment as ${VAR} or
${VAR:-default}, e.g. '# TIMEOUT: ${T:-600}', so a test's timeout knob is
settable from a dev shell without editing the script.  As in the shell, the
default applies when the var is unset or set-empty; a bare ${VAR} that is
unset is an error, not an empty expansion.

REQUIRE_ENV names variables that must be set and non-empty in the runner's
environment, else the test is skipped.

An extensionless file is a marker for a C test binary: the runner execs
build-dir/test/<name> in place of a script — used by the unit/ markers for
the standalone C test binaries of the build tree.

A parameter variant of a base test is a symlink whose name encodes the extra
arguments, base___<server-opts>___<client-args>.sh (or base_____<...>.sh
with no server opts).  The runner decodes the name into EXTRA_SERVER_OPTS and
EXTRA_CLIENT_ARGS; a base script reads them with a sensible default so the
same file serves both the base run and every variant.

For each test the runner:
  1. Creates a temp directory under ($TMPDIR or /var/tmp)/niova-system-tests-<user>-<timestamp>/<category>/<name>/, with a
     logs/ subdirectory for all process output.
  2. Injects environment variables and runs the script under bash:
       BUILD_DIR                                        — build-tree root
       RVAL                                             — per-test seed
       EXTRA_SERVER_OPTS, EXTRA_CLIENT_ARGS           — decoded from a variant symlink
       TEST_TMPDIR, TEST_LOGDIR                       — scratch dir / logs

A watchdog thread kills the script if it exceeds its timeout.  Each test runs
in its own cgroup-v2 leaf, so teardown — and an abort (Ctrl+C) — reaps every
descendant the test spawned.  Designed to run standalone — no make
integration required.
"""

from __future__ import annotations

import argparse
import atexit
import dataclasses
import fnmatch
import gzip
import configparser
import os
import random
import re
import resource
import shlex
import shutil
import signal
import subprocess
import sys
import threading
import time
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

OPEN_FILE_LIMIT = 16384          # RLIMIT_NOFILE soft cap; parallel tests' sockets + io_uring rings + log files exhaust the usual 1024 default
DEFAULT_RUN_DIR = Path("/var/tmp")  # run-dir fallback; /var/tmp survives reboots ($TMPDIR overrides)
DEFAULT_SCRIPT_TIMEOUT = 1800.0  # watchdog kill for a hung script; the single global limit
SKIP_EXIT_CODE = 77             # automake convention: a script that exits 77 skipped itself

# Default number of trailing lines of each log file to dump on failure.
_PRINT_LOG_TAIL_LINES = 15

# Per-test record of every process the runner signalled, alongside its logs.
_KILL_LOG_FILE = "kills.out"

# Run-level trace at the run dir's top level: everything the runner printed,
# plus the records it deliberately kept off the terminal.
_RUN_CONSOLE_FILE = "run.out"


# --- helpers ---------------------------------------------------------------

def find_build_dir(arg: str | None) -> Path:
    """Resolve the build directory from --build-dir or NIOVA_BUILD_DIR; exit if absent."""
    candidates = [arg, os.environ.get("NIOVA_BUILD_DIR")]
    for cand in candidates:
        if cand:
            path = Path(cand).resolve()
            if not path.is_dir():
                sys.exit(f"build dir does not exist: {path}")
            return path
    sys.exit("error: --build-dir or NIOVA_BUILD_DIR must be provided")


def logs_dir(tmp_root: Path) -> Path:
    """Per-test subdirectory holding all process output (created by prepare_test)."""
    return tmp_root / "logs"


def parse_symlink_params(path: Path) -> tuple[list[str], list[str]]:
    """Decode the server opts and client args encoded in a variant symlink name.

    A parameter variant of a base test is a symlink named
    base___<server-opts>___<client-args>.sh (or base_____<client-args>.sh
    with no server opts).  The stem is split on '__'; field 1 carries the server
    opts and field 2 the client args.  Each segment is decoded by turning every
    '_' into ' -' and splitting on whitespace, e.g. '_N10000_Z32' -> ['-N10000',
    '-Z32'].  A non-symlink (a base script run directly) carries no parameters.
    """
    if not path.is_symlink():
        return [], []
    fields = path.stem.split('__')

    def decode(segment: str) -> list[str]:
        return segment.replace('_', ' -').split()

    server_opts = decode(fields[1]) if len(fields) > 1 else []
    client_args = decode(fields[2]) if len(fields) > 2 else []
    return server_opts, client_args


@dataclasses.dataclass
class ConsoleMirror:
    """Live state of the fd-level tee of stdout/stderr into the run log."""

    sink: object = None
    # (fd, dup of the original fd) pairs, restored when the mirror drains.
    saved: list[tuple[int, int]] = dataclasses.field(default_factory=list)
    pumps: list[threading.Thread] = dataclasses.field(default_factory=list)


_CONSOLE_MIRROR = ConsoleMirror()


def _pump_fd(read_fd: int, real_fd: int, sink) -> None:
    """Copy one redirected stream to both the real fd and the run log."""
    while True:
        try:
            chunk = os.read(read_fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        for write in (lambda: os.write(real_fd, chunk),
                      lambda: (sink.write(chunk), sink.flush())):
            try:
                write()
            except (OSError, ValueError):
                pass
    os.close(read_fd)


def mirror_console(path: Path) -> None:
    """Tee this process' stdout and stderr into path; never raises.

    A CI artifact is a directory rather than a terminal.  Done at the
    file-descriptor level so anything inheriting fd 1/2 lands in the same file.
    """
    if _CONSOLE_MIRROR.sink is not None:
        return
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        sink = path.open("ab")
    except OSError as exc:
        sys.stderr.write(f"could not mirror the console to {path}: {exc}\n")
        return
    _CONSOLE_MIRROR.sink = sink
    for fd in (1, 2):
        try:
            real = os.dup(fd)
            read_fd, write_fd = os.pipe()
            os.dup2(write_fd, fd)
            os.close(write_fd)
        except OSError as exc:
            sys.stderr.write(f"could not mirror fd {fd}: {exc}\n")
            continue
        pump = threading.Thread(target=_pump_fd, args=(read_fd, real, sink),
                                daemon=True)
        pump.start()
        _CONSOLE_MIRROR.saved.append((fd, real))
        _CONSOLE_MIRROR.pumps.append(pump)
    atexit.register(drain_console)


def drain_console() -> None:
    """Restore the real stdout/stderr and flush the mirrored tail; never raises.

    Reached from atexit and from the interrupt handler, whose os._exit would
    otherwise drop the summary the pump threads still hold.
    """
    mirror = _CONSOLE_MIRROR
    if mirror.sink is None:
        return
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.flush()
        except (OSError, ValueError):
            pass
    # Restoring the real fd drops the pipe's last writer, so each pump reaches
    # EOF once it has copied what was already in flight.
    for fd, real in mirror.saved:
        try:
            os.dup2(real, fd)
        except OSError:
            pass
    for pump in mirror.pumps:
        pump.join(timeout=5)
    mirror.pumps.clear()
    # Only now: a pump still draining would write its last chunk to a closed
    # descriptor and that line would reach the file but never the console.
    for _fd, real in mirror.saved:
        try:
            os.close(real)
        except OSError:
            pass
    mirror.saved.clear()
    try:
        mirror.sink.close()
    except OSError:
        pass
    mirror.sink = None


def write_run_console(text: str) -> bool:
    """Append text to the run log without echoing it to the terminal.

    The mirror tees stdout and stderr, so printing a record puts it on the
    terminal too; one that belongs in the artifact but not between the result
    lines has to enter the sink directly.  One write per record: the sink
    serialises a single write against the pump threads, a split one would
    interleave.  Never raises.

    @return True if the text landed in the run log.
    """
    sink = _CONSOLE_MIRROR.sink
    if sink is None:
        return False
    try:
        sink.write(text.encode())
        sink.flush()
    except (OSError, ValueError):  # ValueError: drain_console closed the sink
        return False
    return True


def log_run(text: str) -> None:
    """Record per-test output in the run log instead of printing it.

    Under -j the tests report concurrently, so anything a single test emits
    arrives interleaved with the others' results; the terminal is reserved for
    the verdicts.  stderr is the last resort for text that reached no sink.
    """
    if not write_run_console(text):
        sys.stderr.write(text)
        sys.stderr.flush()


def read_proc_cmdline(pid: int | str) -> str:
    """Return pid's argv as one line, or "" when it cannot be read."""
    try:
        raw = Path(f"/proc/{pid}/cmdline").read_bytes().decode(errors="replace")
    except OSError:
        return ""
    return " ".join(arg for arg in raw.split("\0") if arg)


def _read_proc_field(path: Path, field: str) -> str:
    """Return a "Field:\tvalue" line's value from a /proc status file, or "?"."""
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return "?"
    for line in text.splitlines():
        if line.startswith(f"{field}:"):
            return line.split(":", 1)[1].strip()
    return "?"


def describe_pids(pids: list[str] | list[int]) -> str:
    """Return one identifying line per pid: comm, state, parent, argv.

    Read before the signal goes out: afterwards the process is gone and its pid
    alone says nothing about what was killed.  Never raises.
    """
    lines = []
    for pid in pids:
        status = Path(f"/proc/{pid}/status")
        lines.append(f"    pid {pid} comm={_read_proc_field(status, 'Name')} "
                     f"state={_read_proc_field(status, 'State')} "
                     f"ppid={_read_proc_field(status, 'PPid')} "
                     f"cmd={read_proc_cmdline(pid) or '?'}")
    return "\n".join(lines) + "\n" if lines else "    (none)\n"


def write_kill_log(logs: Path | None, text: str) -> None:
    """Append a kill record to logs/kills.out and to the run log; never raises.

    Not printed: a teardown SIGKILL per test explains nothing and would bury
    the result lines.  stderr is the last resort for a record that reached
    neither file -- logs None (a kill with no test context, e.g. the run-wide
    interrupt), a logs dir already removed (a passing test's scratch dir is
    deleted before its cgroup leaf is reaped), and no mirror to fall back on.
    """
    wrote = False
    if logs is not None and logs.is_dir():
        path = logs / _KILL_LOG_FILE
        try:
            with path.open("a") as fh:
                fh.write(text)
            wrote = True
        except OSError as exc:
            sys.stderr.write(f"could not write {path}: {exc}\n")
    if write_run_console(text):
        wrote = True
    if not wrote:
        sys.stderr.write(text)
        sys.stderr.flush()


def record_kill(logs: Path | None, action: str,
                pids: list[str] | list[int]) -> None:
    """Log an about-to-be-sent signal together with every process it covers."""
    pids = list(pids)
    write_kill_log(logs,
                   f"kill: {action}: {len(pids)} process(es)\n"
                   + describe_pids(pids))


def pgroup_pids(pgid: int) -> list[str]:
    """Return every pid in process group pgid; never raises.

    /proc/<pid>/stat field 5 is the process group, but the comm field ahead of
    it may itself contain spaces and parentheses, so the split starts past the
    last ')'.
    """
    found: list[str] = []
    try:
        pids = [entry.name for entry in Path("/proc").iterdir()
                if entry.name.isdigit()]
    except OSError:
        return found
    for pid in sorted(pids, key=int):
        try:
            line = Path(f"/proc/{pid}/stat").read_text(errors="replace")
        except OSError:
            continue  # exited between the scan and the read
        # After the comm field: state, ppid, pgrp -> index 2.
        fields = line[line.rfind(")") + 1:].split()
        if len(fields) > 2 and fields[2] == str(pgid):
            found.append(pid)
    return found


def stream_output(
    proc: subprocess.Popen,
    log_path: Path,
    echo_fn: Callable[[str], None] | None = None,
) -> None:
    """Read proc.stdout line by line into log_path; call echo_fn per line when set.

    Opens the log in append mode so a reproduction preamble written before the
    process starts (see write_log_preamble) is preserved.
    """
    with log_path.open("ab") as log:
        assert proc.stdout is not None
        for line in iter(proc.stdout.readline, b""):
            log.write(line)
            log.flush()
            if echo_fn:
                echo_fn(line.decode(errors="replace").rstrip())


# Env keys worth showing in a reproduction snippet: the vars the runner injects
# plus the few inherited vars a niova binary actually needs to run.  Everything
# else (locale, LS_COLORS, DISPLAY, ssh/agent vars, ...) is inherited shell
# noise that only buries the relevant command.
_REPRO_ENV_PREFIXES = ("NIOVA", "EXTRA_", "TEST_")
_REPRO_ENV_NAMES = frozenset({
    "BUILD_DIR", "RVAL", "PATH", "LD_LIBRARY_PATH",
    "ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS", "TSAN_OPTIONS",
})


def repro_env(env: dict[str, str],
              always_keys: set[str] | None = None) -> dict[str, str]:
    """Subset of env worth emitting in a reproduction snippet.

    always_keys forces extra keys to be kept regardless of the allowlist.
    """
    always = always_keys or set()
    return {
        key: val for key, val in env.items()
        if key in always or key in _REPRO_ENV_NAMES
        or key.startswith(_REPRO_ENV_PREFIXES)
    }


@dataclasses.dataclass
class _ReproEntry:
    """One recorded command launch for repro.sh."""

    label: str
    argv: list[str]
    cwd: Path
    env: dict[str, str]
    always_keys: set[str]
    background: bool


def repro_entry_lines(entry: _ReproEntry, shared: dict[str, str]) -> list[str]:
    """Format one recorded command as shell lines for repro.sh.

    Emits only the env keys whose value differs from the shared export block so
    each command shows just its own deltas.
    """
    shown = repro_env(entry.env, entry.always_keys)
    inline = {key: val for key, val in shown.items()
              if shared.get(key) != val}
    prefix = "".join(f"{key}={shlex.quote(inline[key])} "
                     for key in sorted(inline))
    command = " ".join(shlex.quote(str(arg)) for arg in entry.argv)
    lines = [
        f"# ---- {entry.label} ----",
        prefix + command + (" &" if entry.background else ""),
    ]
    if entry.background:
        lines.append("# (runner waited for readiness before continuing)")
    lines.append("")
    return lines


class ReproRecorder:
    """Collects every command the runner launches for one test and writes a
    consolidated, copy-pasteable repro.sh manifest.

    The runner cannot see the commands the test script issues itself; those
    are captured separately in script.xtrace.
    """

    def __init__(self) -> None:
        """Start with an empty list of recorded launches."""
        self._entries: list[_ReproEntry] = []

    def add(self, label: str, argv: list[str], cwd: Path, env: dict[str, str],
            always_keys: set[str], background: bool) -> None:
        """Record one command launch (copies argv/env so later mutation is safe)."""
        self._entries.append(
            _ReproEntry(label, list(argv), cwd, dict(env),
                        set(always_keys), background)
        )

    def write(self, path: Path) -> None:
        """Write the recorded launches to path as a runnable repro.sh manifest."""
        if not self._entries:
            return
        # The last entry is the script launch; its env is the superset, so use
        # it as the shared export block.  Each command then emits only the keys
        # that differ from that block.
        script_entry = self._entries[-1]
        shared = repro_env(script_entry.env)
        out = [
            "#!/bin/bash",
            f"# Reproduction manifest for {script_entry.label}",
            "# Generated by run-system-tests.py -- commands in launch order.",
            "# The commands the script itself ran are traced in script.xtrace.",
            "set -u",
            f"cd {shlex.quote(str(script_entry.cwd))}",
            "",
            "# ---- shared environment (inherited shell env omitted) ----",
        ]
        out += [f"export {key}={shlex.quote(shared[key])}"
                for key in sorted(shared)]
        out.append("")
        for entry in self._entries:
            out += repro_entry_lines(entry, shared)
        path.write_text("\n".join(out) + "\n")


def write_log_preamble(
    log_path: Path,
    label: str,
    argv: list[str],
    cwd: Path,
    env: dict[str, str],
    always_keys: set[str] | None = None,
    recorder: ReproRecorder | None = None,
    background: bool = False,
) -> None:
    """Write a copy-pasteable, commented reproduction snippet to log_path.

    The exact argv, working directory, and the niova-relevant subset of the
    environment (see repro_env) are emitted as a shell command so a failing
    run can be reproduced by stripping the leading '# ' from each line; the
    inherited shell environment is omitted to keep the command readable.
    Truncates the file; process output is appended afterwards by stream_output
    (append mode).  When recorder is set, the same launch is also appended to
    the per-test repro.sh manifest.
    """
    shown = repro_env(env, always_keys)
    lines = [
        f"# --- {label} ---",
        f"# cd {shlex.quote(str(cwd))}",
        "# env \\",
    ]
    for key in sorted(shown):
        lines.append(f"#   {key}={shlex.quote(shown[key])} \\")
    command = " ".join(shlex.quote(str(arg)) for arg in argv)
    lines.append(f"#   {command}")
    lines.append("# (inherited shell environment omitted)")
    log_path.write_text("\n".join(lines) + "\n\n")
    if recorder is not None:
        recorder.add(label, argv, cwd, env, always_keys or set(), background)


# --- spec parsing ----------------------------------------------------------

@dataclasses.dataclass
class TestSpec:
    """A test's resource requirements parsed from its comment header."""

    timeout: float
    require_env: list[str] = dataclasses.field(default_factory=list)
    expect_failure: bool = False  # EXPECT: fail -- inverted only under --selftest


def parse_directive(line: str) -> tuple[str, str] | None:
    """Split a header comment into (KEYWORD, value).

    Recognises '# KEYWORD' and '# KEYWORD: value'.  Returns None for blank
    lines, shebangs, and plain comments whose text after '#' is not a single
    word (i.e. contains spaces before the first colon).
    """
    stripped = line.strip()
    if not stripped.startswith('#') or stripped.startswith('#!'):
        return None
    content = stripped[1:].strip()
    keyword, _, value = content.partition(':')
    keyword = keyword.strip()
    if not keyword or ' ' in keyword:
        return None
    return keyword.upper(), value.strip()


# ${VAR} or ${VAR:-default} inside a header directive value.  The default may
# not itself contain '}' — nested expansion is not supported and not needed.
_DIRECTIVE_VAR_RE = re.compile(
    r"\$\{([A-Za-z_][A-Za-z0-9_]*)(?::-([^}]*))?\}"
)

# Keywords parse_header acts on.  Expansion is limited to these so that a plain
# prose comment that merely looks like a directive ('# TODO: fix ${foo}') stays
# silently ignored instead of failing the whole test's discovery.
_EXPANDING_DIRECTIVES = frozenset({"REQUIRE_ENV", "TIMEOUT", "EXPECT"})


def expand_directive_value(value: str, keyword: str, path: Path) -> str:
    """Expand ${VAR} / ${VAR:-default} in a directive value from the runner's env.

    Follows shell ':-' semantics: the default applies when the variable is
    unset OR set-empty.  A bare ${VAR} that is unset is an error rather than a
    silent expansion to empty, which would turn '# TIMEOUT: ${T}' into a
    confusing float parse failure far from the real cause.
    """
    def replace(match: re.Match) -> str:
        name, default = match.group(1), match.group(2)
        current = os.environ.get(name)
        if default is not None:
            return current if current else default
        if current is None:
            raise ValueError(
                f"{path}: {keyword} references ${{{name}}}, which is unset; "
                f"set it or give a default as ${{{name}:-<default>}}"
            )
        return current

    return _DIRECTIVE_VAR_RE.sub(replace, value)


def parse_header(path: Path) -> TestSpec:
    """Read the comment header at the top of a test script and return its TestSpec.

    Scans lines until the first non-comment, non-shebang line.  Each
    recognised directive updates the spec; unrecognised comment lines are
    silently ignored.
    """
    require_env: list[str] = []
    timeout = DEFAULT_SCRIPT_TIMEOUT
    expect_failure = False
    with path.open() as f:
        for line in f:
            stripped = line.strip()
            if not stripped or stripped.startswith('#!'):
                continue
            if not stripped.startswith('#'):
                break  # first non-comment line ends the header

            directive = parse_directive(line)
            if directive is None:
                continue
            keyword, value = directive
            if keyword in _EXPANDING_DIRECTIVES:
                value = expand_directive_value(value, keyword, path)

            if keyword == "REQUIRE_ENV":
                require_env.extend(value.split())
            elif keyword == "TIMEOUT":
                timeout = float(value)
            elif keyword == "EXPECT":
                outcome = value.strip().lower()
                if outcome not in ("pass", "success", "fail", "failure"):
                    raise ValueError(f"{path}: unsupported EXPECT '{value}'")
                expect_failure = outcome in ("fail", "failure")

    return TestSpec(timeout=timeout, require_env=require_env,
                    expect_failure=expect_failure)


# --- cgroup isolation ------------------------------------------------------

def rmdir_quiet(path: Path) -> bool:
    """rmdir path, swallowing the error if it is missing or not yet empty."""
    try:
        path.rmdir()
        return True
    except OSError:
        return False


class CgroupManager:
    """Per-run cgroup-v2 tree that guarantees no test process survives an abort.

    One parent cgroup for the run holds one leaf per test.  Each test process is
    moved into its leaf at spawn time, so every descendant — client workloads,
    shell subprocesses the runner never sees — inherits it.  Writing
    cgroup.kill then reaps the whole subtree at once.  When the environment offers no writable delegated cgroup (e.g. CI
    under a non-delegated system.slice), create() returns a disabled manager
    and teardown falls back to process-group kills.
    """

    def __init__(self, root: Path | None):
        """Wrap a run cgroup root and seed the leaf counter; root None disables.

        A disabled manager (no delegated cgroup available) makes every
        operation a no-op so the runner falls back to process-group teardown.
        """
        self._root = root
        self._counter = 0
        self._lock = threading.Lock()

    @classmethod
    def create(cls, tag: str) -> "CgroupManager":
        """Create the run's cgroup root under the delegated base.

        Returns a disabled manager (warning to stderr) when no writable
        delegated cgroup-v2 base exists — e.g. CI running under a
        non-delegated system.slice, where mkdir hits EACCES.  Prunes empty
        leftovers from earlier aborted runs first.
        """
        base = cls.delegated_base()
        reason = None
        if base is None:
            reason = "no delegated cgroup-v2 base"
        else:
            # Prune empty leftovers from earlier aborted runs (their procs are
            # long dead); rmdir fails harmlessly on a concurrent run that still
            # has procs.
            for stale in base.glob("niova-tests-*"):
                for leaf in stale.glob("*"):
                    rmdir_quiet(leaf)
                rmdir_quiet(stale)
            root = base / tag
            try:
                root.mkdir()
            except OSError as exc:
                reason = f"cannot create {root}: {exc}"
            else:
                if (root / "cgroup.kill").exists():
                    return cls(root)
                rmdir_quiet(root)
                reason = f"cgroup.kill missing under {root}"
        print(f"cgroup isolation disabled ({reason}); using process-group "
              f"teardown only", file=sys.stderr, flush=True)
        return cls(None)

    @staticmethod
    def delegated_base() -> Path | None:
        """Locate the delegated cgroup node the runner may create children under.

        The runner's own cgroup is the 0:: entry of /proc/self/cgroup; its parent
        is the systemd-delegated node (e.g. user@<uid>.service).  Returns None
        when no cgroup-v2 unified hierarchy is available.
        """
        try:
            entries = Path("/proc/self/cgroup").read_text().splitlines()
        except OSError:
            return None
        rel = next(
            (line.split(":", 2)[2] for line in entries
             if line.split(":", 2)[:2] == ["0", ""]),
            "",
        )
        if not rel or rel == "/":
            return None
        own = Path("/sys/fs/cgroup") / rel.lstrip("/")
        return own.parent

    def new_leaf(self) -> Path | None:
        """Create and return a fresh per-test leaf cgroup, or None when disabled."""
        if self._root is None:
            return None
        with self._lock:
            n = self._counter
            self._counter += 1
        leaf = self._root / f"t{n:04d}"
        leaf.mkdir(exist_ok=True)
        return leaf

    @staticmethod
    def join(pid: int, leaf: Path | None) -> None:
        """Move pid — and thereby its future descendants — into leaf's cgroup."""
        if leaf is None:
            return
        (leaf / "cgroup.procs").write_text(str(pid))

    @staticmethod
    def read_procs(cgroup: Path) -> list[str]:
        """Return the pids in a cgroup's cgroup.procs; empty when unreadable."""
        try:
            return (cgroup / "cgroup.procs").read_text().split()
        except OSError:
            return []

    @classmethod
    def kill_leaf(cls, leaf: Path | None, logs: Path | None = None) -> None:
        """Kill everything in leaf and remove it; a no-op kill on an empty leaf.

        Whatever is still in cgroup.procs here is what the test leaked -- a
        workload's children, a stray daemon.  The one write kills them all at
        once, so they are named first.
        """
        if leaf is None:
            return
        members = cls.read_procs(leaf)
        if members:
            record_kill(logs, f"cgroup.kill {leaf}", members)
        try:
            (leaf / "cgroup.kill").write_text("1")
        except OSError:
            pass
        # cgroup.kill reaps asynchronously; rmdir needs the procs gone first.
        for _ in range(50):
            if rmdir_quiet(leaf):
                return
            time.sleep(0.02)
        survivors = cls.read_procs(leaf)
        write_kill_log(logs, f"kill: {len(survivors)} process(es) survived "
                             f"cgroup.kill {leaf}\n" + describe_pids(survivors))

    def kill_all(self) -> None:
        """Reap the whole run subtree with one write; called from the handler."""
        if self._root is None:
            return
        members = [pid for leaf in sorted(self._root.glob("*"))
                   for pid in self.read_procs(leaf)]
        record_kill(None, f"cgroup.kill {self._root} (run interrupted)",
                    members)
        try:
            (self._root / "cgroup.kill").write_text("1")
        except OSError:
            pass

    def cleanup(self) -> None:
        """Remove the run's cgroup tree after a normal (non-aborted) run."""
        if self._root is None:
            return
        for leaf in self._root.glob("*"):
            rmdir_quiet(leaf)
        rmdir_quiet(self._root)


# Sentinel marking the re-exec'd child, so the systemd-run scope wrap is
# attempted at most once and can never loop.
REEXEC_SCOPE_ENV = "NIOVA_TESTS_SCOPED"


def reexec_under_user_scope_if_needed() -> None:
    """Re-exec under 'systemd-run --user --scope' when our cgroup parent is a
    non-delegated slice, so per-run cgroup isolation can be created.

    A plain login/ssh session lands in user-<uid>.slice/session-N.scope, whose
    parent user-<uid>.slice is root-owned: the run cgroup mkdir EACCESes there,
    and processes cannot be migrated into the delegated user@<uid> subtree
    either (the migration's common ancestor is the root-owned slice).  Letting
    the user manager fork us inside its subtree makes our parent a writable
    delegated node and fixes both.  A no-op (keeping the process-group teardown
    fallback) when already re-exec'd, when the parent is already writable (e.g.
    launched from tmux), or when no usable user manager / systemd-run exists.
    """
    if os.environ.get(REEXEC_SCOPE_ENV):
        return
    base = CgroupManager.delegated_base()
    if base is None or os.access(base, os.W_OK):
        return
    if not os.environ.get("XDG_RUNTIME_DIR") or shutil.which("systemd-run") is None:
        return
    # Probe once: a user manager that cannot start scopes (no session bus, a
    # disabled --user instance) must leave the fallback in place rather than
    # abort the run when the execvp target fails.
    probe = subprocess.run(
        ["systemd-run", "--user", "--scope", "--collect", "--quiet", "true"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    if probe.returncode != 0:
        return
    os.environ[REEXEC_SCOPE_ENV] = "1"
    os.execvp(
        "systemd-run",
        ["systemd-run", "--user", "--scope", "--collect", "--quiet",
         sys.executable, *sys.argv],
    )


# --- runner ----------------------------------------------------------------

class NiovaTestRunner:
    """Orchestrates test execution: runs scripts and binaries, tears down."""

    def __init__(
        self,
        build_dir: Path,
        run_dir: Path,
        verbose: bool = False,
        print_err: bool = False,
        keep_logs: bool = False,
        remove_failed_data: bool = False,
        selftest: bool = False,
        log_lines: int = _PRINT_LOG_TAIL_LINES,
        compress_logs: bool = False,
    ):
        """Record the run's paths and options."""
        self.build_dir = build_dir
        self.run_dir = run_dir
        self.verbose = verbose
        self.print_err = print_err
        self.keep_logs = keep_logs
        self.remove_failed_data = remove_failed_data
        self.selftest = selftest
        self.log_lines = log_lines
        self.compress_logs = compress_logs
        self._print_lock = threading.Lock()
        self._interrupted = threading.Event()
        # The run's cgroup tree; created in run_all once a run actually starts.
        self._cgroup: CgroupManager | None = None

    @staticmethod
    def test_tag(script: Path) -> str:
        """Per-test tag for log-line prefixes: the filename stem."""
        return script.stem

    @staticmethod
    def test_label(script: Path, test_dir: Path) -> str:
        """A short, stable identifier like 'generic/foo.sh'.

        Does not resolve symlinks: a variant symlink and the base script it
        points at must get distinct labels so each variant reports separately.
        """
        try:
            return str(script.relative_to(test_dir))
        except ValueError:
            return script.name

    def log(self, msg: str) -> None:
        """Print one line to stdout under a lock (suppressed once interrupted)."""
        if self._interrupted.is_set():
            return
        with self._print_lock:
            print(msg, flush=True)

    def make_echo_fn(self, test_tag: str) -> Callable[[str], None]:
        """Return a log function with test_tag already bound, for passing to start()."""
        return lambda msg: self.log(f"[{test_tag}] {msg}")

    def vlog(self, test_tag: str, msg: str) -> None:
        """Log only when --verbose is active."""
        if self.verbose:
            self.log(f"[{test_tag}] {msg}")

    def base_env(self) -> dict[str, str]:
        """Build environment for child processes."""
        env = os.environ.copy()
        # Without halt_on_error a ubsan build reports and runs on, so the test
        # passes with the violation in it.  A caller-set value wins.
        env.setdefault("UBSAN_OPTIONS", "print_stacktrace=1:halt_on_error=1")
        return env

    def dump_logs_to_stderr(self, label: str, tmp_root: Path) -> None:
        """Dump the abort report (or tail) of every captured *.out log to stderr."""
        logs = sorted(p for p in logs_dir(tmp_root).rglob("*.out*")
                      if log_suffix(p) == ".out")
        if not logs:
            return
        with self._print_lock:
            sys.stderr.write(f"===== logs for FAIL {label} =====\n")
            for log_path in logs:
                sys.stderr.write(f"----- {log_path} -----\n")
                try:
                    sys.stderr.write(log_excerpt(log_path, self.log_lines))
                except OSError as exc:
                    sys.stderr.write(f"(could not read: {exc})\n")
                sys.stderr.write("\n")
            sys.stderr.write(f"===== end logs for {label} =====\n")
            sys.stderr.flush()

    def script_watchdog(
        self,
        script_proc: subprocess.Popen,
        timeout: float,
        test_tag: str,
        stop_event: threading.Event,
        state: dict[str, str | None],
        tmp_root: Path,
    ) -> None:
        """Kill the script's process group if the script runs longer than
        `timeout`.  Records the reason in `state`.
        """
        deadline = time.monotonic() + timeout
        while not stop_event.is_set():
            if time.monotonic() >= deadline:
                state["reason"] = f"timeout after {timeout:.0f}s"
                self.log(
                    f"[{test_tag}] watchdog: {state['reason']}; killing script"
                )
                self.kill_pgroup(script_proc, logs_dir(tmp_root),
                                 state["reason"])
                return
            # 0.2 s poll: responsive enough without burning CPU.
            if stop_event.wait(0.2):
                return

    @staticmethod
    def kill_pgroup(proc: subprocess.Popen, logs: Path | None = None,
                    reason: str = "") -> None:
        """SIGKILL the entire process group to catch any children the script spawned.

        The group's members are named first: they are whatever the test left
        running, and after the killpg nothing identifies them any more.
        """
        if proc.poll() is not None:
            return
        record_kill(logs,
                    f"SIGKILL process group {proc.pid}"
                    + (f" ({reason})" if reason else ""),
                    pgroup_pids(proc.pid))
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass

    def make_join_fn(
        self, leaf: Path
    ) -> Callable[[subprocess.Popen], None]:
        """Return a post-spawn hook that moves a process into this test's cgroup.

        Bound per test so concurrent tests place their processes in separate
        leaves.  If a signal already fired, the just-spawned process is killed
        by process group rather than left running uncaptured.
        """
        def join(proc: subprocess.Popen) -> None:
            if self._interrupted.is_set():
                self.kill_pgroup(proc, reason="run interrupted")
                return
            CgroupManager.join(proc.pid, leaf)
        return join

    def interrupt_handler(self, signum: int = 0, frame: object = None) -> None:
        """Signal handler: mark interrupted, reap the whole run subtree, exit."""
        self._interrupted.set()
        if self._cgroup is not None:
            self._cgroup.kill_all()
        print("\ninterrupted", file=sys.stderr, flush=True)
        drain_console()  # os._exit skips atexit; the mirrored tail is the record
        os._exit(1)

    # --- test driver ---

    def prepare_test(
        self,
        script: Path,
        test_dir: Path,
    ) -> tuple[str, str, Path, dict[str, str], Path]:
        """Create the per-test scratch directory and cgroup, build the env dict."""
        label = self.test_label(script, test_dir)
        test_tag = self.test_tag(script)
        category = (
            script.parent.relative_to(test_dir)
            if script.is_relative_to(test_dir)
            else Path(".")
        )
        base = self.run_dir / str(category)
        tmp_root = base / script.stem
        tmp_root.mkdir(parents=True, exist_ok=True)
        assert self._cgroup is not None  # set in run_all before any test starts
        cgroup_leaf = self._cgroup.new_leaf()

        logs = logs_dir(tmp_root)
        logs.mkdir(parents=True, exist_ok=True)

        env = self.base_env()
        # Build-tree root so unit/ wrappers can exec a C test binary directly.
        env["BUILD_DIR"] = str(self.build_dir)
        env["TEST_TMPDIR"] = str(tmp_root)
        env["TEST_LOGDIR"] = str(logs)
        # Mint a seed once per test so ported tests need not generate their own.
        env["RVAL"] = str(random.randint(1, (1 << 31) - 1))
        return label, test_tag, tmp_root, env, cgroup_leaf

    def launch_script_proc(
        self,
        script: Path,
        env: dict[str, str],
        logs: Path,
        label: str,
        recorder: ReproRecorder,
    ) -> subprocess.Popen:
        """Write the repro preamble + manifest, then launch the script under bash.

        bash runs with xtrace routed to script.xtrace via a dedicated fd: every
        command the script runs — the client/ctl invocations the runner cannot
        see — is captured with its variables expanded, without polluting the
        script's stdout in script.out.  Returns the live Popen.
        """
        script_log = logs / "script.out"
        xtrace_log = logs / "script.xtrace"
        # Record the logical launch ('bash <script>') for the preamble and the
        # repro.sh manifest, then write the manifest before the script starts so
        # it survives a hang/kill.  The actual run enables xtrace below.
        write_log_preamble(script_log, label, ["bash", str(script)], logs, env,
                           recorder=recorder)
        recorder.write(logs / "repro.sh")
        xfd = os.open(str(xtrace_log), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
        script_env = dict(env)
        script_env["BASH_XTRACEFD"] = str(xfd)
        script_env["PS4"] = "+ ${BASH_SOURCE##*/}:${LINENO}: "
        try:
            proc = subprocess.Popen(
                ["bash", "-o", "xtrace", str(script)],
                env=script_env,
                cwd=logs,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                start_new_session=True,
                pass_fds=(xfd,),
            )
        finally:
            os.close(xfd)  # the child holds its own copy until the script exits
        return proc

    def spawn_watchdog(
        self,
        proc: subprocess.Popen,
        timeout: float,
        test_tag: str,
        tmp_root: Path,
    ) -> tuple[threading.Thread, threading.Event, dict[str, str | None]]:
        """Start the watchdog thread; return (thread, stop_event, state)."""
        state: dict[str, str | None] = {"reason": None}
        stop_event = threading.Event()
        watchdog = threading.Thread(
            target=self.script_watchdog,
            args=(proc, timeout, test_tag,
                  stop_event, state, tmp_root),
            daemon=True,
        )
        watchdog.start()
        return watchdog, stop_event, state

    def run_test_case(
        self,
        script: Path,
        spec: TestSpec,
        tmp_root: Path,
        test_tag: str,
        label: str,
        env: dict[str, str],
        join_fn: Callable[[subprocess.Popen], None],
        recorder: ReproRecorder,
    ) -> tuple[int, str | None]:
        """Execute a test shell script from test/system-tests/ and wait for it
        to exit.

        A watchdog thread kills the script early if `spec.timeout` is
        exceeded.  Returns (exit_code, kill_reason) where
        kill_reason is None on a clean exit or a description string when the
        watchdog intervened.
        """
        logs = logs_dir(tmp_root)
        script_log = logs / "script.out"
        self.vlog(
            test_tag,
            f"running script: {label} (log: {script_log}) "
            f"timeout={spec.timeout:.0f}s",
        )
        proc = self.launch_script_proc(script, env, logs, label, recorder)
        join_fn(proc)  # capture the script's whole process subtree in the cgroup
        watchdog, stop_event, watchdog_state = self.spawn_watchdog(
            proc, spec.timeout, test_tag, tmp_root,
        )
        try:
            echo_fn = (lambda line: self.log(f"[{test_tag}] script: {line}")) \
                if self.verbose else None
            stream_output(proc, script_log, echo_fn)
            proc.wait()
        finally:
            stop_event.set()
            watchdog.join(timeout=2.0)
        kill_reason = watchdog_state["reason"]
        self.vlog(
            test_tag,
            f"script exited rc={proc.returncode}"
            + (f" ({kill_reason})" if kill_reason else ""),
        )
        return proc.returncode, kill_reason

    def finalize_pass(self, tmp_root: Path) -> tuple[str, str, list[str]]:
        """Clean up a passing test's scratch dir (unless --keep-logs); return PASS."""
        if not self.keep_logs:
            shutil.rmtree(tmp_root, ignore_errors=True)
            try:
                tmp_root.parent.rmdir()  # remove category dir when empty
            except OSError:
                pass  # non-empty (parallel tests still running) or gone
        return "PASS", "", []

    def compress_test_logs(self, tmp_root: Path) -> None:
        """gzip this test's surviving logs, once every process of it has exited.

        Called from the teardown path, after the cgroup leaf has been reaped, so
        there is no live writer and no buffered tail to lose.  Deliberately NOT
        done by piping each process through gzip: such a gzip is a descendant of
        the test, so `cgroup.kill` (CgroupManager.kill_leaf) SIGKILLs it along
        with everything else, truncating up to a buffer's worth of the log on
        exactly the watchdog/timeout path where the log is the only evidence.

        A passing test's scratch dir is already gone by now (finalize_pass), so
        this only ever compresses logs that are actually being kept.
        """
        if not self.compress_logs:
            return
        logs = logs_dir(tmp_root)
        if not logs.is_dir():
            return
        for path in sorted(logs.rglob("*")):
            if not is_log_file(path) or path.suffix == ".gz":
                continue
            try:
                with path.open("rb") as src, gzip.open(f"{path}.gz", "wb") as dst:
                    shutil.copyfileobj(src, dst)
                path.unlink()
            except OSError as exc:
                log_run(f"warning: failed to compress {path}: {exc}\n")

    def remove_failed_data_artifacts(self, tmp_root: Path) -> int:
        """Delete heavy failed-test backing files while preserving logs/repro data."""
        removed = 0
        for path in tmp_root.rglob("*.img"):
            if not path.is_file():
                continue
            try:
                path.unlink()
                removed += 1
            except OSError as exc:
                log_run(f"warning: failed to remove {path}: {exc}\n")
        return removed

    def fail_details(
        self,
        label: str,
        tmp_root: Path,
        kill_reason: str | None,
    ) -> list[str]:
        """Assemble the indented detail lines for a failed test (and dump logs)."""
        details: list[str] = []
        if kill_reason:
            details.append(f"  watchdog: {kill_reason}")
        details.append(f"  logs: {logs_dir(tmp_root)}")
        if self.print_err:
            self.dump_logs_to_stderr(label, tmp_root)
        if self.remove_failed_data:
            removed = self.remove_failed_data_artifacts(tmp_root)
            details.append(
                f"  failed data removed (--remove-failed-data): {removed} file(s)"
            )
        return details

    def run_binary_body(
        self, path: Path, test_dir: Path, label: str
    ) -> tuple[str, str, list[str]]:
        """Run a unit-test binary directly (no bash).

        A marker is a non-#! file whose stem names a build-tree binary at
        build_dir/test/<stem>.  The binary runs under the test's cgroup leaf
        with the env prepare_test() builds.
        """
        binary = self.build_dir / "test" / path.stem
        if not binary.is_file():
            return "FAIL", f": binary not found: {binary}", []
        _, test_tag, tmp_root, env, cgroup_leaf = self.prepare_test(path, test_dir)
        join_fn = self.make_join_fn(cgroup_leaf)
        logs = logs_dir(tmp_root)
        script_log = logs / "script.out"
        argv = [str(binary)]
        write_log_preamble(script_log, label, argv, logs, env)
        self.vlog(test_tag, f"start: binary={binary} dir={tmp_root}")
        kill_reason = None
        try:
            proc = subprocess.Popen(
                argv, env=env, cwd=logs,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            join_fn(proc)  # capture the binary's subtree in the cgroup leaf
            # stream_output blocks until stdout EOF (process exit); run it in a
            # reader thread so this worker thread can wait with a deadline and
            # kill on expiry.
            reader = threading.Thread(
                target=stream_output, args=(proc, script_log, None), daemon=True
            )
            reader.start()
            reader.join(timeout=DEFAULT_SCRIPT_TIMEOUT)
            if reader.is_alive():
                kill_reason = f"timeout ({DEFAULT_SCRIPT_TIMEOUT:.0f}s)"
                record_kill(logs, f"SIGKILL {binary.name} ({kill_reason})",
                            [proc.pid])
                proc.kill()
            proc.wait()
            reader.join()
            ok = proc.returncode == 0 and kill_reason is None
            if ok:
                return self.finalize_pass(tmp_root)
            details = self.fail_details(label, tmp_root, kill_reason)
            return "FAIL", f" (rc={proc.returncode})", details
        except Exception as exc:
            details = self.fail_details(label, tmp_root, kill_reason)
            return "FAIL", f": {exc}", details
        finally:
            self._cgroup.kill_leaf(cgroup_leaf, logs)
            # After the reap: no writer left to truncate, and the logs above
            # were already dumped to stderr uncompressed.
            self.compress_test_logs(tmp_root)

    def resolve_variant_args(
        self, script: Path, env: dict[str, str]
    ) -> None:
        """Decode a variant symlink's encoded args into the env.

        Both the server opts and the client args are exposed to the script as
        EXTRA_SERVER_OPTS / EXTRA_CLIENT_ARGS.
        """
        server_opts, client_args = parse_symlink_params(script)
        env["EXTRA_SERVER_OPTS"] = " ".join(server_opts)
        env["EXTRA_CLIENT_ARGS"] = " ".join(client_args)

    def run_test_body(
        self, script: Path, test_dir: Path, label: str
    ) -> tuple[str, str, list[str]]:
        """Run one test; return (status, suffix, detail_lines).

        status is 'PASS'/'FAIL'/'SKIP'.  suffix is the extra text that follows
        the label on the status line (e.g. ' (rc=1)').  detail_lines are the
        indented follow-up lines (watchdog/logs).  run_test emits the
        single status line — so it can append the elapsed time — then the
        detail lines, keeping them below their header.
        """
        # An extensionless marker (a '# ...' comment, no '#!') names a build-tree
        # binary the runner execs directly; a '#!' script falls through to the
        # header logic below.
        try:
            with script.open("rb") as fh:
                is_script = fh.read(2) == b"#!"
        except OSError as exc:
            return "FAIL", f": cannot open {script}: {exc}", []
        if not is_script:
            return self.run_binary_body(script, test_dir, label)

        try:
            spec = parse_header(script)
        except Exception as exc:
            return "FAIL", f": header parse error: {exc}", []

        # REQUIRE_ENV gate: skip before creating the scratch dir so a missing
        # external service costs no work.
        missing = [name for name in spec.require_env if not os.environ.get(name)]
        if missing:
            return "SKIP", f" (unset env: {' '.join(missing)})", []

        # prepare_test recomputes the same label; discard it and keep the one
        # the caller already holds for the status line.
        _, test_tag, tmp_root, env, cgroup_leaf = self.prepare_test(
            script, test_dir,
        )
        join_fn = self.make_join_fn(cgroup_leaf)
        recorder = ReproRecorder()
        # A variant symlink encodes extra arguments in its name; they reach the
        # script as env vars.
        self.resolve_variant_args(script, env)

        self.vlog(test_tag, f"start: script={script} dir={tmp_root}")

        try:
            script_rc, kill_reason = self.run_test_case(
                script, spec, tmp_root, test_tag, label, env, join_fn,
                recorder,
            )

            clean = kill_reason is None
            # A clean exit 77 is the script skipping itself at runtime; report
            # SKIP, not FAIL.  Gate on `clean` so a watchdog kill stays a real
            # failure even if the script happened to exit 77.
            if script_rc == SKIP_EXIT_CODE and clean:
                self.finalize_pass(tmp_root)  # for the scratch-dir cleanup side effect
                return "SKIP", f" (exit {SKIP_EXIT_CODE})", []
            # Under --selftest an `# EXPECT: fail` test must fail (cleanly) to
            # pass: this is how the self-test exercises the runner's FAIL path.
            # A hang is never a pass, whatever the expectation.
            want_rc_zero = not (self.selftest and spec.expect_failure)
            ok = ((script_rc == 0) == want_rc_zero) and clean
            # In self-test mode spell out the expected vs actual outcome on the
            # status line, so a PASS that really means "failed as required" is
            # not mistaken for a test that ran clean.
            if self.selftest:
                expected = "fail" if spec.expect_failure else "pass"
                if kill_reason is not None:
                    got = f"killed ({kill_reason})"
                elif script_rc == 0:
                    got = "pass"
                else:
                    got = f"fail (rc={script_rc})"
                suffix = f" (expected {expected}, got {got})"
                if ok:
                    self.finalize_pass(tmp_root)  # for the cleanup side effect
                    return "PASS", suffix, []
                return "FAIL", suffix, self.fail_details(
                    label, tmp_root, kill_reason
                )
            if ok:
                return self.finalize_pass(tmp_root)
            details = self.fail_details(label, tmp_root, kill_reason)
            return "FAIL", f" (rc={script_rc})", details
        except Exception as exc:
            # A launch failure aborts before run_test_case wrote the manifest;
            # emit a partial one from whatever was recorded.
            recorder.write(logs_dir(tmp_root) / "repro.sh")
            # kill_reason is None: nothing was watchdog-killed on this path.
            details = self.fail_details(label, tmp_root, None)
            return "FAIL", f": {exc}", details
        finally:
            # Reaps anything the test leaked and removes the now-empty leaf.
            self._cgroup.kill_leaf(cgroup_leaf, logs_dir(tmp_root))
            # After the reap: no writer left to truncate, and the logs above
            # were already dumped to stderr uncompressed.
            self.compress_test_logs(tmp_root)

    def run_test(self, script: Path, test_dir: Path) -> str:
        """Run one test and emit its PASS/FAIL/SKIP line with elapsed time."""
        label = self.test_label(script, test_dir)
        t0 = time.monotonic()
        status, suffix, details = self.run_test_body(script, test_dir, label)
        self.log(f"{status} {label}{suffix} : {int(time.monotonic() - t0)}s")
        for line in details:
            self.log(line)
        return status

    # --- discovery / orchestration ---

    @staticmethod
    def discover(test_dir: Path) -> list[Path]:
        """Discover runnable tests: one .sh script or unit-binary marker per category.

        lib/ holds template scripts reached only through variant symlinks (and
        the helpers those tests source), never run on their own; *.pyc and other
        extensions are not tests.
        """
        return sorted(
            p for p in test_dir.glob("*/*")
            if p.parent.name != "lib"
            and p.is_file()
            and p.suffix in ("", ".sh")
        )

    def print_summary(self, results: dict[str, str]) -> list[str]:
        """Print the pass/skip/fail summary; return the list of failed labels."""
        passed = [n for n, s in results.items() if s == "PASS"]
        failed = [n for n, s in results.items() if s == "FAIL"]
        skipped = [n for n, s in results.items() if s == "SKIP"]
        self.log("")
        summary = f"== {len(passed)}/{len(results)} passed"
        if skipped:
            summary += f", {len(skipped)} skipped"
        self.log(summary + " ==")
        if skipped:
            self.log("skipped:")
            for name in skipped:
                self.log(f"  {name}")
        if failed:
            self.log("failed:")
            for name in failed:
                label_path = Path(name)
                logs = self.run_dir / str(label_path.parent) / label_path.stem / "logs"
                self.log(f"  {name}")
                self.log(f"    logs: {logs}")
        return failed

    def cleanup_run_dir(self) -> None:
        """Remove the run dir after an all-pass run that left nothing behind.

        Per-test cleanup already removed each passing test's tmp_root and its
        category dir when empty; this drops the now-empty run_dir itself.
        """
        # run_dir is created lazily by prepare_test; if every test was skipped
        # it never came into being and there is nothing to clean up.
        if not self.run_dir.exists():
            return
        # Nothing failed, so the mirrored console has nothing left to explain;
        # keeping it would also make the run dir outlive every clean run.
        (self.run_dir / _RUN_CONSOLE_FILE).unlink(missing_ok=True)
        for subdir in list(self.run_dir.iterdir()):
            try:
                subdir.rmdir()
            except OSError:
                pass
        try:
            self.run_dir.rmdir()
        except OSError:
            pass

    def run_all(
        self,
        scripts: list[Path],
        test_dir: Path,
        random_order: bool,
        parallel: int,
        repeat: int = 1,
    ) -> int:
        """Run all selected tests in a thread pool; print the summary; return exit code.

        With repeat > 1 the tests are run up to repeat passes, all sharing this
        run's single log root and cgroup tree.  A test that fails in one pass is
        dropped from later passes (SKIPs stay); the loop stops early once no
        tests remain.  The exit code is non-zero if any test failed in any pass.
        """
        if random_order:
            random.shuffle(scripts)
        if not scripts:
            self.log("no tests selected")
            return 0

        def run(script: Path) -> tuple[str, str]:
            if self._interrupted.is_set():
                return self.test_label(script, test_dir), "SKIP"
            status = self.run_test(script, test_dir)
            return self.test_label(script, test_dir), status

        self._cgroup = CgroupManager.create(f"niova-tests-{os.getpid()}")
        old_sigint = signal.signal(signal.SIGINT, self.interrupt_handler)
        old_sigterm = signal.signal(signal.SIGTERM, self.interrupt_handler)
        overall_failed: set[str] = set()
        remaining = list(scripts)
        try:
            for iteration in range(repeat):
                if not remaining:
                    self.log(f"no tests remaining after {iteration} pass(es); stopping")
                    break
                if repeat > 1:
                    self.log(f"== pass {iteration + 1}/{repeat} "
                             f"({len(remaining)} tests) ==")
                results: dict[str, str] = {}
                with ThreadPoolExecutor(max_workers=parallel) as executor:
                    try:
                        for name, status in executor.map(run, remaining):
                            results[name] = status
                    except KeyboardInterrupt:
                        self.interrupt_handler()
                failed = set(self.print_summary(results))
                overall_failed |= failed
                # Drop tests that failed this pass so later passes narrow down
                # toward the still-passing set.
                remaining = [
                    p for p in remaining
                    if self.test_label(p, test_dir) not in failed
                ]
        except KeyboardInterrupt:
            self.interrupt_handler()
        finally:
            signal.signal(signal.SIGINT, old_sigint)
            signal.signal(signal.SIGTERM, old_sigterm)
            self._cgroup.cleanup()

        # Exit non-zero only on real failures; a SKIP is not a failure.
        if not overall_failed and not self.keep_logs:
            self.cleanup_run_dir()
        return 0 if not overall_failed else 1


# --- post-processing -------------------------------------------------------

# Only files with these extensions are printed by print_logged_errors; binary
# artifacts (core dumps, disk images) and the generated repro.sh are skipped.
# Matched through a trailing .gz, so --compress-logs output stays readable.
_PRINT_LOG_EXTENSIONS = frozenset({".out", ".xtrace"})


def log_suffix(path: Path) -> str:
    """The log's own extension, seeing through a trailing '.gz'.

    'script.out' and 'script.out.gz' both yield '.out', so a reader need not
    care whether --compress-logs ran.
    """
    return Path(path.stem).suffix if path.suffix == ".gz" else path.suffix


def is_log_file(path: Path) -> bool:
    """True for a readable text log (compressed or not), not a binary artifact."""
    return path.is_file() and log_suffix(path) in _PRINT_LOG_EXTENSIONS


def read_log_text(path: Path) -> str:
    """Read a log whole, transparently decompressing a '.gz' written at teardown."""
    if path.suffix == ".gz":
        with gzip.open(path, "rt", errors="replace") as handle:
            return handle.read()
    return path.read_text(errors="replace")


# A crash/abort report whose backtrace a reader needs in full: a sanitizer
# block, a UBSAN diagnostic, a niova FATAL log line (NIOVA_ASSERT / FATAL_IF /
# the "exiting on signal" handler), or a stack frame emitted by any of them.
_ABORT_SIGNATURES = re.compile(
    r"#\d+\s+0x[0-9a-fA-F]+"                              # stack frame
    r"|:fatal:"                                           # niova FATAL log line
    r"|(?:Address|Leak|Thread|UndefinedBehavior)Sanitizer"
    r"|runtime error:"                                    # UBSAN diagnostic
    r"|SUMMARY:"                                          # sanitizer summary
)
# Lines kept above the first and below the last abort match, so the report's
# lead-in and trailing aftermath survive even when the rest is truncated.
_ABORT_CONTEXT_BEFORE = 3
_ABORT_CONTEXT_AFTER = 10


def log_excerpt(path: Path, tail: int) -> str:
    """Return the salient slice of a log: its abort report (if any) plus tail.

    When the file holds a crash/abort report, return the whole region from the
    first to the last matching line -- with a few lines of lead-in and at least
    `_ABORT_CONTEXT_AFTER` below -- so an arbitrarily long backtrace survives
    the dump intact, followed by the file's last `tail` lines for end-state.
    Falls back to the last `tail` lines when no report is present.
    """
    lines = read_log_text(path).splitlines(keepends=True)
    hits = [idx for idx, line in enumerate(lines) if _ABORT_SIGNATURES.search(line)]
    if not hits:
        return "".join(lines[-tail:])

    start = max(0, hits[0] - _ABORT_CONTEXT_BEFORE)
    end = min(len(lines), hits[-1] + 1 + _ABORT_CONTEXT_AFTER)
    excerpt = "".join(lines[start:end])
    # Append the file tail too when the report is not already at EOF; clamp the
    # tail start to `end` so the two windows never overlap.
    if end < len(lines):
        tail_start = max(end, len(lines) - tail)
        excerpt += "----- tail -----\n" + "".join(lines[tail_start:])
    return excerpt


def print_logged_errors(run_dir: Path, tail: int = _PRINT_LOG_TAIL_LINES) -> int:
    """Walk run_dir for surviving test directories and dump their logs to stderr.

    A directory under run_dir that still exists after a run is a failed test —
    the runner removes passing test directories unless --keep-logs was given.
    Files are emitted in a useful order: *.xtrace first, then *.out.  Each
    file's abort report (if any) is shown in full, else its last `tail` lines.
    Each logs dir is walked recursively, and each file named by its path
    relative to it.

    @return 1 if any logs were found, 0 if the directory is empty or absent.
    """
    failed_dirs = sorted(run_dir.glob("*/*/logs"))
    if not failed_dirs:
        print(f"no failed test logs found under {run_dir}", flush=True)
        return 0

    def sort_key(p: Path) -> tuple[int, str]:
        if log_suffix(p) == ".xtrace":
            return (0, str(p))
        return (1, str(p))

    for logs_dir_path in failed_dirs:
        tmp_root = logs_dir_path.parent
        label = f"{tmp_root.parent.name}/{tmp_root.name}"
        sys.stderr.write(f"===== logs for FAIL {label} =====\n")

        files = sorted(
            (p for p in logs_dir_path.rglob("*") if is_log_file(p)),
            key=sort_key,
        )
        for log_path in files:
            sys.stderr.write(f"----- {log_path.relative_to(logs_dir_path)} -----\n")
            try:
                sys.stderr.write(log_excerpt(log_path, tail))
            except OSError as exc:
                sys.stderr.write(f"(could not read: {exc})\n")
            sys.stderr.write("\n")

        sys.stderr.write(f"===== end logs for {label} =====\n\n")
        sys.stderr.flush()

    return 1


# --- CLI -------------------------------------------------------------------

# Categories discovered but excluded from the no-args default sweep.  Naming one
# positionally (e.g. 'selftest/foo') includes it.
OFF_BY_DEFAULT = frozenset({"selftest"})


def select_positional(
    all_tests: list[Path], positional: list[str], test_dir: Path
) -> list[Path]:
    """Resolve explicit positional selectors (paths, labels, categories, prefixes).

    Each selector may be a filesystem path, an exact label (with or without the
    .sh suffix), a whole category, or a 'category/<prefix>' multi-select.  Order
    and de-duplication follow first appearance.  Exits on a selector that
    matches nothing.
    """
    by_label = {
        NiovaTestRunner.test_label(p, test_dir): p for p in all_tests
    }

    def canonical(path: Path) -> Path:
        # Resolve the directory chain (collapsing '..' and any symlinked
        # parent dirs) but NOT a final symlink: a variant symlink must stay
        # itself so its parameter-encoding name survives, rather than
        # collapsing to the base script it points at and losing the params.
        return path.parent.resolve() / path.name

    by_path = {canonical(p): p for p in all_tests}
    selected: list[Path] = []
    seen: set[Path] = set()

    def add(paths: list[Path]) -> None:
        for path in paths:
            if path not in seen:
                seen.add(path)
                selected.append(path)

    for arg in positional:
        candidate = Path(arg)
        if candidate.is_file():
            # Map a filesystem path back to the discovered test so a variant
            # symlink keeps its identity; an arbitrary off-tree file is taken
            # as-is, still without dereferencing its final component.
            key = canonical(candidate)
            add([by_path.get(key, key)])
            continue
        if candidate.is_dir():
            # A path to a category directory (relative, multi-segment, or
            # absolute) selects that whole category; categories are the single
            # level under test_dir, so only the basename is significant.
            category = candidate.name
            matches = [p for p in all_tests if p.parent.name == category]
            if not matches:
                sys.exit(f"no test matches: {arg}")
            add(matches)
            continue
        if arg in by_label:
            add([by_label[arg]])
            continue
        # exact label without the .sh suffix
        if arg in {label.removesuffix(".sh") for label in by_label}:
            add([by_label[arg + ".sh"]])
            continue
        # category/<prefix> — a prefix multi-select; a bare or trailing-slash
        # category (empty prefix) selects the whole category.
        if "/" in arg:
            cat, _, stem = arg.partition("/")
            prefix = f"{cat}/{stem}"
            matches = [
                p for p in all_tests
                if NiovaTestRunner.test_label(p, test_dir).startswith(prefix)
            ]
        else:
            matches = [
                p for p in all_tests
                if p.name == arg or p.stem == arg
                or p.name.startswith(f"{arg}-")
            ]
        if not matches:
            sys.exit(f"no test matches: {arg}")
        add(matches)
    return selected


def select_default(
    all_tests: list[Path],
    test_dir: Path,
    filter_pat: str | None,
    enabled: frozenset[str],
) -> list[Path]:
    """Default sweep: all tests minus off-by-default categories, optionally filtered.

    Off-by-default categories are included only when toggled on (in `enabled`);
    an fnmatch `filter_pat` further narrows the result by label or basename.
    """
    default = [
        p for p in all_tests
        if p.parent.name not in OFF_BY_DEFAULT or p.parent.name in enabled
    ]
    if filter_pat:
        return [
            p for p in default
            if fnmatch.fnmatch(
                NiovaTestRunner.test_label(p, test_dir), filter_pat
            )
            or fnmatch.fnmatch(p.name, filter_pat)
        ]
    return default


def select_tests(
    test_dir: Path,
    positional: list[str],
    filter_pat: str | None,
    enabled: frozenset[str] = frozenset(),
) -> list[Path]:
    """Resolve the CLI test selection into an ordered list of test paths."""
    # Discovery is ungated so positional selection can name any category; the
    # off-by-default drop applies only to the no-args default sweep.
    all_tests = NiovaTestRunner.discover(test_dir)
    if positional:
        return select_positional(all_tests, positional, test_dir)
    return select_default(all_tests, test_dir, filter_pat, enabled)


def build_arg_parser() -> argparse.ArgumentParser:
    """Construct the command-line argument parser."""
    parser = argparse.ArgumentParser(
        description="Run niova-core system tests.",
    )
    parser.add_argument("--build-dir", help="build directory containing src/ and test/")
    parser.add_argument(
        "--filter",
        help="fnmatch pattern over 'category/script' labels (or basenames)",
    )
    parser.add_argument("--random-order", action="store_true")
    parser.add_argument(
        "--repeat", type=int, default=1, metavar="N",
        help="run the selected tests up to N passes, sharing one log dir; a "
             "test that fails is dropped from later passes and the run stops "
             "early once no tests remain (default: 1)",
    )
    parser.add_argument("--list", action="store_true", help="list selected tests and exit")
    parser.add_argument(
        "-j", "--jobs", type=int, default=1,
        help="number of tests to run concurrently (default: 1)",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true",
        help="stream script output to the console as it runs",
    )
    parser.add_argument(
        "--print-err", action="store_true",
        help="dump all captured logs to stderr when a test fails "
             "(useful in CI where the scratch directory is not accessible)",
    )
    parser.add_argument(
        "--print-logged-errors",
        metavar="DIR",
        help="post-processing mode: walk DIR for surviving (failed) test log "
             "directories and dump their captured logs to stderr; no tests are "
             "run and --build-dir is not required",
    )
    parser.add_argument(
        "--log-lines",
        type=int,
        default=_PRINT_LOG_TAIL_LINES,
        metavar="N",
        help=f"number of trailing lines of each log file to dump on failure "
             f"(default: {_PRINT_LOG_TAIL_LINES})",
    )
    parser.add_argument(
        "--compress-logs", action="store_true",
        help="gzip each kept test's logs at teardown (default off; CI's "
             "upload-artifact already zips the artifact)",
    )
    parser.add_argument(
        "--keep-logs", action="store_true",
        help="keep log directories for passing tests (failed tests always keep logs)",
    )
    parser.add_argument(
        "--remove-failed-data",
        action="store_true",
        help="after a failed test, remove heavy backing data files while keeping "
             "logs and repro metadata",
    )
    parser.add_argument(
        "--rundir",
        help="root directory for the test run (logs and scratch dirs) "
             "(default: $NIOVA_SYSTEM_TESTS_RUNDIR verbatim if set, else "
             "persisted config, then $TMPDIR, then /var/tmp, with a "
             "/niova-system-tests-<user>-<timestamp> leaf appended)",
    )
    parser.add_argument(
        "--persist-rundir",
        metavar="DIR",
        help="persist DIR as the run-dir base in ~/.config/niova/niova.conf "
             "and exit; later runs use it unless --rundir is given",
    )
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="select the selftest/ category (when no explicit selector is "
             "given) and judge each test against its '# EXPECT:' outcome -- an "
             "EXPECT: fail test passes only when it fails",
    )
    parser.add_argument("tests", nargs="*", help="explicit tests to run")
    return parser


# Persisted runner settings; --persist-rundir writes run_dir here so later runs
# pick it up without re-passing --rundir.
_CONFIG_SECTION = "system-tests"
_CONFIG_RUN_DIR_KEY = "run_dir"


def config_path() -> Path:
    """Path to the persisted runner config (~/.config/niova/niova.conf)."""
    return Path.home() / ".config" / "niova" / "niova.conf"


def read_configured_run_dir() -> str | None:
    """Return the persisted run_dir, or None if unset or unreadable."""
    path = config_path()
    if not path.exists():
        return None
    parser = configparser.ConfigParser()
    try:
        parser.read(path)
    except (OSError, configparser.Error):
        return None
    value = parser.get(_CONFIG_SECTION, _CONFIG_RUN_DIR_KEY, fallback="").strip()
    return value or None


def write_configured_run_dir(run_dir: str) -> None:
    """Persist run_dir to the runner config, creating the config dir if needed."""
    path = config_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    parser = configparser.ConfigParser()
    parser.read(path)  # preserve any other keys already present
    if not parser.has_section(_CONFIG_SECTION):
        parser.add_section(_CONFIG_SECTION)
    parser.set(_CONFIG_SECTION, _CONFIG_RUN_DIR_KEY, run_dir)
    with path.open("w") as config_file:
        parser.write(config_file)


def resolve_run_dir(args: argparse.Namespace) -> Path:
    """Pick the run dir by precedence: --rundir, $NIOVA_SYSTEM_TESTS_RUNDIR,
    config, $TMPDIR, DEFAULT_RUN_DIR.

    --rundir and $NIOVA_SYSTEM_TESTS_RUNDIR are used verbatim as the root; the
    env var lets a Makefile-driven run (no --rundir) be pinned to a known dir
    for post-failure log analysis.  The persisted config value, $TMPDIR, and the
    DEFAULT_RUN_DIR fallback are bases: a niova-system-tests-<user>-<ts> leaf is
    appended so successive runs do not collide.
    """
    if args.rundir:
        return Path(args.rundir)
    env_rundir = os.environ.get("NIOVA_SYSTEM_TESTS_RUNDIR")
    if env_rundir:
        return Path(env_rundir)
    user = os.environ.get("USER") or os.environ.get("LOGNAME") or "unknown"
    timestamp = time.strftime("%y%m%d%H%M%S")
    leaf = f"niova-system-tests-{user}-{timestamp}"
    base = read_configured_run_dir() or os.environ.get("TMPDIR") or str(DEFAULT_RUN_DIR)
    return Path(base) / leaf


def raise_open_file_limit(target: int = OPEN_FILE_LIMIT) -> None:
    """Raise this process's RLIMIT_NOFILE soft limit toward `target`.

    Children (test binaries, scripts) inherit the raised limit.  Lifting
    the hard cap needs privilege, so an unprivileged caller is capped at the
    existing hard cap and warned if that is still below `target`.
    """
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    infinity = resource.RLIM_INFINITY
    if soft == infinity or soft >= target:
        return
    if hard == infinity or hard >= target:
        # Hard cap already allows it: raise only the soft limit (no privilege).
        resource.setrlimit(resource.RLIMIT_NOFILE, (target, hard))
        return
    # Hard cap below target: lifting it needs privilege.  Try, then fall back
    # to the hard cap and warn.
    try:
        resource.setrlimit(resource.RLIMIT_NOFILE, (target, target))
    except (ValueError, OSError):
        resource.setrlimit(resource.RLIMIT_NOFILE, (hard, hard))
        print(f"warning: open files limited to {hard} (hard cap); raise it "
              f"with 'ulimit -Hn {target}' for higher -j parallelism",
              file=sys.stderr, flush=True)


def main(argv: list[str] | None = None) -> int:
    """Parse arguments, select tests, and run them (or post-process logs)."""
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    if args.print_logged_errors:
        return print_logged_errors(Path(args.print_logged_errors), args.log_lines)

    if args.persist_rundir:
        write_configured_run_dir(args.persist_rundir)
        print(f"persisted run directory to {config_path()}", flush=True)
        return 0

    if args.jobs < 1:
        sys.exit("--jobs must be >= 1")

    if args.repeat < 1:
        sys.exit("--repeat must be >= 1")

    test_dir = Path(__file__).resolve().parent
    # --selftest runs only the selftest/ category (unless explicit selectors are
    # given); the verdict-inversion mode is keyed off the flag in the runner.
    selectors = args.tests or (["selftest/"] if args.selftest else [])
    scripts = select_tests(test_dir, selectors, args.filter)
    if args.random_order:
        random.shuffle(scripts)

    if args.list:
        for path in scripts:
            print(NiovaTestRunner.test_label(path, test_dir))
        return 0

    # Wrap the run in a systemd user scope when needed so per-test cgroup
    # isolation has a writable delegated parent; replaces this process when it
    # acts, returns unchanged when the parent is already delegated.
    reexec_under_user_scope_if_needed()

    # Parallel tests open many fds; raise RLIMIT_NOFILE so children avoid EMFILE
    # under -j.  (CI already raises it via prlimit; this covers local runs.)
    raise_open_file_limit()

    run_dir = resolve_run_dir(args)
    # Before the first line of the run: the artifact a failure is debugged from
    # is this directory, so the console has to be in it.
    mirror_console(run_dir / _RUN_CONSOLE_FILE)
    print(f"run directory: {run_dir}", flush=True)

    build_dir = find_build_dir(args.build_dir)
    runner = NiovaTestRunner(
        build_dir=build_dir,
        run_dir=run_dir,
        verbose=args.verbose,
        print_err=args.print_err,
        keep_logs=args.keep_logs,
        remove_failed_data=args.remove_failed_data,
        selftest=args.selftest,
        log_lines=args.log_lines,
        compress_logs=args.compress_logs,
    )

    return runner.run_all(
        scripts, test_dir=test_dir, random_order=False, parallel=args.jobs,
        repeat=args.repeat,
    )


if __name__ == "__main__":
    sys.exit(main())
