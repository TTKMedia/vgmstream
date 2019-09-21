/**
 * vgmstream for foobar2000
 */

#ifdef _MSC_VER
#define _CRT_SECURE_NO_DEPRECATE
#endif
#include <stdio.h>
#include <io.h>

#include <foobar2000.h>
#include <ATLHelpers/ATLHelpersLean.h>
#include <shared.h>

extern "C" {
#include "../src/vgmstream.h"
#include "../src/plugins.h"
}
#include "foo_vgmstream.h"
#include "foo_filetypes.h"

#ifndef VERSION
#include "version.h"
#endif

#ifndef VERSION
#define PLUGIN_VERSION  __DATE__
#else
#define PLUGIN_VERSION  VERSION
#endif

#define APP_NAME "vgmstream plugin"
#define PLUGIN_DESCRIPTION "vgmstream plugin " VERSION " " __DATE__ "\n" \
            "by hcs, FastElbja, manakoAT, bxaimc, snakemeat, soneek, kode54, bnnm and many others\n" \
            "\n" \
            "foobar2000 plugin by Josh W, kode54\n" \
            "\n" \
            "https://github.com/kode54/vgmstream/\n" \
            "https://sourceforge.net/projects/vgmstream/ (original)"

// called every time a file is added to the playlist (to get info) or when playing
input_vgmstream::input_vgmstream() {
    vgmstream = NULL;
    subsong = 0; // 0 = not set, will be properly changed on first setup_vgmstream
    direct_subsong = false;
    output_channels = 0;

    decoding = false;
    paused = 0;
    decode_pos_ms = 0;
    decode_pos_samples = 0;
    stream_length_samples = 0;
    fade_samples = 0;
    seek_pos_samples = 0;

    fade_seconds = 10.0f;
    fade_delay_seconds = 0.0f;
    loop_count = 2.0f;
    loop_forever = false;
    ignore_loop = 0;
    disable_subsongs = false;
    downmix_channels = 0;
    tagfile_disable = false;
    tagfile_name = "!tags.m3u";
    override_title = false;

    load_settings();
}

// called on stop or when playlist info has been read
input_vgmstream::~input_vgmstream() {
    close_vgmstream(vgmstream);
    vgmstream = NULL;
}

// called first when a new file is accepted, before playing it
void input_vgmstream::open(service_ptr_t<file> p_filehint, const char * p_path,t_input_open_reason p_reason,abort_callback & p_abort) {

    if (!p_path) { // shouldn't be possible
        throw exception_io_data();
        return; //???
    }

    filename = p_path;

    // allow non-existing files in some cases
    bool infile_virtual = !filesystem::g_exists(p_path, p_abort)
        && vgmstream_is_virtual_filename(filename) == 1;

    // don't try to open virtual files as it'll fail
    // (doesn't seem to have any adverse effect, except maybe no stats)
    // setup_vgmstream also makes further checks before file is finally opened
    if (!infile_virtual) {
        // keep file stats around (timestamp, filesize)
        if ( p_filehint.is_empty() )
            input_open_file_helper( p_filehint, filename, p_reason, p_abort );
        stats = p_filehint->get_stats( p_abort );
    }

    switch(p_reason) {
        case input_open_decode: // prepare to retrieve info and decode
        case input_open_info_read: // prepare to retrieve info
            setup_vgmstream(p_abort); // must init vgmstream to get subsongs
            break;

        case input_open_info_write: // prepare to retrieve info and tag
            throw exception_io_data();
            break;

        default: // nothing else should be possible
            throw exception_io_data();
            break;
    }
}

// called after opening file (possibly per subsong too)
unsigned input_vgmstream::get_subsong_count() {
    // if the plugin uses input_factory_t template and returns > 1 here when adding a song to the playlist,
    // foobar will automagically "unpack" it by calling decode_initialize/get_info with all subsong indexes.
    // There is no need to add any playlist code, only properly handle the subsong index.
    if (disable_subsongs)
        return 1;

    // vgmstream ready as method is valid after open() with any reason
    int subsong_count = vgmstream->num_streams;
    if (subsong_count == 0)
        subsong_count = 1; // most formats don't have subsongs

    // pretend we don't have subsongs as we don't want foobar to unpack the rest
    if (direct_subsong)
        subsong_count = 1;

    return subsong_count;
}

