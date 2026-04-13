--[[
  Shot Presets — OBS Lua Script

  Smoothly animate a single camera source between saved crop/transform presets.
  No scene switching needed. Works within one scene.

  Install: OBS → Tools → Scripts → + → select this file
--]]

obs = obslua

-- ── Settings ────────────────────────────────────────────────
local source_name = ""
local duration_ms = 400
local easing_type = "in_out"  -- "none", "in", "out", "in_out"
local easing_func = "cubic"   -- "linear", "quad", "cubic", "sine", "expo"

-- ── Preset storage ──────────────────────────────────────────
local MAX_PRESETS = 8
local presets = {}  -- { [1] = { name="Wide", active=true, pos_x=0, ... }, ... }
local num_presets = 0

-- ── Animation state ─────────────────────────────────────────
local animating = false
local anim_elapsed = 0.0
local anim_from = {}
local anim_to = {}

-- ── Easing functions ────────────────────────────────────────

local function ease_linear(t) return t end

local function ease_quad_in(t) return t * t end
local function ease_quad_out(t) return t * (2 - t) end
local function ease_quad_inout(t)
	if t < 0.5 then return 2 * t * t end
	return -1 + (4 - 2 * t) * t
end

local function ease_cubic_in(t) return t * t * t end
local function ease_cubic_out(t) local f = t - 1; return f * f * f + 1 end
local function ease_cubic_inout(t)
	if t < 0.5 then return 4 * t * t * t end
	local f = 2 * t - 2
	return 0.5 * f * f * f + 1
end

local function ease_sine_in(t) return 1 - math.cos(t * math.pi * 0.5) end
local function ease_sine_out(t) return math.sin(t * math.pi * 0.5) end
local function ease_sine_inout(t) return 0.5 * (1 - math.cos(t * math.pi)) end

local function ease_expo_in(t)
	if t == 0 then return 0 end
	return math.pow(2, 10 * (t - 1))
end
local function ease_expo_out(t)
	if t == 1 then return 1 end
	return 1 - math.pow(2, -10 * t)
end
local function ease_expo_inout(t)
	if t == 0 then return 0 end
	if t == 1 then return 1 end
	if t < 0.5 then return 0.5 * math.pow(2, 20 * t - 10) end
	return 1 - 0.5 * math.pow(2, -20 * t + 10)
end

local ease_table = {
	linear = { ease_linear, ease_linear, ease_linear },
	quad   = { ease_quad_in, ease_quad_out, ease_quad_inout },
	cubic  = { ease_cubic_in, ease_cubic_out, ease_cubic_inout },
	sine   = { ease_sine_in, ease_sine_out, ease_sine_inout },
	expo   = { ease_expo_in, ease_expo_out, ease_expo_inout },
}

local function get_eased(t)
	local funcs = ease_table[easing_func] or ease_table["cubic"]
	if easing_type == "none" then return t end
	if easing_type == "in" then return funcs[1](t) end
	if easing_type == "out" then return funcs[2](t) end
	return funcs[3](t) -- in_out
end

-- ── Scene item lookup ───────────────────────────────────────

local function find_scene_item()
	local scene_source = obs.obs_frontend_get_current_scene()
	if not scene_source then return nil, nil end

	local scene = obs.obs_scene_from_source(scene_source)
	local item = obs.obs_scene_find_source_recursive(scene, source_name)

	-- Don't release scene_source yet — caller must do it after using item
	return item, scene_source
end

local function capture_current()
	local item, scene_source = find_scene_item()
	if not item then
		if scene_source then obs.obs_source_release(scene_source) end
		return nil
	end

	local pos = obs.vec2()
	obs.obs_sceneitem_get_pos(item, pos)

	local scale = obs.vec2()
	obs.obs_sceneitem_get_scale(item, scale)

	local crop = obs.obs_sceneitem_crop()
	obs.obs_sceneitem_get_crop(item, crop)

	local bounds = obs.vec2()
	obs.obs_sceneitem_get_bounds(item, bounds)

	local result = {
		pos_x = pos.x, pos_y = pos.y,
		scale_x = scale.x, scale_y = scale.y,
		rotation = obs.obs_sceneitem_get_rot(item),
		crop_left = crop.left, crop_top = crop.top,
		crop_right = crop.right, crop_bottom = crop.bottom,
		bounds_x = bounds.x, bounds_y = bounds.y,
	}

	obs.obs_source_release(scene_source)
	return result
