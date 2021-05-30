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
#include <gst/video/video.h>
#include <gst/audio/audio.h>
#include <gst/app/app.h>
#define GST_USE_UNSTABLE_API
#include <gst/gl/gl.h>
#include <gst/gl/egl/gstglmemoryegl.h>
#undef GST_USE_UNSTABLE_API

#include <epoxy/egl.h>

typedef struct {
	GstElement *pipe;
	obs_source_t *source;
	obs_data_t *settings;
	gint64 frame_count;
	gint64 audio_count;
	guint timeout_id;
	GThread *thread;
	GMainLoop *loop;
	GMutex mutex;
	GCond cond;

	GMutex draw_mutex;
	GCond draw_cond;

	gs_texture_t *texture;
	/* void* last_handle; */

	EGLDisplay eglDisplay;
	EGLContext eglContext;
	EGLSurface eglSurface;
	EGLConfig eglConfig;
	GLuint textureId;
	GLuint videoTextureId;
	unsigned program;
	unsigned textureUniform;
	GstVideoFrame videoFrame;
	gboolean videoFrameMapped;
	int width;
	int height;
	GstSample* sample;
    GLuint fbo_handle;
    GLuint depth;
} data_t;

static GstGLDisplay *gstGLDisplay = NULL;
static GstGLContext *gstGLContext = NULL;

static gboolean bus_callback(GstBus *bus, GstMessage *message,
			     gpointer user_data)
{
	data_t *data = user_data;

	switch (GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_ERROR: {
		GError *err;
		gst_message_parse_error(message, &err, NULL);
		blog(LOG_ERROR, "%s", err->message);
		g_error_free(err);
	} // fallthrough
	case GST_MESSAGE_EOS:
		gst_element_set_state(data->pipe, GST_STATE_NULL);
		break;
	case GST_MESSAGE_WARNING: {
		GError *err;
		gst_message_parse_warning(message, &err, NULL);
		blog(LOG_WARNING, "%s", err->message);
		g_error_free(err);
	} break;
	default:
		break;
	}

	return TRUE;
}

static void _gl_mem_copy(GstGLContext *context, gpointer user_data)
{
	data_t *data = user_data;
	GstBuffer *buffer = gst_sample_get_buffer(data->sample);
	GstMemory *mem = gst_buffer_peek_memory(buffer, 0);
	GstGLMemory *gl_mem = GST_GL_MEMORY_CAST(mem);
	unsigned out_tex_target =
		gst_gl_texture_target_to_gl(GST_GL_TEXTURE_TARGET_2D);

	obs_enter_graphics();
	glBindTexture(out_tex_target, data->textureId);

	gst_gl_memory_copy_teximage(gl_mem, data->textureId,
				    GST_GL_TEXTURE_TARGET_2D, GST_GL_RGBA,
				    data->width, data->height);
	glBindTexture(out_tex_target, 0);
	obs_leave_graphics();
	/* g_mutex_lock(&data->draw_mutex); */
	/* g_cond_signal(&data->draw_cond); */
	/* g_mutex_unlock(&data->draw_mutex); */
}

