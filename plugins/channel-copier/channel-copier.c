#include "obs-properties.h"
#include "obs.h"
#include "util/bmem.h"
#include "util/c99defs.h"
#include <stdint.h> // uint8_t, uint32_t, uint64_t,
#include <obs-module.h>
#include <stdio.h>
#include <string.h> // memcpy
#include <util/deque.h>
#include <pthread.h>

static const ssize_t INVALID_MAPPING = -1;

/*
  We need to associate a source and a channel mapping to this source.
 */
struct channel_copier {
	/* Each index is a mapping from ix to dest_ix */
	ssize_t dest_channels[MAX_AUDIO_CHANNELS];

	obs_weak_source_t *source;

	/* the name of the source we pull data from*/
	char *source_name;

	/* save from the source to overwrite onto self. */
	struct deque source_data[MAX_AUDIO_CHANNELS];

	/* store the sample rate of obs output */
	uint32_t sample_rate;

	/* Do we want to replace the current values or add to them? (mixing) */
	bool mix_mode;

	pthread_mutex_t mutex;
};

static const char *ccopier_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Channel Copier");
}

// capture data from the target source so that we can overwrite the filter target.
static void capture(void *param, obs_source_t *source, const struct audio_data *audio_data, bool muted)
{
	UNUSED_PARAMETER(muted);
	UNUSED_PARAMETER(source);
	struct channel_copier *ccopier = param;

	pthread_mutex_lock(&ccopier->mutex);

	// to avoid syncing issue, we only allow max 80ms of buffer
	// but if that is shorter than 2x of the captures chunk size, use the latter
	size_t captured_chunk_size = audio_data->frames * sizeof(float);
	size_t max_buffer_size = ccopier->sample_rate * 80 / 1000 * sizeof(float);
	if (max_buffer_size < captured_chunk_size * 2) {
		max_buffer_size = captured_chunk_size * 2;
	}

	/* free up space for more current data */
	if (ccopier->source_data[0].size + captured_chunk_size > max_buffer_size) {
		size_t num_of_bytes_to_be_discarded =
			ccopier->source_data[0].size + captured_chunk_size - max_buffer_size;
		for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++) {
			deque_pop_front(&ccopier->source_data[i], NULL, num_of_bytes_to_be_discarded);
		}
		blog(LOG_WARNING,
		     "channel-copier: overflow, "
		     "%lu samples discarded",
		     num_of_bytes_to_be_discarded / sizeof(float));
	}

	// note that we're explicitly ignoring the possibility of the source
	// being muted. This filter is used specifically to create a pseudo-source
	// that copies from other sources to allow MIDI interfaces etc. to control individual
	// channels of a source. If you want to mute, mute this.
	for (size_t ix = 0; ix < MAX_AUDIO_CHANNELS; ix += 1) {
		deque_push_back(&ccopier->source_data[ix], audio_data->data[ix], audio_data->frames * sizeof(float));
	}

	pthread_mutex_unlock(&ccopier->mutex);
}

/* as opposed to overwriting, this will pop from the queue and mix the
   samples with whatever is already in the audio channel. */
static void mix_samples_peek(struct deque *dq, float *dest, size_t size)
{
	assert(size <= dq->size);
	assert(dest);

	size_t start_size = (dq->capacity - dq->start_pos) / sizeof(float);
	if (start_size < size) {
		float *read_head = dq->data + dq->start_pos;
		size_t ix = 0;
		for (; ix < start_size; ix += 1) {
			dest[ix] += read_head[ix];
		}

		/* loop around for the read */
		read_head = dq->data;
		for (; ix < size - start_size; ix += 1) {
			dest[ix] += read_head[ix];
		}
	} else {
		//memcpy(data, (uint8_t *)dq->data + dq->start_pos, size);
		float *read_head = dq->data + dq->start_pos;
		for (size_t ix = 0; ix < size; ix += 1) {
			dest[ix] += read_head[ix];
		}
	}
}

// This filter completely discards whatever the input data was and instead overwrites it
// with the contents of the callback result.
static struct obs_audio_data *ccopier_filter_audio(void *data, struct obs_audio_data *audio)
{
	struct channel_copier *ccopier = data;

	pthread_mutex_lock(&ccopier->mutex);

	size_t populate_zero_count = 0;

	if (audio->frames * sizeof(float) > ccopier->source_data[0].size) {
		populate_zero_count = audio->frames * sizeof(float) - ccopier->source_data[0].size;
		blog(LOG_WARNING,
		     "channel-copier: underflow, "
		     "%lu samples filled with zero",
		     populate_zero_count / sizeof(float));
	}

	// copy over the source data to the target in order.
	// this will overwrite whatever is in the input buffer.
	for (size_t ix = 0; ix < MAX_AUDIO_CHANNELS; ix += 1) {
		/* if there's not enough data in the deque, populate the queue. */
		if (populate_zero_count > 0) {
			deque_push_back_zero(&ccopier->source_data[ix], populate_zero_count);
		}

		/* We only want to write to mapped channels. */
		ssize_t mapping = ccopier->dest_channels[ix];
		if (mapping == INVALID_MAPPING) {
			continue;
		}

		/* In the event that there is overlap in channels (ex: duplicating)
                   we want to be careful to make sure each source is getting the same data. */
		if (ccopier->mix_mode) {
			mix_samples_peek(&ccopier->source_data[ix], (float *)audio->data[mapping], audio->frames);
		} else {
			deque_peek_front(&ccopier->source_data[ix], audio->data[mapping],
					 audio->frames * sizeof(float));
		}
	}

