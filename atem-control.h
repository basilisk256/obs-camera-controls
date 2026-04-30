#pragma once

/* Direct in-process ATEM control for the Shot Presets OBS plugin.
 *
 * Implementation is C++ (BMD SDK is COM/C++) but the API is pure C so
 * it can be called from filter.c. A dedicated worker thread owns the
 * COM apartment + the IBMDSwitcher connection; the plugin's render /
 * frontend threads enqueue switch requests via atem_set_program_input
 * and return immediately.
 *
 * No external helper executable, no FNN runtime dependency, no HTTP.
 * The plugin opens its own connection to whatever USB ATEM is present
 * via the BMD ATEM Switchers SDK that the user has installed. */

#ifdef __cplusplus
extern "C" {
#endif

/* Spawn the worker thread, CoInitialize, attempt initial Connect. Safe
 * to call multiple times; second+ calls are no-ops. */
void atem_init(void);

/* Stop worker thread, disconnect, CoUninitialize. Called from
 * obs_module_unload. */
void atem_shutdown(void);

/* Queue a switch to the given program input (1-based). Returns
 * immediately; the actual SetProgramInput happens asynchronously on the
 * worker thread. input <= 0 is a no-op. */
void atem_set_program_input(int input);

/* 1 if currently connected to a switcher, 0 otherwise. Lock-free read. */
int atem_is_connected(void);

#ifdef __cplusplus
}
#endif
