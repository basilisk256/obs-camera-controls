#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/dstr.h>
#include <stdio.h>
#include <string.h>
#include "easing.h"
#include "version.h"

/* ================================================================
 *  Shot Presets — OBS Filter Plugin
 *
 *  Adds an effect filter to any source that stores multiple named
 *  crop/transform presets and smoothly animates between them using
 *  the same video_tick mechanism as obs-move-transition.
 *
 *  No scene switching needed — camera stays in one scene.
 * ================================================================ */

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("basilisk256")
OBS_MODULE_USE_DEFAULT_LOCALE("obs-shot-presets", "en-US")

/* ── Constants ───────────────────────────────────────────── */
#define MAX_PRESETS 12
#define PRESET_NAME_LEN 64

/* ── Data structures ─────────────────────────────────────── */

struct shot_preset {
	char name[PRESET_NAME_LEN];
	bool active;      /* true once Get Transform has been called */
	float pos_x, pos_y;
	float scale_x, scale_y;
	float rotation;
	int crop_left, crop_top, crop_right, crop_bottom;
	float bounds_x, bounds_y;
	enum obs_bounds_type bounds_type;
	uint32_t bounds_align;
};

struct shot_presets_data {
	obs_source_t *source;

	struct shot_preset presets[MAX_PRESETS];
	int num_presets;
	int current_preset;       /* last activated preset index, -1 = none */

	/* Animation */
	bool animating;
	float running_duration;   /* seconds elapsed */
	struct shot_preset from;  /* captured at animation start */
	struct shot_preset to;    /* target */

	/* Settings */
	int duration_ms;
	int easing_type;
	int easing_function;

	/* Hotkeys */
	obs_hotkey_id hotkeys[MAX_PRESETS];
};

/* ── Helpers ─────────────────────────────────────────────── */

static float lerp_f(float a, float b, float t) { return a + (b - a) * t; }
static int   lerp_i(int a, int b, float t) { return (int)((float)a + ((float)b - (float)a) * t); }

/* Walk the current scene (or preview in studio mode) to find the
 * scene item that corresponds to this filter's parent source. */
static obs_sceneitem_t *find_scene_item(struct shot_presets_data *d)
{
	obs_source_t *parent = obs_filter_get_parent(d->source);
	if (!parent)
		return NULL;

	const char *parent_name = obs_source_get_name(parent);

	obs_source_t *scene_source = obs_frontend_get_current_scene();
	if (!scene_source)
		scene_source = obs_frontend_get_current_preview_scene();
	if (!scene_source)
		return NULL;

	obs_scene_t *scene = obs_scene_from_source(scene_source);
	obs_sceneitem_t *item = NULL;
	if (scene) {
		item = obs_scene_find_source_recursive(scene, parent_name);
	}
	obs_source_release(scene_source);
	return item;
}

/* Read the scene item's current transform into a preset struct */
static bool capture_transform(struct shot_presets_data *d, struct shot_preset *p)
{
	obs_sceneitem_t *item = find_scene_item(d);
	if (!item)
		return false;

	struct vec2 pos, scale, bounds;
	struct obs_sceneitem_crop crop;

	obs_sceneitem_get_pos(item, &pos);
	obs_sceneitem_get_scale(item, &scale);
	obs_sceneitem_get_bounds(item, &bounds);
	obs_sceneitem_get_crop(item, &crop);

	p->pos_x = pos.x;
	p->pos_y = pos.y;
	p->scale_x = scale.x;
	p->scale_y = scale.y;
	p->rotation = obs_sceneitem_get_rot(item);
	p->crop_left = crop.left;
	p->crop_top = crop.top;
	p->crop_right = crop.right;
	p->crop_bottom = crop.bottom;
	p->bounds_x = bounds.x;
	p->bounds_y = bounds.y;
	p->bounds_type = obs_sceneitem_get_bounds_type(item);
	p->bounds_align = obs_sceneitem_get_bounds_alignment(item);
	return true;
}

