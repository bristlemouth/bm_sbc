#pragma once

/// @file platform_linux.h
/// @brief Linux platform wrappers for bm_sbc.
///
/// Provides config partition, RTC, and DFU stubs for the Linux backend.

/// Initialize Linux platform services.
/// @return 0 on success, non-zero on failure
int platform_linux_init(void);

