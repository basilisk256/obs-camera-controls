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

/* Queue a hardware mix (cross-fade) transition to the given input over
 * `duration_frames` ATEM frames (typically 30fps). Sets transition
 * style to Mix, sets preview input, then calls PerformAutoTransition.
 * Used for Fade-mode shot presets so the camera change is a hardware
 * crossfade instead of a hard cut. input <= 0 is a no-op; if duration
 * <= 0 the function falls back to a hard cut. */
void atem_perform_mix_transition(int input, int duration_frames);

/* 1 if currently connected to a switcher, 0 otherwise. Lock-free read. */
int atem_is_connected(void);

/* The most recent program input we asked the ATEM to switch to (via
 * either atem_set_program_input or atem_perform_mix_transition).
 * Initialised to whatever GetProgramInput reports on connect. -1 if
 * never set / not connected. Used to detect "same-input fade" so we
 * can skip a no-op ATEM mix and let the OBS-side cross-dissolve run
 * instead. */
int atem_get_last_program_input(void);

/* Counter incremented by the BMD MixEffectCallback each time an in-
 * flight mix transition crosses the 50% midpoint. The plugin filter
 * snapshots this value at fade-trigger time and watches it in
 * filter_tick — when the value increments, the ATEM (and hence the
 * camera fade visible in OBS after render delay) is at its midpoint.
 * Used by the FADE path to schedule the framing snap relative to the
 * ATEM's actual midpoint instead of guessing with a static delay. */
unsigned long long atem_get_mix_midpoint_counter(void);

#ifdef __cplusplus
}
#endif