// called after get_subsong_count to play subsong N (even when count is 1)
t_uint32 input_vgmstream::get_subsong(unsigned p_index) {
    return p_index + 1; // translates index (0..N < subsong_count) for vgmstream: 1=first
}

// called before playing to get info
void input_vgmstream::get_info(t_uint32 p_subsong, file_info & p_info, abort_callback & p_abort) {
    int length_in_ms=0, channels = 0, samplerate = 0;
    int total_samples = -1;
    int bitrate = 0;
    int loop_flag = -1, loop_start = -1, loop_end = -1;
    pfc::string8 description;
    pfc::string8_fast temp;

    get_subsong_info(p_subsong, temp, &length_in_ms, &total_samples, &loop_flag, &loop_start, &loop_end, &samplerate, &channels, &bitrate, description, p_abort);


    /* set tag info (metadata tab in file properties) */

    /* Shows a custom subsong title by default with subsong name, to simplify for average users.
     * This can be overriden and extended using the exported STREAM_x below and foobar's formatting.
     * foobar defaults to filename minus extension if there is no meta "title" value. */
    if (!override_title) {
        p_info.meta_set("TITLE",temp);
    }
    if (get_description_tag(temp,description,"stream count: ")) p_info.meta_set("stream_count",temp);
    if (get_description_tag(temp,description,"stream index: ")) p_info.meta_set("stream_index",temp);
    if (get_description_tag(temp,description,"stream name: ")) p_info.meta_set("stream_name",temp);

    /* get external file tags */
    //todo optimize and don't parse tags again for this session (not sure how), seems foobar
    // calls get_info on every play even if the file hasn't changes, and won't refresh "meta"
    // unless forced or closing playlist+exe
    if (!tagfile_disable) {
        //todo use foobar's fancy-but-arcane string functions
        char tagfile_path[PATH_LIMIT];
        strcpy(tagfile_path, filename);

        char *path = strrchr(tagfile_path,'\\');
        if (path!=NULL) {
            path[1] = '\0';  /* includes "\", remove after that from tagfile_path */
            strcat(tagfile_path,tagfile_name);
        }
        else { /* ??? */
            strcpy(tagfile_path,tagfile_name);
        }

        STREAMFILE *tagFile = open_foo_streamfile(tagfile_path, &p_abort, NULL);
        if (tagFile != NULL) {
            VGMSTREAM_TAGS *tags;
            const char *tag_key, *tag_val;

            tags = vgmstream_tags_init(&tag_key, &tag_val);
            vgmstream_tags_reset(tags, filename);
            while (vgmstream_tags_next_tag(tags, tagFile)) {
                if (replaygain_info::g_is_meta_replaygain(tag_key)) {
                    p_info.info_set_replaygain(tag_key,tag_val);
                    /* there is info_set_replaygain_auto too but no doc */
                }
                else {
                    p_info.meta_set(tag_key,tag_val);
                }
            }
            vgmstream_tags_close(tags);
            close_streamfile(tagFile);
        }
    }


    /* set technical info (details tab in file properties) */

    p_info.info_set("vgmstream_version",PLUGIN_VERSION);
    p_info.info_set_int("samplerate", samplerate);
    p_info.info_set_int("channels", channels);
    p_info.info_set_int("bitspersample",16);
    /* not quite accurate but some people are confused by "lossless"
     * (could set lossless if PCM, but then again PCMFloat or PCM8 are converted/"lossy" in vgmstream) */
    p_info.info_set("encoding","lossy/lossless");
    p_info.info_set_bitrate(bitrate / 1000);
    if (total_samples > 0)
        p_info.info_set_int("stream_total_samples", total_samples);
    if (loop_start >= 0 && loop_end > loop_start) {
        if (loop_flag <= 0) p_info.info_set("looping", "disabled");
        p_info.info_set_int("loop_start", loop_start);
        p_info.info_set_int("loop_end", loop_end);
    }
    p_info.set_length(((double)length_in_ms)/1000);

    if (get_description_tag(temp,description,"encoding: ")) p_info.info_set("codec",temp);
    if (get_description_tag(temp,description,"layout: ")) p_info.info_set("layout",temp);
    if (get_description_tag(temp,description,"interleave: ",' ')) p_info.info_set("interleave",temp);
    if (get_description_tag(temp,description,"interleave last block:",' ')) p_info.info_set("interleave_last_block",temp);

    if (get_description_tag(temp,description,"block size: ")) p_info.info_set("block_size",temp);
    if (get_description_tag(temp,description,"metadata from: ")) p_info.info_set("metadata_source",temp);
    if (get_description_tag(temp,description,"stream count: ")) p_info.info_set("stream_count",temp);
    if (get_description_tag(temp,description,"stream index: ")) p_info.info_set("stream_index",temp);
    if (get_description_tag(temp,description,"stream name: ")) p_info.info_set("stream_name",temp);

    if (get_description_tag(temp,description,"channel mask: ")) p_info.info_set("channel_mask",temp);
    if (get_description_tag(temp,description,"output channels: ")) p_info.info_set("output_channels",temp);
    if (get_description_tag(temp,description,"input channels: ")) p_info.info_set("input_channels",temp);

}

