#include "obs-data.h"
#include "obs-properties.h"
#include "util/c99defs.h"
#include "util/serializer.h"
#include <stdint.h> // uint8_t, uint32_t, uint64_t,
#include <util/dstr.hpp>
#include <obs-module.h>
#include <queue>    // std::queue
#include <string.h> // memcpy
#include <cmath>

struct channel_copier {
	int selection;
};

static const char *
ccopier_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Channel Copier");
}

static void *ccopier_filter_create(obs_data_t *settings, obs_source_t *ctx)
{
	UNUSED_PARAMETER(settings);
	UNUSED_PARAMETER(ctx);

	channel_copier *ccopier = new channel_copier;

	return ccopier;
}

static void ccopier_filter_destroy(void *data)
{
	auto ccopier = reinterpret_cast<channel_copier *>(data);
	delete ccopier;
	return;
}

static void ccopier_filter_update(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	auto ccopier = reinterpret_cast<channel_copier *>(data);

	ccopier->selection = obs_data_get_int(settings, "_selection");
	return;
}

static void ccopier_filter_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(seconds);
	return;
}

static void ccopier_filter_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "_selection", 0);
	return;
}

static obs_properties_t *ccopier_filter_properites(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_int(props, "_selection", "Channel Selection", 0, 3,
			       1);

	return props;
}

// remember that this is a silent source otherwise so we don't need to do much
// to audio itself.
static struct obs_audio_data *ccopier_filter_audio(void *data,
						   struct obs_audio_data *audio)
{
	auto ccopier = reinterpret_cast<channel_copier *>(data);
	auto selection = ccopier->selection * 2;
	if (selection == 0) {
		return audio;
	}

	/* Copy the data over and clear the old buffer */
	for (size_t ix = 0; ix < audio->frames; ix += 1) {
		audio->data[selection][ix] = audio->data[0][ix];
		audio->data[0][ix] = 0;
		audio->data[selection + 1][ix] = audio->data[1][ix];
		audio->data[1][ix] = 0;
	}

	return audio;
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("channel-copier", "en-US")
MODULE_EXPORT const char *obs_module_description(void) {
    return "Mux up to channel count / 2 sources into a single output.";
}

struct obs_source_info copier_source = {
    .id = "copier_filter",
    .version = 2,
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_AUDIO,
    .get_name = ccopier_filter_get_name,
    .create = ccopier_filter_create,
    .destroy = ccopier_filter_destroy,
    .update = ccopier_filter_update,
    .video_tick = ccopier_filter_tick,
    .get_defaults = ccopier_filter_defaults,
    .filter_audio = ccopier_filter_audio,
    .get_properties = ccopier_filter_properites,
};

bool obs_module_load(void) {
    obs_register_source(&copier_source);
	return true;
}
