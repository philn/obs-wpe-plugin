/*
 * obs-wpe-plugin. OBS Studio plugin.
 * Copyright (C) 2021 Philippe Normand <phil@base-art.net>
 *
 * This file is part of obs-wpe-plugin.
 *
 * obs-wpe-plugin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * obs-wpe-plugin is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with obs-wpe-plugin. If not, see <http://www.gnu.org/licenses/>.
 */

#include <obs/obs-module.h>
#include <gst/gst.h>

extern const char *obs_wpe_version;

OBS_DECLARE_MODULE()

extern const char *wpe_source_get_name(void *type_data);
extern void *wpe_source_create(obs_data_t *settings,
				     obs_source_t *source);
extern void wpe_source_destroy(void *data);
extern void wpe_source_get_defaults(obs_data_t *settings);
extern obs_properties_t *wpe_source_get_properties(void *data);
extern void wpe_source_update(void *data, obs_data_t *settings);
extern void wpe_source_show(void *data);
extern void wpe_source_hide(void *data);
extern void wpe_source_render(void *data, gs_effect_t*);
extern uint32_t wpe_source_get_width(void *data);
extern uint32_t wpe_source_get_height(void *data);

bool obs_module_load(void)
{
	blog(LOG_INFO, "obs-wpe-plugin build: %s", obs_wpe_version);

	struct obs_source_info source_info = {
		.id = "wpe-source",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_VIDEO /*  | OBS_SOURCE_AUDIO | */
		|OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_INTERACTION,

		.get_name = wpe_source_get_name,
		.create = wpe_source_create,
		.destroy = wpe_source_destroy,

		.get_defaults = wpe_source_get_defaults,
		.get_properties = wpe_source_get_properties,
		.update = wpe_source_update,
		.show = wpe_source_show,
		.hide = wpe_source_hide,
		.get_width = wpe_source_get_width,
		.get_height = wpe_source_get_height,
		.video_render = wpe_source_render,
	};

	obs_register_source(&source_info);

	gst_init(NULL, NULL);

	return true;
}