t_filestats input_vgmstream::get_file_stats(abort_callback & p_abort) {
    return stats;
}

// called right before actually playing (decoding) a song/subsong
void input_vgmstream::decode_initialize(t_uint32 p_subsong, unsigned p_flags, abort_callback & p_abort) {
    force_ignore_loop = !!(p_flags & input_flag_no_looping);

    // if subsong changes recreate vgmstream
    if (subsong != p_subsong && !direct_subsong) {
        subsong = p_subsong;
        setup_vgmstream(p_abort);
    }

    decode_seek( 0, p_abort );
};

// called when audio buffer needs to be filled
bool input_vgmstream::decode_run(audio_chunk & p_chunk,abort_callback & p_abort) {
    if (!decoding) return false;
    if (!vgmstream) return false;

    int max_buffer_samples = SAMPLE_BUFFER_SIZE;
    int samples_to_do = max_buffer_samples;
    t_size bytes;

    {
        bool loop_okay = config.song_play_forever && vgmstream->loop_flag && !config.song_ignore_loop && !force_ignore_loop;
        if (decode_pos_samples + max_buffer_samples > stream_length_samples && !loop_okay)
            samples_to_do = stream_length_samples - decode_pos_samples;
        else
            samples_to_do = max_buffer_samples;

        if (samples_to_do /*< DECODE_SIZE*/ == 0) {
            decoding = false;
            return false; /* EOF, didn't decode samples in this call */
        }


        render_vgmstream(sample_buffer,samples_to_do,vgmstream);

        /* fade! */
        if (vgmstream->loop_flag && fade_samples > 0 && !loop_okay) {
            int fade_channels = output_channels;
            int samples_into_fade = decode_pos_samples - (stream_length_samples - fade_samples);
            if (samples_into_fade + samples_to_do > 0) {
                int j,k;
                for (j=0;j<samples_to_do;j++,samples_into_fade++) {
                    if (samples_into_fade > 0) {
                        double fadedness = (double)(fade_samples-samples_into_fade)/fade_samples;
                        for (k = 0; k < fade_channels; k++) {
                            sample_buffer[j*fade_channels+k] =
                                (short)(sample_buffer[j*fade_channels+k]*fadedness);
                        }
                    }
                }
            }
        }

        unsigned channel_config = vgmstream->channel_layout;
        if (!channel_config)
            channel_config = audio_chunk::g_guess_channel_config(output_channels);
        bytes = (samples_to_do*output_channels * sizeof(sample_buffer[0]));
        p_chunk.set_data_fixedpoint((char*)sample_buffer, bytes, vgmstream->sample_rate, output_channels, 16, channel_config);

        decode_pos_samples+=samples_to_do;
        decode_pos_ms=decode_pos_samples*1000LL/vgmstream->sample_rate;

        return true; /* decoded in this call (sample_buffer or less) */
    }
}