/* Apply interpolated transform each frame */
static void apply_transform(struct shot_presets_data *d,
                            struct shot_preset *from,
                            struct shot_preset *to, float t)
{
	obs_sceneitem_t *item = find_scene_item(d);
	if (!item)
		return;

	float ct = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);

	obs_sceneitem_defer_update_begin(item);

	struct vec2 pos;
	pos.x = lerp_f(from->pos_x, to->pos_x, t);
	pos.y = lerp_f(from->pos_y, to->pos_y, t);
	obs_sceneitem_set_pos(item, &pos);

	struct vec2 scale;
	scale.x = lerp_f(from->scale_x, to->scale_x, t);
	scale.y = lerp_f(from->scale_y, to->scale_y, t);
	obs_sceneitem_set_scale(item, &scale);

	obs_sceneitem_set_rot(item,
		lerp_f(from->rotation, to->rotation, t));

	struct obs_sceneitem_crop crop;
	crop.left   = lerp_i(from->crop_left,   to->crop_left,   ct);
	crop.top    = lerp_i(from->crop_top,    to->crop_top,    ct);
	crop.right  = lerp_i(from->crop_right,  to->crop_right,  ct);
	crop.bottom = lerp_i(from->crop_bottom, to->crop_bottom, ct);
	obs_sceneitem_set_crop(item, &crop);

	struct vec2 bounds;
	bounds.x = lerp_f(from->bounds_x, to->bounds_x, t);
	bounds.y = lerp_f(from->bounds_y, to->bounds_y, t);
	obs_sceneitem_set_bounds(item, &bounds);
	obs_sceneitem_set_bounds_type(item, to->bounds_type);
	obs_sceneitem_set_bounds_alignment(item, to->bounds_align);

	obs_sceneitem_defer_update_end(item);
}

/* ── Trigger animation ───────────────────────────────────── */

static void go_to_preset(struct shot_presets_data *d, int index)
{
	if (index < 0 || index >= d->num_presets)
		return;
	if (!d->presets[index].active)
		return;
	if (!capture_transform(d, &d->from))
		return;

	d->to = d->presets[index];
	d->current_preset = index;
	d->running_duration = 0.0f;
	d->animating = true;
}

/* ── Hotkey callback ─────────────────────────────────────── */

static void hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
                      bool pressed)
{
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	struct shot_presets_data *d = data;
	for (int i = 0; i < d->num_presets; i++) {
		if (d->hotkeys[i] == id) {
			go_to_preset(d, i);
			return;
		}
	}
}

/* ── Save presets to obs_data ────────────────────────────── */

static void save_presets(struct shot_presets_data *d, obs_data_t *settings)
{
	obs_data_array_t *arr = obs_data_array_create();
	for (int i = 0; i < d->num_presets; i++) {
		struct shot_preset *p = &d->presets[i];
		obs_data_t *obj = obs_data_create();
		obs_data_set_string(obj, "name", p->name);
		obs_data_set_bool(obj, "active", p->active);
		obs_data_set_double(obj, "pos_x", p->pos_x);
		obs_data_set_double(obj, "pos_y", p->pos_y);
		obs_data_set_double(obj, "scale_x", p->scale_x);
		obs_data_set_double(obj, "scale_y", p->scale_y);
		obs_data_set_double(obj, "rotation", p->rotation);
		obs_data_set_int(obj, "crop_left", p->crop_left);
		obs_data_set_int(obj, "crop_top", p->crop_top);
		obs_data_set_int(obj, "crop_right", p->crop_right);
		obs_data_set_int(obj, "crop_bottom", p->crop_bottom);
		obs_data_set_double(obj, "bounds_x", p->bounds_x);
		obs_data_set_double(obj, "bounds_y", p->bounds_y);
		obs_data_set_int(obj, "bounds_type", (int)p->bounds_type);
		obs_data_set_int(obj, "bounds_align", (int)p->bounds_align);
		obs_data_array_push_back(arr, obj);
		obs_data_release(obj);
	}
	obs_data_set_array(settings, "presets", arr);
	obs_data_array_release(arr);
}

