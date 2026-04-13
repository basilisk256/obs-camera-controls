#pragma once

#include <obs-module.h>

#define MAX_PRESETS 12
#define PRESET_NAME_LEN 64

/* One saved camera framing */
struct shot_preset {
	char name[PRESET_NAME_LEN];
	bool active;

	/* Transform */
	float pos_x;
	float pos_y;
	float scale_x;
	float scale_y;
	float rotation;

	/* Crop (pixels) */
	int crop_left;
	int crop_top;
	int crop_right;
	int crop_bottom;

	/* Bounds */
	float bounds_x;
	float bounds_y;
	enum obs_bounds_type bounds_type;
	uint32_t bounds_align;
};

/* Filter instance data */
struct shot_presets_data {
	obs_source_t *source;

	struct shot_preset presets[MAX_PRESETS];
	int num_presets;

	/* Animation state */
	bool animating;
	float running_duration;    /* seconds elapsed */
	int target_preset;         /* index we're moving to */

	/* Interpolation endpoints */
	struct shot_preset from;   /* captured at animation start */
	struct shot_preset to;     /* target preset */

	/* Settings */
	int duration_ms;           /* transition duration */
	int easing_type;           /* EASE_NONE / IN / OUT / IN_OUT */
	int easing_function;       /* EASING_CUBIC etc. */

	/* Hotkeys */
	obs_hotkey_id hotkeys[MAX_PRESETS];
};

extern struct obs_source_info shot_presets_filter_info;
