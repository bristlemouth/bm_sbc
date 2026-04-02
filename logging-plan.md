# Logging Plan for bm_sbc

## Context

All bm_sbc logging is currently `printf()` via a `bm_debug()` macro, writing to stdout only. There is no file output, no log rotation, no severity levels, and no structured format. Multiple bm_sbc processes running simultaneously produce interleaved output with no way to separate them unless the caller redirects each to a different file. The goal is proper Linux system logging that works with standard sysadmin tooling.

## OpenTelemetry Evaluation

**Conclusion: adopt OTEL log data model format, do not use OTEL SDK.**

The opentelemetry-cpp SDK is inappropriate for the Pi Zero 2W:
- Pulls protobuf + gRPC/HTTP + abseil (~15 transitive deps, 20-40MB binary bloat)
- Thread pools and batching exporters consume meaningful CPU on the single-core BCM2710A1
- Massive CMake/cross-compile burden for zero runtime benefit at this scale

Instead: emit logs in an OTEL-compatible semi-structured text format. An OTEL Collector with a `filelog` receiver can ingest these logs via regex if OTEL infrastructure is ever deployed. Zero runtime cost — just `snprintf`.

## Log Format

Semi-structured, one line per entry, human-readable and grep-friendly:

```
2024-01-15T10:30:45.123456Z INFO  [multinode node=0x0123456789abcdef] stack initialized
2024-01-15T10:30:45.234567Z WARN  [multinode node=0x0123456789abcdef] vpd_send: flood peer 3 failed errno=111
```

Layout: `<ISO-8601 UTC timestamp>Z <SEVERITY 5-char padded> [<app_name> node=<0x hex16>] <message>`

Severity levels (mapping to OTEL SeverityNumber ranges):

| Level | Name  | Use |
|-------|-------|-----|
| 0 | TRACE | Protocol frame dumps |
| 1 | DEBUG | Neighbor events, pubsub, all existing bm_debug calls |
| 2 | INFO  | Startup, shutdown, configuration |
| 3 | WARN  | Recoverable errors, truncation warnings |
| 4 | ERROR | Unrecoverable errors, init failures |
| 5 | FATAL | Process-terminating errors |

Rationale for semi-structured over JSON: human-readable when tailing, parseable by grep/awk/OTEL filelog receiver, lower CPU than JSON serialization. JSON can be added later as `--log-format json`.

## Log Destination: Direct File I/O

**Decision: write directly to files in `/var/log/bm_sbc/`, not syslog.**

Why not syslog: `openlog()` ident is limited/static, rsyslog per-process routing needs system config, syslog strips structured fields.

Why direct files: each process gets a unique file (no collision), full format control, logrotate works natively, simple SIGHUP handler for rotation.

### File naming

```
/var/log/bm_sbc/<app_name>_<node_id_hex16>.log
```

Examples: `multinode_0000000000000001.log`, `example_00000000deadbeef.log`

Each process has a unique (app_name, node_id) pair.

### stdout preservation

- Also log to stdout when `BM_SBC_LOG_STDOUT=1` env var is set, or when stdout is a TTY (development)
- Production (non-TTY): file only by default

## Logging API

### New files: `src/core/bm_log.h` and `src/core/bm_log.c`

C-compatible API (bm_core is pure C):

```c
typedef enum {
    BM_LOG_TRACE = 0, BM_LOG_DEBUG = 1, BM_LOG_INFO  = 2,
    BM_LOG_WARN  = 3, BM_LOG_ERROR = 4, BM_LOG_FATAL = 5,
} BmLogLevel;

int  bm_log_init(const char *app_name, uint64_t node_id,
                 const char *log_dir, bool also_stdout);
void bm_log_set_level(BmLogLevel level);
void bm_log(BmLogLevel level, const char *fmt, ...) __attribute__((format(printf,2,3)));
void bm_log_reopen(void);   // called by SIGHUP handler
void bm_log_shutdown(void);
```

Convenience macros: `bm_log_trace(...)`, `bm_log_debug(...)`, `bm_log_info(...)`, `bm_log_warn(...)`, `bm_log_error(...)`, `bm_log_fatal(...)`.

Compile-time gate: `BM_LOG_MIN_LEVEL` strips lower-severity calls from release builds.

### bm_debug backward compatibility

Single-line change in `src/core/bm_config.h`:

```c
// Before:
#define bm_debug(format, ...) printf(format, ##__VA_ARGS__)
// After:
#include "bm_log.h"
#define bm_debug(format, ...) bm_log_debug(format, ##__VA_ARGS__)
```

This routes all ~225 bm_core `bm_debug()` calls through the new system at DEBUG severity, with zero changes to the bm_core submodule.

## Implementation Details

### bm_log.c internals (~200 lines)