/* ── Filter callbacks ────────────────────────────────────── */

static const char *filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Shot Presets";
}

static void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct shot_presets_data *d = bzalloc(sizeof(*d));
	d->source = source;
	d->current_preset = -1;
	d->duration_ms = (int)obs_data_get_int(settings, "duration");
	d->easing_type = (int)obs_data_get_int(settings, "easing_type");
	d->easing_function = (int)obs_data_get_int(settings, "easing_function");

	/* Load saved presets */
	obs_data_array_t *arr = obs_data_get_array(settings, "presets");
	if (arr) {
		size_t count = obs_data_array_count(arr);
		if (count > MAX_PRESETS)
			count = MAX_PRESETS;
		for (size_t i = 0; i < count; i++) {
			obs_data_t *obj = obs_data_array_item(arr, i);
			struct shot_preset *p = &d->presets[i];
			const char *name = obs_data_get_string(obj, "name");
			snprintf(p->name, PRESET_NAME_LEN, "%s",
			         name && *name ? name : "Untitled");
			p->active     = obs_data_get_bool(obj, "active");
			p->pos_x      = (float)obs_data_get_double(obj, "pos_x");
			p->pos_y      = (float)obs_data_get_double(obj, "pos_y");
			p->scale_x    = (float)obs_data_get_double(obj, "scale_x");
			p->scale_y    = (float)obs_data_get_double(obj, "scale_y");
			p->rotation   = (float)obs_data_get_double(obj, "rotation");
			p->crop_left  = (int)obs_data_get_int(obj, "crop_left");
			p->crop_top   = (int)obs_data_get_int(obj, "crop_top");
			p->crop_right = (int)obs_data_get_int(obj, "crop_right");
			p->crop_bottom= (int)obs_data_get_int(obj, "crop_bottom");
			p->bounds_x   = (float)obs_data_get_double(obj, "bounds_x");
			p->bounds_y   = (float)obs_data_get_double(obj, "bounds_y");
			p->bounds_type = (int)obs_data_get_int(obj, "bounds_type");
			p->bounds_align= (uint32_t)obs_data_get_int(obj, "bounds_align");
			d->num_presets++;
			obs_data_release(obj);
		}
		obs_data_array_release(arr);
	}

	/* Default to 3 presets if none saved */
	if (d->num_presets == 0) {
		const char *default_names[] = {"Wide", "Medium", "Close-up"};
		for (int i = 0; i < 3; i++) {
			snprintf(d->presets[i].name, PRESET_NAME_LEN, "%s",
			         default_names[i]);
			d->presets[i].scale_x = 1.0f;
			d->presets[i].scale_y = 1.0f;
		}
		d->num_presets = 3;
	}

	/* Register hotkeys */
	for (int i = 0; i < d->num_presets; i++) {
		struct dstr hk_name = {0};
		struct dstr hk_desc = {0};
		dstr_printf(&hk_name, "shot_preset_%d", i);
		dstr_printf(&hk_desc, "Shot Preset: %s", d->presets[i].name);
		d->hotkeys[i] = obs_hotkey_register_source(
			source, hk_name.array, hk_desc.array, hotkey_cb, d);
		dstr_free(&hk_name);
		dstr_free(&hk_desc);
	}

	return d;
}

static void filter_destroy(void *data)
{
	bfree(data);
}

/* ── Properties UI ───────────────────────────────────────── */

