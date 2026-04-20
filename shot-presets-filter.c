#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/dstr.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "easing.h"
#include "shot-presets-shared.h"
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
	uint32_t alignment;    /* source anchor within its box (OBS_ALIGN_*) */
	int crop_left, crop_top, crop_right, crop_bottom;
	float bounds_x, bounds_y;
	enum obs_bounds_type bounds_type;
	uint32_t bounds_align;
	int duration_ms;       /* 0 = use global default */
	int transition_type;   /* 0 = move (animated), 1 = cut (instant) */
};

struct shot_presets_data {
	obs_source_t *source;

	struct shot_preset presets[MAX_PRESETS];
	int num_presets;
	int current_preset;       /* last activated preset index, -1 = none */

	/* Animation */
	bool animating;
	float running_duration;   /* seconds elapsed */
	int active_duration_ms;   /* duration for currently-running animation */
	struct shot_preset from;  /* captured at animation start */
	struct shot_preset to;    /* target */

	/* Live-edit-while-parked: once the user has explicitly activated a
	 * preset (click / hotkey / cut), any subsequent sceneitem transform
	 * edits get captured back into that preset so the user can refine
	 * framing in OBS's preview and have it save automatically. Stays
	 * false at startup so loaded presets aren't trampled. */
	bool user_activated;
	float sync_debounce;      /* seconds since last disk write */
	bool sync_dirty;          /* unsaved sceneitem change pending */

	/* Settings */
	int duration_ms;
	int easing_type;
	int easing_function;

	/* Hotkeys */
	obs_hotkey_id hotkeys[MAX_PRESETS];
};

/* ── Global instance tracking ──────────────────────────────
 * g_active_instance points at whichever filter should back the dock
 * right now. We track all live instances so that on scene change we
 * can pick the one whose parent source is in the current scene. */

#define MAX_INSTANCES 32
struct instance_entry {
	obs_source_t *filter_source;
	struct shot_presets_data *data;
};
static struct instance_entry g_instances[MAX_INSTANCES];
static int g_instance_count = 0;
static struct shot_presets_data *g_active_instance = NULL;

/* forward declaration */
void go_to_preset(struct shot_presets_data *d, int index);
void cut_to_preset(struct shot_presets_data *d, int index);
bool capture_transform(struct shot_presets_data *d, struct shot_preset *p);
void save_presets(struct shot_presets_data *d, obs_data_t *settings);
static void apply_transform(struct shot_presets_data *d,
                            struct shot_preset *from,
                            struct shot_preset *to, float t);
static void hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
                      bool pressed);

/* ── Shared API for the dock ─────────────────────────────── */

void shot_presets_go_to(int preset_index)
{
	blog(LOG_INFO,
	     "[Shot Presets] shot_presets_go_to(%d) called, g_active_instance=%p",
	     preset_index, (void *)g_active_instance);
	if (g_active_instance)
		go_to_preset(g_active_instance, preset_index);
	else
		blog(LOG_WARNING,
		     "[Shot Presets] go_to: no active instance — is the "
		     "source with the filter in the current scene?");
}

void shot_presets_cut(int preset_index)
{
	blog(LOG_INFO,
	     "[Shot Presets] shot_presets_cut(%d) called, g_active_instance=%p",
	     preset_index, (void *)g_active_instance);
	if (g_active_instance)
		cut_to_preset(g_active_instance, preset_index);
	else
		blog(LOG_WARNING,
		     "[Shot Presets] cut: no active instance");
}

int shot_presets_get_count(void)
{
	return g_active_instance ? g_active_instance->num_presets : 0;
}

const char *shot_presets_get_name(int index)
{
	if (!g_active_instance || index < 0 ||
	    index >= g_active_instance->num_presets)
		return "";
	return g_active_instance->presets[index].name;
}

void shot_presets_set_name(int index, const char *name)
{
	if (!g_active_instance || index < 0 ||
	    index >= g_active_instance->num_presets || !name)
		return;
	struct shot_presets_data *d = g_active_instance;
	snprintf(d->presets[index].name, PRESET_NAME_LEN, "%s",
	         *name ? name : "Untitled");
	obs_data_t *settings = obs_source_get_settings(d->source);
	save_presets(d, settings);
	obs_data_release(settings);
}

int shot_presets_is_active(int index)
{
	if (!g_active_instance || index < 0 ||
	    index >= g_active_instance->num_presets)
		return 0;
	return g_active_instance->presets[index].active ? 1 : 0;
}

int shot_presets_get_duration(void)
{
	return g_active_instance ? g_active_instance->duration_ms : 400;
}