- **Thread safety**: single `pthread_mutex_t` serializes writes. Lock duration ~5-10us (snprintf + fwrite + fflush). At <100 lines/sec, contention is negligible.
- **Timestamp**: `clock_gettime(CLOCK_REALTIME)` (vDSO on Pi, no syscall overhead) + `strftime` + manual microsecond append.
- **Buffer**: stack-local `char buf[1024]` per call. No heap allocation on log path. Messages >~900 chars truncated with `...`.
- **File I/O**: `fwrite()` + `fflush()` per line for immediate visibility.
- **SIGHUP**: handler sets `volatile sig_atomic_t` flag; checked inside mutex section of `bm_log()` to close/reopen file. Async-signal-safe.
- **Pre-init fallback**: if `bm_log()` called before `bm_log_init()`, falls back to `fprintf(stderr, ...)` with no structured prefix. Handles early CLI parsing errors.
- **Directory creation**: `bm_log_init()` attempts `mkdir(log_dir, 0755)` as fallback (may fail without root, in which case logs go to stdout-only with a stderr warning).

## CLI Changes to `runtime.cpp`

New flags:

| Flag | Default | Description |
|------|---------|-------------|
| `--log-dir <path>` | `/var/log/bm_sbc` | Log file directory |
| `--log-level <level>` | `info` | Minimum: trace/debug/info/warn/error/fatal |
| `--log-stdout` | auto (tty=yes) | Also log to stdout |

Env var overrides: `BM_SBC_LOG_DIR`, `BM_SBC_LOG_LEVEL`, `BM_SBC_LOG_STDOUT`.

Init ordering: parse CLI -> `bm_log_init()` -> register SIGHUP -> rest of stack init.

Pre-init `fprintf(stderr, ...)` calls for CLI parsing errors remain as-is (process terminates immediately on those errors; stderr is the correct Unix convention destination).

## Log Rotation

### `deploy/logrotate.d/bm_sbc`

```
/var/log/bm_sbc/*.log {
    daily
    rotate 7
    compress
    delaycompress
    missingok
    notifempty
    copytruncate
    maxsize 10M
}
```

`copytruncate` works even without SIGHUP. The SIGHUP handler is belt-and-suspenders for users preferring `create` + `postrotate`.

### Directory provisioning: `deploy/tmpfiles.d/bm_sbc.conf`

```
d /var/log/bm_sbc 0755 root root -
```

## Test Script Compatibility

The test script (`scripts/multinode_test.sh`) uses `grep -qF` (fixed-string substring match) on captured stdout. Since the new system only **prepends** a structured prefix to each line, all existing patterns (`"multinode app: setup"`, `"bcmp_seq="`, `"PUBSUB_RX from="`, `"NEIGHBOR_UP node="`, `"vpd: peer count 16 exceeds cap 15"`) remain valid substrings.

Only change: set `BM_SBC_LOG_STDOUT=1` in `start_node()` so test processes also write to stdout (which gets captured to the log file via redirection).

## Files to Create

| File | Purpose |
|------|---------|
| `src/core/bm_log.h` | Public logging API header |
| `src/core/bm_log.c` | Logging implementation |
| `deploy/logrotate.d/bm_sbc` | logrotate config |
| `deploy/tmpfiles.d/bm_sbc.conf` | systemd-tmpfiles log dir creation |

## Files to Modify

| File | Change |
|------|--------|
| `src/core/bm_config.h:10` | Redefine `bm_debug` to route through `bm_log_debug` |
| `src/core/runtime.cpp` | Add `--log-dir/level/stdout` flags, call `bm_log_init()`, register SIGHUP, migrate post-init log calls |
| `src/core/main.cpp` | Add `bm_log_shutdown()` call |
| `src/transports/uart_l2/uart_l2_transport.cpp` | Replace `fprintf(stderr, ...)` with `bm_log_error()`/`bm_log_warn()` |
| `apps/multinode/app_main.cpp` | Replace `printf()`+`fflush()` with `bm_log_info()`/`bm_log_debug()` |
| `apps/example/app_main.cpp` | Replace `printf()` with `bm_log_info()` |
| `CMakeLists.txt:60-71` | Add `src/core/bm_log.c` to `bm_sbc_core` sources |
| `scripts/multinode_test.sh:41-43` | Add `BM_SBC_LOG_STDOUT=1` env var |

## Implementation Sequence

1. Create `bm_log.h` + `bm_log.c`, add to CMakeLists.txt
2. Update `bm_config.h` to redirect `bm_debug` (functionally a no-op until init is called — falls through to stderr fallback)
3. Update `runtime.cpp` — CLI flags, `bm_log_init()`, SIGHUP handler
4. Migrate app files (multinode, example, uart_l2_transport)
5. Update test script with `BM_SBC_LOG_STDOUT=1`
6. Add deploy configs (logrotate, tmpfiles.d)

## Verification

1. `cmake --preset example && cmake --build --preset example` — builds cleanly
2. `cmake -B build -S . -DBUILD_ALL_APPS=ON && cmake --build build` — all apps build
3. Run `./scripts/multinode_test.sh` — all tests pass
4. Manual: run a process, verify log file appears in `/var/log/bm_sbc/` (or `--log-dir /tmp/test_logs`)
5. Manual: run two processes with different node IDs, verify separate log files
6. Manual: verify log lines match the documented format
7. Manual: `kill -HUP <pid>`, verify log file is reopened (rotate test)
