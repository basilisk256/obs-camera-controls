# Shot Presets — Session Notes (2026-04-17)

Context hand-off document. Written before a computer restart so the next
Claude Code session can pick up without re-deriving everything.

## Project

- **Repo path:** `C:/Users/griff/dev/obs-shot-presets`
- **GitHub:** `basilisk256/obs-camera-controls`
- **Purpose:** OBS Studio plugin. In-scene "move tool" that stores
  crop/transform presets on a source filter and animates/cuts between
  them without scene switching (similar to Exeldro's obs-move-transition).
- **Build:** CMake + MSVC Release.
- **Install target:** `C:\Program Files\obs-studio\obs-plugins\64bit\obs-shot-presets.dll`
  (requires Admin / UAC).
- **Install script:** `install-admin.ps1` — elevate with
  `powershell -Command "Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','C:\Users\griff\dev\obs-shot-presets\install-admin.ps1'"`
  **OBS must be closed** or the file is locked and the elevated copy
  silently fails.

## Architecture

- `shot-presets-filter.c` — the OBS source filter. Stores a preset
  struct per slot, handles animation via `video_tick`, exposes a C API
  (`shot-presets-shared.h`) for the dock to call.
- `shot-presets-dock.cpp` / `.hpp` — the Qt dock UI (buttons, edit
  panel with name/transition/duration/crop, "Paste from scene" menu).
  Polls the filter via the shared C API on a 1 Hz refresh timer.
- `easing.c` / `.h` — easing curves for animation.
- `g_active_instance` — global pointer to the filter instance that
  currently backs the dock. Updated when the frontend reports a scene
  change (see `update_active_for_current_scene`).

## `struct shot_preset` fields

```c
struct shot_preset {
    char name[PRESET_NAME_LEN];
    bool active;            /* true once transform has been captured */
    float pos_x, pos_y;
    float scale_x, scale_y;
    float rotation;
    uint32_t alignment;     /* OBS_ALIGN_* bitmask */
    int crop_left, crop_top, crop_right, crop_bottom;
    float bounds_x, bounds_y;
    enum obs_bounds_type bounds_type;
    uint32_t bounds_align;
    int duration_ms;        /* 0 = use global default */
    int transition_type;    /* 0 = move (animated), 1 = cut */
};
```

Saves load with a `obs_data_has_user_value(obj, "alignment")` fallback
to `OBS_ALIGN_TOP|OBS_ALIGN_LEFT` for presets that predate the
`alignment` field.

## Key APIs / OBS quirks learnt this session

- `OBS_ALIGN_*`: `CENTER=0`, `LEFT=1`, `RIGHT=2`, `TOP=4`, `BOTTOM=8`.
  Default scene-item alignment is `TOP|LEFT = 5`.
- `OBS_BOUNDS_NONE` vs. `OBS_BOUNDS_SCALE_INNER`/`_OUTER` / `STRETCH` —
  when `bounds_type != NONE`, scale is ignored and the sceneitem
  renders at bounds dimensions.
- OBS 30+ labels `NONE` as "Automatic" in the UI when the scene item
  hasn't been given an explicit bounds type. A bounds dropdown of
  "Fit" corresponds to `OBS_BOUNDS_SCALE_INNER`.
- `obs_sceneitem_defer_update_begin/end` — batch transform writes so
  OBS only recomputes once.
- `obs_scene_find_source_recursive` — walks nested groups.

## What has been changed this session

1. Added `alignment` field to `shot_preset` + round-trip through
   capture/apply/save/load/paste (previous version missed it, which
   caused pasted transforms to render at the wrong position).
2. "Paste from scene" now live-applies via `cut_to_preset` so the
   preview updates immediately; dock refresh pulls the new crop into
   the spinboxes.
3. Added `normalize_preset_to_center(...)`: when animating or cutting,
   both endpoints are normalised to `OBS_ALIGN_CENTER` coords on the
   fly. Makes scale interpolation feel like a centred camera zoom
   instead of growing from the top-left corner. **Stored presets are
   unchanged** — normalisation is applied only to `d->from` / `d->to`.
4. Re-added the live-edit-while-parked feature that was previously
   removed (it had corrupted presets during setup). New version is
   gated on a `user_activated` flag that stays `false` until the user
   explicitly clicks a preset this session. Sync writes are debounced
   at ~0.4 s via `sync_debounce` / `sync_dirty`. Sync happens in
   `filter_tick` via `sync_sceneitem_to_parked_preset`.
5. Diagnostic `blog(LOG_INFO, ...)` lines added on: `filter_create`,
   `update_active_for_current_scene`, `shot_presets_go_to`, `go_to_preset`
   bail paths, `move START / FINISH`, `cut_to_preset`, `set_crop`,
   `paste_from_scene` (success + `WARNING` on not-found scene/item).

## Current open bug

User report, Wide→Left direction works OK-ish after the centre
normalisation, **but cutting from Left to Wide does not tween the
framing**. Instead Wide "enlarges from the middle of the screen out of
black." User also feels Left is **not being saved** when they reframe
it in OBS preview.

Hypotheses (untested):

1. `bounds_type` mismatch between Left (pasted from a scene that uses
   `OBS_BOUNDS_SCALE_INNER` / "Fit") and Wide (captured when bounds
   were `OBS_BOUNDS_NONE` / "Automatic").
   - `apply_transform` writes `to->bounds_type` every frame. At t=0
     the sceneitem's bounds_type snaps to Wide's NONE, which
     instantly changes how the item renders. If from's scale/pos was
     arbitrary, the sceneitem can collapse to a tiny box at the
     canvas origin until the interp catches up — matches "enlarges
     out of black from the middle."
2. Left's auto-sync might be overwriting Left with the sceneitem's
   post-animation state (which equals Left) so the user's in-preview
   drag *should* be captured, but maybe the sync runs during tail
   frames of the animation (t=1 path not fully flushing
   `d->animating=false` in time) and writes the final animated state
   back. Less likely because sync requires `!d->animating`.
3. Alignment mismatch: when the Wide preset was captured, alignment
   might have been set differently. Log line
   `[Shot Presets] move START -> 'Wide' ... from al=X to al=Y`
   should show it.

### Next step — capture a bounds log

The `move START` log line was expanded this session to include
`bt`/`bnds`/`ba`/`al` for both from and to. Ask the user to:

1. Relaunch OBS with the 12:44 DLL build.
2. Cut to Left (verify it lands visually correct).
3. Cut to Wide.
4. Paste the full `move START` line from `Help → Log Files → View
   Current Log`.

If `from.bounds_type != to.bounds_type`, that's the bug. Likely fix:
interpolate bounds_type only at t≥0.99 (or a threshold) **and** set
`to.bounds_type` once at animation start with bounds pre-populated
from whatever produces the same visible region as the from state.

Exeldro's `move-filter.c` at
`C:/Users/griff/AppData/Local/Temp/obs-move-transition/` has a
working reference — in particular around line 559 (`move_filter_tick`).

## Building & installing

```powershell
# From repo root
cmake --build build --config Release

# Close OBS first, then:
powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','C:\Users\griff\dev\obs-shot-presets\install-admin.ps1'"

# Verify the install copied (compare timestamps)
ls -la 'C:/Program Files/obs-studio/obs-plugins/64bit/obs-shot-presets.dll'
ls -la 'C:/Users/griff/dev/obs-shot-presets/build/Release/obs-shot-presets.dll'
```

If OBS is running the copy will silently no-op due to the file lock —
always kill OBS first.

## Files the next session will care about

- `shot-presets-filter.c` — filter + animation (all the heavy logic).
- `shot-presets-dock.cpp` — Qt dock (not touched much recently).
- `shot-presets-shared.h` — the C API between filter and dock.
- `shot-presets.lua` — unclear if active; predates the C++ dock.

## Last installed DLL

- Path: `C:/Program Files/obs-studio/obs-plugins/64bit/obs-shot-presets.dll`
- Timestamp (at time of writing): **Apr 17 12:44, 267264 bytes**.
- Source build timestamp: **Apr 17 12:44** (matches — install was clean).