void shot_presets_set_duration(int ms)
{
	if (!g_active_instance)
		return;
	if (ms < 50)
		ms = 400;
	g_active_instance->duration_ms = ms;
}

void shot_presets_capture(int preset_index)
{
	if (!g_active_instance || preset_index < 0 ||
	    preset_index >= g_active_instance->num_presets)
		return;
	struct shot_presets_data *d = g_active_instance;
	if (capture_transform(d, &d->presets[preset_index])) {
		d->presets[preset_index].active = true;
		/* Route subsequent live-edit-while-parked writes into THIS
		 * slot, not whichever preset was parked before the capture.
		 * Otherwise a drag right after capturing Medium would sync
		 * into Wide because current_preset never moved. */
		d->current_preset = preset_index;
		d->user_activated = true;
		d->animating = false;
		d->sync_dirty = false;
		d->sync_debounce = 0.0f;
		obs_data_t *settings = obs_source_get_settings(d->source);
		save_presets(d, settings);
		obs_data_release(settings);
	}
}

int shot_presets_get_preset_duration(int index)
{
	if (!g_active_instance || index < 0 ||
	    index >= g_active_instance->num_presets)
		return 0;
	return g_active_instance->presets[index].duration_ms;
}

void shot_presets_set_preset_duration(int index, int ms)
{
	if (!g_active_instance || index < 0 ||
	    index >= g_active_instance->num_presets)
		return;
	g_active_instance->presets[index].duration_ms = ms;
	obs_data_t *settings = obs_source_get_settings(g_active_instance->source);
	save_presets(g_active_instance, settings);
	obs_data_release(settings);
}

void shot_presets_get_crop(int index, int *l, int *t, int *r, int *b)
{
	if (!g_active_instance || index < 0 ||
	    index >= g_active_instance->num_presets) {
		if (l) *l = 0;
		if (t) *t = 0;
		if (r) *r = 0;
		if (b) *b = 0;
		return;
	}
	struct shot_preset *p = &g_active_instance->presets[index];
	if (l) *l = p->crop_left;
	if (t) *t = p->crop_top;
	if (r) *r = p->crop_right;
	if (b) *b = p->crop_bottom;
}

int shot_presets_get_transition(int index)
{
	if (!g_active_instance || index < 0 ||
	    index >= g_active_instance->num_presets)
		return 0;
	return g_active_instance->presets[index].transition_type;
}

void shot_presets_set_transition(int index, int type)
{
	if (!g_active_instance || index < 0 ||
	    index >= g_active_instance->num_presets)
		return;
	g_active_instance->presets[index].transition_type = type;
	obs_data_t *settings = obs_source_get_settings(g_active_instance->source);
	save_presets(g_active_instance, settings);
	obs_data_release(settings);
}

int shot_presets_has_active(void)
{
	return g_active_instance ? 1 : 0;
}

void shot_presets_add_preset(const char *name)
{
	if (!g_active_instance)
		return;
	struct shot_presets_data *d = g_active_instance;
	if (d->num_presets >= MAX_PRESETS)
		return;

	struct shot_preset *p = &d->presets[d->num_presets];
	memset(p, 0, sizeof(*p));
	if (name && *name)
		snprintf(p->name, PRESET_NAME_LEN, "%s", name);
	else
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
}

void shot_presets_remove_preset(int index)
{
	if (!g_active_instance)
		return;
	struct shot_presets_data *d = g_active_instance;
	if (index < 0 || index >= d->num_presets)
		return;
	for (int i = index; i < d->num_presets - 1; i++) {
		d->presets[i] = d->presets[i + 1];
	}
	d->num_presets--;
	obs_data_t *settings = obs_source_get_settings(d->source);
	save_presets(d, settings);
	obs_data_release(settings);
}

void shot_presets_for_each_source_scene(shot_presets_scene_cb cb, void *user)
{
	if (!g_active_instance || !cb)
		return;
	obs_source_t *parent = obs_filter_get_parent(g_active_instance->source);
	if (!parent)
		return;
	const char *parent_name = obs_source_get_name(parent);
	if (!parent_name)
		return;

	struct obs_frontend_source_list scenes = {0};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *s = scenes.sources.array[i];
		obs_scene_t *sc = obs_scene_from_source(s);
		if (!sc)
			continue;
		if (obs_scene_find_source_recursive(sc, parent_name))
			cb(obs_source_get_name(s), user);
	}
	obs_frontend_source_list_free(&scenes);
}

