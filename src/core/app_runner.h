#pragma once

/// @file app_runner.h
/// @brief Arduino-like app runner for bm_sbc applications.
///
/// The app contract requires each application to define:
///   void setup(void);
///   void loop(void);
///
/// The runner calls setup() once, then calls loop() repeatedly
/// on a scheduler-friendly cadence.

/// App contract: called once at startup.
extern void setup(void);

/// App contract: called repeatedly after setup().
extern void loop(void);

/// Run the app (calls setup once, then loop repeatedly).
/// Does not return under normal operation.
void bm_sbc_app_run(void);

