//#include <util/dstr.hpp>
#include <obs-module.h>
#include <stdio.h>

/*
 In order to understand the purpose of this source, consider the purpose it
 was originally designed to fulfill:
 
 - OBS does not support multiple audio tracks in output for a single source.
 - OBS supports 7.1 surround sound (effectively 4 x L/R channel tracks)
 - Multiple video sources will inevitably drift when using FFMPEG sources
    - this is due to inaccurate timing in the media playback system
        - There is up to 70ms of slop and no resetting until 200ms of desync of PTS.
            - and this slop progressively degenerates
                - the 70ms is desync from _predicted_ timestamp, not from the input timestamp.
        - Additionally, there is no time stretching
        - and no syncing between PTSs of different streams.

 As such, in order to get, applying filters to, and then resync multiple audio
 /channels/ in a single OBS instance without external retiming on the output, we
 need to process the syncing as we have doneso in this filter.
 */

// This is quite a simple copier wrapper just to contain
// the values set within the UI.
struct ccopier_filter_t {
	float gain_1;
	float gain_2;
	float gain_3;
	float gain_4;
};


static const char *ccopier_filter_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text("Channel Mixer");
}

static void *ccopier_filter_create(obs_data_t *settings, obs_source_t *ctx) {
    UNUSED_PARAMETER(settings);
    UNUSED_PARAMETER(ctx);

    struct ccopier_filter_t *ccopier = bzalloc(sizeof(struct ccopier_filter_t));
    ccopier->gain_1 = 1.0f;
    ccopier->gain_2 = 1.0f;
    ccopier->gain_3 = 1.0f;
    ccopier->gain_4 = 1.0f;

        printf("%f %f %f %f\n", ccopier->gain_1, ccopier->gain_2, ccopier->gain_3,
	   ccopier->gain_4);

    return ccopier;
}

static void ccopier_filter_destroy(void *data) {
	bfree(data);
    return;
}

static void ccopier_filter_update(void *data, obs_data_t *settings) {
	struct ccopier_filter_t *ccopier = (struct ccopier_filter_t *)data;

	    printf("%f %f %f %f\n", ccopier->gain_1, ccopier->gain_2,
	       ccopier->gain_3, ccopier->gain_4);

	ccopier->gain_1 = obs_db_to_mul((float)obs_data_get_double(settings, "db_ch1"));
	ccopier->gain_2 = obs_db_to_mul((float)obs_data_get_double(settings, "db_ch2"));
	ccopier->gain_3 =
		obs_db_to_mul((float)obs_data_get_double(settings, "db_ch3"));
	ccopier->gain_4 = obs_db_to_mul((float)obs_data_get_double(settings, "db_ch4"));
	return;
}

static void ccopier_filter_tick(void *data, float seconds) {
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(seconds);
    return;
}

static void ccopier_filter_defaults(obs_data_t *s) {
	obs_data_set_default_double(s, "db_ch1", 0.0f);
	obs_data_set_default_double(s, "db_ch2", 0.0f);
	obs_data_set_default_double(s, "db_ch3", 0.0f);
	obs_data_set_default_double(s, "db_ch4", 0.0f);
    return;
}

static obs_properties_t *ccopier_filter_properites(void *data) {
    UNUSED_PARAMETER(data);
    obs_properties_t *props = obs_properties_create();

    obs_property_t *p_1 = obs_properties_add_float_slider(props, "db_ch1", "ch1/2", -30.0, 30.0, 0.1);
    obs_property_float_set_suffix(p_1, " db");

        obs_property_t *p_2 = obs_properties_add_float_slider(
	    props, "db_ch2", "ch3/4", -30.0, 30.0, 0.1);
    obs_property_float_set_suffix(p_2, " db");

        obs_property_t *p_3 = obs_properties_add_float_slider(
	    props, "db_ch3", "ch4/5", -30.0, 30.0, 0.1);
    obs_property_float_set_suffix(p_3, " db");

        obs_property_t *p_4 = obs_properties_add_float_slider(
	    props, "db_ch4", "ch6/7", -30.0, 30.0, 0.1);
    obs_property_float_set_suffix(p_4, " db");
    
    return props;
}

// apply gain to a given channel
static inline void apply_gain(float *data, size_t frames, float gain_amount) {
    for (size_t ix = 0; ix < frames; ix += 1) {
        data[ix] *= gain_amount;
    }
}

// apply a simple hardcoded gain to a given 7.1 surround audio channel
static struct obs_audio_data *ccopier_filter_audio(void *data, struct obs_audio_data *audio) {
    struct ccopier_filter_t *ccopier = (struct ccopier_filter_t *)data;
    float **samples = (float **)(audio->data); // 8 planes
    const size_t frames = audio->frames;

    printf("%f %f %f %f\n", ccopier->gain_1, ccopier->gain_2, ccopier->gain_3,
	   ccopier->gain_4);
    // track 1
    apply_gain(samples[0], frames, ccopier->gain_1);
    apply_gain(samples[1], frames, ccopier->gain_1);
    
    // track 2
    apply_gain(samples[2], frames, ccopier->gain_2);
    apply_gain(samples[3], frames, ccopier->gain_2);

    // track 3
    apply_gain(samples[4], frames, ccopier->gain_3);
    apply_gain(samples[5], frames, ccopier->gain_3);
    
    // track 4
    apply_gain(samples[6], frames, ccopier->gain_4);
    apply_gain(samples[7], frames, ccopier->gain_4);
    
    return audio;
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
    .filter_audio = ccopier_filter_audio,
    .get_defaults = ccopier_filter_defaults,
    .get_properties = ccopier_filter_properites,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("channel-copier", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
    return "Mux up to channel count / 2 sources into a single output.";
}

bool obs_module_load(void)
{
    obs_register_source(&copier_source);
	return true;
}