void shot_presets_paste_from_scene(int preset_index, const char *scene_name)
{
	if (!g_active_instance || !scene_name || preset_index < 0 ||
	    preset_index >= g_active_instance->num_presets)
		return;
	struct shot_presets_data *d = g_active_instance;
	obs_source_t *parent = obs_filter_get_parent(d->source);
	if (!parent)
		return;
	const char *parent_name = obs_source_get_name(parent);
	if (!parent_name)
		return;

	obs_source_t *scene_src = obs_get_source_by_name(scene_name);
	if (!scene_src) {
		blog(LOG_WARNING,
		     "[Shot Presets] PASTE: scene '%s' not found",
		     scene_name);
		return;
	}
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	obs_sceneitem_t *item = NULL;
	if (scene)
		item = obs_scene_find_source_recursive(scene, parent_name);

	if (!item) {
		blog(LOG_WARNING,
		     "[Shot Presets] PASTE: source '%s' not found in scene '%s'",
		     parent_name, scene_name);
	}

	if (item) {
		struct shot_preset *p = &d->presets[preset_index];
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
		p->alignment = obs_sceneitem_get_alignment(item);
		p->crop_left = crop.left;
		p->crop_top = crop.top;
		p->crop_right = crop.right;
		p->crop_bottom = crop.bottom;
		p->bounds_x = bounds.x;
		p->bounds_y = bounds.y;
		p->bounds_type = obs_sceneitem_get_bounds_type(item);
		p->bounds_align = obs_sceneitem_get_bounds_alignment(item);
		p->active = true;

		blog(LOG_INFO,
		     "[Shot Presets] PASTE from '%s' -> preset %d ('%s'): "
		     "pos=(%.1f,%.1f) scale=(%.3f,%.3f) rot=%.1f align=%u "
		     "crop=(%d,%d,%d,%d) bounds_type=%d bounds=(%.1f,%.1f) "
		     "b_align=%u",
		     scene_name, preset_index, p->name, p->pos_x, p->pos_y,
		     p->scale_x, p->scale_y, p->rotation, p->alignment,
		     p->crop_left, p->crop_top, p->crop_right, p->crop_bottom,
		     (int)p->bounds_type, p->bounds_x, p->bounds_y,
		     p->bounds_align);

		/* Live-apply so the preview reflects the pasted crop/transform
		 * immediately — same behaviour as shot_presets_set_crop. */
		cut_to_preset(d, preset_index);

		obs_data_t *settings = obs_source_get_settings(d->source);
		save_presets(d, settings);
		obs_data_release(settings);
	}
	obs_source_release(scene_src);
}

int shot_presets_get_transform(int index, shot_preset_transform_t *out)
{
	if (!g_active_instance || !out || index < 0 ||
	    index >= g_active_instance->num_presets)
		return 0;
	struct shot_preset *p = &g_active_instance->presets[index];
	out->pos_x = p->pos_x;
	out->pos_y = p->pos_y;
	out->scale_x = p->scale_x;
	out->scale_y = p->scale_y;
	out->rotation = p->rotation;
	out->alignment = p->alignment;
	out->bounds_type = (int)p->bounds_type;
	out->bounds_x = p->bounds_x;
	out->bounds_y = p->bounds_y;
	out->bounds_align = p->bounds_align;
	return p->active ? 1 : 0;
}

void shot_presets_set_transform(int index, const shot_preset_transform_t *in)
{
	if (!g_active_instance || !in || index < 0 ||
	    index >= g_active_instance->num_presets)
		return;
	struct shot_presets_data *d = g_active_instance;
	struct shot_preset *p = &d->presets[index];

	/* Preserve crop — dock has a separate control path for that. */
	int cl = p->crop_left, ct = p->crop_top;
	int cr = p->crop_right, cb = p->crop_bottom;

	if (!p->active) {
		if (capture_transform(d, p))
			p->active = true;
	}

	p->pos_x = in->pos_x;
	p->pos_y = in->pos_y;
	p->scale_x = in->scale_x;
	p->scale_y = in->scale_y;
	p->rotation = in->rotation;
	p->alignment = in->alignment;
	p->bounds_type = (enum obs_bounds_type)in->bounds_type;
	p->bounds_x = in->bounds_x;
	p->bounds_y = in->bounds_y;
	p->bounds_align = in->bounds_align;
	p->crop_left = cl;
	p->crop_top = ct;
	p->crop_right = cr;
	p->crop_bottom = cb;
	p->active = true;

	cut_to_preset(d, index);

	obs_data_t *settings = obs_source_get_settings(d->source);
	save_presets(d, settings);
	obs_data_release(settings);
}