static bool on_get_transform(obs_properties_t *props, obs_property_t *prop,
                             void *data)
{
	UNUSED_PARAMETER(props);
	struct shot_presets_data *d = data;
	const char *pname = obs_property_name(prop);
	int idx = -1;
	sscanf(pname, "get_transform_%d", &idx);
	if (idx < 0 || idx >= d->num_presets)
		return false;

	if (capture_transform(d, &d->presets[idx])) {
		d->presets[idx].active = true;
		obs_data_t *settings = obs_source_get_settings(d->source);
		save_presets(d, settings);
		obs_data_release(settings);
		blog(LOG_INFO, "[Shot Presets] Saved transform for '%s'",
		     d->presets[idx].name);
	}
	return true;
}

static bool on_go_to(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	struct shot_presets_data *d = data;
	const char *pname = obs_property_name(prop);
	int idx = -1;
	sscanf(pname, "go_to_%d", &idx);
	if (idx >= 0)
		go_to_preset(d, idx);
	return false;
}

static bool on_add_preset(obs_properties_t *props, obs_property_t *prop,
                          void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	struct shot_presets_data *d = data;
	if (d->num_presets >= MAX_PRESETS)
		return true;

	struct shot_preset *p = &d->presets[d->num_presets];
	memset(p, 0, sizeof(*p));
	snprintf(p->name, PRESET_NAME_LEN, "Shot %d", d->num_presets + 1);
	p->scale_x = 1.0f;
	p->scale_y = 1.0f;
	d->num_presets++;

	struct dstr hk_name = {0};
	struct dstr hk_desc = {0};
	dstr_printf(&hk_name, "shot_preset_%d", d->num_presets - 1);
	dstr_printf(&hk_desc, "Shot Preset: %s", p->name);
	d->hotkeys[d->num_presets - 1] = obs_hotkey_register_source(
		d->source, hk_name.array, hk_desc.array, hotkey_cb, d);
	dstr_free(&hk_name);
	dstr_free(&hk_desc);

	obs_data_t *settings = obs_source_get_settings(d->source);
	save_presets(d, settings);
	obs_data_release(settings);
	return true;
}

static bool on_remove_preset(obs_properties_t *props, obs_property_t *prop,
                             void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	struct shot_presets_data *d = data;
	if (d->num_presets <= 0)
		return true;
	d->num_presets--;
	obs_data_t *settings = obs_source_get_settings(d->source);
	save_presets(d, settings);
	obs_data_release(settings);
	return true;
}