static GstFlowReturn video_new_sample(GstAppSink *appsink, gpointer user_data)
{
	data_t *data = user_data;
	GstSample *sample = gst_app_sink_pull_sample(appsink);
	GstBuffer *buffer = gst_sample_get_buffer(sample);
	GstCaps* caps = gst_sample_get_caps(sample);
	GstVideoInfo videoInfo;

	gst_video_info_init(&videoInfo);
	gst_video_info_from_caps(&videoInfo, caps);

	data->width = GST_VIDEO_INFO_WIDTH(&videoInfo);
	data->height = GST_VIDEO_INFO_HEIGHT(&videoInfo);

    if (data->videoFrameMapped)
        gst_video_frame_unmap(&data->videoFrame);
    if (data->sample)
        gst_sample_unref(data->sample);

    data->sample = sample;
    data->videoFrameMapped = gst_video_frame_map(&data->videoFrame, &videoInfo, buffer, GST_MAP_READ | GST_MAP_GL);
    data->videoTextureId = *(GLuint*)(data->videoFrame.data[0]);
    /* gst_printerrln("tex: %u", data->videoTextureId); */

    obs_enter_graphics();
    g_clear_pointer(&data->texture, gs_texture_destroy);
    data->texture = gs_texture_create(data->width, data->height, GS_RGBA, 1, NULL, GS_DYNAMIC);
    const GLuint gltex = *(GLuint *)gs_texture_get_obj(data->texture);
    obs_leave_graphics();

    GstMemory *mem = gst_buffer_peek_memory(buffer, 0);
    GstGLMemory *gl_mem = GST_GL_MEMORY_CAST(mem);
    GstGLContext *context = gl_mem->mem.context;
    data->textureId = gltex;

    /* g_mutex_lock(&data->draw_mutex); */
    gst_gl_context_thread_add(context, _gl_mem_copy, data);
    /* g_cond_wait(&data->draw_cond, &data->draw_mutex); */
    /* g_mutex_unlock(&data->draw_mutex); */

    /* GstGLSyncMeta *meta = gst_buffer_get_gl_sync_meta(buffer); */
    /* if (meta) { */
    /*     gst_gl_sync_meta_wait_cpu(meta, context); */
    /* } */
	return GST_FLOW_OK;
}

const char *wpe_source_get_name(void *type_data)
{
	return "WPE Browser Source";
}

static gboolean loop_startup(gpointer user_data)
{
	data_t *data = user_data;

	g_mutex_lock(&data->mutex);
	g_cond_signal(&data->cond);
	g_mutex_unlock(&data->mutex);
	return FALSE;
}

static void
sync_bus_call (GstBus * bus, GstMessage * msg, gpointer data)
{
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_NEED_CONTEXT:
    {
      const gchar *context_type;

      gst_message_parse_context_type (msg, &context_type);

      obs_enter_graphics();
      if (g_strcmp0(context_type, GST_GL_DISPLAY_CONTEXT_TYPE) == 0) {
	      GstContext *display_context =
		      gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
	      gstGLDisplay =
		      GST_GL_DISPLAY(gst_gl_display_egl_new_with_egl_display(
			      (gpointer)eglGetCurrentDisplay()));
	      gst_context_set_gl_display(display_context, gstGLDisplay);
	      gst_element_set_context(GST_ELEMENT(msg->src), display_context);
	      gst_context_unref(display_context);
      } else if (g_strcmp0(context_type, "gst.gl.app_context") == 0) {
	      GstContext *app_context =
		      gst_context_new("gst.gl.app_context", TRUE);
	      gstGLContext = gst_gl_context_new_wrapped(
		      gstGLDisplay, (guintptr)eglGetCurrentContext(),
		      GST_GL_PLATFORM_EGL, GST_GL_API_OPENGL);
	      gst_gl_context_activate(gstGLContext, TRUE);
	      gst_gl_context_fill_info(gstGLContext, NULL);
          GstStructure *s =
		      gst_context_writable_structure(app_context);
	      gst_structure_set(s, "context", GST_TYPE_GL_CONTEXT, gstGLContext, NULL);
	      gst_element_set_context(GST_ELEMENT(msg->src), app_context);
	      gst_context_unref(app_context);
      }
      obs_leave_graphics();
      break;
    }
    default:
      break;
  }
}

static void create_pipeline(data_t *data)
{
    data->texture = NULL;
    data->textureId = 0;
    data->program = 0;
    data->textureUniform = 0;
    data->videoFrameMapped = FALSE;

	GError *err = NULL;
	const gchar *pipeline =
		"wpevideosrc location=https://webkit.org/blog-files/3d-transforms/poster-circle.html ! tee name=t t. ! queue ! appsink name=video-sink";
	data->pipe = gst_parse_launch(pipeline, &err);

	if (err != NULL) {
		blog(LOG_ERROR, "Cannot start GStreamer pipeline: %s", err->message);
		g_error_free(err);
		obs_source_output_video(data->source, NULL);
		return;
	}

	GstAppSinkCallbacks video_cbs = {NULL, NULL, video_new_sample};
	GstElement *appsink =
		gst_bin_get_by_name(GST_BIN(data->pipe), "video-sink");
	gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &video_cbs, data, NULL);
	gst_object_unref(appsink);

	data->frame_count = 0;
	data->audio_count = 0;

	GstBus *bus = gst_element_get_bus(data->pipe);
	gst_bus_add_watch(bus, bus_callback, data);

	gst_bus_enable_sync_message_emission(bus);
	g_signal_connect(bus, "sync-message", G_CALLBACK(sync_bus_call), NULL);
	gst_object_unref(bus);

	gst_element_set_state(data->pipe, GST_STATE_PLAYING);
}

