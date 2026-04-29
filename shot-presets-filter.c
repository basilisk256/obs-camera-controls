#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/matrix4.h>
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
#define SCENE_NAME_LEN 256
#define MAX_SCENE_BUCKETS 32

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

/* Per-scene bucket of presets. Each scene the filter is active in gets
 * its own bucket so a single source filter can drive different framings
 * in different scenes (e.g. wide-cam used in both Wide and Guest scenes).
 * scene_name="" is the legacy/default bucket used as a migration template
 * and as a fallback before any scene-change event has fired. */
struct shot_preset_bucket {
	char scene_name[SCENE_NAME_LEN];
	struct shot_preset presets[MAX_PRESETS];
	int num_presets;
	int current_preset;        /* last activated preset, -1 = none */
	int default_preset;        /* scene's default framing on activation,
	                             * -1 = none (falls back to current_preset
	                             * then preset 0). Set via dock star
	                             * toggle. */
	bool user_activated;       /* user has clicked at least once in this scene */
};

struct shot_presets_data {
	obs_source_t *source;

	/* Enabled-scene binding. The filter lives on a shared source (e.g.
	 * a webcam used in multiple scenes), but shot presets should only
	 * drive sceneitem transforms in scenes the user explicitly enables.
	 * Other scenes using the same source keep their own transforms
	 * untouched. Empty list = unbound → falls back to "any scene with
	 * this source" so initial setup just works. */
#define MAX_ENABLED_SCENES 32
	char enabled_scenes[MAX_ENABLED_SCENES][256];
	int num_enabled_scenes;

	/* Per-scene preset buckets. Resolved by g_active_scene_name; auto-
	 * created lazily when a scene with the parent source first activates
	 * and a write or query touches the active bucket. */
	struct shot_preset_bucket buckets[MAX_SCENE_BUCKETS];
	int num_buckets;

	/* Migration template: populated from a legacy flat "presets" array on
	 * load. When a new scene's bucket is auto-created, it's seeded from
	 * this template so old data isn't lost. has_legacy_template clears
	 * once the template has been used at least once. */
	struct shot_preset legacy_presets[MAX_PRESETS];
	int legacy_num_presets;
	bool has_legacy_template;

	/* Animation */
	bool animating;
	float running_duration;   /* seconds elapsed */
	int active_duration_ms;   /* duration for currently-running animation */
	struct shot_preset from;  /* captured at animation start */
	struct shot_preset to;    /* target */

	/* Live-edit-while-parked: sync_dirty/sync_debounce are transient
	 * timing state for the currently active bucket. user_activated lives
	 * on each bucket so engaging in scene A doesn't auto-capture in
	 * scene B before the user clicks there too. */
	float sync_debounce;      /* seconds since last disk write */
	bool sync_dirty;          /* unsaved sceneitem change pending */

	/* Settings */
	int duration_ms;
	int easing_type;
	int easing_function;

	/* Hotkeys: registered MAX_PRESETS times at create. Callback resolves
	 * to the active bucket and invokes the preset at that slot, so a
	 * single keybinding works across all scenes (preset 1 in scene Wide
	 * vs preset 1 in scene Guest both fire on the same key). */
	obs_hotkey_id hotkeys[MAX_PRESETS];

	/* Fade (cross-dissolve) transition state.
	 *
	 * When a FADE preset is triggered, the filter temporarily becomes
	 * a canvas-sized compositor: it captures the source pixels into
	 * fade_source_cap, overrides its reported width/height to the
	 * canvas dimensions, and overrides the sceneitem transform to
	 * "identity fill canvas". Each fade frame, filter_render draws
	 * the captured source twice into the canvas-sized output — once
	 * with fade_from_mtx at opacity (1-t), once with fade_to_mtx at
	 * opacity t — giving a true cross-fade between the two framings.
	 * On finish, the real target transform is restored.
	 *
	 * fade_effect is a small alpha-multiply shader used for both
	 * draws; it has one uniform, `opacity`, that we set per draw. */
	gs_effect_t *fade_effect;
	bool fade_enabled;
	int fade_diag_frames;
	float fade_t;                /* eased 0→1 progress, for logging */
	gs_texrender_t *fade_source_cap;  /* source-native-sized capture */
	struct matrix4 fade_from_mtx;     /* source-quad → canvas (from) */
	struct matrix4 fade_to_mtx;       /* source-quad → canvas (to)   */
	uint32_t fade_canvas_w;
	uint32_t fade_canvas_h;
	uint32_t fade_src_w;              /* source dims captured at start */
	uint32_t fade_src_h;
};

/* Embedded HLSL for the fade effect — avoids shipping a data file. */
static const char *FADE_EFFECT_HLSL =
"uniform float4x4 ViewProj;\n"
"uniform texture2d image;\n"
"uniform float opacity;\n"
"\n"
"sampler_state textureSampler {\n"
"    Filter   = Linear;\n"
"    AddressU = Clamp;\n"
"    AddressV = Clamp;\n"
"};\n"
"\n"
"struct VertData {\n"
"    float4 pos : POSITION;\n"
"    float2 uv  : TEXCOORD0;\n"
"};\n"
"\n"
"VertData VSDefault(VertData v_in) {\n"
"    VertData v_out;\n"
"    v_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);\n"
"    v_out.uv  = v_in.uv;\n"
"    return v_out;\n"
"}\n"
"\n"
"float4 PSDraw(VertData v_in) : TARGET {\n"
"    return image.Sample(textureSampler, v_in.uv) * opacity;\n"
"}\n"
"\n"
"technique Draw {\n"
"    pass {\n"
"        vertex_shader = VSDefault(v_in);\n"
"        pixel_shader  = PSDraw(v_in);\n"
"    }\n"
"}\n";

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

/* Name of the scene OBS reports as current. Set by
 * update_active_for_current_scene; consulted by active_bucket() to pick
 * which bucket each instance's preset operations should target. */
static char g_active_scene_name[SCENE_NAME_LEN] = "";

/* forward declaration */
void go_to_preset(struct shot_presets_data *d, int index);
static void go_to_preset_override(struct shot_presets_data *d, int index,
                                   int transition_override);
void cut_to_preset(struct shot_presets_data *d, int index);
bool capture_transform(struct shot_presets_data *d, struct shot_preset *p);
void save_presets(struct shot_presets_data *d, obs_data_t *settings);
static void bind_home_scene_if_needed(struct shot_presets_data *d);
static bool is_scene_enabled(struct shot_presets_data *d, const char *name);
static void apply_transform(struct shot_presets_data *d,
                            struct shot_preset *from,
                            struct shot_preset *to, float t);