static obs_properties_t *filter_properties(void *data)
{
	struct shot_presets_data *d = data;
	obs_properties_t *props = obs_properties_create();

	/* Transition settings */
	obs_properties_add_int(props, "duration",
		"Transition Duration (ms)", 50, 5000, 50);

	obs_property_t *et = obs_properties_add_list(props, "easing_type",
		"Easing Type", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(et, "None", EASE_NONE);
	obs_property_list_add_int(et, "Ease In", EASE_IN);
	obs_property_list_add_int(et, "Ease Out", EASE_OUT);
	obs_property_list_add_int(et, "Ease In/Out", EASE_IN_OUT);

	obs_property_t *ef = obs_properties_add_list(props, "easing_function",
		"Easing Function", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(ef, "Linear", EASING_LINEAR);
	obs_property_list_add_int(ef, "Quadratic", EASING_QUADRATIC);
	obs_property_list_add_int(ef, "Cubic", EASING_CUBIC);
	obs_property_list_add_int(ef, "Quartic", EASING_QUARTIC);
	obs_property_list_add_int(ef, "Quintic", EASING_QUINTIC);
	obs_property_list_add_int(ef, "Sine", EASING_SINE);
	obs_property_list_add_int(ef, "Circular", EASING_CIRCULAR);
	obs_property_list_add_int(ef, "Exponential", EASING_EXPONENTIAL);
	obs_property_list_add_int(ef, "Elastic", EASING_ELASTIC);
	obs_property_list_add_int(ef, "Bounce", EASING_BOUNCE);
	obs_property_list_add_int(ef, "Back", EASING_BACK);

	/* Per-preset controls */
	for (int i = 0; i < d->num_presets; i++) {
		char prop_name[64], get_btn[64], go_btn[64];
		struct dstr label = {0};

		snprintf(prop_name, sizeof(prop_name), "preset_name_%d", i);
		dstr_printf(&label, "Preset %d Name", i + 1);
		obs_properties_add_text(props, prop_name, label.array,
		                        OBS_TEXT_DEFAULT);
		dstr_free(&label);

		snprintf(get_btn, sizeof(get_btn), "get_transform_%d", i);
		dstr_printf(&label, "Get Transform → %s", d->presets[i].name);
		obs_properties_add_button2(props, get_btn, label.array,
		                           on_get_transform, d);
		dstr_free(&label);

		snprintf(go_btn, sizeof(go_btn), "go_to_%d", i);
		dstr_printf(&label, "%s Go To %s",
		            d->presets[i].active ? "[Ready]" : "[Empty]",
		            d->presets[i].name);
		obs_properties_add_button2(props, go_btn, label.array,
		                           on_go_to, d);
		dstr_free(&label);
	}

	obs_properties_add_button2(props, "add_preset", "+ Add Preset",
	                           on_add_preset, d);
	if (d->num_presets > 0)
		obs_properties_add_button2(props, "remove_preset",
		                           "- Remove Last Preset",
		                           on_remove_preset, d);

	return props;
}

static void filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "duration", 400);
	obs_data_set_default_int(settings, "easing_type", EASE_IN_OUT);
	obs_data_set_default_int(settings, "easing_function", EASING_CUBIC);
}

static void filter_update(void *data, obs_data_t *settings)
{
	struct shot_presets_data *d = data;
	d->duration_ms = (int)obs_data_get_int(settings, "duration");
	d->easing_type = (int)obs_data_get_int(settings, "easing_type");
	d->easing_function = (int)obs_data_get_int(settings, "easing_function");

	for (int i = 0; i < d->num_presets; i++) {
		char prop_name[64];
		snprintf(prop_name, sizeof(prop_name), "preset_name_%d", i);
		const char *new_name = obs_data_get_string(settings, prop_name);
		if (new_name && *new_name)
			snprintf(d->presets[i].name, PRESET_NAME_LEN, "%s",
			         new_name);
	}
}

/* ── video_tick — runs every frame (30/60fps) ────────────── */

static void filter_tick(void *data, float seconds)
{
	struct shot_presets_data *d = data;
	if (!d->animating)
		return;

	d->running_duration += seconds;

	float duration_s = (float)d->duration_ms / 1000.0f;
	if (duration_s <= 0.0f)
		duration_s = 0.001f;

	float t = d->running_duration / duration_s;
	if (t >= 1.0f) {
		t = 1.0f;
		d->animating = false;
	}

	float eased_t = get_eased(t, d->easing_type, d->easing_function);
	apply_transform(d, &d->from, &d->to, eased_t);
}

/* ── video_render — passthrough (no pixel changes) ───────── */

static void filter_render(void *data, gs_effect_t *effect)
{
	struct shot_presets_data *d = data;
	obs_source_skip_video_filter(d->source);
	UNUSED_PARAMETER(effect);
}

/* ── Registration ────────────────────────────────────────── */

static struct obs_source_info shot_presets_filter_info = {
	.id = "shot_presets_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = filter_get_name,
	.create = filter_create,
	.destroy = filter_destroy,
	.get_properties = filter_properties,
	.get_defaults = filter_defaults,
	.update = filter_update,
	.video_tick = filter_tick,
	.video_render = filter_render,
};

bool obs_module_load(void)
{
	obs_register_source(&shot_presets_filter_info);
	blog(LOG_INFO, "[Shot Presets] Plugin loaded (version %s)",
	     PROJECT_VERSION);
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[Shot Presets] Plugin unloaded");
}
