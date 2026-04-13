#include <obs-module.h>
#include "shot-presets-filter.h"

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("FactMachine")
OBS_MODULE_USE_DEFAULT_LOCALE("obs-shot-presets", "en-US")

bool obs_module_load(void)
{
	obs_register_source(&shot_presets_filter_info);
	blog(LOG_INFO, "[Shot Presets] Plugin loaded (version %s)",
	     PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[Shot Presets] Plugin unloaded");
}