static void hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
                      bool pressed);

/* ── Bucket helpers ─────────────────────────────────────────
 * Each filter instance stores presets in per-scene buckets. The "active"
 * bucket is whichever one matches g_active_scene_name. Auto-created on
 * first access so adding a preset in a never-visited scene Just Works. */

static struct shot_preset_bucket *find_bucket(struct shot_presets_data *d,
                                               const char *name)
{
	if (!name)
		name = "";
	for (int i = 0; i < d->num_buckets; i++) {
		if (strcmp(d->buckets[i].scene_name, name) == 0)
			return &d->buckets[i];
	}
	return NULL;
}

/* Seed a freshly-created bucket from the legacy template if one exists.
 * Used so that a save loaded from before per-scene buckets keeps its
 * presets when the user first visits a scene with the filter source. */
static void seed_bucket_from_legacy(struct shot_presets_data *d,
                                     struct shot_preset_bucket *b)
{
	if (!d->has_legacy_template || d->legacy_num_presets <= 0)
		return;
	int n = d->legacy_num_presets;
	if (n > MAX_PRESETS) n = MAX_PRESETS;
	for (int i = 0; i < n; i++)
		b->presets[i] = d->legacy_presets[i];
	b->num_presets = n;
}

static struct shot_preset_bucket *get_or_create_bucket(
	struct shot_presets_data *d, const char *name)
{
	struct shot_preset_bucket *b = find_bucket(d, name);
	if (b)
		return b;
	if (d->num_buckets >= MAX_SCENE_BUCKETS)
		return NULL;
	b = &d->buckets[d->num_buckets++];
	memset(b, 0, sizeof(*b));
	snprintf(b->scene_name, sizeof(b->scene_name), "%s",
	         name ? name : "");
	b->current_preset = -1;
	b->default_preset = -1;
	b->user_activated = false;
	seed_bucket_from_legacy(d, b);
	return b;
}

/* Bucket for the current OBS scene. Auto-creates if writing to a scene
 * that hasn't been touched before. Returns NULL only if MAX_SCENE_BUCKETS
 * is exceeded — callers must guard. */
static struct shot_preset_bucket *active_bucket(struct shot_presets_data *d)
{
	if (!d)
		return NULL;
	return get_or_create_bucket(d, g_active_scene_name);
}

/* Read-only variant: returns the active bucket if it exists, else NULL.
 * Use for queries that should report "no presets" rather than auto-create
 * a bucket as a side effect. */
static struct shot_preset_bucket *peek_active_bucket(
	struct shot_presets_data *d)
{
	if (!d)
		return NULL;
	return find_bucket(d, g_active_scene_name);
}

/* ── Shared API for the dock ─────────────────────────────── */

void shot_presets_go_to(int preset_index)
{
	blog(LOG_INFO,
	     "[Shot Presets] shot_presets_go_to(%d) called, g_active_instance=%p",
	     preset_index, (void *)g_active_instance);
	if (g_active_instance) {
		bind_home_scene_if_needed(g_active_instance);
		go_to_preset(g_active_instance, preset_index);
	} else
		blog(LOG_WARNING,
		     "[Shot Presets] go_to: no active instance — is the "
		     "source with the filter in the current scene?");
}

void shot_presets_cut(int preset_index)
{
	blog(LOG_INFO,
	     "[Shot Presets] shot_presets_cut(%d) called, g_active_instance=%p",
	     preset_index, (void *)g_active_instance);
	if (g_active_instance) {
		bind_home_scene_if_needed(g_active_instance);
		cut_to_preset(g_active_instance, preset_index);
	} else
		blog(LOG_WARNING,
		     "[Shot Presets] cut: no active instance");
}

void shot_presets_fade(int preset_index)
{
	blog(LOG_INFO,
	     "[Shot Presets] shot_presets_fade(%d) called, g_active_instance=%p",
	     preset_index, (void *)g_active_instance);
	if (g_active_instance) {
		bind_home_scene_if_needed(g_active_instance);
		go_to_preset_override(g_active_instance, preset_index,
		                       SHOT_TRANSITION_FADE);
	} else
		blog(LOG_WARNING,
		     "[Shot Presets] fade: no active instance");
}

int shot_presets_get_count(void)
{
	if (!g_active_instance)
		return 0;
	struct shot_preset_bucket *b = peek_active_bucket(g_active_instance);
	return b ? b->num_presets : 0;
}

const char *shot_presets_get_name(int index)
{
	if (!g_active_instance)
		return "";
	struct shot_preset_bucket *b = peek_active_bucket(g_active_instance);
	if (!b || index < 0 || index >= b->num_presets)
		return "";
	return b->presets[index].name;
}

void shot_presets_set_name(int index, const char *name)
{
	if (!g_active_instance || !name)
		return;
	struct shot_presets_data *d = g_active_instance;
	struct shot_preset_bucket *b = active_bucket(d);
	if (!b || index < 0 || index >= b->num_presets)
		return;
	snprintf(b->presets[index].name, PRESET_NAME_LEN, "%s",
	         *name ? name : "Untitled");
	obs_data_t *settings = obs_source_get_settings(d->source);
	save_presets(d, settings);
	obs_data_release(settings);
}