end

local function apply_lerped(from, to, t)
	local item, scene_source = find_scene_item()
	if not item then
		if scene_source then obs.obs_source_release(scene_source) end
		return
	end

	-- Clamp for crop
	local ct = math.max(0, math.min(1, t))

	local function lerp(a, b, tt) return a + (b - a) * tt end

	obs.obs_sceneitem_defer_update_begin(item)

	local pos = obs.vec2()
	pos.x = lerp(from.pos_x, to.pos_x, t)
	pos.y = lerp(from.pos_y, to.pos_y, t)
	obs.obs_sceneitem_set_pos(item, pos)

	local scale = obs.vec2()
	scale.x = lerp(from.scale_x, to.scale_x, t)
	scale.y = lerp(from.scale_y, to.scale_y, t)
	obs.obs_sceneitem_set_scale(item, scale)

	obs.obs_sceneitem_set_rot(item, lerp(from.rotation, to.rotation, t))

	local crop = obs.obs_sceneitem_crop()
	crop.left = math.floor(lerp(from.crop_left, to.crop_left, ct))
	crop.top = math.floor(lerp(from.crop_top, to.crop_top, ct))
	crop.right = math.floor(lerp(from.crop_right, to.crop_right, ct))
	crop.bottom = math.floor(lerp(from.crop_bottom, to.crop_bottom, ct))
	obs.obs_sceneitem_set_crop(item, crop)

	local bounds = obs.vec2()
	bounds.x = lerp(from.bounds_x, to.bounds_x, t)
	bounds.y = lerp(from.bounds_y, to.bounds_y, t)
	obs.obs_sceneitem_set_bounds(item, bounds)

	obs.obs_sceneitem_defer_update_end(item)

	obs.obs_source_release(scene_source)
end

-- ── Animation tick ──────────────────────────────────────────

local function tick_animation()
	if not animating then return end

	-- OBS timer fires at ~100Hz; each tick is ~10ms
	anim_elapsed = anim_elapsed + 10

	local dur = duration_ms
	if dur <= 0 then dur = 1 end

	local t = anim_elapsed / dur
	if t >= 1.0 then
		t = 1.0
		animating = false
		obs.timer_remove(tick_animation)
	end

	local eased_t = get_eased(t)
	apply_lerped(anim_from, anim_to, eased_t)
end

-- ── Go to preset ────────────────────────────────────────────

local function go_to_preset(index)
	if index < 1 or index > num_presets then return end
	local p = presets[index]
	if not p or not p.active then
		obs.script_log(obs.LOG_WARNING,
			"Shot Presets: preset " .. index .. " has no saved transform")
		return
	end

	local current = capture_current()
	if not current then
		obs.script_log(obs.LOG_WARNING,
			"Shot Presets: could not find source '" .. source_name .. "'")
		return
	end

	anim_from = current
	anim_to = p
	anim_elapsed = 0

	-- Stop any running animation
	if animating then
		obs.timer_remove(tick_animation)
	end

	animating = true
	obs.timer_add(tick_animation, 10) -- ~100Hz update
end

-- ── Hotkey callbacks (one per preset) ───────────────────────

local hotkey_ids = {}

local function make_hotkey_cb(index)
	return function(pressed)
		if pressed then go_to_preset(index) end
	end
end

-- ── Script interface ────────────────────────────────────────

function script_description()
	return [[<h2>Shot Presets</h2>
<p>Save and recall camera crop/transform presets with smooth animated transitions. No scene switching needed.</p>
<ol>
<li>Select your camera source</li>
<li>Add presets (up to 8)</li>
<li>Position your source, then click <b>Get Transform</b> for each preset</li>
<li>Use the <b>Go To</b> buttons or assign hotkeys in Settings → Hotkeys</li>
</ol>]]
end

