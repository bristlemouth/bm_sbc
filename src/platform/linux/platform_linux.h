#pragma once

/// @file platform_linux.h
/// @brief Linux platform wrappers for bm_sbc.
///
/// Provides config partition, RTC, and DFU stubs for the Linux backend.

/// Initialize Linux platform services.
/// @return 0 on success, non-zero on failure
int platform_linux_init(void);

/// Set the directory used for config partition files.
/// Files are named config.user.bin, config.sys.bin, config.hw.bin within this
/// directory.  The directory is created if it does not exist.
/// Must be called before config_init() / any bm_config_read/write calls.
/// @param dir  Null-terminated directory path.
void platform_linux_set_cfg_dir(const char *dir);

// ---------------------------------------------------------------------------
// DFU support
// ---------------------------------------------------------------------------

/// Store argc/argv so execv() can restart the process after a DFU binary swap.
/// Also derives all DFU file paths from /proc/self/exe:
///   <exe>           — install path
///   <exe>.staging   — incoming binary is written here during transfer
///   <exe>.bak       — hard link to the previous binary (rollback target)
///   <dir>/dfu_pending.bin — serialised PlatformDfuMarker (noinit substitute)
/// Must be called once at the very top of main(), before anything else.
/// @param argc  Argument count from main().
/// @param argv  Argument vector from main().
void platform_linux_set_argv(int argc, char **argv);

/// Restore client_update_reboot_info from the DFU marker file (if present)
/// and handle automatic rollback if the boot-attempt counter exceeds the limit.
/// Must be called before bcmp_init() / bm_dfu_init() so the DFU state machine
/// sees DFU_REBOOT_MAGIC on the first run after a successful binary swap.
void platform_linux_dfu_restore_state(void);

/// Register a callback invoked immediately before execv() in both
/// set_pending_and_reset() and fail_update_and_reset().
/// Use this for application-level cleanup that must happen before the process
/// image is replaced (e.g. stopping systemd services started by sbc_command).
/// Only one callback is supported; calling this again replaces the previous one.
/// The callback must not call any bm_log functions (logging is already shut
/// down at the call site) but may call system() or other libc functions.
void platform_linux_set_pre_exec_cb(void (*cb)(void));

