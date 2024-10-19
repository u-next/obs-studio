#include "obs-properties.h"
#include "obs.h"
#include "util/bmem.h"
#include "util/c99defs.h"
#include <stdint.h> // uint8_t, uint32_t, uint64_t,
#include <obs-module.h>
#include <string.h> // memcpy
#include <util/deque.h>
#include <pthread.h>

/* single byte power of 2 outside of the range of valid channels */
static const size_t INVALID_CHANNEL_SOURCE = 128;

static const size_t NUM_CHANNELS = 2;

/*
  We need to associate a source and a channel mapping to this source.
 */
struct channel_copier {
	obs_source_t *self;
	size_t mapped_channel; /* we will map n, n+1 channels to the output of this source. */
	obs_weak_source_t *source;

	/* save from the source to overwrite onto self. */
	struct deque source_data[2];
	float *data_buf;

	pthread_mutex_t mutex;
};

static const char *ccopier_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Channel Copier");
}

// capture data from the target source so that we can overwrite the filter target.
static void capture(void *param, obs_source_t *source,
		    const struct audio_data *audio_data, bool muted)
{
	UNUSED_PARAMETER(muted);
	UNUSED_PARAMETER(source);
	struct channel_copier *ccopier = param;

	pthread_mutex_lock(&ccopier->mutex);

	size_t expected_size = audio_data->frames * sizeof(float);

	/* free up space for more current data */
	if (ccopier->source_data[0].size > expected_size * 2) {
		for (size_t i = 0; i < NUM_CHANNELS; i++) {
			deque_pop_front(&ccopier->source_data[i], NULL,
					expected_size);
		}
	}

	// note that we're explicitly ignoring the possibility of the source
	// being muted. This filter is used specifically to create a pseudo-source
	// that copies from other sources to allow MIDI interfaces etc. to control individual
	// channels of a source. If you want to mute, mute this.
	for (size_t ix = 0; ix < NUM_CHANNELS; ix += 1) {
		deque_push_back(&ccopier->source_data[ix], audio_data->data[ix + ccopier->mapped_channel],
				audio_data->frames * sizeof(float));
	}

	pthread_mutex_unlock(&ccopier->mutex);
}

// This filter completely discards whatever the input data was and instead overwrites it
// with the contents of the callback result.
static struct obs_audio_data *ccopier_filter_audio(void *data,
						   struct obs_audio_data *audio)
{
	struct channel_copier *ccopier = data;

	pthread_mutex_lock(&ccopier->mutex);

	size_t populate_zero_count = 0;

	/* clear out whatever noise may be in the carrier channel. */
	for (size_t ix = 0; ix < NUM_CHANNELS; ix += 1) {
		memset(audio->data[ix], 0x00, audio->frames * sizeof(float));
	}

	if (audio->frames * sizeof(float) > ccopier->source_data[0].size) {
		populate_zero_count =
		    audio->frames * sizeof(float) - ccopier->source_data[0].size;
	}

	// copy over the source data to the target in order.
	// this will overwrite whatever is in the input buffer.
	for (size_t ix = 0; ix < NUM_CHANNELS; ix += 1) {

		// if there's not enough data in the deque, populate the queue.
		deque_push_back_zero(&ccopier->source_data[ix],
				     populate_zero_count);

		// otherwise, grab all of the data in the deque.
		deque_pop_front(&ccopier->source_data[ix], audio->data[ix],
				audio->frames * sizeof(float));
	}

	pthread_mutex_unlock(&ccopier->mutex);

	return audio;
}

static void ccopier_filter_update(void *data, obs_data_t *settings)
{
	struct channel_copier *ccopier = data;
    
    if (ccopier->source) {
        obs_source_t *old_source = obs_weak_source_get_source(ccopier->source);
        if (old_source) {
            obs_source_remove_audio_capture_callback(old_source, capture, ccopier);
        }
    }
    
	const char *sidechain_name =
		obs_data_get_string(settings, "ccopier_source");

	bool valid_sidechain = *sidechain_name &&
			       strcmp(sidechain_name, "none") != 0;
	if (!valid_sidechain) {
		return;
	}
    
    /* get the matched channel */
    ccopier->mapped_channel = obs_data_get_int(settings, "ccopier_chan") * 2;

	pthread_mutex_lock(&ccopier->mutex);

	obs_source_t *source = obs_get_source_by_name(sidechain_name);
	obs_weak_source_t *weak_ref =
		source ? obs_source_get_weak_source(source) : NULL;

	ccopier->source = weak_ref;

	if (source) {
		obs_source_add_audio_capture_callback(source, capture, ccopier);

		//obs_weak_source_release(weak_ref);
		obs_source_release(source);
	}

	pthread_mutex_unlock(&ccopier->mutex);

	return;
}

static void ccopier_filter_destroy(void *data)
{
	UNUSED_PARAMETER(data);
	// TODO(free and clean up)
	return;
}

static void *ccopier_filter_create(obs_data_t *settings, obs_source_t *ctx)
{
	UNUSED_PARAMETER(settings);
	UNUSED_PARAMETER(ctx);

	struct channel_copier *ccopier = bzalloc(sizeof(struct channel_copier));
	ccopier->mapped_channel = INVALID_CHANNEL_SOURCE;
	ccopier->self = ctx;
	ccopier->source = NULL;

	if (pthread_mutex_init(&ccopier->mutex, NULL) != 0) {
		bfree(ccopier);
		return NULL;
	}
    
    /* We want to register callbacks immediately if possible */
    ccopier_filter_update(ccopier, settings);

	return ccopier;
}

static void ccopier_filter_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(seconds);
	return;
}

static void ccopier_filter_defaults(obs_data_t *s)
{
	UNUSED_PARAMETER(s);
	return;
}

struct ccopier_cb_info {
	obs_property_t *list;
	obs_source_t *self;
};

static bool add_sources(void *data, obs_source_t *source)
{
	struct ccopier_cb_info *info = data;
	uint32_t caps = obs_source_get_output_flags(source);

	if ((caps & OBS_SOURCE_AUDIO) == 0)
		return true;

	const char *name = obs_source_get_name(source);
	obs_property_list_add_string(info->list, name, name);
	return true;
}

static obs_properties_t *ccopier_filter_properites(void *data)
{
	//struct channel_copier *ccopier = data;
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
    
    obs_properties_add_int(props, "ccopier_chan", "Track", 0, 3, 1);

	obs_property_t *sources = obs_properties_add_list(
		props, "ccopier_source", "Compressor.SidechainSource",
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(sources, obs_module_text("None"), "none");

	struct ccopier_cb_info info = {sources, NULL};
	obs_enum_sources(add_sources, &info);

	return props;
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("channel-copier", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
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