void shot_presets_set_crop(int index, int l, int t, int r, int b)
{
	if (!g_active_instance || index < 0 ||
	    index >= g_active_instance->num_presets)
		return;
	struct shot_presets_data *d = g_active_instance;
	struct shot_preset *p = &d->presets[index];

	blog(LOG_INFO,
	     "[Shot Presets] set_crop(%d '%s') l=%d t=%d r=%d b=%d "
	     "(was l=%d t=%d r=%d b=%d, active=%d)",
	     index, p->name, l, t, r, b,
	     p->crop_left, p->crop_top, p->crop_right, p->crop_bottom,
	     p->active);

	/* If preset has no saved transform yet, seed it from the current
	 * scene item so pos/scale/bounds are sensible. */
	if (!p->active) {
		if (capture_transform(d, p))
			p->active = true;
	}

	p->crop_left = l;
	p->crop_top = t;
	p->crop_right = r;
	p->crop_bottom = b;

	cut_to_preset(d, index);

	obs_data_t *settings = obs_source_get_settings(d->source);
	save_presets(d, settings);
	obs_data_release(settings);
}

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
bool capture_transform(struct shot_presets_data *d, struct shot_preset *p)
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
	p->alignment = obs_sceneitem_get_alignment(item);
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
	obs_sceneitem_set_alignment(item, to->alignment);

	obs_sceneitem_defer_update_end(item);
}

/* Convert a preset's pos+alignment into an equivalent CENTER-aligned pos
 * so interpolation produces a centered pan/zoom (like a camera move)
 * instead of growing from the top-left corner. The resulting preset has
 * alignment = 0 (OBS_ALIGN_CENTER) and pos_x/pos_y pointing at the
 * image's centre on the canvas.
 *
 * Stored presets are unchanged — we only normalise the animation
 * endpoints (d->from / d->to) and the one-shot copy used by cut. */
static void normalize_preset_to_center(struct shot_preset *p,
                                        uint32_t src_w, uint32_t src_h)
{
	float w, h;
	if (p->bounds_type == OBS_BOUNDS_NONE) {
		int cw = (int)src_w - p->crop_left - p->crop_right;
		int ch = (int)src_h - p->crop_top - p->crop_bottom;
		if (cw < 0) cw = 0;
		if (ch < 0) ch = 0;
		w = (float)cw * p->scale_x;
		h = (float)ch * p->scale_y;
	} else {
		w = p->bounds_x;
		h = p->bounds_y;
	}

	float cx = p->pos_x, cy = p->pos_y;
	if (p->alignment & OBS_ALIGN_LEFT)
		cx += w * 0.5f;
	else if (p->alignment & OBS_ALIGN_RIGHT)
		cx -= w * 0.5f;
	if (p->alignment & OBS_ALIGN_TOP)
		cy += h * 0.5f;
	else if (p->alignment & OBS_ALIGN_BOTTOM)
		cy -= h * 0.5f;

	p->pos_x = cx;
	p->pos_y = cy;
	p->alignment = 0; /* OBS_ALIGN_CENTER */
}

/* Convert a preset with any bounds_type to an equivalent NONE+scale so
 * endpoints with mismatched bounds_types can be interpolated without
 * rendering 0×0 during the animation. Computes the on-canvas width/height
 * that the bounds_type+bounds would produce, then back-solves a scale that
 * reproduces that size with bounds_type=NONE. Stored presets are unchanged;
 * only the animation endpoints (d->from / d->to) are normalized.
 *
 * Call this *after* normalize_preset_to_center so pos already references
 * the centre of the rendered rect, which is preserved by this transform
 * (we only rewrite scale+bounds_type). */
