/*
Copyright (C) 2014 by Leonhard Oelke <leonhard@in-verted.de>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <obs-module.h>
#include <util/dstr.h>
#include "xcursor.h"
#include "xhelpers.h"

#define XSHM_DATA(voidptr) struct xshm_data *data = voidptr;

#define blog(level, msg, ...) blog(level, "xshm-input: " msg, ##__VA_ARGS__)

struct xshm_data {
	/** The source object */
	obs_source_t *source;
	/** Xlib display object */
	Display *dpy;
	/** Xlib screen object */
	Screen *screen;
	/** user setting - the server name to capture from */
	char *server;
	/** user setting - the id of the screen that should be captured */
	uint_fast32_t screen_id;
	/** root coordinates for the capture */
	int_fast32_t x_org, y_org;
	/** size for the capture */
	int_fast32_t width, height;
	/** shared memory management object */
	xshm_t *xshm;
	/** the texture used to display the capture */
	gs_texture_t *texture;
	/** cursor object for displaying the server */
	xcursor_t *cursor;
	/** user setting - if cursor should be displayed  */
	bool show_cursor;
	/** set if xinerama is available and active on the screen */
	bool use_xinerama;
	/** user setting - if advanced settings should be displayed */
	bool advanced;
};

/**
 * Resize the texture
 *
 * This will automatically create the texture if it does not exist
 *
 * @note requires to be called within the obs graphics context
 */
static inline void xshm_resize_texture(struct xshm_data *data)
{
	if (data->texture)
		gs_texture_destroy(data->texture);
	data->texture = gs_texture_create(data->width, data->height,
		GS_BGRA, 1, NULL, GS_DYNAMIC);
}

/**
 * Update the capture
 *
 * @return < 0 on error, 0 when size is unchanged, > 1 on size change
 */
static int_fast32_t xshm_update_geometry(struct xshm_data *data)
{
	int_fast32_t old_width = data->width;
	int_fast32_t old_height = data->height;

	if (data->use_xinerama) {
		if (xinerama_screen_geo(data->dpy, data->screen_id,
			&data->x_org, &data->y_org,
			&data->width, &data->height) < 0) {
			return -1;
		}
		data->screen = XDefaultScreenOfDisplay(data->dpy);
	}
	else {
		data->x_org = 0;
		data->y_org = 0;
		if (x11_screen_geo(data->dpy, data->screen_id,
			&data->width, &data->height) < 0) {
			return -1;
		}
		data->screen = XScreenOfDisplay(data->dpy, data->screen_id);
	}

	if (!data->width || !data->height) {
		blog(LOG_ERROR, "Failed to get geometry");
		return -1;
	}

	blog(LOG_INFO, "Geometry %"PRIdFAST32"x%"PRIdFAST32
		" @ %"PRIdFAST32",%"PRIdFAST32,
		data->width, data->height, data->x_org, data->y_org);

	if (old_width == data->width && old_height == data->height)
		return 0;

	return 1;
}

/**
 * Returns the name of the plugin
 */
static const char* xshm_getname(void)
{
	return obs_module_text("X11SharedMemoryScreenInput");
}

/**
 * Stop the capture
 */
static void xshm_capture_stop(struct xshm_data *data)
{
	obs_enter_graphics();

	if (data->texture) {
		gs_texture_destroy(data->texture);
		data->texture = NULL;
	}
	if (data->cursor) {
		xcursor_destroy(data->cursor);
		data->cursor = NULL;
	}

	obs_leave_graphics();

	if (data->xshm) {
		xshm_detach(data->xshm);
		data->xshm = NULL;
	}

	if (data->dpy) {
		XSync(data->dpy, true);
		XCloseDisplay(data->dpy);
		data->dpy = NULL;
	}

	if (data->server) {
		bfree(data->server);
		data->server = NULL;
	}
}

/**
 * Start the capture
 */
static void xshm_capture_start(struct xshm_data *data)
{
	const char *server = (data->advanced && *data->server)
			? data->server : NULL;

	data->dpy = XOpenDisplay(server);
	if (!data->dpy) {
		blog(LOG_ERROR, "Unable to open X display !");
		goto fail;
	}

	if (!XShmQueryExtension(data->dpy)) {
		blog(LOG_ERROR, "XShm extension not found !");
		goto fail;
	}

	data->use_xinerama = xinerama_is_active(data->dpy) ? true : false;

	if (xshm_update_geometry(data) < 0) {
		blog(LOG_ERROR, "failed to update geometry !");
		goto fail;
	}

	data->xshm = xshm_attach(data->dpy, data->screen,
		data->width, data->height);
	if (!data->xshm) {
		blog(LOG_ERROR, "failed to attach shm !");
		goto fail;
	}

	obs_enter_graphics();

	data->cursor = xcursor_init(data->dpy);
	xcursor_offset(data->cursor, data->x_org, data->y_org);
	xshm_resize_texture(data);

	obs_leave_graphics();

	return;
fail:
	xshm_capture_stop(data);
}

/**
 * Update the capture with changed settings
 */
static void xshm_update(void *vptr, obs_data_t *settings)
{
	XSHM_DATA(vptr);

	xshm_capture_stop(data);

	data->screen_id   = obs_data_get_int(settings, "screen");
	data->show_cursor = obs_data_get_bool(settings, "show_cursor");
	data->advanced    = obs_data_get_bool(settings, "advanced");
	data->server      = bstrdup(obs_data_get_string(settings, "server"));

	xshm_capture_start(data);
}

/**
 * Get the default settings for the capture
 */
static void xshm_defaults(obs_data_t *defaults)
{
	obs_data_set_default_int(defaults, "screen", 0);
	obs_data_set_default_bool(defaults, "show_cursor", true);
	obs_data_set_default_bool(defaults, "advanced", false);
}

