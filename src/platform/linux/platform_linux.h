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

