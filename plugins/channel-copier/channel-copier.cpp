#include <stdint.h> // uint8_t, uint32_t, uint64_t,
#include <util/dstr.hpp>
#include <obs-module.h>
#include <queue> // std::queue
#include <string.h> // memcpy
#include <cmath>

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
 
 Note that when using this filter, you either need to add the source you created
 this filter on to the source list or you need to create an "output" source and use
 that as the filter destination. The latter is the expected use-case.
 
 The source this filter is applied to acts as the "carrier" for the other
 N sources added. As such, its timing is used as the base for syncing the
 samples taken from the other sources.
 */

struct Chunk {
    uint64_t timestamp;
    uint8_t **data;
    size_t size;
};

// The general purpose of this structure is to map between a timestamp
// and some data associated with that point in time. Ideally this timestamp
// exists before the playback modification that happens within media and instead
// corresponds more closely with the input audio PTS. This is yet to be seen.
// In order to map between these two values we need to somehow "normalize" them
// such that we get a version that actually corresponds, regardless of processing
// time or minute differences on the ns level.
//
// As such, timestamps are normalized to the nearest factor defined as C * 1/sample_rate
// where C is a tuneable number that i've set to 10 for the time being. If C is too high
// we will end up clobbering data as there will be samples lost. If C is too low, we will
// end up with gaps in our data where we fail to correlate.
struct ChannelCopier {
    // TODO(Ben): We want to have a list of weak sources so that
    //            the references can go stale without issue.
    
    // This is a mapping from a normalized timestamp (see definition)
    std::queue<Chunk> audio_data;
};

static const char *ccopier_filter_get_name(void *unused) {
    UNUSED_PARAMETER(unused);
    return obs_module_text("Channel Copier");
}


static void capture(void *param, obs_source_t *source, const struct audio_data *audio_data, bool muted) {
    UNUSED_PARAMETER(source);
    UNUSED_PARAMETER(muted); // this shouldnt be ignored in reality.
    
    ChannelCopier *ccopier = reinterpret_cast<ChannelCopier *>(param);
    

    //const uint32_t sample_rate = audio_output_get_sample_rate(obs_get_audio());
    //const uint64_t time_sample_rate = 10 * (1 / sample_rate); // normalizing factor.
    const auto size = audio_data->frames;
    const auto timestamp = audio_data->timestamp;
    
    if (size == 0) { return; }
    
    printf("Encountered %s with %d byte encountered.\n", obs_source_get_name(source), (int)audio_data->data[0][0]);
    // there's definitely a bitshift i can use in this place. this is so wasteful to be done as a div.
    // the exact value of 1ms is largely unimportant.
    const auto normalized_timestamp = timestamp / 10'000'000ULL;
    uint8_t **data = new uint8_t*[2];
    for (auto ix = 0; ix < 2; ix += 1) { data[ix] = new uint8_t[size]; }
    
    memcpy(data[0], audio_data->data[0], size);
    memcpy(data[1], audio_data->data[1], size);
    ccopier->audio_data.push(Chunk {
        normalized_timestamp,
        data,
        size,
    });
}


static void *ccopier_filter_create(obs_data_t *settings, obs_source_t *ctx) {
    UNUSED_PARAMETER(settings);
    UNUSED_PARAMETER(ctx);
    
    // TODO(Ben): Note that this process is only for testing.
    //obs_source_t *source_a1 = obs_get_source_by_name("V1+A1");
    obs_source_t *source_a2 = obs_get_source_by_name("A2");
    //obs_source_t *source_a3 = obs_get_source_by_name("A3");
    
    ChannelCopier *ccopier = new ChannelCopier;
    
    //obs_source_add_audio_capture_callback(source_a1, capture, ccopier);
    obs_source_add_audio_capture_callback(source_a2, capture, ccopier);
    //obs_source_add_audio_capture_callback(source_a3, capture, ccopier);
    
    return ccopier;
}

static void ccopier_filter_destroy(void *data) {
    // TODO(Ben): Note this is only for testing.
    obs_source_t *source = obs_get_source_by_name("A2");
    obs_source_remove_audio_capture_callback(source, capture, data);
    
    ChannelCopier *ccopier = reinterpret_cast<ChannelCopier *>(data);
    delete ccopier;
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

// remember that this is a silent source otherwise so we don't need to do much
// to audio itself.
static struct obs_audio_data *ccopier_filter_audio(void *data, struct obs_audio_data *audio) {
    auto ccopier = reinterpret_cast<ChannelCopier *>(data);
    //const auto normalized_timestamp = audio->timestamp / 10'000'000ULL;
    
    //const auto epsilon = 5;

    // we want to pop elements until we either end up in the future or end up within
    // epsilon on a popped element
    while (!ccopier->audio_data.empty()) {
        auto elem = ccopier->audio_data.front();
        ccopier->audio_data.pop();
        
        
        // TODO(Ben): need past check
//        if (std::abs((int)elem.timestamp - (int)normalized_timestamp) < epsilon) { // margin of error
            // it seems as thiough audio->frames is invariantly <= elem.size, so probably
            // not a problem in a testing environment, but in reality there needs to be clamping
            // based on which is larger, and arguably resizing of the buffer based on the samples
            // from other sources.
            memcpy(audio->data[0], elem.data[0], audio->frames);
            memcpy(audio->data[1], elem.data[1], audio->frames);
            break;
//        }
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