int shot_presets_is_active(int index)
{
	if (!g_active_instance)
		return 0;
	struct shot_preset_bucket *b = peek_active_bucket(g_active_instance);
	if (!b || index < 0 || index >= b->num_presets)
		return 0;
	return b->presets[index].active ? 1 : 0;
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
	if (!g_active_instance)
		return;
	struct shot_presets_data *d = g_active_instance;
	bind_home_scene_if_needed(d);
	struct shot_preset_bucket *bk = active_bucket(d);
	if (!bk || preset_index < 0 || preset_index >= bk->num_presets)
		return;
	if (capture_transform(d, &bk->presets[preset_index])) {
		bk->presets[preset_index].active = true;
		/* Route subsequent live-edit-while-parked writes into THIS
		 * slot, not whichever preset was parked before the capture.
		 * Otherwise a drag right after capturing Medium would sync
		 * into Wide because current_preset never moved. */
		bk->current_preset = preset_index;
		bk->user_activated = true;
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
	if (!g_active_instance)
		return 0;
	struct shot_preset_bucket *b = peek_active_bucket(g_active_instance);
	if (!b || index < 0 || index >= b->num_presets)
		return 0;
	return b->presets[index].duration_ms;
}

void shot_presets_set_preset_duration(int index, int ms)
{
	if (!g_active_instance)
		return;
	struct shot_presets_data *d = g_active_instance;
	struct shot_preset_bucket *b = active_bucket(d);
	if (!b || index < 0 || index >= b->num_presets)
		return;
	b->presets[index].duration_ms = ms;
	obs_data_t *settings = obs_source_get_settings(d->source);
	save_presets(d, settings);
	obs_data_release(settings);
}

void shot_presets_get_crop(int index, int *l, int *t, int *r, int *bot)
{
	if (l) *l = 0;
	if (t) *t = 0;
	if (r) *r = 0;
	if (bot) *bot = 0;
	if (!g_active_instance)
		return;
	struct shot_preset_bucket *bk = peek_active_bucket(g_active_instance);
	if (!bk || index < 0 || index >= bk->num_presets)
		return;
	struct shot_preset *p = &bk->presets[index];
	if (l) *l = p->crop_left;
	if (t) *t = p->crop_top;
	if (r) *r = p->crop_right;
	if (bot) *bot = p->crop_bottom;
}

int shot_presets_get_transition(int index)
{
	if (!g_active_instance)
		return 0;
	struct shot_preset_bucket *b = peek_active_bucket(g_active_instance);
	if (!b || index < 0 || index >= b->num_presets)
		return 0;
	return b->presets[index].transition_type;
}

void shot_presets_set_transition(int index, int type)
{
	if (!g_active_instance)
		return;
	struct shot_presets_data *d = g_active_instance;
	struct shot_preset_bucket *b = active_bucket(d);
	if (!b || index < 0 || index >= b->num_presets)
		return;
	b->presets[index].transition_type = type;
	obs_data_t *settings = obs_source_get_settings(d->source);
	save_presets(d, settings);
	obs_data_release(settings);
}

int shot_presets_has_active(void)
{
	return g_active_instance ? 1 : 0;
}

int shot_presets_get_default_preset(void)
{
	if (!g_active_instance)
		return -1;
	struct shot_preset_bucket *b = peek_active_bucket(g_active_instance);
	return b ? b->default_preset : -1;
}

void shot_presets_set_default_preset(int index)
{
	if (!g_active_instance)
		return;
	struct shot_presets_data *d = g_active_instance;
	struct shot_preset_bucket *b = active_bucket(d);
	if (!b)
		return;
	if (index < -1 || index >= b->num_presets)
		return;
	if (b->default_preset == index)
		return; /* no change */
	b->default_preset = index;
	obs_data_t *settings = obs_source_get_settings(d->source);
	save_presets(d, settings);
	obs_data_release(settings);
	blog(LOG_INFO,
	     "[Shot Presets] scene '%s' default_preset = %d",
	     b->scene_name, index);
}

void shot_presets_add_preset(const char *name)
{
	if (!g_active_instance)
		return;
	struct shot_presets_data *d = g_active_instance;
	struct shot_preset_bucket *bk = active_bucket(d);
	if (!bk || bk->num_presets >= MAX_PRESETS)
		return;

	struct shot_preset *p = &bk->presets[bk->num_presets];
	memset(p, 0, sizeof(*p));
	if (name && *name)
		snprintf(p->name, PRESET_NAME_LEN, "%s", name);
	else
		snprintf(p->name, PRESET_NAME_LEN, "Shot %d",
		         bk->num_presets + 1);
	p->scale_x = 1.0f;
	p->scale_y = 1.0f;
	bk->num_presets++;

	/* Hotkeys are registered MAX_PRESETS times at filter_create with
	 * generic names — the callback resolves to the active bucket and
	 * the same key works across scenes for the same slot. No new
	 * registration needed when adding presets. */

	obs_data_t *settings = obs_source_get_settings(d->source);
	save_presets(d, settings);
	obs_data_release(settings);
}

void shot_presets_remove_preset(int index)
{
	if (!g_active_instance)
		return;
	struct shot_presets_data *d = g_active_instance;
	struct shot_preset_bucket *bk = active_bucket(d);
	if (!bk || index < 0 || index >= bk->num_presets)
		return;
	for (int i = index; i < bk->num_presets - 1; i++) {
		bk->presets[i] = bk->presets[i + 1];
	}
	bk->num_presets--;
	if (bk->current_preset == index)
		bk->current_preset = -1;
	else if (bk->current_preset > index)
		bk->current_preset--;
	if (bk->default_preset == index)
		bk->default_preset = -1;
	else if (bk->default_preset > index)
		bk->default_preset--;
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
	if (!g_active_instance || !scene_name)
		return;
	struct shot_presets_data *d = g_active_instance;
	struct shot_preset_bucket *bk = active_bucket(d);
	if (!bk || preset_index < 0 || preset_index >= bk->num_presets)
		return;
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
		struct shot_preset *p = &bk->presets[preset_index];
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
	if (!g_active_instance || !out)
		return 0;
	struct shot_preset_bucket *bk = peek_active_bucket(g_active_instance);
	if (!bk || index < 0 || index >= bk->num_presets)
		return 0;
	struct shot_preset *p = &bk->presets[index];
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
	if (!g_active_instance || !in)
		return;
	struct shot_presets_data *d = g_active_instance;
	struct shot_preset_bucket *bk = active_bucket(d);
	if (!bk || index < 0 || index >= bk->num_presets)
		return;
	struct shot_preset *p = &bk->presets[index];

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
	if (!g_active_instance)
		return;
	struct shot_presets_data *d = g_active_instance;
	struct shot_preset_bucket *bk = active_bucket(d);
	if (!bk || index < 0 || index >= bk->num_presets)
		return;
	struct shot_preset *p = &bk->presets[index];

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
/* Is the named scene in the filter's enabled list? Unbound (empty list)
 * matches every scene so initial setup doesn't require the user to first
 * tick a checkbox. */
static bool is_scene_enabled(struct shot_presets_data *d, const char *name)
{
	if (!name)
		return false;
	if (d->num_enabled_scenes == 0)
		return true;
	for (int i = 0; i < d->num_enabled_scenes; i++) {
		if (strcmp(d->enabled_scenes[i], name) == 0)
			return true;
	}
	return false;
}

/* If the enabled-scenes list is empty, seed it with the current scene.
 * Called from explicit user actions (go_to / capture / cut) so the first
 * scene in which the user actually drives shot presets becomes enabled.
 * Also flips the corresponding `scene_enabled_<name>` bool in settings so
 * the properties-dialog checkbox reflects the auto-binding next open. */
static void bind_home_scene_if_needed(struct shot_presets_data *d)
{
	if (d->num_enabled_scenes > 0)
		return;
	obs_source_t *scene = obs_frontend_get_current_scene();
	if (!scene)
		scene = obs_frontend_get_current_preview_scene();
	if (!scene)
		return;
	const char *name = obs_source_get_name(scene);
	if (name && *name) {
		snprintf(d->enabled_scenes[0], 256, "%s", name);
		d->num_enabled_scenes = 1;
		blog(LOG_INFO,
		     "[Shot Presets] auto-enabled in scene '%s'", name);
		obs_data_t *settings = obs_source_get_settings(d->source);
		char key[320];
		snprintf(key, sizeof(key), "scene_enabled_%s", name);
		obs_data_set_bool(settings, key, true);
		save_presets(d, settings);
		obs_data_release(settings);
	}
	obs_source_release(scene);
}

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

	/* Only target the sceneitem if this scene is enabled for shot
	 * presets. When the user is on an un-enabled scene (e.g. a guest
	 * layout that also references the webcam), shot-preset clicks are
	 * no-ops — the sceneitem there keeps whatever transform the user
	 * configured in OBS. */
	const char *scene_name = obs_source_get_name(scene_source);
	if (!is_scene_enabled(d, scene_name)) {
		obs_source_release(scene_source);
		return NULL;
	}

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
	go_to_preset_override(d, index, -1);
}

static void go_to_preset_override(struct shot_presets_data *d, int index,
                                   int transition_override)
{
	struct shot_preset_bucket *bk = active_bucket(d);
	if (!bk) {
		blog(LOG_WARNING,
		     "[Shot Presets] go_to: no bucket available for scene '%s'",
		     g_active_scene_name);
		return;
	}
	if (index < 0 || index >= bk->num_presets) {
		blog(LOG_WARNING,
		     "[Shot Presets] go_to: bad index %d (num_presets=%d, scene='%s')",
		     index, bk->num_presets, bk->scene_name);
		return;
	}
	if (!bk->presets[index].active) {
		/* First click on an empty slot seeds it from the live
		 * sceneitem transform instead of silently refusing. Without
		 * this, the click is a no-op but the user thinks they
		 * switched — any subsequent drag in preview then corrupts
		 * whichever preset was *previously* parked (because
		 * current_preset never moves). Treat first click as:
		 * capture live → mark active → cut (no animation). */
		if (!capture_transform(d, &bk->presets[index])) {
			blog(LOG_WARNING,
			     "[Shot Presets] go_to: preset %d ('%s') not active and "
			     "capture_transform failed — source missing from scene?",
			     index, bk->presets[index].name);
			return;
		}
		bk->presets[index].active = true;
		blog(LOG_INFO,
		     "[Shot Presets] go_to: seeded empty preset %d '%s' from "
		     "current sceneitem",
		     index, bk->presets[index].name);
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

	d->to = bk->presets[index];
	bk->current_preset = index;
	bk->user_activated = true;
	d->sync_dirty = false;
	d->running_duration = 0.0f;
	d->active_duration_ms = bk->presets[index].duration_ms > 0
		? bk->presets[index].duration_ms
		: d->duration_ms;
	if (d->active_duration_ms < 50)
		d->active_duration_ms = 400;

	int effective_transition = (transition_override >= 0)
		? transition_override
		: bk->presets[index].transition_type;
	bool is_fade = (effective_transition == SHOT_TRANSITION_FADE);
	d->fade_enabled = false;
	d->fade_t = 0.0f;

	if (is_fade) {
		/* Set up cross-dissolve. Need: (1) current sceneitem box
		 * matrix (from framing), (2) target box matrix computed by
		 * temporarily applying `to` framing, (3) override sceneitem
		 * to "identity fill canvas" so the canvas-sized filter
		 * output maps 1:1 onto canvas during the fade. */
		obs_sceneitem_t *item = find_scene_item(d);
		struct obs_video_info ovi;
		bool got_ovi = obs_get_video_info(&ovi);
		if (item && got_ovi) {
			uint32_t cw = ovi.base_width;
			uint32_t ch = ovi.base_height;
			obs_source_t *parent = obs_filter_get_parent(d->source);
			uint32_t sw = parent ? obs_source_get_width(parent) : 0;
			uint32_t sh = parent ? obs_source_get_height(parent) : 0;

			if (sw && sh && cw && ch) {
				/* (1) box matrix for the current framing. */
				obs_sceneitem_get_box_transform(item, &d->fade_from_mtx);

				/* (2) apply `to` temporarily, read its matrix. */
				struct shot_preset to_tmp = bk->presets[index];
				apply_transform(d, &to_tmp, &to_tmp, 1.0f);
				obs_sceneitem_force_update_transform(item);
				obs_sceneitem_get_box_transform(item, &d->fade_to_mtx);

				/* (3) override to identity-fill-canvas. With the
				 * filter reporting canvas dims, this draws the
				 * canvas-sized filter output at (0,0) 1:1. */
				struct shot_preset id = {0};
				snprintf(id.name, sizeof(id.name), "_fade_id_");
				id.active = true;
				id.scale_x = 1.0f;
				id.scale_y = 1.0f;
				id.alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
				id.bounds_type = OBS_BOUNDS_NONE;
				apply_transform(d, &id, &id, 1.0f);
				obs_sceneitem_force_update_transform(item);

				d->fade_canvas_w = cw;
				d->fade_canvas_h = ch;
				d->fade_src_w = sw;
				d->fade_src_h = sh;
				d->fade_enabled = true;
				d->fade_diag_frames = 0;
				blog(LOG_INFO,
				     "[Shot Presets] fade START cw=%u ch=%u sw=%u sh=%u "
				     "effect=%p texrender=%p from_mtx[0]=(%.2f,%.2f,%.2f,%.2f) "
				     "to_mtx[0]=(%.2f,%.2f,%.2f,%.2f)",
				     cw, ch, sw, sh,
				     (void *)d->fade_effect,
				     (void *)d->fade_source_cap,
				     d->fade_from_mtx.x.x, d->fade_from_mtx.x.y,
				     d->fade_from_mtx.x.z, d->fade_from_mtx.x.w,
				     d->fade_to_mtx.x.x, d->fade_to_mtx.x.y,
				     d->fade_to_mtx.x.z, d->fade_to_mtx.x.w);
			}
		}
		if (!d->fade_enabled)
			blog(LOG_WARNING,
			     "[Shot Presets] fade setup failed; falling "
			     "back to move for this transition");
	}

	if (!d->fade_enabled) {
		/* Move path: normalise both endpoints to centre-aligned so
		 * interpolation produces a centred pan+zoom rather than a
		 * corner-anchored fly. */
		obs_source_t *parent = obs_filter_get_parent(d->source);
		if (parent) {
			uint32_t sw = obs_source_get_width(parent);
			uint32_t sh = obs_source_get_height(parent);
			if (sw && sh) {
				normalize_preset_to_center(&d->from, sw, sh);
				normalize_preset_to_center(&d->to,   sw, sh);
				/* If the endpoints have different bounds_type,
				 * apply_transform would hold to->bounds_type all
				 * frame while lerping bounds from→to — which
				 * renders 0×0 (black) at t≈0 whenever either
				 * bounds value is zero. Flatten both to
				 * NONE+equivalent-scale for the animation; the
				 * original target bounds_type is restored at
				 * finish. */
				if (d->from.bounds_type != d->to.bounds_type) {
					normalize_preset_to_none(&d->from, sw, sh);
					normalize_preset_to_none(&d->to,   sw, sh);
				}
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
	     bk->presets[index].name, d->active_duration_ms,
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
	struct shot_preset_bucket *bk = active_bucket(d);
	if (!bk || index < 0 || index >= bk->num_presets)
		return;
	if (!bk->presets[index].active)
		return;

	d->animating = false;
	struct shot_preset target = bk->presets[index];

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
	struct shot_preset_bucket *bk = active_bucket(d);
	if (!bk || index < 0 || index >= bk->num_presets)
		return;
	if (!bk->presets[index].active)
		return;

	d->animating = false;
	bk->current_preset = index;
	bk->user_activated = true;
	d->sync_dirty = false;
	struct shot_preset target = bk->presets[index];

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
	/* Hotkeys are registered MAX_PRESETS times in slot order; resolve
	 * id → slot, then dispatch to the active bucket so the same key works
	 * across scenes. go_to_preset itself looks up active_bucket(d). */
	for (int i = 0; i < MAX_PRESETS; i++) {
		if (d->hotkeys[i] == id) {
			go_to_preset(d, i);
			return;
		}
	}
}

/* ── Save presets to obs_data ────────────────────────────── */

/* Serialize one preset slot. Used by both the per-bucket preset array
 * and the legacy migration template (kept on disk in case a downgrade
 * needs to read it). */
static void save_one_preset(obs_data_t *obj, const struct shot_preset *p)
{
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
}

void save_presets(struct shot_presets_data *d, obs_data_t *settings)
{
	/* Per-scene buckets — primary persistence format. */
	obs_data_array_t *barr = obs_data_array_create();
	for (int i = 0; i < d->num_buckets; i++) {
		struct shot_preset_bucket *bk = &d->buckets[i];
		obs_data_t *bobj = obs_data_create();
		obs_data_set_string(bobj, "scene_name", bk->scene_name);
		obs_data_set_int(bobj, "current_preset", bk->current_preset);
		obs_data_set_int(bobj, "default_preset", bk->default_preset);
		obs_data_array_t *parr = obs_data_array_create();
		for (int j = 0; j < bk->num_presets; j++) {
			obs_data_t *pobj = obs_data_create();
			save_one_preset(pobj, &bk->presets[j]);
			obs_data_array_push_back(parr, pobj);
			obs_data_release(pobj);
		}
		obs_data_set_array(bobj, "presets", parr);
		obs_data_array_release(parr);
		obs_data_array_push_back(barr, bobj);
		obs_data_release(bobj);
	}
	obs_data_set_array(settings, "scene_buckets", barr);
	obs_data_array_release(barr);

	/* Strip the legacy flat "presets" key — older versions read from it
	 * and would clobber the buckets on load. Leaving it stale risks data
	 * divergence if someone downgrades. */
	obs_data_erase(settings, "presets");

	/* Persist enabled scenes both as an explicit array (primary record)
	 * and as scene_enabled_<name> bools (so the properties-dialog
	 * checkboxes stay in sync). */
	obs_data_array_t *sarr = obs_data_array_create();
	for (int i = 0; i < d->num_enabled_scenes; i++) {
		obs_data_t *obj = obs_data_create();
		obs_data_set_string(obj, "name", d->enabled_scenes[i]);
		obs_data_array_push_back(sarr, obj);
		obs_data_release(obj);
	}
	obs_data_set_array(settings, "enabled_scenes", sarr);
	obs_data_array_release(sarr);
}

/* Load a single preset slot from obs_data. Inverse of save_one_preset. */
static void load_one_preset(obs_data_t *obj, struct shot_preset *p)
{
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

	obs_enter_graphics();
	char *err = NULL;
	d->fade_effect = gs_effect_create(FADE_EFFECT_HLSL,
	                                  "shot-presets-fade.effect", &err);
	d->fade_source_cap = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	if (!d->fade_effect) {
		blog(LOG_ERROR,
		     "[Shot Presets] fade effect compile failed: %s",
		     err ? err : "(no error message)");
	}
	if (err) bfree(err);
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

	/* Enabled-scenes list. Prefer the explicit array if present;
	 * fall back to legacy `home_scene` string for backward compat. */
	obs_data_array_t *sarr = obs_data_get_array(settings, "enabled_scenes");
	if (sarr) {
		size_t count = obs_data_array_count(sarr);
		if (count > MAX_ENABLED_SCENES)
			count = MAX_ENABLED_SCENES;
		for (size_t i = 0; i < count; i++) {
			obs_data_t *obj = obs_data_array_item(sarr, i);
			const char *nm = obs_data_get_string(obj, "name");
			if (nm && *nm) {
				snprintf(d->enabled_scenes[d->num_enabled_scenes],
				         256, "%s", nm);
				d->num_enabled_scenes++;
			}
			obs_data_release(obj);
		}
		obs_data_array_release(sarr);
	} else {
		const char *legacy_hs =
			obs_data_get_string(settings, "home_scene");
		if (legacy_hs && *legacy_hs) {
			snprintf(d->enabled_scenes[0], 256, "%s", legacy_hs);
			d->num_enabled_scenes = 1;
		}
	}

	/* Load presets — preferring per-scene buckets, falling back to the
	 * legacy flat "presets" array which gets migrated into a template
	 * that seeds new buckets on first scene activation. */
	obs_data_array_t *barr = obs_data_get_array(settings, "scene_buckets");
	if (barr) {
		size_t bcount = obs_data_array_count(barr);
		if (bcount > MAX_SCENE_BUCKETS) bcount = MAX_SCENE_BUCKETS;
		for (size_t bi = 0; bi < bcount; bi++) {
			obs_data_t *bobj = obs_data_array_item(barr, bi);
			struct shot_preset_bucket *bk = &d->buckets[d->num_buckets++];
			memset(bk, 0, sizeof(*bk));
			const char *sn = obs_data_get_string(bobj, "scene_name");
			snprintf(bk->scene_name, sizeof(bk->scene_name), "%s",
			         sn ? sn : "");
			bk->current_preset = obs_data_has_user_value(bobj,
				"current_preset") ? (int)obs_data_get_int(bobj,
				"current_preset") : -1;
			bk->default_preset = obs_data_has_user_value(bobj,
				"default_preset") ? (int)obs_data_get_int(bobj,
				"default_preset") : -1;
			obs_data_array_t *parr = obs_data_get_array(bobj, "presets");
			if (parr) {
				size_t pcount = obs_data_array_count(parr);
				if (pcount > MAX_PRESETS) pcount = MAX_PRESETS;
				for (size_t pi = 0; pi < pcount; pi++) {
					obs_data_t *pobj = obs_data_array_item(parr, pi);
					load_one_preset(pobj, &bk->presets[bk->num_presets++]);
					obs_data_release(pobj);
				}
				obs_data_array_release(parr);
			}
			obs_data_release(bobj);
		}
		obs_data_array_release(barr);
	} else {
		/* Legacy migration path: read the flat "presets" array into the
		 * legacy_template. The first time each scene's bucket is auto-
		 * created, seed_bucket_from_legacy copies it in. We also pre-
		 * seed buckets for every currently-enabled scene so the user's
		 * existing data lands somewhere reachable even without revisits. */
		obs_data_array_t *arr = obs_data_get_array(settings, "presets");
		if (arr) {
			size_t count = obs_data_array_count(arr);
			if (count > MAX_PRESETS) count = MAX_PRESETS;
			for (size_t i = 0; i < count; i++) {
				obs_data_t *obj = obs_data_array_item(arr, i);
				load_one_preset(obj, &d->legacy_presets[d->legacy_num_presets++]);
				obs_data_release(obj);
			}
			obs_data_array_release(arr);
			if (d->legacy_num_presets > 0) {
				d->has_legacy_template = true;
				blog(LOG_INFO,
				     "[Shot Presets] migrating %d legacy presets to "
				     "per-scene buckets",
				     d->legacy_num_presets);
				/* Pre-seed a bucket per enabled scene, plus a default
				 * "" bucket so unbound filters still resolve before
				 * the first scene-change event. */
				for (int i = 0; i < d->num_enabled_scenes &&
				     d->num_buckets < MAX_SCENE_BUCKETS; i++) {
					get_or_create_bucket(d,
						d->enabled_scenes[i]);
				}
				if (d->num_buckets == 0)
					get_or_create_bucket(d, "");
			}
		}
	}

	/* Default to 3 presets in the default bucket if nothing was loaded. */
	if (d->num_buckets == 0) {
		struct shot_preset_bucket *bk = get_or_create_bucket(d, "");
		if (bk && bk->num_presets == 0) {
			const char *default_names[] = {"Wide", "Medium", "Close-up"};
			for (int i = 0; i < 3; i++) {
				snprintf(bk->presets[i].name, PRESET_NAME_LEN, "%s",
				         default_names[i]);
				bk->presets[i].scale_x = 1.0f;
				bk->presets[i].scale_y = 1.0f;
			}
			bk->num_presets = 3;
		}
	}

	/* Register MAX_PRESETS hotkeys with generic names. The callback
	 * resolves to the active bucket so the same key works across scenes
	 * for the same slot. Description is generic since preset names now
	 * vary per scene. */
	for (int i = 0; i < MAX_PRESETS; i++) {
		struct dstr hk_name = {0};
		struct dstr hk_desc = {0};
		dstr_printf(&hk_name, "shot_preset_%d", i);
		dstr_printf(&hk_desc, "Shot Preset %d (per-scene)", i + 1);
		d->hotkeys[i] = obs_hotkey_register_source(
			source, hk_name.array, hk_desc.array, hotkey_cb, d);
		dstr_free(&hk_name);
		dstr_free(&hk_desc);
	}

	blog(LOG_INFO, "[Shot Presets] filter_create returning d=%p (buckets=%d)",
	     (void *)d, d->num_buckets);
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
	if (d->fade_effect || d->fade_source_cap) {
		obs_enter_graphics();
		if (d->fade_effect) gs_effect_destroy(d->fade_effect);
		if (d->fade_source_cap) gs_texrender_destroy(d->fade_source_cap);
		obs_leave_graphics();
	}
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
	struct shot_preset_bucket *bk = active_bucket(d);
	if (!bk || idx < 0 || idx >= bk->num_presets)
		return false;

	if (capture_transform(d, &bk->presets[idx])) {
		bk->presets[idx].active = true;
		obs_data_t *settings = obs_source_get_settings(d->source);
		save_presets(d, settings);
		obs_data_release(settings);
		blog(LOG_INFO, "[Shot Presets] Saved transform for '%s'",
		     bk->presets[idx].name);
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
	struct shot_preset_bucket *bk = active_bucket(d);
	if (!bk || bk->num_presets >= MAX_PRESETS)
		return true;

	struct shot_preset *p = &bk->presets[bk->num_presets];
	memset(p, 0, sizeof(*p));
	snprintf(p->name, PRESET_NAME_LEN, "Shot %d", bk->num_presets + 1);
	p->scale_x = 1.0f;
	p->scale_y = 1.0f;
	bk->num_presets++;

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
	struct shot_preset_bucket *bk = active_bucket(d);
	if (!bk || bk->num_presets <= 0)
		return true;
	bk->num_presets--;
	if (bk->current_preset >= bk->num_presets)
		bk->current_preset = -1;
	if (bk->default_preset >= bk->num_presets)
		bk->default_preset = -1;
	obs_data_t *settings = obs_source_get_settings(d->source);
	save_presets(d, settings);
	obs_data_release(settings);
	return true;
}

static obs_properties_t *filter_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	/* The Shot Presets dock owns all the preset UI. This dialog is just
	 * the per-scene enable/disable checkboxes: tick every scene where
	 * this filter should drive shot presets. Leave all unchecked to
	 * apply in any scene containing the parent source. */
	obs_properties_add_text(props, "scene_help",
		"Enable Shot Presets in these scenes "
		"(leave all unchecked to use in any scene):",
		OBS_TEXT_INFO);

	struct obs_frontend_source_list scenes = {0};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		const char *name =
			obs_source_get_name(scenes.sources.array[i]);
		if (!name || !*name)
			continue;
		char key[320];
		snprintf(key, sizeof(key), "scene_enabled_%s", name);
		obs_properties_add_bool(props, key, name);
	}
	obs_frontend_source_list_free(&scenes);

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

	/* Rebuild the enabled-scenes list from per-scene checkbox state.
	 * Every scene currently known to the frontend gets a `scene_enabled_<name>`
	 * bool; we collect the true ones into the in-memory list. */
	d->num_enabled_scenes = 0;
	struct obs_frontend_source_list scenes = {0};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0;
	     i < scenes.sources.num &&
	     d->num_enabled_scenes < MAX_ENABLED_SCENES;
	     i++) {
		const char *name =
			obs_source_get_name(scenes.sources.array[i]);
		if (!name || !*name)
			continue;
		char key[320];
		snprintf(key, sizeof(key), "scene_enabled_%s", name);
		if (obs_data_get_bool(settings, key)) {
			snprintf(d->enabled_scenes[d->num_enabled_scenes],
			         256, "%s", name);
			d->num_enabled_scenes++;
		}
	}
	obs_frontend_source_list_free(&scenes);

	/* Apply any preset_name_<i> overrides from the properties dialog to
	 * the active bucket only. The properties UI doesn't enumerate buckets;
	 * it operates on whichever scene is currently active. */
	struct shot_preset_bucket *bk = peek_active_bucket(d);
	if (bk) {
		for (int i = 0; i < bk->num_presets; i++) {
			char prop_name[64];
			snprintf(prop_name, sizeof(prop_name),
			         "preset_name_%d", i);
			const char *new_name = obs_data_get_string(settings,
				prop_name);
			if (new_name && *new_name)
				snprintf(bk->presets[i].name, PRESET_NAME_LEN,
				         "%s", new_name);
		}
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
	if (d->animating)
		return;
	struct shot_preset_bucket *bk = peek_active_bucket(d);
	if (!bk || !bk->user_activated)
		return;
	if (bk->current_preset < 0 || bk->current_preset >= bk->num_presets)
		return;

	struct shot_preset *p = &bk->presets[bk->current_preset];
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

	if (d->fade_enabled) {
		/* Cross-dissolve: transform stays at "identity fill canvas"
		 * on the sceneitem for the whole fade; filter_render draws
		 * the source twice with from_mtx/to_mtx. Here we just drive
		 * t so filter_render knows the blend weight. */
		d->fade_t = get_eased(t, d->easing_type, d->easing_function);
	} else {
		float eased_t = get_eased(t, d->easing_type, d->easing_function);
		apply_transform(d, &d->from, &d->to, eased_t);
	}

	if (finished) {
		struct shot_preset_bucket *fbk = peek_active_bucket(d);
		if (d->fade_enabled) {
			/* Restore the real target transform so the parked
			 * sceneitem reflects the user's preset, not the
			 * identity-fill-canvas override. */
			if (fbk && fbk->current_preset >= 0 &&
			    fbk->current_preset < fbk->num_presets &&
			    fbk->presets[fbk->current_preset].active) {
				struct shot_preset final =
					fbk->presets[fbk->current_preset];
				apply_transform(d, &final, &final, 1.0f);
			}
			d->fade_enabled = false;
			d->fade_t = 1.0f;
			blog(LOG_INFO,
			     "[Shot Presets] fade FINISH after %.3f s",
			     d->running_duration);
		} else {
			/* d->to was flattened to bounds_type=NONE for the
			 * animation when the endpoints had mismatched
			 * bounds_types. Snap to the original preset now so the
			 * parked sceneitem carries the user's real
			 * bounds_type/bounds/alignment. */
			if (fbk && fbk->current_preset >= 0 &&
			    fbk->current_preset < fbk->num_presets &&
			    fbk->presets[fbk->current_preset].active) {
				struct shot_preset final =
					fbk->presets[fbk->current_preset];
				apply_transform(d, &final, &final, 1.0f);
			}
			blog(LOG_INFO,
			     "[Shot Presets] move FINISH after %.3f s",
			     d->running_duration);
		}
		/* Reset debounce so first post-animation sync waits a beat. */
		d->sync_debounce = 0.0f;
		d->sync_dirty = false;
	}
}

/* ── Size callbacks ──────────────────────────────────────────
 * During a cross-dissolve the filter produces a canvas-sized output
 * that composites the source at two different framings. OBS queries
 * these to size the filter's output texture and set up the
 * projection, so reporting canvas dims here is what makes the
 * canvas-space compositing line up with the scene. */

/* Use get_target (the source as it feeds INTO this filter) + base_width
 * to avoid re-entering the filter chain and recursing infinitely. */
static uint32_t filter_get_width(void *data)
{
	struct shot_presets_data *d = data;
	if (d->fade_enabled && d->fade_canvas_w > 0)
		return d->fade_canvas_w;
	obs_source_t *target = obs_filter_get_target(d->source);
	return target ? obs_source_get_base_width(target) : 0;
}

static uint32_t filter_get_height(void *data)
{
	struct shot_presets_data *d = data;
	if (d->fade_enabled && d->fade_canvas_h > 0)
		return d->fade_canvas_h;
	obs_source_t *target = obs_filter_get_target(d->source);
	return target ? obs_source_get_base_height(target) : 0;
}

/* ── video_render ────────────────────────────────────────────
 * Pass-through when not fading. During a cross-dissolve, capture
 * the source pixels into a source-sized texrender, then draw that
 * capture twice into the current (canvas-sized) filter output using
 * fade_from_mtx / fade_to_mtx at complementary opacities — giving a
 * true cross-fade between the two framings. */

static void draw_capture_at_matrix(gs_effect_t *effect, gs_texture_t *tex,
                                   const struct matrix4 *mtx, float opacity,
                                   uint32_t sw, uint32_t sh)
{
	gs_eparam_t *p_img = gs_effect_get_param_by_name(effect, "image");
	gs_eparam_t *p_op  = gs_effect_get_param_by_name(effect, "opacity");
	if (p_img) gs_effect_set_texture(p_img, tex);
	if (p_op)  gs_effect_set_float(p_op, opacity);

	/* sprite verts are in (0..sw, 0..sh); the sceneitem box matrix
	 * maps normalized (0..1) box-space to canvas-space. Pre-scale the
	 * verts down to 0..1 before the matrix sends them to canvas. */
	gs_matrix_push();
	gs_matrix_mul(mtx);
	gs_matrix_scale3f(1.0f / (float)sw, 1.0f / (float)sh, 1.0f);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(tex, 0, 0, 0);
	gs_matrix_pop();
}

static void filter_render(void *data, gs_effect_t *effect)
{
	struct shot_presets_data *d = data;
	UNUSED_PARAMETER(effect);

	if (!d->fade_enabled || !d->fade_effect || !d->fade_source_cap) {
		obs_source_skip_video_filter(d->source);
		return;
	}

	uint32_t sw = d->fade_src_w;
	uint32_t sh = d->fade_src_h;
	uint32_t cw = d->fade_canvas_w;
	uint32_t ch = d->fade_canvas_h;
	if (!sw || !sh || !cw || !ch) {
		obs_source_skip_video_filter(d->source);
		return;
	}

	/* Step 1 — capture source pixels (what our filter would see as
	 * input) into a source-sized offscreen texture, so we can draw
	 * it multiple times into the canvas-sized output.
	 *
	 * Pattern matters: process_filter_begin FIRST (it sets up filter
	 * input state), THEN texrender_begin, ortho, process_filter_end,
	 * texrender_end. Reversing these silently produces an empty tex. */
	if (obs_source_process_filter_begin(d->source, GS_RGBA,
	                                    OBS_ALLOW_DIRECT_RENDERING)) {
		gs_texrender_reset(d->fade_source_cap);
		if (gs_texrender_begin(d->fade_source_cap, sw, sh)) {
			struct vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)sw, 0.0f, (float)sh,
			         -100.0f, 100.0f);

			obs_source_process_filter_end(d->source,
				obs_get_base_effect(OBS_EFFECT_DEFAULT),
				sw, sh);

			gs_texrender_end(d->fade_source_cap);
		}
	}
	gs_texture_t *cap = gs_texrender_get_texture(d->fade_source_cap);
	if (d->fade_diag_frames < 3) {
		blog(LOG_INFO,
		     "[Shot Presets] fade render frame=%d t=%.3f sw=%u sh=%u "
		     "cw=%u ch=%u cap=%p effect=%p",
		     d->fade_diag_frames, d->fade_t, sw, sh, cw, ch,
		     (void *)cap, (void *)d->fade_effect);
		d->fade_diag_frames++;
	}
	if (!cap) {
		obs_source_skip_video_filter(d->source);
		return;
	}

	/* Step 2 — we're now back in the filter's (canvas-sized) output
	 * render target. Draw the capture twice, cross-blended. */
	float t = d->fade_t;
	float op_from = 1.0f - t;
	float op_to   = t;

	/* Additive blending onto the cleared (transparent black) target so
	 * the two passes sum to from*(1-t) + to*t. SRCALPHA/INVSRCALPHA
	 * would double-darken the mid-point (second pass draws onto an
	 * already-faded buffer, compounding the attenuation). */
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
	gs_enable_depth_test(false);

	if (op_from > 0.001f)
		draw_capture_at_matrix(d->fade_effect, cap,
		                       &d->fade_from_mtx, op_from, sw, sh);
	if (op_to > 0.001f)
		draw_capture_at_matrix(d->fade_effect, cap,
		                       &d->fade_to_mtx, op_to, sw, sh);

	gs_blend_state_pop();
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
	int total = 0;
	for (int i = 0; i < d->num_buckets; i++)
		total += d->buckets[i].num_presets;
	blog(LOG_INFO,
	     "[Shot Presets] filter_save flushed %d buckets / %d presets for instance=%p",
	     d->num_buckets, total, (void *)d);
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
	.get_width = filter_get_width,
	.get_height = filter_get_height,
};

/* Defined in shot-presets-dock.cpp */
extern void shot_presets_dock_init(void);

static void update_active_for_current_scene(void)
{
	obs_source_t *scene_src = obs_frontend_get_current_scene();
	if (!scene_src) {
		g_active_instance = NULL;
		g_active_scene_name[0] = '\0';
		blog(LOG_INFO,
		     "[Shot Presets] active=NULL (no current scene)");
		return;
	}
	const char *scene_name = obs_source_get_name(scene_src);
	/* Update the global current-scene name BEFORE resolving the active
	 * instance — active_bucket() consults it. */
	snprintf(g_active_scene_name, sizeof(g_active_scene_name), "%s",
	         scene_name ? scene_name : "");
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
			struct shot_presets_data *cand =
				g_instances[i].data;
			/* Honor enabled-scene list: a filter only activates
			 * in scenes the user has ticked. Unbound filters
			 * (empty list) fall back to matching any scene
			 * containing their source — so the dock is reachable
			 * for initial setup. */
			if (!is_scene_enabled(cand, scene_name))
				continue;
			if (obs_scene_find_source_recursive(scene, pname)) {
				match = cand;
				matched_source = pname;
				break;
			}
		}
	}
	g_active_instance = match;
	int bucket_count = 0;
	int active_bucket_presets = 0;
	if (match) {
		bucket_count = match->num_buckets;
		struct shot_preset_bucket *bk = peek_active_bucket(match);
		if (bk) active_bucket_presets = bk->num_presets;
	}
	blog(LOG_INFO,
	     "[Shot Presets] scene='%s' instances=%d active=%p (source='%s') "
	     "buckets=%d active_bucket_presets=%d",
	     scene_name ? scene_name : "(null)", g_instance_count,
	     (void *)g_active_instance,
	     matched_source ? matched_source : "(none)",
	     bucket_count, active_bucket_presets);
	obs_source_release(scene_src);

	/* Baseline snap: on scene activation, put the sceneitem into a
	 * known state so every subsequent go_to starts from the preset
	 * it's tweening from — not whatever OBS happened to save on the
	 * sceneitem. Prefer current_preset (user's last click, preserved
	 * across scene switches); fall back to preset[0] as the default
	 * framing. Silent — doesn't flip user_activated, so live-edit
	 * sync stays gated until the user explicitly picks a preset. */
	if (match) {
		struct shot_preset_bucket *bk = peek_active_bucket(match);
		if (bk) {
			/* Snap target priority: scene's explicit default →
			 * last-clicked (current_preset) → preset 0. Each
			 * fallback is gated on the preset being active. */
			int target = -1;
			if (bk->default_preset >= 0 &&
			    bk->default_preset < bk->num_presets &&
			    bk->presets[bk->default_preset].active)
				target = bk->default_preset;
			if (target < 0 && bk->current_preset >= 0 &&
			    bk->current_preset < bk->num_presets &&
			    bk->presets[bk->current_preset].active)
				target = bk->current_preset;
			if (target < 0 && bk->num_presets > 0 &&
			    bk->presets[0].active)
				target = 0;
			if (target >= 0) {
				snap_to_preset_silent(match, target);
				blog(LOG_INFO,
				     "[Shot Presets] baseline snap -> preset %d '%s' "
				     "(default=%d current=%d)",
				     target, bk->presets[target].name,
				     bk->default_preset, bk->current_preset);
			}
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