function script_properties()
	local props = obs.obs_properties_create()

	-- Source picker
	local src = obs.obs_properties_add_list(props, "source_name",
		"Camera Source", obs.OBS_COMBO_TYPE_EDITABLE,
		obs.OBS_COMBO_FORMAT_STRING)
	local sources = obs.obs_enum_sources()
	if sources then
		for _, s in ipairs(sources) do
			obs.obs_property_list_add_string(src, obs.obs_source_get_name(s),
				obs.obs_source_get_name(s))
		end
		obs.source_list_release(sources)
	end

	-- Transition settings
	obs.obs_properties_add_int(props, "duration", "Duration (ms)", 50, 5000, 50)

	local et = obs.obs_properties_add_list(props, "easing_type", "Easing Type",
		obs.OBS_COMBO_TYPE_LIST, obs.OBS_COMBO_FORMAT_STRING)
	obs.obs_property_list_add_string(et, "None", "none")
	obs.obs_property_list_add_string(et, "Ease In", "in")
	obs.obs_property_list_add_string(et, "Ease Out", "out")
	obs.obs_property_list_add_string(et, "Ease In/Out", "in_out")

	local ef = obs.obs_properties_add_list(props, "easing_func", "Easing Function",
		obs.OBS_COMBO_TYPE_LIST, obs.OBS_COMBO_FORMAT_STRING)
	obs.obs_property_list_add_string(ef, "Linear", "linear")
	obs.obs_property_list_add_string(ef, "Quadratic", "quad")
	obs.obs_property_list_add_string(ef, "Cubic", "cubic")
	obs.obs_property_list_add_string(ef, "Sine", "sine")
	obs.obs_property_list_add_string(ef, "Exponential", "expo")

	-- Number of presets
	local np = obs.obs_properties_add_int_slider(props, "num_presets",
		"Number of Presets", 1, MAX_PRESETS, 1)
	obs.obs_property_set_modified_callback(np, function(p2, property, settings)
		-- Refresh properties when count changes
		return true
	end)

	obs.obs_properties_add_separator(props)

	-- Per-preset controls
	for i = 1, num_presets do
		local p = presets[i]
		local name = (p and p.name) or ("Shot " .. i)
		local status = (p and p.active) and "Ready" or "Empty"

		obs.obs_properties_add_text(props, "preset_name_" .. i,
			"Preset " .. i .. " Name", obs.OBS_TEXT_DEFAULT)

		obs.obs_properties_add_button(props, "get_transform_" .. i,
			"[" .. name .. "] Get Transform",
			function(p2, prop)
				local current = capture_current()
				if current then
					if not presets[i] then presets[i] = {} end
					for k, v in pairs(current) do presets[i][k] = v end
					presets[i].active = true
					presets[i].name = name
					-- Save to settings
					local s = obs.obs_source_get_settings(nil) -- hack: use script_update
					save_presets()
					obs.script_log(obs.LOG_INFO,
						"Shot Presets: saved transform for '" .. name .. "'")
				else
					obs.script_log(obs.LOG_WARNING,
						"Shot Presets: could not find source")
				end
				return true
			end)

		obs.obs_properties_add_button(props, "go_to_" .. i,
			"[" .. status .. "] Go To " .. name,
			function(p2, prop)
				go_to_preset(i)
				return false
			end)

		obs.obs_properties_add_separator(props)
	end

	return props
end

function script_defaults(settings)
	obs.obs_data_set_default_int(settings, "duration", 400)
	obs.obs_data_set_default_string(settings, "easing_type", "in_out")
	obs.obs_data_set_default_string(settings, "easing_func", "cubic")
	obs.obs_data_set_default_int(settings, "num_presets", 3)
end

-- ── Save / Load presets to OBS settings ─────────────────────

function save_presets()
	-- This is called via script_save
end