// called when seeking
void input_vgmstream::decode_seek(double p_seconds,abort_callback & p_abort) {
    seek_pos_samples = (int) audio_math::time_to_samples(p_seconds, vgmstream->sample_rate);
    int max_buffer_samples = SAMPLE_BUFFER_SIZE;
    bool loop_okay = config.song_play_forever && vgmstream->loop_flag && !config.song_ignore_loop && !force_ignore_loop;
    int loop_skips = 0;

    // possible when disabling looping without refreshing foobar's cached song length
    // (with infinite looping on p_seconds can't go over seek bar though)
    if (seek_pos_samples > stream_length_samples)
        seek_pos_samples = stream_length_samples;

    int corrected_pos_samples = seek_pos_samples;

    // optimize seeks withing loops
    if (vgmstream->loop_flag && (vgmstream->loop_end_sample - vgmstream->loop_start_sample) && seek_pos_samples >= vgmstream->loop_end_sample) {
        int loop_length = (vgmstream->loop_end_sample - vgmstream->loop_start_sample);

        corrected_pos_samples -= vgmstream->loop_start_sample;
        loop_skips = corrected_pos_samples / loop_length;
        corrected_pos_samples %= loop_length;
        corrected_pos_samples += vgmstream->loop_start_sample;
    }

    // Allow for delta seeks forward, by up to the total length of the stream, if the delta is less than the corrected offset
    if (decode_pos_samples > corrected_pos_samples && decode_pos_samples <= seek_pos_samples &&
            (seek_pos_samples - decode_pos_samples) < stream_length_samples) {
        if (corrected_pos_samples > (seek_pos_samples - decode_pos_samples))
            corrected_pos_samples = seek_pos_samples;
    }
    // Reset of backwards seek
    else if(corrected_pos_samples < decode_pos_samples) {
        reset_vgmstream(vgmstream);
        apply_config(vgmstream, &config); /* config is undone by reset */
        decode_pos_samples = 0;
    }

    // seeking overrun = bad
    if (corrected_pos_samples > stream_length_samples)
        corrected_pos_samples = stream_length_samples;

    while(decode_pos_samples < corrected_pos_samples) {
        int seek_samples = max_buffer_samples;
        if((decode_pos_samples + max_buffer_samples >= stream_length_samples) && !loop_okay)
            seek_samples = stream_length_samples - seek_pos_samples;
        if(decode_pos_samples + max_buffer_samples > seek_pos_samples)
            seek_samples = seek_pos_samples - decode_pos_samples;

        decode_pos_samples += seek_samples;
        render_vgmstream(sample_buffer, seek_samples, vgmstream);
    }

    // seek may have been clamped to skip unneeded loops, adjust as some internals need this value
    vgmstream->loop_count += loop_skips; //todo evil, make seek_vgmstream

    // remove seek loop correction from counter so file ends correctly
    decode_pos_samples = seek_pos_samples;

    decode_pos_ms = decode_pos_samples * 1000LL / vgmstream->sample_rate;

    decoding = loop_okay || decode_pos_samples < stream_length_samples;
}

bool input_vgmstream::decode_can_seek() {return true;}
bool input_vgmstream::decode_get_dynamic_info(file_info & p_out, double & p_timestamp_delta) { return false; }
bool input_vgmstream::decode_get_dynamic_info_track(file_info & p_out, double & p_timestamp_delta) {return false;}
void input_vgmstream::decode_on_idle(abort_callback & p_abort) {/*m_file->on_idle(p_abort);*/}

void input_vgmstream::retag_set_info(t_uint32 p_subsong, const file_info & p_info, abort_callback & p_abort) { /*throw exception_io_data();*/ }
void input_vgmstream::retag_commit(abort_callback & p_abort) { /*throw exception_io_data();*/ }

bool input_vgmstream::g_is_our_content_type(const char * p_content_type) {return false;}

// called to check if file can be processed by the plugin
bool input_vgmstream::g_is_our_path(const char * p_path,const char * p_extension) {
    const char ** ext_list;
    size_t ext_list_len;
    int i;

    ext_list = vgmstream_get_formats(&ext_list_len);

    for (i=0; i < ext_list_len; i++) {
        if (!stricmp_utf8(p_extension, ext_list[i]))
            return 1;
    }

    /* some extensionless files can be handled by vgmstream, try to play */
    if (strlen(p_extension) <= 0) {
        return 1;
    }

    return 0;
}

// internal util to create a VGMSTREAM
VGMSTREAM * input_vgmstream::init_vgmstream_foo(t_uint32 p_subsong, const char * const filename, abort_callback & p_abort) {
    VGMSTREAM *vgmstream = NULL;

    STREAMFILE *streamFile = open_foo_streamfile(filename, &p_abort, &stats);
    if (streamFile) {
        streamFile->stream_index = p_subsong;
        vgmstream = init_vgmstream_from_STREAMFILE(streamFile);
        close_streamfile(streamFile);
    }
    return vgmstream;
}