static gpointer _start(gpointer user_data)
{
	data_t *data = user_data;

	GMainContext *context = g_main_context_new();

	g_main_context_push_thread_default(context);

	create_pipeline(data);

	data->loop = g_main_loop_new(context, FALSE);

	GSource *source = g_idle_source_new();
	g_source_set_callback(source, loop_startup, data, NULL);
	g_source_attach(source, context);

	g_main_loop_run(data->loop);

	if (data->pipe != NULL) {
		gst_element_set_state(data->pipe, GST_STATE_NULL);

		GstBus *bus = gst_element_get_bus(data->pipe);
		gst_bus_remove_watch(bus);
		gst_object_unref(bus);

		gst_object_unref(data->pipe);
		data->pipe = NULL;
	}

	if (data->timeout_id != 0) {
		g_source_remove(data->timeout_id);
		data->timeout_id = 0;
	}

	g_main_loop_unref(data->loop);
	data->loop = NULL;

	g_main_context_unref(context);

	return NULL;
}

static void start(data_t *data)
{
	g_mutex_lock(&data->mutex);

	data->thread = g_thread_new("GStreamer Source", _start, data);

	g_cond_wait(&data->cond, &data->mutex);
	g_mutex_unlock(&data->mutex);
}

void *wpe_source_create(obs_data_t *settings, obs_source_t *source)
{
	data_t *data = g_new0(data_t, 1);

	data->source = source;
	data->settings = settings;

	g_mutex_init(&data->mutex);
	g_cond_init(&data->cond);

	g_mutex_init(&data->draw_mutex);
	g_cond_init(&data->draw_cond);

	if (obs_data_get_bool(settings, "stop_on_hide") == false)
		start(data);

	return data;
}

static void stop(data_t *data)
{
	if (data->thread == NULL)
		return;

	g_main_loop_quit(data->loop);

	g_thread_join(data->thread);
	data->thread = NULL;

	obs_source_output_video(data->source, NULL);
}

void wpe_source_destroy(void *user_data)
{
	data_t *data = user_data;

	stop(data);

	g_mutex_clear(&data->mutex);
	g_cond_clear(&data->cond);

	g_free(data);
}

void wpe_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "stop_on_hide", true);
}

void wpe_source_update(void *data, obs_data_t *settings);

static bool on_apply_clicked(obs_properties_t *props, obs_property_t *property,
			     void *data)
{
	wpe_source_update(data, ((data_t *)data)->settings);
	return false;
}

obs_properties_t *wpe_source_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_properties_add_bool(props, "stop_on_hide",
				"Stop pipeline when hidden");
	obs_properties_add_button2(props, "apply", "Apply", on_apply_clicked,
				   data);
	return props;
}

void wpe_source_update(void *data, obs_data_t *settings)
{
	stop(data);

	// Don't start the pipeline if source is hidden and 'stop_on_hide' is set.
	// From GUI this is probably irrelevant but works around some quirks when
	// controlled from script.
	if (obs_data_get_bool(settings, "stop_on_hide") &&
	    !obs_source_showing(((data_t *)data)->source))
		return;

	start(data);
}

void wpe_source_show(void *data)
{
	if (((data_t *)data)->pipe == NULL)
		start(data);
}

void wpe_source_hide(void *data)
{
	if (obs_data_get_bool(((data_t *)data)->settings, "stop_on_hide"))
		stop(data);
}

uint32_t wpe_source_get_width(void *user_data)
{
	data_t* data = (data_t*) user_data;
	return data->width;
}

uint32_t wpe_source_get_height(void *user_data)
{
	data_t* data = (data_t*) user_data;
	return data->height;
}

void wpe_source_render(void *user_data, gs_effect_t* effect)
{
	data_t* data = (data_t*) user_data;
    if (!data->texture)
        return;

    gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, data->texture);
}