static void normalize_preset_to_none(struct shot_preset *p,
                                      uint32_t src_w, uint32_t src_h)
{
	if (p->bounds_type == OBS_BOUNDS_NONE)
		return;

	int cwi = (int)src_w - p->crop_left - p->crop_right;
	int chi = (int)src_h - p->crop_top - p->crop_bottom;
	if (cwi < 1) cwi = 1;
	if (chi < 1) chi = 1;
	float cw = (float)cwi;
	float ch = (float)chi;

	float bx = p->bounds_x;
	float by = p->bounds_y;
	float final_w = cw * p->scale_x;
	float final_h = ch * p->scale_y;

	switch (p->bounds_type) {
	case OBS_BOUNDS_STRETCH:
		final_w = bx;
		final_h = by;
		break;
	case OBS_BOUNDS_SCALE_INNER: {
		float sx = (cw > 0) ? bx / cw : 1.0f;
		float sy = (ch > 0) ? by / ch : 1.0f;
		float s = sx < sy ? sx : sy;
		final_w = cw * s;
		final_h = ch * s;
		break;
	}
	case OBS_BOUNDS_SCALE_OUTER: {
		float sx = (cw > 0) ? bx / cw : 1.0f;
		float sy = (ch > 0) ? by / ch : 1.0f;
		float s = sx > sy ? sx : sy;
		final_w = cw * s;
		final_h = ch * s;
		break;
	}
	case OBS_BOUNDS_SCALE_TO_WIDTH: {
		float s = (cw > 0) ? bx / cw : 1.0f;
		final_w = bx;
		final_h = ch * s;
		break;
	}
	case OBS_BOUNDS_SCALE_TO_HEIGHT: {
		float s = (ch > 0) ? by / ch : 1.0f;
		final_w = cw * s;
		final_h = by;
		break;
	}
	case OBS_BOUNDS_MAX_ONLY: {
		float fw = cw * p->scale_x;
		float fh = ch * p->scale_y;
		if (bx > 0 && fw > bx) fw = bx;
		if (by > 0 && fh > by) fh = by;
		final_w = fw;
		final_h = fh;
		break;
	}
	default:
		break;
	}

	p->scale_x = final_w / cw;
	p->scale_y = final_h / ch;
	p->bounds_type = OBS_BOUNDS_NONE;
	p->bounds_x = 0.0f;
	p->bounds_y = 0.0f;
	p->bounds_align = 0;
}

/* ── Trigger animation ───────────────────────────────────── */

void go_to_preset(struct shot_presets_data *d, int index)
{
	if (index < 0 || index >= d->num_presets) {
		blog(LOG_WARNING,
		     "[Shot Presets] go_to: bad index %d (num_presets=%d)",
		     index, d->num_presets);
		return;
	}
	if (!d->presets[index].active) {
		/* First click on an empty slot seeds it from the live
		 * sceneitem transform instead of silently refusing. Without
		 * this, the click is a no-op but the user thinks they
		 * switched — any subsequent drag in preview then corrupts
		 * whichever preset was *previously* parked (because
		 * current_preset never moves). Treat first click as:
		 * capture live → mark active → cut (no animation). */
		if (!capture_transform(d, &d->presets[index])) {
			blog(LOG_WARNING,
			     "[Shot Presets] go_to: preset %d ('%s') not active and "
			     "capture_transform failed — source missing from scene?",
			     index, d->presets[index].name);
			return;
		}
		d->presets[index].active = true;
		blog(LOG_INFO,
		     "[Shot Presets] go_to: seeded empty preset %d '%s' from "
		     "current sceneitem",
		     index, d->presets[index].name);
		cut_to_preset(d, index);
		obs_data_t *settings = obs_source_get_settings(d->source);
		save_presets(d, settings);
		obs_data_release(settings);
		return;
	}
	if (!capture_transform(d, &d->from)) {
		blog(LOG_WARNING,
		     "[Shot Presets] go_to: capture_transform failed — "
		     "source '%s' not found in current scene",
		     obs_source_get_name(obs_filter_get_parent(d->source)));
		return;
	}

	d->to = d->presets[index];
	d->current_preset = index;
	d->user_activated = true;
	d->sync_dirty = false;
	d->running_duration = 0.0f;
	d->active_duration_ms = d->presets[index].duration_ms > 0
		? d->presets[index].duration_ms
		: d->duration_ms;
	if (d->active_duration_ms < 50)
		d->active_duration_ms = 400;

	/* Normalise both endpoints to centre-aligned so interpolation
	 * produces a centred pan+zoom rather than a corner-anchored fly. */
	obs_source_t *parent = obs_filter_get_parent(d->source);
	if (parent) {
		uint32_t sw = obs_source_get_width(parent);
		uint32_t sh = obs_source_get_height(parent);
		if (sw && sh) {
			normalize_preset_to_center(&d->from, sw, sh);
			normalize_preset_to_center(&d->to,   sw, sh);
			/* If the endpoints have different bounds_type, apply_transform
			 * would hold to->bounds_type all frame while lerping bounds
			 * from→to — which renders 0×0 (black) at t≈0 whenever either
			 * bounds value is zero. Flatten both to NONE+equivalent-scale
			 * for the animation; the original target bounds_type is
			 * restored at finish. */
			if (d->from.bounds_type != d->to.bounds_type) {
				normalize_preset_to_none(&d->from, sw, sh);
				normalize_preset_to_none(&d->to,   sw, sh);
			}
		}
	}

	d->animating = true;
	blog(LOG_INFO,
	     "[Shot Presets] move START -> '%s' (%d ms)  "
	     "from pos=(%.1f,%.1f) scale=(%.2f,%.2f) crop=(%d,%d,%d,%d) "
	     "bt=%d bnds=(%.0f,%.0f) ba=%u al=%u   "
	     "to pos=(%.1f,%.1f) scale=(%.2f,%.2f) crop=(%d,%d,%d,%d) "
	     "bt=%d bnds=(%.0f,%.0f) ba=%u al=%u",
	     d->presets[index].name, d->active_duration_ms,
	     d->from.pos_x, d->from.pos_y, d->from.scale_x, d->from.scale_y,
	     d->from.crop_left, d->from.crop_top, d->from.crop_right, d->from.crop_bottom,
	     (int)d->from.bounds_type, d->from.bounds_x, d->from.bounds_y,
	     d->from.bounds_align, d->from.alignment,
	     d->to.pos_x, d->to.pos_y, d->to.scale_x, d->to.scale_y,
	     d->to.crop_left, d->to.crop_top, d->to.crop_right, d->to.crop_bottom,
	     (int)d->to.bounds_type, d->to.bounds_x, d->to.bounds_y,
	     d->to.bounds_align, d->to.alignment);
}

