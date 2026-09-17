# System Tests

Every test lives under `test/system-tests/<category>/` and is run by
`run-system-tests.py`, which `make check` invokes.  A test is either a shell
script (`<name>.sh`) or an extensionless marker naming a C test binary in the
build tree (`test/<name>`); the runner reports PASS, FAIL, or SKIP per test.

The runner is self-contained — it needs only a built tree and runs standalone.

## Running

`make check` runs the whole suite: the `check-system-tests` target invokes the
runner with `--build-dir $(abs_top_builddir)`, the `-jN` from `make -jN`
(falling back to `nproc`) and `--print-err`, so a failing test's logs are
dumped to stderr where CI can see them.

To run the runner directly:

```
test/system-tests/run-system-tests.py --build-dir <build-dir>
```

`<build-dir>` is the configured build tree; it must contain `test/`.
`NIOVA_BUILD_DIR` in the environment is an alternative to `--build-dir`.

Run a single test, or select a whole category:

```
test/system-tests/run-system-tests.py --build-dir <build-dir> unit/ec-test
test/system-tests/run-system-tests.py --build-dir <build-dir> unit/
```

With no positional selector the runner sweeps every category except
`selftest/`.  `--selftest` runs `selftest/`, judging each test against its
`# EXPECT:` line; `--list` prints the selection without running it.

## Layout

- `unit/` — one marker per C test binary; the marker is a one-line comment
  and the runner execs `<build-dir>/test/<marker name>`.
- `selftest/` — tests of the runner itself (`# EXPECT: fail` scripts).
- `lib/` — templates reached only through symlinks; never run directly.

## Adding a test

A C test binary: add it to `noinst_PROGRAMS` in `Makefile.am` and create
`test/system-tests/unit/<name>` holding one comment line.  Do not add it to
automake's `TESTS` — `all-local` refuses to build with it set, because only
`make check` would run it.

A shell script: `test/system-tests/<category>/<name>.sh`, executable, with an
optional comment header:

```
# REQUIRE_ENV: MINIO_ENDPOINT   SKIP unless every named var is set
# TIMEOUT: 60                   per-script timeout (default 1800)
# EXPECT: fail                  under --selftest, the test must fail to pass
```

A directive value may use `${VAR}` or `${VAR:-default}` from the runner's
environment.  The script sees `BUILD_DIR`, `TEST_TMPDIR`, `TEST_LOGDIR`,
`RVAL` (per-test seed) and, for a variant symlink named
`base___<server-opts>___<client-args>.sh`, `EXTRA_SERVER_OPTS` and
`EXTRA_CLIENT_ARGS`.  Exit 77 reports SKIP.

## Logs

Each run writes under a run directory (`--rundir`, `$NIOVA_SYSTEM_TESTS_RUNDIR`,
the value persisted with `--persist-rundir`, else `$TMPDIR` or `/var/tmp`, with
a `niova-system-tests-<user>-<timestamp>` leaf).  A passing test's directory is
removed unless `--keep-logs`; a failed test keeps `logs/script.out`,
`logs/script.xtrace` (bash trace), `logs/repro.sh` and `logs/kills.out`.
`--print-err` dumps a failed test's logs to stderr as it fails;
`--print-logged-errors <run-dir>` does the same afterwards.

## Sync with niova-block

`run-system-tests.py` is niova-block's runner with the server, ublk and nvme
handling removed.  A fix to the shared part is applied by hand in both trees.
