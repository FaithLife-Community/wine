/*
 * Copyright 2026 FaithLife Community contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "unix_private.h"

#include <libavutil/hwcontext.h>

#include "mfapi.h"
#include "mmreg.h"

#include "wine/debug.h"

#ifdef HAVE_FFMPEG

WINE_DEFAULT_DEBUG_CHANNEL(dmo);

static inline const char *debugstr_averr( int err )
{
    return wine_dbg_sprintf( "%d (%s)", err, av_err2str(err) );
}

struct decoder
{
    AVCodecContext *ctx;
    AVFrame *frame;
    AVFrame *sw_frame; /* system memory copy of a hardware decoded frame */
    BOOL vaapi_no_map; /* VA-API frames can't be mapped for reading */
    BOOL has_frame; /* frame holds output that hasn't been returned yet */
};

/* Hardware decoders to try, in order: NVDEC on NVIDIA's driver (its VA-API
 * support is a third party wrapper), VA-API for Intel and AMD (and NVIDIA with
 * that wrapper), then Vulkan Video on any driver that supports it. */
static const struct
{
    enum AVHWDeviceType type;
    enum AVPixelFormat format;
    const char *name;
}
hw_decoders[] =
{
    {AV_HWDEVICE_TYPE_CUDA, AV_PIX_FMT_CUDA, "NVDEC"},
    {AV_HWDEVICE_TYPE_VAAPI, AV_PIX_FMT_VAAPI, "VA-API"},
    {AV_HWDEVICE_TYPE_VULKAN, AV_PIX_FMT_VULKAN, "Vulkan"},
};

/* Use the hardware decoder when FFmpeg offers it for the stream, otherwise
 * whatever software format it prefers. FFmpeg calls this again without the
 * hardware format if the hardware decoder fails to initialize. */
static enum AVPixelFormat get_hw_format( AVCodecContext *ctx, const enum AVPixelFormat *formats )
{
    const AVHWDeviceContext *device = (const AVHWDeviceContext *)ctx->hw_device_ctx->data;
    const enum AVPixelFormat *format;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(hw_decoders); i++)
    {
        if (hw_decoders[i].type != device->type) continue;
        for (format = formats; *format != AV_PIX_FMT_NONE; format++)
        {
            if (*format != hw_decoders[i].format) continue;
            TRACE( "Decoding %s with %s\n", avcodec_get_name( ctx->codec_id ), hw_decoders[i].name );
            return *format;
        }
    }

    WARN( "No hardware decoding for %s, decoding in software\n", avcodec_get_name( ctx->codec_id ) );
    return avcodec_default_get_format( ctx, formats );
}

static BOOL use_hw_decoder(void)
{
    const char *env = getenv( "WINEDMO_HWACCEL" );
    return !env || strcmp( env, "0" );
}

/* Create a device for the first hardware decoder that works here. FFmpeg may
 * be built without some of them, and the host may have no driver for them:
 * then decoding stays in software. Returns whether one was found. */
static BOOL init_hw_decoder( AVCodecContext *ctx )
{
    AVBufferRef *device;
    unsigned int i;
    int ret;

    if (!use_hw_decoder()) return FALSE;

    for (i = 0; i < ARRAY_SIZE(hw_decoders); i++)
    {
        if ((ret = av_hwdevice_ctx_create( &device, hw_decoders[i].type, NULL, NULL, 0 )) < 0)
        {
            TRACE( "No %s device, error %s\n", hw_decoders[i].name, debugstr_averr( ret ) );
            continue;
        }
        TRACE( "Created %s device\n", hw_decoders[i].name );
        ctx->hw_device_ctx = device;
        ctx->get_format = get_hw_format;
        return TRUE;
    }

    WARN( "No hardware decoder available, decoding in software\n" );
    return FALSE;
}

static struct decoder *get_decoder( struct winedmo_decoder decoder )
{
    return (struct decoder *)(UINT_PTR)decoder.handle;
}

