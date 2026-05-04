#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Called by the dock to trigger a preset on the active filter instance.
 * The filter registers/unregisters itself so only one instance is active. */
void shot_presets_go_to(int preset_index);

/* Same as go_to, but applies the target transform instantly (no animation) */
void shot_presets_cut(int preset_index);

/* Same as go_to, but forces a cross-dissolve regardless of the preset's
 * configured transition. Used by the dock's per-row Fade button so the
 * user can override the default move/cut on the fly without editing the
 * preset's transition setting. */
void shot_presets_fade(int preset_index);

/* Query how many presets the active filter has */
int shot_presets_get_count(void);

/* Get a preset's name (returns "" if invalid) */
const char *shot_presets_get_name(int index);

/* Rename a preset */
void shot_presets_set_name(int index, const char *name);

/* Check if a preset has a saved transform */
int shot_presets_is_active(int index);

/* Get/set global default duration (used for Move animations and as a
 * fallback for any preset whose own duration is 0) */
int shot_presets_get_duration(void);
void shot_presets_set_duration(int ms);

/* Get/set the global default Fade duration (used by the per-row Fade
 * button when the preset's own duration is 0). Separate from the move
 * default because cross-fades typically need longer. Also drives the
 * ATEM hardware mix transition rate when ATEM input is wired. */
int shot_presets_get_fade_duration(void);
void shot_presets_set_fade_duration(int ms);

/* Per-preset duration override (0 = use global default) */
int shot_presets_get_preset_duration(int index);
void shot_presets_set_preset_duration(int index, int ms);

/* Read/write per-preset crop (live-applies to source on set) */
void shot_presets_get_crop(int index, int *l, int *t, int *r, int *b);
void shot_presets_set_crop(int index, int l, int t, int r, int b);

/* Save current transform into preset. Returns 1 on success, 0 if the
 * capture failed (e.g. parent source not found in the active scene, or
 * filter restricted to other scenes via the properties dialog). The dock
 * uses the return to flash green vs red. */
int shot_presets_capture(int preset_index);

/* Per-preset transition type */
typedef enum {
	SHOT_TRANSITION_MOVE = 0, /* animated transform */
	SHOT_TRANSITION_CUT  = 1, /* instant */
	SHOT_TRANSITION_FADE = 2, /* cross-fade: opacity 1→0, cut transform,
	                             opacity 0→1 over full duration */
} shot_preset_transition_t;

int shot_presets_get_transition(int index);
void shot_presets_set_transition(int index, int type);

/* Per-preset ATEM program input. 0 = no ATEM action, 1-8 = switch the
 * ATEM to that input when this preset fires. The actual switch is done
 * by the FNN runtime over HTTP at 127.0.0.1:4173. */
int shot_presets_get_atem_input(int index);
void shot_presets_set_atem_input(int index, int input);

/* Add / remove presets in the active filter */
void shot_presets_add_preset(const char *name);
void shot_presets_remove_preset(int index);

/* Is there an active Shot Presets filter on the current scene? */
int shot_presets_has_active(void);

/* Index of the currently-parked preset (last clicked) for the active
 * scene's bucket. -1 if none. The dock uses this to highlight the row
 * the user is "on" so they know what they're editing. */
int shot_presets_get_current_preset(void);

/* Global ATEM sync delay in ms. When a preset with atem_input fires,
 * the ATEM SetProgramInput is queued immediately but the sceneitem
 * transform is held back by this many ms — so the OBS framing change
 * lands together with the new ATEM video frame (USB capture latency +
 * any Render Delay filter the user has on the source). 0 = disabled,
 * apply transform immediately. */
int  shot_presets_get_atem_sync_delay(void);
void shot_presets_set_atem_sync_delay(int ms);

/* Per-scene default preset. The filter snaps to this preset when the scene
 * activates, instead of the last-clicked one. -1 = no default set (falls
 * back to current_preset, then preset 0). Setting clears any previous
 * default in the same scene's bucket. */
int shot_presets_get_default_preset(void);
void shot_presets_set_default_preset(int index);

/* Enumerate scenes whose scene graph contains this filter's parent source.
 * The callback is invoked once per matching scene name. */
typedef void (*shot_presets_scene_cb)(const char *scene_name, void *user);
void shot_presets_for_each_source_scene(shot_presets_scene_cb cb, void *user);

/* Copy the transform of this source as it lives in the named scene into
 * the given preset slot. */
void shot_presets_paste_from_scene(int preset_index, const char *scene_name);

/* Full transform struct for dock UI. Mirrors obs-sceneitem fields. */
typedef struct shot_preset_transform {
	float pos_x, pos_y;
	float scale_x, scale_y;
	float rotation;
	unsigned int alignment;
	int bounds_type;
	float bounds_x, bounds_y;
	unsigned int bounds_align;
} shot_preset_transform_t;

/* Read the full transform of a preset. Returns 1 if the preset has a
 * captured transform, 0 otherwise (fields still populated). */
int shot_presets_get_transform(int index, shot_preset_transform_t *out);

/* Write the full transform of a preset and live-apply to the sceneitem. */
void shot_presets_set_transform(int index,
                                const shot_preset_transform_t *in);

#ifdef __cplusplus
}
#endif