// internal util to initialize vgmstream
void input_vgmstream::setup_vgmstream(abort_callback & p_abort) {
    // close first in case of changing subsongs
    if (vgmstream) {
        close_vgmstream(vgmstream);
    }

    // subsong and filename are always defined before this
    vgmstream = init_vgmstream_foo(subsong, filename, p_abort);
    if (!vgmstream) {
        throw exception_io_data();
        return;
    }

    // default subsong is 0, meaning first init (vgmstream should open first stream, but not set stream_index).
    // if the stream_index is already set, then the subsong was opened directly by some means (txtp, playlist, etc).
    // add flag as in that case we only want to play the subsong but not unpack subsongs (which foobar does by default).
    if (subsong == 0 && vgmstream->stream_index > 0) {
        subsong = vgmstream->stream_index;
        direct_subsong = true;
    }
    if (subsong == 0)
        subsong = 1;

    set_config_defaults(&config);
    apply_config(vgmstream, &config);

    /* enable after all config but before outbuf (though ATM outbuf is not dynamic so no need to read input_channels) */
    vgmstream_mixing_autodownmix(vgmstream, downmix_channels);
    vgmstream_mixing_enable(vgmstream, SAMPLE_BUFFER_SIZE, NULL /*&input_channels*/, &output_channels);

    decode_pos_ms = 0;
    decode_pos_samples = 0;
    paused = 0;
    stream_length_samples = get_vgmstream_play_samples(config.song_loop_count,config.song_fade_time,config.song_fade_delay,vgmstream);
    fade_samples = (int)(config.song_fade_time * vgmstream->sample_rate);
}

// internal util to get info
void input_vgmstream::get_subsong_info(t_uint32 p_subsong, pfc::string_base & title, int *length_in_ms, int *total_samples, int *loop_flag, int *loop_start, int *loop_end, int *sample_rate, int *channels, int *bitrate, pfc::string_base & description, abort_callback & p_abort) {
    VGMSTREAM * infostream = NULL;
    bool is_infostream = false;
    foobar_song_config infoconfig;
    char temp[1024];
    int info_channels;

    // reuse current vgmstream if not querying a new subsong
    // if it's a direct subsong then subsong may be N while p_subsong 1
    // there is no need to recreate the infostream, there is only one subsong used
    if (subsong != p_subsong && !direct_subsong) {
        infostream = init_vgmstream_foo(p_subsong, filename, p_abort);
        if (!infostream) {
            throw exception_io_data();
        }
        set_config_defaults(&infoconfig);
        apply_config(infostream,&infoconfig);
        is_infostream = true;

        vgmstream_mixing_autodownmix(infostream, downmix_channels);
        vgmstream_mixing_enable(infostream, 0, NULL /*&input_channels*/, &info_channels);
    } else {
        // vgmstream ready as get_info is valid after open() with any reason
        infostream = vgmstream;
        infoconfig = config;
        info_channels = output_channels;
    }


    if (length_in_ms) {
        *length_in_ms = -1000;
        if (infostream) {
            *channels = info_channels;
            *sample_rate = infostream->sample_rate;
            *total_samples = infostream->num_samples;
            *bitrate = get_vgmstream_average_bitrate(infostream);
            *loop_flag = infostream->loop_flag;
            *loop_start = infostream->loop_start_sample;
            *loop_end = infostream->loop_end_sample;

            int num_samples = get_vgmstream_play_samples(infoconfig.song_loop_count,infoconfig.song_fade_time,infoconfig.song_fade_delay,infostream);
            *length_in_ms = num_samples*1000LL / infostream->sample_rate;

            char temp[1024];
            describe_vgmstream(infostream, temp, 1024);
            description = temp;
        }
    }


    /* infostream gets added with index 0 (other) or 1 (current) */
    if (infostream && title) {
        const char *p = filename + strlen(filename);
        while (*p != '\\' && p >= filename) p--;
        p++;
        const char *e = filename + strlen(filename);
        while (*e != '.' && e >= filename) e--;
        title.set_string(p, e - p); /* name without ext */

        const char* info_name = infostream->stream_name;
        int info_streams = infostream->num_streams;
        int info_subsong = infostream->stream_index;
        if (info_subsong == 0)
            info_subsong = 1;

        /* show number if file has more than 1 subsong */
        if (info_streams > 1) {
            sprintf(temp,"#%d",info_subsong);
            title += temp;
        }

        /* show name if file has subsongs (implicitly shows also for TXTP) */
        if (info_name[0] != '\0' && info_streams > 0) {
            sprintf(temp," (%s)",info_name);
            title += temp;
        }
    }

    // and only close if was querying a new subsong
    if (is_infostream) {
        close_vgmstream(infostream);
        infostream = NULL;
    }
}