static INT64 get_user_time( INT64 time )
{
    return time == AV_NOPTS_VALUE ? INT64_MIN : time;
}

static int frame_channels( const AVFrame *frame )
{
#if LIBAVUTIL_VERSION_MAJOR >= 58
    return frame->ch_layout.nb_channels;
#else
    return frame->channels;
#endif
}

static INT64 frame_duration( const AVFrame *frame )
{
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 30, 100)
    return frame->duration ? frame->duration : INT64_MIN;
#else
    return frame->pkt_duration ? frame->pkt_duration : INT64_MIN;
#endif
}

static enum AVCodecID video_codec_id( const MFVIDEOFORMAT *format )
{
    const struct AVCodecTag *table[] = {avformat_get_riff_video_tags(), avformat_get_mov_video_tags(), 0};

    if (IsEqualGUID( &format->guidFormat, &MFVideoFormat_H264 )) return AV_CODEC_ID_H264;
    if (IsEqualGUID( &format->guidFormat, &MFVideoFormat_H264_ES )) return AV_CODEC_ID_H264;
    if (IsEqualGUID( &format->guidFormat, &MFVideoFormat_HEVC )) return AV_CODEC_ID_HEVC;
    return av_codec_get_id( table, format->guidFormat.Data1 );
}

static enum AVCodecID audio_codec_id( const WAVEFORMATEX *format, const BYTE **extra, UINT *extra_size )
{
    const struct AVCodecTag *table[] = {avformat_get_riff_audio_tags(), avformat_get_mov_audio_tags(), 0};
    UINT tag = format->wFormatTag;

    *extra = (const BYTE *)(format + 1);
    *extra_size = format->cbSize;

