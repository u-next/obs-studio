#include <util/dstr.hpp>
#include <obs-module.h>

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

static const char *ccopier_filter_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text("Channel Copier");
}

static void *ccopier_filter_create(obs_data_t *settings, obs_source_t *ctx) {
    UNUSED_PARAMETER(settings);
    UNUSED_PARAMETER(ctx);
    return nullptr;
}

static void ccopier_filter_destroy(void *data) {
    UNUSED_PARAMETER(data);
    return;
}

static void ccopier_filter_update(void *data, obs_data_t *settings) {
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(settings);
    return;
}

static void ccopier_filter_tick(void *data, float seconds) {
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(seconds);
    return;
}

static void ccopier_filter_defaults(obs_data_t *s) {
    UNUSED_PARAMETER(s);
    return;
}

static obs_properties_t *ccopier_filter_properites(void *data) {
    UNUSED_PARAMETER(data);
    obs_properties_t *props = obs_properties_create();

    return props;
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