bool input_vgmstream::get_description_tag(pfc::string_base & temp, pfc::string_base const& description, const char *tag, char delimiter) {
    // extract a "tag" from the description string
    t_size pos = description.find_first(tag);
    t_size eos;
    if (pos != pfc::infinite_size) {
        pos += strlen(tag);
        eos = description.find_first(delimiter, pos);
        if (eos == pfc::infinite_size) eos = description.length();
        temp.set_string(description + pos, eos - pos);
        //console::formatter() << "tag=" << tag << ", delim=" << delimiter << "temp=" << temp << ", pos=" << pos << "" << eos;
        return true;
    }
    return false;
}

void input_vgmstream::set_config_defaults(foobar_song_config *current) {
    current->song_play_forever = loop_forever;
    current->song_loop_count = loop_count;
    current->song_fade_time = fade_seconds;
    current->song_fade_delay = fade_delay_seconds;
    current->song_ignore_loop = ignore_loop;
    current->song_really_force_loop = 0;
    current->song_ignore_fade = 0;
}

void input_vgmstream::apply_config(VGMSTREAM * vgmstream, foobar_song_config *current) {

    /* honor suggested config, if any (defined order matters)
     * note that ignore_fade and play_forever should take priority */
    if (vgmstream->config_loop_count) {
        current->song_loop_count = vgmstream->config_loop_count;
    }
    if (vgmstream->config_fade_delay) {
        current->song_fade_delay = vgmstream->config_fade_delay;
    }
    if (vgmstream->config_fade_time) {
        current->song_fade_time = vgmstream->config_fade_time;
    }
    if (vgmstream->config_force_loop) {
        current->song_really_force_loop = 1;
    }
    if (vgmstream->config_ignore_loop) {
        current->song_ignore_loop = 1;
    }
    if (vgmstream->config_ignore_fade) {
        current->song_ignore_fade = 1;
    }

    /* remove non-compatible options */
    if (current->song_play_forever) {
        current->song_ignore_fade = 0;
        current->song_ignore_loop = 0;
    }

    /* change loop stuff, in no particular order */
    if (current->song_really_force_loop) {
        vgmstream_force_loop(vgmstream, 1, 0,vgmstream->num_samples);
    }
    if (current->song_ignore_loop) {
        vgmstream_force_loop(vgmstream, 0, 0,0);
        current->song_fade_time = 0;
    }

    /* loop N times, but also play stream end instead of fading out */
    if (current->song_loop_count > 0 && current->song_ignore_fade) {
        vgmstream_set_loop_target(vgmstream, (int)current->song_loop_count);
    }
}

GUID input_vgmstream::g_get_guid() {
    static const GUID guid = { 0x9e7263c7, 0x4cdd, 0x482c,{ 0x9a, 0xec, 0x5e, 0x71, 0x28, 0xcb, 0xc3, 0x4 } };
    return guid;
}

const char * input_vgmstream::g_get_name() {
    return "vgmstream";
}

GUID input_vgmstream::g_get_preferences_guid() {
    static const GUID guid = { 0x2b5d0302, 0x165b, 0x409c,{ 0x94, 0x74, 0x2c, 0x8c, 0x2c, 0xd7, 0x6a, 0x25 } };;
    return guid;
}

// checks priority (foobar 1.4+)
bool input_vgmstream::g_is_low_merit() {
    return true;
}

// foobar plugin defs
static input_factory_t<input_vgmstream> g_input_vgmstream_factory;

DECLARE_COMPONENT_VERSION(APP_NAME,PLUGIN_VERSION,PLUGIN_DESCRIPTION);
VALIDATE_COMPONENT_FILENAME("foo_input_vgmstream.dll");