    if (tag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
    {
        const WAVEFORMATEXTENSIBLE *extensible = (const WAVEFORMATEXTENSIBLE *)format;
        tag = extensible->SubFormat.Data1;
        *extra = (const BYTE *)(extensible + 1);
        *extra_size -= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    }

    if (tag == WAVE_FORMAT_MPEG_HEAAC)
    {
        const HEAACWAVEINFO *info = (const HEAACWAVEINFO *)format;
        UINT info_size = sizeof(*info) - sizeof(WAVEFORMATEX);

        /* the AudioSpecificConfig follows the HEAACWAVEINFO fields, ADTS and
         * LOAS streams carry their configuration in band */
        if (format->cbSize < info_size || info->wPayloadType) *extra_size = 0;
        else
        {
            *extra = (const BYTE *)(info + 1);
            *extra_size = format->cbSize - info_size;
        }
        return AV_CODEC_ID_AAC;
    }
    if (tag == WAVE_FORMAT_RAW_AAC1) return AV_CODEC_ID_AAC;
    if (tag == WAVE_FORMAT_MPEG_ADTS_AAC)
    {
        *extra_size = 0;
        return AV_CODEC_ID_AAC;
    }

    return av_codec_get_id( table, tag );
}

static BOOL is_annexb( const BYTE *data, UINT size )
{
    if (size >= 3 && !data[0] && !data[1] && data[2] == 1) return TRUE;
    if (size >= 4 && !data[0] && !data[1] && !data[2] && data[3] == 1) return TRUE;
    return FALSE;
}

NTSTATUS decoder_create( void *arg )
{
    struct decoder_create_params *params = arg;
    const void *format = (const void *)(UINT_PTR)params->format;
    const BYTE *extra = NULL;
    UINT extra_size = 0;
    struct decoder *decoder;
    const AVCodec *codec;
    AVCodecContext *ctx;
    enum AVCodecID id;
    int ret;

    if (IsEqualGUID( &params->major, &MFMediaType_Video ))
    {
        const MFVIDEOFORMAT *video = format;

        if (params->format_size < sizeof(*video)) return STATUS_INVALID_PARAMETER;
        id = video_codec_id( video );
        extra = (const BYTE *)(video + 1);
        extra_size = min( video->dwSize, params->format_size ) - sizeof(*video);

        /* MF sources send H.264 in Annex B, configuration from an "avcC" box
         * would make FFmpeg expect length prefixed NAL units instead. */
        if (id == AV_CODEC_ID_H264 && !is_annexb( extra, extra_size )) extra_size = 0;
    }
    else if (IsEqualGUID( &params->major, &MFMediaType_Audio ))
    {
        const WAVEFORMATEX *audio = format;

        if (params->format_size < sizeof(*audio)) return STATUS_INVALID_PARAMETER;
        id = audio_codec_id( audio, &extra, &extra_size );
    }
    else
    {
        FIXME( "Unsupported major type %s\n", debugstr_guid( &params->major ) );
        return STATUS_NOT_SUPPORTED;
    }

    if (id == AV_CODEC_ID_NONE || !(codec = avcodec_find_decoder( id )))
    {
        WARN( "No FFmpeg decoder for codec %#x (%s)\n", id, avcodec_get_name( id ) );
        return STATUS_NOT_SUPPORTED;
    }

    if (!(decoder = calloc( 1, sizeof(*decoder) ))) return STATUS_NO_MEMORY;
    if (!(decoder->frame = av_frame_alloc()) || !(decoder->sw_frame = av_frame_alloc())
            || !(decoder->ctx = ctx = avcodec_alloc_context3( codec )))
        goto failed;

    ctx->pkt_timebase = (AVRational){1, 10000000};
    /* Most H.264 streams have one slice per frame, so slice threading alone
     * decodes on one core, too slow for HD video. Frame threading holds back
     * one frame per thread before the first output, and the media session
     * feeds a decoder that returns no output ever more slowly, so keep the
     * frame threads few. */
    ctx->thread_count = 0;
    ctx->thread_type = FF_THREAD_SLICE;
    if (IsEqualGUID( &params->major, &MFMediaType_Video ))
    {
        long cpus = sysconf( _SC_NPROCESSORS_ONLN );
        ctx->thread_count = cpus > 4 ? 4 : cpus > 1 ? cpus : 1;
        ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        /* The GPU decodes; frame threads would only add latency and surfaces. */
        if (init_hw_decoder( ctx )) ctx->thread_count = 1;
    }

    if (IsEqualGUID( &params->major, &MFMediaType_Video ))
    {
        const MFVIDEOFORMAT *video = format;
        ctx->width = video->videoInfo.dwWidth;
        ctx->height = video->videoInfo.dwHeight;
    }
    else
    {
        const WAVEFORMATEX *audio = format;
#if LIBAVUTIL_VERSION_MAJOR >= 58
        av_channel_layout_default( &ctx->ch_layout, audio->nChannels );
#else
        ctx->channels = audio->nChannels;
#endif
        ctx->sample_rate = audio->nSamplesPerSec;
        ctx->block_align = audio->nBlockAlign;
        ctx->bit_rate = (INT64)audio->nAvgBytesPerSec * 8;
        ctx->bits_per_coded_sample = audio->wBitsPerSample;
    }

    if (extra_size)
    {
        if (!(ctx->extradata = av_mallocz( extra_size + AV_INPUT_BUFFER_PADDING_SIZE ))) goto failed;
        memcpy( ctx->extradata, extra, extra_size );
        ctx->extradata_size = extra_size;
    }

    if ((ret = avcodec_open2( ctx, codec, NULL )) < 0)
    {
        ERR( "Failed to open %s decoder, error %s\n", codec->name, debugstr_averr( ret ) );
        goto failed;
    }

    TRACE( "Created %s decoder %p, extradata size %u\n", codec->name, decoder, extra_size );
    params->decoder.handle = (UINT_PTR)decoder;
    return STATUS_SUCCESS;

failed:
    avcodec_free_context( &decoder->ctx );
    av_frame_free( &decoder->sw_frame );
    av_frame_free( &decoder->frame );
    free( decoder );
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS decoder_destroy( void *arg )
{
    struct decoder_destroy_params *params = arg;
    struct decoder *decoder = get_decoder( params->decoder );

    TRACE( "decoder %p\n", decoder );

    avcodec_free_context( &decoder->ctx );
    av_frame_free( &decoder->sw_frame );
    av_frame_free( &decoder->frame );
    free( decoder );
    return STATUS_SUCCESS;
}

NTSTATUS decoder_process_input( void *arg )
{
    struct decoder_process_input_params *params = arg;
    struct decoder *decoder = get_decoder( params->decoder );
    struct sample *sample = &params->sample;
    AVPacket *packet;
    int ret;

    TRACE( "decoder %p, size %#"PRIx64", pts %"PRId64"\n", decoder, sample->size, sample->pts );

    if (!sample->data)
    {
        /* no more input: let the decoder output what it still holds */
        if ((ret = avcodec_send_packet( decoder->ctx, NULL )) < 0 && ret != AVERROR_EOF)
            WARN( "Failed to start draining, error %s\n", debugstr_averr( ret ) );
        return STATUS_SUCCESS;
    }

    if (!(packet = av_packet_alloc())) return STATUS_NO_MEMORY;
    if (av_new_packet( packet, sample->size ) < 0)
    {
        av_packet_free( &packet );
        return STATUS_NO_MEMORY;
    }
    memcpy( packet->data, (const void *)(UINT_PTR)sample->data, sample->size );
    packet->pts = sample->pts == INT64_MIN ? AV_NOPTS_VALUE : sample->pts;
    packet->duration = sample->duration == INT64_MIN ? 0 : sample->duration;

    ret = avcodec_send_packet( decoder->ctx, packet );
    av_packet_free( &packet );

    if (ret == AVERROR(EAGAIN)) return STATUS_PENDING;
    if (ret == AVERROR_EOF)
    {
        /* input after a drain without reading all output: start over */
        avcodec_flush_buffers( decoder->ctx );
        return STATUS_PENDING;
    }
    /* a corrupt packet must not stop playback, the decoder conceals it */
    if (ret < 0) WARN( "Failed to decode packet, error %s\n", debugstr_averr( ret ) );
    return STATUS_SUCCESS;
}

static void fill_rows( BYTE *dst, UINT stride, UINT width, UINT rows, BYTE value )
{
    while (rows--)
    {
        memset( dst, value, width );
        dst += stride;
    }
}

static void copy_rows( BYTE *dst, UINT dst_stride, const BYTE *src, int src_stride, UINT width, UINT rows )
{
    while (rows--)
    {
        memcpy( dst, src, width );
        dst += dst_stride;
        src += src_stride;
    }
}

/* chroma sample (x, y) of a 4:2:0 frame, as u or v */
static inline BYTE frame_chroma( const AVFrame *frame, BOOL interleaved, UINT plane, UINT x, UINT y )
{
    if (interleaved) return frame->data[1][y * frame->linesize[1] + 2 * x + plane];
    return frame->data[1 + plane][y * frame->linesize[1 + plane] + x];
}

static NTSTATUS write_video_frame( const AVFrame *frame, const struct winedmo_frame_format *format, BYTE *dst, UINT64 *size )
{
    UINT width = min( frame->width, format->width ), height = min( frame->height, format->height );
    UINT stride = format->stride, chroma_width = (width + 1) / 2, chroma_height = (height + 1) / 2;
    UINT out_chroma_height = (format->height + 1) / 2, x, y, needed;
    BOOL interleaved;

    switch (frame->format)
    {
    case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P: interleaved = FALSE; break;
    case AV_PIX_FMT_NV12: interleaved = TRUE; break;
    default:
        FIXME( "Unsupported pixel format %s\n", av_get_pix_fmt_name( frame->format ) );
        return STATUS_NOT_SUPPORTED;
    }

    if (IsEqualGUID( &format->subtype, &MFVideoFormat_YUY2 )) needed = stride * format->height;
    else needed = stride * format->height + 2 * (stride / 2) * out_chroma_height;
    if (IsEqualGUID( &format->subtype, &MFVideoFormat_NV12 )) needed = stride * (format->height + out_chroma_height);
    if (*size < needed)
    {
        *size = needed;
        return STATUS_BUFFER_TOO_SMALL;
    }
    *size = needed;

    if (IsEqualGUID( &format->subtype, &MFVideoFormat_YUY2 ))
    {
        for (y = 0; y < height; y++)
        {
            const BYTE *luma = frame->data[0] + y * frame->linesize[0];
            BYTE *row = dst + y * stride;
            for (x = 0; x < width; x++)
            {
                row[2 * x] = luma[x];
                row[2 * x + 1] = frame_chroma( frame, interleaved, x & 1, x / 2, y / 2 );
            }
        }
        for (; y < format->height; y++)
            for (x = 0; x < stride / 2; x++)
            {
                dst[y * stride + 2 * x] = 16;
                dst[y * stride + 2 * x + 1] = 128;
            }
        return STATUS_SUCCESS;
    }

    /* luma, then black below the picture if the output is taller */
    copy_rows( dst, stride, frame->data[0], frame->linesize[0], width, height );
    fill_rows( dst + height * stride, stride, stride, format->height - height, 16 );
    dst += format->height * stride;

    if (IsEqualGUID( &format->subtype, &MFVideoFormat_NV12 ))
    {
        for (y = 0; y < chroma_height; y++)
        {
            BYTE *row = dst + y * stride;
            if (interleaved) memcpy( row, frame->data[1] + y * frame->linesize[1], 2 * chroma_width );
            else for (x = 0; x < chroma_width; x++)
            {
                row[2 * x] = frame->data[1][y * frame->linesize[1] + x];
                row[2 * x + 1] = frame->data[2][y * frame->linesize[2] + x];
            }
        }
        fill_rows( dst + chroma_height * stride, stride, stride, out_chroma_height - chroma_height, 128 );
        return STATUS_SUCCESS;
    }

    if (IsEqualGUID( &format->subtype, &MFVideoFormat_I420 ) || IsEqualGUID( &format->subtype, &MFVideoFormat_IYUV )
            || IsEqualGUID( &format->subtype, &MFVideoFormat_YV12 ))
    {
        UINT plane, chroma_stride = stride / 2;
        BOOL swap = IsEqualGUID( &format->subtype, &MFVideoFormat_YV12 );

        for (plane = 0; plane < 2; plane++)
        {
            UINT source = swap ? 1 - plane : plane;
            BYTE *out = dst + plane * chroma_stride * out_chroma_height;

            if (!interleaved)
                copy_rows( out, chroma_stride, frame->data[1 + source], frame->linesize[1 + source],
                           chroma_width, chroma_height );
            else for (y = 0; y < chroma_height; y++)
                for (x = 0; x < chroma_width; x++)
                    out[y * chroma_stride + x] = frame_chroma( frame, TRUE, source, x, y );
            fill_rows( out + chroma_height * chroma_stride, chroma_stride, chroma_stride,
                       out_chroma_height - chroma_height, 128 );
        }
        return STATUS_SUCCESS;
    }

    FIXME( "Unsupported output format %s\n", debugstr_guid( &format->subtype ) );
    return STATUS_NOT_SUPPORTED;
}

static float frame_sample( const AVFrame *frame, int channels, int channel, int index )
{
    const BYTE *const *data = (const BYTE *const *)frame->extended_data;
    int packed = index * channels + channel;

    switch (frame->format)
    {
    case AV_SAMPLE_FMT_FLTP: return ((const float *)data[channel])[index];
    case AV_SAMPLE_FMT_FLT: return ((const float *)data[0])[packed];
    case AV_SAMPLE_FMT_DBLP: return ((const double *)data[channel])[index];
    case AV_SAMPLE_FMT_DBL: return ((const double *)data[0])[packed];
    case AV_SAMPLE_FMT_S16P: return ((const INT16 *)data[channel])[index] / 32768.0f;
    case AV_SAMPLE_FMT_S16: return ((const INT16 *)data[0])[packed] / 32768.0f;
    case AV_SAMPLE_FMT_S32P: return ((const INT32 *)data[channel])[index] / 2147483648.0f;
    case AV_SAMPLE_FMT_S32: return ((const INT32 *)data[0])[packed] / 2147483648.0f;
    case AV_SAMPLE_FMT_U8P: return (data[channel][index] - 128) / 128.0f;
    case AV_SAMPLE_FMT_U8: return (data[0][packed] - 128) / 128.0f;
    default: return 0.0f;
    }
}

static NTSTATUS write_audio_frame( const AVFrame *frame, const struct winedmo_frame_format *format, BYTE *dst, UINT64 *size )
{
    int channels = frame_channels( frame ), i, c;
    BOOL is_float = IsEqualGUID( &format->subtype, &MFAudioFormat_Float );
    UINT needed = frame->nb_samples * channels * (is_float ? sizeof(float) : sizeof(INT16));

    TRACE( "%d samples, %d channels, %s, writing %s\n", frame->nb_samples, channels,
           av_get_sample_fmt_name( frame->format ), is_float ? "float" : "16-bit" );

    if (frame->format != AV_SAMPLE_FMT_FLTP && frame->format != AV_SAMPLE_FMT_FLT && frame->format != AV_SAMPLE_FMT_DBLP
            && frame->format != AV_SAMPLE_FMT_DBL && frame->format != AV_SAMPLE_FMT_S16P && frame->format != AV_SAMPLE_FMT_S16
            && frame->format != AV_SAMPLE_FMT_S32P && frame->format != AV_SAMPLE_FMT_S32
            && frame->format != AV_SAMPLE_FMT_U8P && frame->format != AV_SAMPLE_FMT_U8)
    {
        FIXME( "Unsupported sample format %s\n", av_get_sample_fmt_name( frame->format ) );
        return STATUS_NOT_SUPPORTED;
    }

    if (*size < needed)
    {
        *size = needed;
        return STATUS_BUFFER_TOO_SMALL;
    }
    *size = needed;

    for (i = 0; i < frame->nb_samples; i++)
    {
        for (c = 0; c < channels; c++)
        {
            float value = frame_sample( frame, channels, c, i );
            if (is_float) ((float *)dst)[i * channels + c] = value;
            else
            {
                value = value * 32768.0f;
                if (value > 32767.0f) value = 32767.0f;
                if (value < -32768.0f) value = -32768.0f;
                ((INT16 *)dst)[i * channels + c] = (INT16)(value >= 0.0f ? value + 0.5f : value - 0.5f);
            }
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS decoder_process_output( void *arg )
{
    struct decoder_process_output_params *params = arg;
    struct decoder *decoder = get_decoder( params->decoder );
    struct winedmo_frame_format *format = &params->format;
    struct sample *sample = &params->sample;
    AVFrame *frame = decoder->frame;
    NTSTATUS status;
    int ret;

    TRACE( "decoder %p, capacity %#"PRIx64"\n", decoder, sample->size );

    if (!decoder->has_frame)
    {
        if ((ret = avcodec_receive_frame( decoder->ctx, frame )) == AVERROR(EAGAIN)) return STATUS_PENDING;
        if (ret == AVERROR_EOF)
        {
            /* drained, accept input again */
            avcodec_flush_buffers( decoder->ctx );
            return STATUS_END_OF_FILE;
        }
        if (ret < 0)
        {
            WARN( "Failed to receive frame, error %s\n", debugstr_averr( ret ) );
            return STATUS_PENDING;
        }
        if (frame->hw_frames_ctx)
        {
            /* Get a hardware decoded frame into system memory. A VA-API frame is
             * mapped for reading, which has the driver copy the surface into an
             * image (vaGetImage), and is written out from there; transferring it
             * would copy it once more. Other devices can only transfer. */
            AVFrame *sw_frame = decoder->sw_frame;
            struct timespec t0, t1;

            if (TRACE_ON(dmo)) clock_gettime( CLOCK_MONOTONIC, &t0 );
            if (frame->format == AV_PIX_FMT_VAAPI && !decoder->vaapi_no_map
                    && (ret = av_hwframe_map( sw_frame, frame, AV_HWFRAME_MAP_READ )) < 0)
            {
                WARN( "Failed to map VA-API frame, error %s, transferring instead\n", debugstr_averr( ret ) );
                decoder->vaapi_no_map = TRUE;
                av_frame_unref( sw_frame );
            }
            if ((!sw_frame->buf[0] && (ret = av_hwframe_transfer_data( sw_frame, frame, 0 )) < 0)
                    || (ret = av_frame_copy_props( sw_frame, frame )) < 0)
            {
                WARN( "Failed to read back hardware frame, error %s\n", debugstr_averr( ret ) );
                av_frame_unref( sw_frame );
                av_frame_unref( frame );
                return STATUS_PENDING;
            }
            if (TRACE_ON(dmo))
            {
                clock_gettime( CLOCK_MONOTONIC, &t1 );
                TRACE( "Read back %s frame in %.2f ms\n", av_get_pix_fmt_name( sw_frame->format ),
                       (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6 );
            }
            av_frame_unref( frame );
            av_frame_move_ref( frame, sw_frame );
        }
        decoder->has_frame = TRUE;
    }

    if (decoder->ctx->codec_type == AVMEDIA_TYPE_VIDEO)
    {
        struct timespec t0, t1;

        /* the output may be larger, padded to the coded size, but not smaller
         * or more than one macroblock larger */
        if (frame->width > format->width || frame->height > format->height
                || format->width >= frame->width + 16 || format->height >= frame->height + 16)
        {
            TRACE( "Frame size changed to %ux%u\n", frame->width, frame->height );
            format->width = frame->width;
            format->height = frame->height;
            return STATUS_MEDIA_CHANGED;
        }

        if (TRACE_ON(dmo)) clock_gettime( CLOCK_MONOTONIC, &t0 );
        status = write_video_frame( frame, format, (BYTE *)(UINT_PTR)sample->data, &sample->size );
        if (TRACE_ON(dmo) && !status && sample->data)
        {
            clock_gettime( CLOCK_MONOTONIC, &t1 );
            TRACE( "Wrote %s frame in %.2f ms\n", av_get_pix_fmt_name( frame->format ),
                   (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6 );
        }
        sample->duration = frame_duration( frame );
    }
    else
    {
        if (frame_channels( frame ) != format->channels || frame->sample_rate != format->rate)
        {
            TRACE( "Audio format changed to %u channels at %u Hz\n", frame_channels( frame ), frame->sample_rate );
            format->channels = frame_channels( frame );
            format->rate = frame->sample_rate;
            return STATUS_MEDIA_CHANGED;
        }
        status = write_audio_frame( frame, format, (BYTE *)(UINT_PTR)sample->data, &sample->size );
        sample->duration = frame->sample_rate ? (INT64)frame->nb_samples * 10000000 / frame->sample_rate : INT64_MIN;
    }

    if (status == STATUS_BUFFER_TOO_SMALL) return status;

    sample->pts = get_user_time( frame->best_effort_timestamp );
    sample->dts = INT64_MIN;
    sample->flags = SAMPLE_FLAG_SYNC_POINT;
    av_frame_unref( frame );
    decoder->has_frame = FALSE;
    return status;
}

NTSTATUS decoder_flush( void *arg )
{
    struct decoder_flush_params *params = arg;
    struct decoder *decoder = get_decoder( params->decoder );

    TRACE( "decoder %p\n", decoder );

    avcodec_flush_buffers( decoder->ctx );
    av_frame_unref( decoder->frame );
    decoder->has_frame = FALSE;
    return STATUS_SUCCESS;
}

#endif /* HAVE_FFMPEG */