/**
 * Toggle visibility of advanced settings
 */
static bool xshm_toggle_advanced(obs_properties_t *props,
		obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const bool visible     = obs_data_get_bool(settings, "advanced");
	obs_property_t *server = obs_properties_get(props, "server");

	obs_property_set_visible(server, visible);

	/* trigger server changed callback so the screen list is refreshed */
	obs_property_modified(server, settings);

	return true;
}

/**
 * The x server was changed
 */
static bool xshm_server_changed(obs_properties_t *props,
		obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);

	bool advanced           = obs_data_get_bool(settings, "advanced");
	int_fast32_t old_screen = obs_data_get_int(settings, "screen");
	const char *server      = obs_data_get_string(settings, "server");
	obs_property_t *screens = obs_properties_get(props, "screen");

	/* we want a real NULL here in case there is no string here */
	server = (advanced && *server) ? server : NULL;

	obs_property_list_clear(screens);

	Display *dpy = XOpenDisplay(server);
	if (!dpy) {
		obs_property_set_enabled(screens, false);
		return true;
	}

	struct dstr screen_info;
	dstr_init(&screen_info);
	bool xinerama = xinerama_is_active(dpy);
	int_fast32_t count = (xinerama) ?
			xinerama_screen_count(dpy) : XScreenCount(dpy);

	for (int_fast32_t i = 0; i < count; ++i) {
		int_fast32_t x, y, w, h;
		x = y = w = h = 0;

		if (xinerama)
			xinerama_screen_geo(dpy, i, &x, &y, &w, &h);
		else
			x11_screen_geo(dpy, i, &w, &h);

		dstr_printf(&screen_info, "Screen %"PRIuFAST32" (%"PRIuFAST32
				"x%"PRIuFAST32" @ %"PRIuFAST32
				",%"PRIuFAST32")", i, w, h, x, y);

		obs_property_list_add_int(screens, screen_info.array, i);
	}

	/* handle missing screen */
	if (old_screen + 1 > count) {
		dstr_printf(&screen_info, "Screen %"PRIuFAST32" (not found)",
				old_screen);
		size_t index = obs_property_list_add_int(screens,
				screen_info.array, old_screen);
		obs_property_list_item_disable(screens, index, true);

	}

	dstr_free(&screen_info);

	XCloseDisplay(dpy);
	obs_property_set_enabled(screens, true);

	return true;
}

/**
 * Get the properties for the capture
 */
static obs_properties_t *xshm_properties(void *vptr)
{
	XSHM_DATA(vptr);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_list(props, "screen", obs_module_text("Screen"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_properties_add_bool(props, "show_cursor",
			obs_module_text("CaptureCursor"));
	obs_property_t *advanced = obs_properties_add_bool(props, "advanced",
			obs_module_text("AdvancedSettings"));
	obs_property_t *server = obs_properties_add_text(props, "server",
			obs_module_text("XServer"), OBS_TEXT_DEFAULT);

	obs_property_set_modified_callback(advanced, xshm_toggle_advanced);
	obs_property_set_modified_callback(server, xshm_server_changed);

	/* trigger server callback to get screen count ... */
	obs_data_t *settings = obs_source_get_settings(data->source);
	obs_property_modified(server, settings);
	obs_data_release(settings);

	return props;
}

/**
 * Destroy the capture
 */
static void xshm_destroy(void *vptr)
{
	XSHM_DATA(vptr);

	if (!data)
		return;

	xshm_capture_stop(data);

	bfree(data);
}

/**
 * Create the capture
 */
static void *xshm_create(obs_data_t *settings, obs_source_t *source)
{
	struct xshm_data *data = bzalloc(sizeof(struct xshm_data));
	data->source = source;

	xshm_update(data, settings);

	return data;
}

/**
 * Prepare the capture data
 */
static void xshm_video_tick(void *vptr, float seconds)
{
	UNUSED_PARAMETER(seconds);
	XSHM_DATA(vptr);

	if (!data->texture)
		return;

	obs_enter_graphics();

	XShmGetImage(data->dpy, XRootWindowOfScreen(data->screen),
		data->xshm->image, data->x_org, data->y_org, AllPlanes);
	gs_texture_set_image(data->texture, (void *) data->xshm->image->data,
		data->width * 4, false);

	xcursor_tick(data->cursor);

	obs_leave_graphics();
}

/**
 * Render the capture data
 */
static void xshm_video_render(void *vptr, gs_effect_t *effect)
{
	XSHM_DATA(vptr);

	if (!data->texture)
		return;

	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, data->texture);

	gs_enable_blending(false);
	gs_draw_sprite(data->texture, 0, 0, 0);

	if (data->show_cursor)
		xcursor_render(data->cursor);

	gs_reset_blend_state();
}

/**
 * Width of the captured data
 */
static uint32_t xshm_getwidth(void *vptr)
{
	XSHM_DATA(vptr);
	return data->width;
}

/**
 * Height of the captured data
 */
static uint32_t xshm_getheight(void *vptr)
{
	XSHM_DATA(vptr);
	return data->height;
}

struct obs_source_info xshm_input = {
	.id             = "xshm_input",
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags   = OBS_SOURCE_VIDEO,
	.get_name       = xshm_getname,
	.create         = xshm_create,
	.destroy        = xshm_destroy,
	.update         = xshm_update,
	.get_defaults   = xshm_defaults,
	.get_properties = xshm_properties,
	.video_tick     = xshm_video_tick,
	.video_render   = xshm_video_render,
	.get_width      = xshm_getwidth,
	.get_height     = xshm_getheight
};