	/* NOW, we drop the data we pulled out.
           We dump all channels so that non-active deques dont get filled */
	for (size_t ix = 0; ix < MAX_AUDIO_CHANNELS; ix += 1) {
		deque_pop_front(&ccopier->source_data[ix], NULL, audio->frames * sizeof(float));
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

		obs_weak_source_release(ccopier->source);
		ccopier->source = NULL;
	}

	const char *source_name = obs_data_get_string(settings, "ccopier_source");

	bool valid_source = *source_name && strcmp(source_name, "none") != 0;
	if (!valid_source) {
		return;
	}

	/* get the matched channel */
	pthread_mutex_lock(&ccopier->mutex);
	for (int ix = 0; ix < MAX_AUDIO_CHANNELS; ix += 1) {
		char property_name[16];
		snprintf(property_name, 16, "ccopier_chan_%d", ix);
		ssize_t mapping = obs_data_get_int(settings, property_name);
		ccopier->dest_channels[ix] = mapping;
	}

	ccopier->mix_mode = obs_data_get_bool(settings, "mix_mode");
	pthread_mutex_unlock(&ccopier->mutex);

	ccopier->source_name = bstrdup(source_name);

	return;
}

static void ccopier_filter_destroy(void *data)
{
	struct channel_copier *ccopier = data;

	pthread_mutex_destroy(&ccopier->mutex);

	if (ccopier->source) {
		obs_source_t *old_source = obs_weak_source_get_source(ccopier->source);
		if (old_source) {
			obs_source_remove_audio_capture_callback(old_source, capture, ccopier);
		}

		obs_weak_source_release(ccopier->source);
		ccopier->source = NULL;
	}

	if (ccopier->source_name) {
		bfree(ccopier->source_name);
		ccopier->source_name = NULL;
	}

	bfree(ccopier);
}

static void *ccopier_filter_create(obs_data_t *settings, obs_source_t *ctx)
{
	UNUSED_PARAMETER(settings);
	UNUSED_PARAMETER(ctx);

	struct channel_copier *ccopier = bzalloc(sizeof(struct channel_copier));
	ccopier->source = NULL;
	ccopier->sample_rate = audio_output_get_sample_rate(obs_get_audio());
	ccopier->source_name = NULL;

	ccopier->mix_mode = false;

	for (size_t ix = 0; ix < MAX_AUDIO_CHANNELS; ix += 1) {
		ccopier->dest_channels[ix] = INVALID_MAPPING;
	}

	if (pthread_mutex_init(&ccopier->mutex, NULL) != 0) {
		bfree(ccopier);
		return NULL;
	}

	/* We want to register callbacks immediately if possible */
	ccopier_filter_update(ccopier, settings);

	return ccopier;
}

// `update' is called before some other sources may have been loaded.
// the net of this is that we cannot register the src source in `update'
// and instead must defer until `tick' is called.
// This is only an issue when OBS starts with the ccopier already setup.
static void ccopier_filter_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct channel_copier *ccopier = data;

	pthread_mutex_lock(&ccopier->mutex);

	// we only want to perform any logic in the event that there was a change
	// to the filter fielded by `update'.
	if (ccopier->source_name == NULL || ccopier->source != NULL) {
		pthread_mutex_unlock(&ccopier->mutex);
		return;
	}

	obs_source_t *source = obs_get_source_by_name(ccopier->source_name);
	obs_weak_source_t *weak_ref = source ? obs_source_get_weak_source(source) : NULL;

	ccopier->source = weak_ref;

	if (source) {
		obs_source_add_audio_capture_callback(source, capture, ccopier);
		obs_source_release(source);
	}

	pthread_mutex_unlock(&ccopier->mutex);

	return;
}

static void ccopier_filter_defaults(obs_data_t *s)
{
	for (int ix = 0; ix < MAX_AUDIO_CHANNELS; ix += 1) {
		char property_name[16];
		char printed_name[16];
		snprintf(property_name, 16, "ccopier_chan_%d", ix);
		snprintf(printed_name, 16, "Track %d", ix);

		obs_data_set_default_int(s, property_name, ix);
	}

	obs_data_set_default_bool(s, "mix_mode", false);

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

	obs_property_t *sources = obs_properties_add_list(props, "ccopier_source", "Copy Source", OBS_COMBO_TYPE_LIST,
							  OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(sources, obs_module_text("None"), "none");

	obs_properties_add_bool(props, "mix_mode", "Mix input(s)");

	struct ccopier_cb_info info = {sources, NULL};
	obs_enum_sources(add_sources, &info);

	for (int ix = 0; ix < MAX_AUDIO_CHANNELS; ix += 1) {
		char property_name[16];
		char printed_name[16];
		snprintf(property_name, 16, "ccopier_chan_%d", ix);
		snprintf(printed_name, 16, "Track %d", ix);

		obs_properties_add_int(props, property_name, printed_name, -1, 7, 1);
	}

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