/* Apply a preset to the sceneitem without changing user_activated,
 * current_preset, or sync dirty state. Used for the auto-snap on scene
 * activation so loaded presets aren't immediately captured-over by any
 * stale sceneitem transform that OBS had saved on the item. */
static void snap_to_preset_silent(struct shot_presets_data *d, int index)
{
	if (index < 0 || index >= d->num_presets)
		return;
	if (!d->presets[index].active)
		return;

	d->animating = false;
	struct shot_preset target = d->presets[index];

	obs_source_t *parent = obs_filter_get_parent(d->source);
	if (parent) {
		uint32_t sw = obs_source_get_width(parent);
		uint32_t sh = obs_source_get_height(parent);
		if (sw && sh)
			normalize_preset_to_center(&target, sw, sh);
	}
	apply_transform(d, &target, &target, 1.0f);
}

void cut_to_preset(struct shot_presets_data *d, int index)
{
	if (index < 0 || index >= d->num_presets)
		return;
	if (!d->presets[index].active)
		return;

	d->animating = false;
	d->current_preset = index;
	d->user_activated = true;
	d->sync_dirty = false;
	struct shot_preset target = d->presets[index];

	obs_source_t *parent = obs_filter_get_parent(d->source);
	if (parent) {
		uint32_t sw = obs_source_get_width(parent);
		uint32_t sh = obs_source_get_height(parent);
		if (sw && sh)
			normalize_preset_to_center(&target, sw, sh);
	}

	blog(LOG_INFO,
	     "[Shot Presets] CUT '%s': pos=(%.1f,%.1f) scale=(%.3f,%.3f) "
	     "rot=%.1f align=%u crop=(%d,%d,%d,%d) bounds_type=%d "
	     "bounds=(%.1f,%.1f) b_align=%u",
	     target.name, target.pos_x, target.pos_y,
	     target.scale_x, target.scale_y,
	     target.rotation, target.alignment,
	     target.crop_left, target.crop_top,
	     target.crop_right, target.crop_bottom,
	     (int)target.bounds_type,
	     target.bounds_x, target.bounds_y, target.bounds_align);
	apply_transform(d, &target, &target, 1.0f);
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

void save_presets(struct shot_presets_data *d, obs_data_t *settings)
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
		obs_data_set_int(obj, "alignment", (int)p->alignment);
		obs_data_set_int(obj, "preset_duration_ms", p->duration_ms);
		obs_data_set_int(obj, "transition_type", p->transition_type);
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
	if (g_instance_count < MAX_INSTANCES) {
		g_instances[g_instance_count].filter_source = source;
		g_instances[g_instance_count].data = d;
		g_instance_count++;
	}
	g_active_instance = d;
	{
		obs_source_t *parent = obs_filter_get_parent(source);
		const char *pname = parent ? obs_source_get_name(parent)
		                           : "(no parent yet)";
		blog(LOG_INFO,
		     "[Shot Presets] filter_create: instance=%p (count=%d) on source='%s'",
		     (void *)d, g_instance_count,
		     pname ? pname : "(null)");
	}
	d->duration_ms = (int)obs_data_get_int(settings, "duration");
	if (d->duration_ms < 50)
		d->duration_ms = 400;
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
			/* Older saves lacked alignment; default to OBS_ALIGN_TOP|LEFT (5) */
			p->alignment = obs_data_has_user_value(obj, "alignment")
				? (uint32_t)obs_data_get_int(obj, "alignment")
				: (OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
			p->duration_ms = (int)obs_data_get_int(obj, "preset_duration_ms");
			p->transition_type = (int)obs_data_get_int(obj, "transition_type");
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
	struct shot_presets_data *d = data;
	for (int i = 0; i < g_instance_count; i++) {
		if (g_instances[i].data == d) {
			g_instances[i] = g_instances[g_instance_count - 1];
			g_instance_count--;
			break;
		}
	}
	if (g_active_instance == d)
		g_active_instance = NULL;
	bfree(d);
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
	if (d->duration_ms < 50)
		d->duration_ms = 400;
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

/* Compare transform fields between a captured sceneitem state and a
 * stored preset. Returns true if they differ enough to warrant a save. */
static bool sceneitem_differs_from_preset(const struct shot_preset *live,
                                           const struct shot_preset *stored)
{
	const float eps = 0.5f;
	if (fabsf(live->pos_x   - stored->pos_x)   > eps) return true;
	if (fabsf(live->pos_y   - stored->pos_y)   > eps) return true;
	if (fabsf(live->scale_x - stored->scale_x) > 0.001f) return true;
	if (fabsf(live->scale_y - stored->scale_y) > 0.001f) return true;
	if (fabsf(live->rotation - stored->rotation) > 0.01f) return true;
	if (live->alignment    != stored->alignment)    return true;
	if (live->crop_left    != stored->crop_left)    return true;
	if (live->crop_top     != stored->crop_top)     return true;
	if (live->crop_right   != stored->crop_right)   return true;
	if (live->crop_bottom  != stored->crop_bottom)  return true;
	if (live->bounds_type  != stored->bounds_type)  return true;
	if (fabsf(live->bounds_x - stored->bounds_x) > eps) return true;
	if (fabsf(live->bounds_y - stored->bounds_y) > eps) return true;
	if (live->bounds_align != stored->bounds_align) return true;
	return false;
}

/* Pull the sceneitem's current transform into the parked preset. */
static void sync_sceneitem_to_parked_preset(struct shot_presets_data *d,
                                              float seconds)
{
	if (!d->user_activated || d->animating)
		return;
	if (d->current_preset < 0 || d->current_preset >= d->num_presets)
		return;

	struct shot_preset *p = &d->presets[d->current_preset];
	if (!p->active)
		return;

	struct shot_preset live = {0};
	if (!capture_transform(d, &live))
		return;

	if (sceneitem_differs_from_preset(&live, p)) {
		p->pos_x   = live.pos_x;
		p->pos_y   = live.pos_y;
		p->scale_x = live.scale_x;
		p->scale_y = live.scale_y;
		p->rotation = live.rotation;
		p->alignment = live.alignment;
		p->crop_left   = live.crop_left;
		p->crop_top    = live.crop_top;
		p->crop_right  = live.crop_right;
		p->crop_bottom = live.crop_bottom;
		p->bounds_type = live.bounds_type;
		p->bounds_x    = live.bounds_x;
		p->bounds_y    = live.bounds_y;
		p->bounds_align = live.bounds_align;
		d->sync_dirty = true;
	}

	/* Debounce disk writes so rapid drags don't hammer settings. */
	d->sync_debounce += seconds;
	if (d->sync_dirty && d->sync_debounce > 0.4f) {
		obs_data_t *settings = obs_source_get_settings(d->source);
		save_presets(d, settings);
		obs_data_release(settings);
		d->sync_dirty = false;
		d->sync_debounce = 0.0f;
	}
}

static void filter_tick(void *data, float seconds)
{
	struct shot_presets_data *d = data;

	if (!d->animating) {
		sync_sceneitem_to_parked_preset(d, seconds);
		return;
	}

	d->running_duration += seconds;

	int dur_ms = d->active_duration_ms > 0 ? d->active_duration_ms
	                                       : d->duration_ms;
	if (dur_ms < 50)
		dur_ms = 400;
	float duration_s = (float)dur_ms / 1000.0f;

	float t = d->running_duration / duration_s;
	bool finished = false;
	if (t >= 1.0f) {
		t = 1.0f;
		d->animating = false;
		finished = true;
	}

	float eased_t = get_eased(t, d->easing_type, d->easing_function);
	apply_transform(d, &d->from, &d->to, eased_t);

	if (finished) {
		/* d->to was flattened to bounds_type=NONE for the animation when
		 * the endpoints had mismatched bounds_types. Snap to the original
		 * preset now so the parked sceneitem carries the user's real
		 * bounds_type/bounds/alignment — matters for OBS's Edit Transform
		 * dialog and for how the source adapts to resolution changes. */
		if (d->current_preset >= 0 &&
		    d->current_preset < d->num_presets &&
		    d->presets[d->current_preset].active) {
			struct shot_preset final = d->presets[d->current_preset];
			apply_transform(d, &final, &final, 1.0f);
		}
		/* Reset debounce so first post-animation sync waits a beat. */
		d->sync_debounce = 0.0f;
		d->sync_dirty = false;
		blog(LOG_INFO,
		     "[Shot Presets] move FINISH after %.3f s",
		     d->running_duration);
	}
}

/* ── video_render — passthrough (no pixel changes) ───────── */

static void filter_render(void *data, gs_effect_t *effect)
{
	struct shot_presets_data *d = data;
	obs_source_skip_video_filter(d->source);
	UNUSED_PARAMETER(effect);
}

/* OBS calls this when saving the scene collection. Flush the in-memory
 * presets array into the settings dict so mid-debounce edits (and any
 * other in-memory state ahead of the dict) are never lost on shutdown. */
static void filter_save(void *data, obs_data_t *settings)
{
	struct shot_presets_data *d = data;
	save_presets(d, settings);
	d->sync_dirty = false;
	d->sync_debounce = 0.0f;
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
	.save = filter_save,
	.video_tick = filter_tick,
	.video_render = filter_render,
};

/* Defined in shot-presets-dock.cpp */
extern void shot_presets_dock_init(void);

static void update_active_for_current_scene(void)
{
	obs_source_t *scene_src = obs_frontend_get_current_scene();
	if (!scene_src) {
		g_active_instance = NULL;
		blog(LOG_INFO,
		     "[Shot Presets] active=NULL (no current scene)");
		return;
	}
	const char *scene_name = obs_source_get_name(scene_src);
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	struct shot_presets_data *match = NULL;
	const char *matched_source = NULL;
	if (scene) {
		for (int i = 0; i < g_instance_count; i++) {
			obs_source_t *filter_src =
				g_instances[i].filter_source;
			if (!filter_src)
				continue;
			obs_source_t *parent =
				obs_filter_get_parent(filter_src);
			if (!parent)
				continue;
			const char *pname = obs_source_get_name(parent);
			if (!pname)
				continue;
			if (obs_scene_find_source_recursive(scene, pname)) {
				match = g_instances[i].data;
				matched_source = pname;
				break;
			}
		}
	}
	g_active_instance = match;
	blog(LOG_INFO,
	     "[Shot Presets] scene='%s' instances=%d active=%p (source='%s')",
	     scene_name ? scene_name : "(null)", g_instance_count,
	     (void *)g_active_instance,
	     matched_source ? matched_source : "(none)");
	obs_source_release(scene_src);

	/* Baseline snap: on scene activation, put the sceneitem into a
	 * known state so every subsequent go_to starts from the preset
	 * it's tweening from — not whatever OBS happened to save on the
	 * sceneitem. Prefer current_preset (user's last click, preserved
	 * across scene switches); fall back to preset[0] as the default
	 * framing. Silent — doesn't flip user_activated, so live-edit
	 * sync stays gated until the user explicitly picks a preset. */
	if (match) {
		int target = match->current_preset;
		if (target < 0 || target >= match->num_presets ||
		    !match->presets[target].active)
			target = 0;
		if (target < match->num_presets &&
		    match->presets[target].active) {
			snap_to_preset_silent(match, target);
			blog(LOG_INFO,
			     "[Shot Presets] baseline snap -> preset %d '%s'",
			     target, match->presets[target].name);
		}
	}
}

static void on_scene_changed(enum obs_frontend_event event, void *unused)
{
	UNUSED_PARAMETER(unused);
	if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED ||
	    event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		update_active_for_current_scene();
	}
}

bool obs_module_load(void)
{
	obs_register_source(&shot_presets_filter_info);
	shot_presets_dock_init();
	obs_frontend_add_event_callback(on_scene_changed, NULL);
	blog(LOG_INFO, "[Shot Presets] Plugin loaded (version %s)",
	     PROJECT_VERSION);
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[Shot Presets] Plugin unloaded");
}