function script_save(settings)
	obs.obs_data_set_int(settings, "num_presets", num_presets)
	for i = 1, num_presets do
		local p = presets[i]
		if p then
			local prefix = "p" .. i .. "_"
			obs.obs_data_set_string(settings, prefix .. "name", p.name or ("Shot " .. i))
			obs.obs_data_set_bool(settings, prefix .. "active", p.active or false)
			obs.obs_data_set_double(settings, prefix .. "pos_x", p.pos_x or 0)
			obs.obs_data_set_double(settings, prefix .. "pos_y", p.pos_y or 0)
			obs.obs_data_set_double(settings, prefix .. "scale_x", p.scale_x or 1)
			obs.obs_data_set_double(settings, prefix .. "scale_y", p.scale_y or 1)
			obs.obs_data_set_double(settings, prefix .. "rotation", p.rotation or 0)
			obs.obs_data_set_int(settings, prefix .. "crop_left", p.crop_left or 0)
			obs.obs_data_set_int(settings, prefix .. "crop_top", p.crop_top or 0)
			obs.obs_data_set_int(settings, prefix .. "crop_right", p.crop_right or 0)
			obs.obs_data_set_int(settings, prefix .. "crop_bottom", p.crop_bottom or 0)
			obs.obs_data_set_double(settings, prefix .. "bounds_x", p.bounds_x or 0)
			obs.obs_data_set_double(settings, prefix .. "bounds_y", p.bounds_y or 0)
		end
	end

	-- Save hotkeys
	for i = 1, MAX_PRESETS do
		if hotkey_ids[i] then
			local arr = obs.obs_hotkey_save(hotkey_ids[i])
			obs.obs_data_set_array(settings, "hotkey_preset_" .. i, arr)
			obs.obs_data_array_release(arr)
		end
	end
end

function script_load(settings)
	-- Load presets
	num_presets = obs.obs_data_get_int(settings, "num_presets")
	if num_presets < 1 then num_presets = 3 end
	if num_presets > MAX_PRESETS then num_presets = MAX_PRESETS end

	for i = 1, num_presets do
		local prefix = "p" .. i .. "_"
		presets[i] = {
			name = obs.obs_data_get_string(settings, prefix .. "name"),
			active = obs.obs_data_get_bool(settings, prefix .. "active"),
			pos_x = obs.obs_data_get_double(settings, prefix .. "pos_x"),
			pos_y = obs.obs_data_get_double(settings, prefix .. "pos_y"),
			scale_x = obs.obs_data_get_double(settings, prefix .. "scale_x"),
			scale_y = obs.obs_data_get_double(settings, prefix .. "scale_y"),
			rotation = obs.obs_data_get_double(settings, prefix .. "rotation"),
			crop_left = obs.obs_data_get_int(settings, prefix .. "crop_left"),
			crop_top = obs.obs_data_get_int(settings, prefix .. "crop_top"),
			crop_right = obs.obs_data_get_int(settings, prefix .. "crop_right"),
			crop_bottom = obs.obs_data_get_int(settings, prefix .. "crop_bottom"),
			bounds_x = obs.obs_data_get_double(settings, prefix .. "bounds_x"),
			bounds_y = obs.obs_data_get_double(settings, prefix .. "bounds_y"),
		}
		if not presets[i].name or presets[i].name == "" then
			presets[i].name = "Shot " .. i
		end
	end

	-- Register hotkeys
	for i = 1, MAX_PRESETS do
		local desc = "Shot Preset: " .. ((presets[i] and presets[i].name) or ("Shot " .. i))
		hotkey_ids[i] = obs.obs_hotkey_register_frontend(
			"shot_preset_" .. i, desc, make_hotkey_cb(i))

		local arr = obs.obs_data_get_array(settings, "hotkey_preset_" .. i)
		if arr then
			obs.obs_hotkey_load(hotkey_ids[i], arr)
			obs.obs_data_array_release(arr)
		end
	end
end

function script_update(settings)
	source_name = obs.obs_data_get_string(settings, "source_name")
	duration_ms = obs.obs_data_get_int(settings, "duration")
	easing_type = obs.obs_data_get_string(settings, "easing_type")
	easing_func = obs.obs_data_get_string(settings, "easing_func")

	local new_count = obs.obs_data_get_int(settings, "num_presets")
	if new_count < 1 then new_count = 1 end
	if new_count > MAX_PRESETS then new_count = MAX_PRESETS end

	-- Initialize new presets
	for i = num_presets + 1, new_count do
		if not presets[i] then
			presets[i] = { name = "Shot " .. i, active = false,
				pos_x = 0, pos_y = 0, scale_x = 1, scale_y = 1,
				rotation = 0, crop_left = 0, crop_top = 0,
				crop_right = 0, crop_bottom = 0,
				bounds_x = 0, bounds_y = 0 }
		end
	end
	num_presets = new_count

	-- Update names from settings
	for i = 1, num_presets do
		local n = obs.obs_data_get_string(settings, "preset_name_" .. i)
		if n and n ~= "" then presets[i].name = n end
	end
end

function script_unload()
	if animating then
		obs.timer_remove(tick_animation)
		animating = false
	end
end
