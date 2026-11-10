/* H.264 decoder transform using FFmpeg through winedmo
 *
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

#include <stdint.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "video_decoder.h"

#include "mferror.h"
#include "mftransform.h"

#include "wine/winedmo.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dmo);

static const GUID *const input_types[] =
{
    &MFVideoFormat_H264,
    &MFVideoFormat_H264_ES,
};

static const GUID *const output_types[] =
{
    &MFVideoFormat_NV12,
    &MFVideoFormat_YV12,
    &MFVideoFormat_IYUV,
    &MFVideoFormat_I420,
    &MFVideoFormat_YUY2,
};

struct h264_decoder
{
    IMFTransform IMFTransform_iface;
    LONG refcount;

    IMFAttributes *attributes;
    IMFAttributes *output_attributes;

    IMFMediaType *input_type;
    IMFMediaType *output_type;
    MFT_INPUT_STREAM_INFO input_info;
    MFT_OUTPUT_STREAM_INFO output_info;

    /* size of the decoded frames, 0 until known */
    UINT32 width;
    UINT32 height;

    struct winedmo_decoder winedmo;
    LONGLONG next_time;
};

static struct h264_decoder *impl_from_IMFTransform(IMFTransform *iface)
{
    return CONTAINING_RECORD(iface, struct h264_decoder, IMFTransform_iface);
}

static void update_output_info_size(struct h264_decoder *decoder)
{
    UINT32 i, size, width = decoder->width ? decoder->width : 1920, height = decoder->height ? decoder->height : 1088;

    decoder->output_info.cbSize = 0;
    for (i = 0; i < ARRAY_SIZE(output_types); ++i)
    {
        if (SUCCEEDED(MFCalculateImageSize(output_types[i], width, height, &size)))
            decoder->output_info.cbSize = max(size, decoder->output_info.cbSize);
    }
}

static void destroy_winedmo_decoder(struct h264_decoder *decoder)
{
    winedmo_decoder_destroy(&decoder->winedmo);
}

static HRESULT create_winedmo_decoder(struct h264_decoder *decoder)
{
    MFVIDEOFORMAT *format, *tmp;
    UINT32 size, header_size;
    NTSTATUS status;
    HRESULT hr;

    if (FAILED(hr = MFCreateMFVideoFormatFromMFMediaType(decoder->input_type, &format, &size)))
        return hr;
    format->guidFormat = MFVideoFormat_H264;

    /* the sequence header has the stream's SPS and PPS in Annex B form */
    if (SUCCEEDED(IMFMediaType_GetBlobSize(decoder->input_type, &MF_MT_MPEG_SEQUENCE_HEADER, &header_size))
            && header_size && (tmp = CoTaskMemRealloc(format, sizeof(*format) + header_size)))
    {
        format = tmp;
        IMFMediaType_GetBlob(decoder->input_type, &MF_MT_MPEG_SEQUENCE_HEADER, (BYTE *)(format + 1), header_size, NULL);
        format->dwSize = sizeof(*format) + header_size;
    }

    status = winedmo_decoder_create(&MFMediaType_Video, (union winedmo_format *)format, &decoder->winedmo);
    CoTaskMemFree(format);
    decoder->next_time = 0;
    return status ? HRESULT_FROM_NT(status) : S_OK;
}

static HRESULT create_output_media_type(struct h264_decoder *decoder, const GUID *subtype, IMFMediaType **media_type)
{
    UINT32 width = decoder->width, height = decoder->height, value;
    MFVideoArea aperture = {{0}}, input_aperture;
    IMFMediaType *type;
    UINT64 ratio;
    LONG stride;
    HRESULT hr;

    if (!width || !height)
    {
        if (FAILED(IMFMediaType_GetUINT64(decoder->input_type, &MF_MT_FRAME_SIZE, &ratio)))
            ratio = (UINT64)1920 << 32 | 1080;
        width = ratio >> 32;
        height = (UINT32)ratio;
    }
    aperture.Area.cx = width;
    aperture.Area.cy = height;

    if (FAILED(hr = MFCreateMediaType(&type)))
        return hr;

    IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, subtype);
    IMFMediaType_SetUINT64(type, &MF_MT_FRAME_SIZE, (UINT64)width << 32 | height);

    if (FAILED(IMFMediaType_GetUINT64(decoder->input_type, &MF_MT_FRAME_RATE, &ratio)))
        ratio = (UINT64)30000 << 32 | 1001;
    IMFMediaType_SetUINT64(type, &MF_MT_FRAME_RATE, ratio);
    if (FAILED(IMFMediaType_GetUINT64(decoder->input_type, &MF_MT_PIXEL_ASPECT_RATIO, &ratio)))
        ratio = (UINT64)1 << 32 | 1;
    IMFMediaType_SetUINT64(type, &MF_MT_PIXEL_ASPECT_RATIO, ratio);

    if (SUCCEEDED(hr = MFCalculateImageSize(subtype, width, height, &value)))
        hr = IMFMediaType_SetUINT32(type, &MF_MT_SAMPLE_SIZE, value);
    if (SUCCEEDED(hr) && SUCCEEDED(hr = MFGetStrideForBitmapInfoHeader(subtype->Data1, width, &stride)))
        hr = IMFMediaType_SetUINT32(type, &MF_MT_DEFAULT_STRIDE, abs(stride));

    IMFMediaType_SetUINT32(type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_MixedInterlaceOrProgressive);
    IMFMediaType_SetUINT32(type, &MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_Wide);
    IMFMediaType_SetUINT32(type, &MF_MT_ALL_SAMPLES_INDEPENDENT, 1);
    IMFMediaType_SetUINT32(type, &MF_MT_FIXED_SIZE_SAMPLES, 1);
    IMFMediaType_SetUINT32(type, &MF_MT_VIDEO_ROTATION, 0);

    /* keep the source's picture area when it fits in the decoded frame */
    if (SUCCEEDED(IMFMediaType_GetBlob(decoder->input_type, &MF_MT_MINIMUM_DISPLAY_APERTURE,
            (BYTE *)&input_aperture, sizeof(input_aperture), NULL))
            && input_aperture.OffsetX.value + input_aperture.Area.cx <= width
            && input_aperture.OffsetY.value + input_aperture.Area.cy <= height)
        aperture = input_aperture;
    IMFMediaType_SetBlob(type, &MF_MT_MINIMUM_DISPLAY_APERTURE, (BYTE *)&aperture, sizeof(aperture));
    IMFMediaType_SetBlob(type, &MF_MT_GEOMETRIC_APERTURE, (BYTE *)&aperture, sizeof(aperture));
    IMFMediaType_SetBlob(type, &MF_MT_PAN_SCAN_APERTURE, (BYTE *)&aperture, sizeof(aperture));

    if (FAILED(hr))
    {
        IMFMediaType_Release(type);
        return hr;
    }
    *media_type = type;
    return S_OK;
}

static HRESULT WINAPI transform_QueryInterface(IMFTransform *iface, REFIID iid, void **out)
{
    struct h264_decoder *decoder = impl_from_IMFTransform(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IMFTransform))
    {
        IMFTransform_AddRef((*out = &decoder->IMFTransform_iface));
        return S_OK;
    }

    *out = NULL;
    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    return E_NOINTERFACE;
}

static ULONG WINAPI transform_AddRef(IMFTransform *iface)
{
    struct h264_decoder *decoder = impl_from_IMFTransform(iface);
    ULONG refcount = InterlockedIncrement(&decoder->refcount);
    TRACE("iface %p increasing refcount to %lu.\n", iface, refcount);
    return refcount;
}

static ULONG WINAPI transform_Release(IMFTransform *iface)
{
    struct h264_decoder *decoder = impl_from_IMFTransform(iface);
    ULONG refcount = InterlockedDecrement(&decoder->refcount);

    TRACE("iface %p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
    {
        destroy_winedmo_decoder(decoder);
        if (decoder->input_type)
            IMFMediaType_Release(decoder->input_type);
        if (decoder->output_type)
            IMFMediaType_Release(decoder->output_type);
        IMFAttributes_Release(decoder->output_attributes);
        IMFAttributes_Release(decoder->attributes);
        free(decoder);
    }

    return refcount;
}

static HRESULT WINAPI transform_GetStreamLimits(IMFTransform *iface, DWORD *input_minimum,
        DWORD *input_maximum, DWORD *output_minimum, DWORD *output_maximum)
{
    TRACE("iface %p, input_minimum %p, input_maximum %p, output_minimum %p, output_maximum %p.\n",
            iface, input_minimum, input_maximum, output_minimum, output_maximum);
    *input_minimum = *input_maximum = *output_minimum = *output_maximum = 1;
    return S_OK;
}

static HRESULT WINAPI transform_GetStreamCount(IMFTransform *iface, DWORD *inputs, DWORD *outputs)
{
    TRACE("iface %p, inputs %p, outputs %p.\n", iface, inputs, outputs);
    *inputs = *outputs = 1;
    return S_OK;
}

static HRESULT WINAPI transform_GetStreamIDs(IMFTransform *iface, DWORD input_size, DWORD *inputs,
        DWORD output_size, DWORD *outputs)
{
    TRACE("iface %p, input_size %lu, inputs %p, output_size %lu, outputs %p.\n", iface,
            input_size, inputs, output_size, outputs);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetInputStreamInfo(IMFTransform *iface, DWORD id, MFT_INPUT_STREAM_INFO *info)
{
    struct h264_decoder *decoder = impl_from_IMFTransform(iface);
    TRACE("iface %p, id %#lx, info %p.\n", iface, id, info);
    *info = decoder->input_info;
    return S_OK;
}

static HRESULT WINAPI transform_GetOutputStreamInfo(IMFTransform *iface, DWORD id, MFT_OUTPUT_STREAM_INFO *info)
{
    struct h264_decoder *decoder = impl_from_IMFTransform(iface);
    TRACE("iface %p, id %#lx, info %p.\n", iface, id, info);
    *info = decoder->output_info;
    return S_OK;
}

static HRESULT WINAPI transform_GetAttributes(IMFTransform *iface, IMFAttributes **attributes)
{
    struct h264_decoder *decoder = impl_from_IMFTransform(iface);
    TRACE("iface %p, attributes %p.\n", iface, attributes);
    IMFAttributes_AddRef((*attributes = decoder->attributes));
    return S_OK;
}

static HRESULT WINAPI transform_GetInputStreamAttributes(IMFTransform *iface, DWORD id, IMFAttributes **attributes)
{
    TRACE("iface %p, id %#lx, attributes %p.\n", iface, id, attributes);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetOutputStreamAttributes(IMFTransform *iface, DWORD id, IMFAttributes **attributes)
{
    struct h264_decoder *decoder = impl_from_IMFTransform(iface);
    TRACE("iface %p, id %#lx, attributes %p.\n", iface, id, attributes);
    if (id)
        return MF_E_INVALIDSTREAMNUMBER;
    IMFAttributes_AddRef((*attributes = decoder->output_attributes));
    return S_OK;
}

static HRESULT WINAPI transform_DeleteInputStream(IMFTransform *iface, DWORD id)
{
    TRACE("iface %p, id %#lx.\n", iface, id);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_AddInputStreams(IMFTransform *iface, DWORD streams, DWORD *ids)
{
    TRACE("iface %p, streams %lu, ids %p.\n", iface, streams, ids);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetInputAvailableType(IMFTransform *iface, DWORD id, DWORD index,
        IMFMediaType **type)
{
    IMFMediaType *media_type;
    HRESULT hr;

    TRACE("iface %p, id %#lx, index %#lx, type %p.\n", iface, id, index, type);

    *type = NULL;
    if (index >= ARRAY_SIZE(input_types))
        return MF_E_NO_MORE_TYPES;
    if (FAILED(hr = MFCreateMediaType(&media_type)))
        return hr;
    IMFMediaType_SetGUID(media_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, input_types[index]);
    *type = media_type;
    return S_OK;
}

static HRESULT WINAPI transform_GetOutputAvailableType(IMFTransform *iface, DWORD id,
        DWORD index, IMFMediaType **type)
{
    struct h264_decoder *decoder = impl_from_IMFTransform(iface);

    TRACE("iface %p, id %#lx, index %#lx, type %p.\n", iface, id, index, type);

    *type = NULL;
    if (!decoder->input_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    if (index >= ARRAY_SIZE(output_types))
        return MF_E_NO_MORE_TYPES;
    return create_output_media_type(decoder, output_types[index], type);
}

static HRESULT WINAPI transform_SetInputType(IMFTransform *iface, DWORD id, IMFMediaType *type, DWORD flags)
{
    struct h264_decoder *decoder = impl_from_IMFTransform(iface);
    GUID major, subtype;
    UINT64 frame_size;
    ULONG i;

    TRACE("iface %p, id %#lx, type %p, flags %#lx.\n", iface, id, type, flags);

    if (type)
    {
        if (FAILED(IMFMediaType_GetGUID(type, &MF_MT_MAJOR_TYPE, &major))
                || FAILED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype)))
            return E_INVALIDARG;
        if (!IsEqualGUID(&major, &MFMediaType_Video))
            return MF_E_INVALIDMEDIATYPE;
        for (i = 0; i < ARRAY_SIZE(input_types); ++i)
            if (IsEqualGUID(&subtype, input_types[i]))
                break;
        if (i == ARRAY_SIZE(input_types))
            return MF_E_INVALIDMEDIATYPE;
        if (flags & MFT_SET_TYPE_TEST_ONLY)
            return S_OK;
    }

    destroy_winedmo_decoder(decoder);
    if (decoder->output_type)
    {
        IMFMediaType_Release(decoder->output_type);
        decoder->output_type = NULL;
    }
    if (decoder->input_type)
    {
        IMFMediaType_Release(decoder->input_type);
        decoder->input_type = NULL;
    }
    decoder->width = decoder->height = 0;

    if (type)
    {
        IMFMediaType_AddRef((decoder->input_type = type));
        if (SUCCEEDED(IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &frame_size)))
        {
            decoder->width = frame_size >> 32;
            decoder->height = (UINT32)frame_size;
        }
    }
    update_output_info_size(decoder);
    return S_OK;
}

static HRESULT WINAPI transform_SetOutputType(IMFTransform *iface, DWORD id, IMFMediaType *type, DWORD flags)
{
    struct h264_decoder *decoder = impl_from_IMFTransform(iface);
    GUID major, subtype;
    UINT64 frame_size;
    HRESULT hr;
    ULONG i;

    TRACE("iface %p, id %#lx, type %p, flags %#lx.\n", iface, id, type, flags);

    if (!type)
    {
        if (decoder->output_type)
        {
            IMFMediaType_Release(decoder->output_type);
            decoder->output_type = NULL;
        }
        destroy_winedmo_decoder(decoder);
        return S_OK;
    }

    if (!decoder->input_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (FAILED(hr = IMFMediaType_GetGUID(type, &MF_MT_MAJOR_TYPE, &major))
            || FAILED(hr = IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype)))
        return hr;
    if (!IsEqualGUID(&major, &MFMediaType_Video))
        return MF_E_INVALIDMEDIATYPE;
    for (i = 0; i < ARRAY_SIZE(output_types); ++i)
        if (IsEqualGUID(&subtype, output_types[i]))
            break;
    if (i == ARRAY_SIZE(output_types))
        return MF_E_INVALIDMEDIATYPE;

    if (FAILED(IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &frame_size)))
        return MF_E_INVALIDMEDIATYPE;
    if (decoder->width && decoder->height && frame_size != ((UINT64)decoder->width << 32 | decoder->height))
        return MF_E_INVALIDMEDIATYPE;
    if (flags & MFT_SET_TYPE_TEST_ONLY)
        return S_OK;

    if (decoder->output_type)
        IMFMediaType_Release(decoder->output_type);
    IMFMediaType_AddRef((decoder->output_type = type));
    decoder->width = frame_size >> 32;
    decoder->height = (UINT32)frame_size;
    update_output_info_size(decoder);

    /* a new output type after a format change keeps decoding the same stream */
    if (!decoder->winedmo.handle && FAILED(hr = create_winedmo_decoder(decoder)))
    {
        IMFMediaType_Release(decoder->output_type);
        decoder->output_type = NULL;
    }
    return hr;
}

static HRESULT WINAPI transform_GetInputCurrentType(IMFTransform *iface, DWORD id, IMFMediaType **type)
{
    struct h264_decoder *decoder = impl_from_IMFTransform(iface);
    HRESULT hr;

    TRACE("iface %p, id %#lx, type %p.\n", iface, id, type);

    if (!decoder->input_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    if (FAILED(hr = MFCreateMediaType(type)))
        return hr;
    return IMFMediaType_CopyAllItems(decoder->input_type, (IMFAttributes *)*type);
}

static HRESULT WINAPI transform_GetOutputCurrentType(IMFTransform *iface, DWORD id, IMFMediaType **type)
{
    struct h264_decoder *decoder = impl_from_IMFTransform(iface);
    GUID subtype;
    HRESULT hr;

    TRACE("iface %p, id %#lx, type %p.\n", iface, id, type);

    if (!decoder->output_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    if (FAILED(hr = IMFMediaType_GetGUID(decoder->output_type, &MF_MT_SUBTYPE, &subtype)))
        return hr;
    /* after a format change, this has the new frame size */
    return create_output_media_type(decoder, &subtype, type);
}

static HRESULT WINAPI transform_GetInputStatus(IMFTransform *iface, DWORD id, DWORD *flags)
{
    struct h264_decoder *decoder = impl_from_IMFTransform(iface);

    TRACE("iface %p, id %#lx, flags %p.\n", iface, id, flags);

    if (!decoder->winedmo.handle)
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    *flags = MFT_INPUT_STATUS_ACCEPT_DATA;
    return S_OK;
}

static HRESULT WINAPI transform_GetOutputStatus(IMFTransform *iface, DWORD *flags)
{
    TRACE("iface %p, flags %p.\n", iface, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_SetOutputBounds(IMFTransform *iface, LONGLONG lower, LONGLONG upper)
{
    TRACE("iface %p, lower %I64d, upper %I64d.\n", iface, lower, upper);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_ProcessEvent(IMFTransform *iface, DWORD id, IMFMediaEvent *event)
{
    TRACE("iface %p, id %#lx, event %p.\n", iface, id, event);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_ProcessMessage(IMFTransform *iface, MFT_MESSAGE_TYPE message, ULONG_PTR param)
{
    struct h264_decoder *decoder = impl_from_IMFTransform(iface);

    TRACE("iface %p, message %#x, param %Ix.\n", iface, message, param);

    switch (message)
    {
    case MFT_MESSAGE_SET_D3D_MANAGER:
        /* frames are written to system memory, the caller copies them */
        return param ? E_NOTIMPL : S_OK;

    case MFT_MESSAGE_COMMAND_DRAIN:
        if (!decoder->winedmo.handle)
            return MF_E_TRANSFORM_TYPE_NOT_SET;
        return HRESULT_FROM_NT(winedmo_decoder_drain(decoder->winedmo));

    case MFT_MESSAGE_COMMAND_FLUSH:
        if (!decoder->winedmo.handle)
            return MF_E_TRANSFORM_TYPE_NOT_SET;
        return HRESULT_FROM_NT(winedmo_decoder_flush(decoder->winedmo));

    default:
        return S_OK;
    }
}

static HRESULT WINAPI transform_ProcessInput(IMFTransform *iface, DWORD id, IMFSample *sample, DWORD flags)
{
    struct h264_decoder *decoder = impl_from_IMFTransform(iface);
    LONGLONG time, duration;
    IMFMediaBuffer *buffer;
    DWORD size;
    NTSTATUS status;
    BYTE *data;
    HRESULT hr;

    TRACE("iface %p, id %#lx, sample %p, flags %#lx.\n", iface, id, sample, flags);

    if (!decoder->winedmo.handle)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (FAILED(IMFSample_GetSampleTime(sample, &time)))
        time = INT64_MIN;
    if (FAILED(IMFSample_GetSampleDuration(sample, &duration)))
        duration = INT64_MIN;

    if (FAILED(hr = IMFSample_ConvertToContiguousBuffer(sample, &buffer)))
        return hr;
    if (SUCCEEDED(hr = IMFMediaBuffer_Lock(buffer, &data, NULL, &size)))
    {
        status = winedmo_decoder_process_input(decoder->winedmo, data, size, time, duration);
        IMFMediaBuffer_Unlock(buffer);
        if (status == STATUS_PENDING)
            hr = MF_E_NOTACCEPTING;
        else if (status)
            hr = HRESULT_FROM_NT(status);
    }
    IMFMediaBuffer_Release(buffer);
    return hr;
}

static HRESULT WINAPI transform_ProcessOutput(IMFTransform *iface, DWORD flags, DWORD count,
        MFT_OUTPUT_DATA_BUFFER *samples, DWORD *status)
{
    struct h264_decoder *decoder = impl_from_IMFTransform(iface);
    struct winedmo_frame_format format = {0};
    IMF2DBuffer *buffer_2d = NULL;
    LONGLONG time, duration;
    IMFMediaBuffer *buffer;
    UINT64 frame_rate;
    DWORD buffer_count;
    UINT32 stride;
    NTSTATUS ret;
    DWORD size;
    BYTE *data;
    HRESULT hr;
    UINT len;

    TRACE("iface %p, flags %#lx, count %lu, samples %p, status %p.\n", iface, flags, count, samples, status);

    if (count != 1)
        return E_INVALIDARG;
    if (!decoder->winedmo.handle || !decoder->output_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    *status = samples->dwStatus = 0;
    if (!samples->pSample)
        return E_INVALIDARG;

    IMFMediaType_GetGUID(decoder->output_type, &MF_MT_SUBTYPE, &format.subtype);
    format.width = decoder->width;
    format.height = decoder->height;
    if (FAILED(IMFMediaType_GetUINT32(decoder->output_type, &MF_MT_DEFAULT_STRIDE, &stride)))
    {
        LONG default_stride;
        MFGetStrideForBitmapInfoHeader(format.subtype.Data1, format.width, &default_stride);
        stride = abs(default_stride);
    }
    format.stride = abs((INT32)stride);

    /* Find out if a frame is ready before locking the output buffer: with the
     * EVR, that maps a Direct3D surface, which is far too slow to do for every
     * call that ends up asking for more input. */
    len = 0;
    ret = winedmo_decoder_process_output(decoder->winedmo, &format, NULL, &len, &time, &duration);
    switch (ret)
    {
    case STATUS_BUFFER_TOO_SMALL:
        break;

    case STATUS_PENDING:
    case STATUS_END_OF_FILE:
        return MF_E_TRANSFORM_NEED_MORE_INPUT;

    case STATUS_MEDIA_CHANGED:
        TRACE("Frame size changed to %ux%u.\n", format.width, format.height);
        decoder->width = format.width;
        decoder->height = format.height;
        update_output_info_size(decoder);
        samples->dwStatus |= MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE;
        *status |= MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE;
        return MF_E_TRANSFORM_STREAM_CHANGE;

    default:
        ERR("Failed to decode, status %#lx.\n", ret);
        return E_FAIL;
    }

    /* With the EVR downstream, the session hands us its Direct3D surface samples,
     * which only support 2D locking with their own pitch. */
    if (FAILED(hr = IMFSample_GetBufferCount(samples->pSample, &buffer_count)) || buffer_count != 1)
        hr = IMFSample_ConvertToContiguousBuffer(samples->pSample, &buffer);
    else
        hr = IMFSample_GetBufferByIndex(samples->pSample, 0, &buffer);
    if (FAILED(hr))
        return hr;

    if (SUCCEEDED(IMFMediaBuffer_QueryInterface(buffer, &IID_IMF2DBuffer, (void **)&buffer_2d)))
    {
        LONG pitch;

        if (FAILED(hr = IMF2DBuffer_Lock2D(buffer_2d, &data, &pitch)))
        {
            IMF2DBuffer_Release(buffer_2d);
            IMFMediaBuffer_Release(buffer);
            return hr;
        }
        if (pitch < 0)
        {
            FIXME("Bottom-up output buffers are not supported.\n");
            IMF2DBuffer_Unlock2D(buffer_2d);
            IMF2DBuffer_Release(buffer_2d);
            IMFMediaBuffer_Release(buffer);
            return E_NOTIMPL;
        }
        format.stride = pitch;
        /* rows of pitch bytes: luma, then chroma for 4:2:0 formats */
        size = pitch * format.height;
        if (!IsEqualGUID(&format.subtype, &MFVideoFormat_YUY2))
            size += pitch * ((format.height + 1) / 2);
    }
    else if (FAILED(hr = IMFMediaBuffer_Lock(buffer, &data, &size, NULL)))
    {
        IMFMediaBuffer_Release(buffer);
        return hr;
    }

    len = size;
    ret = winedmo_decoder_process_output(decoder->winedmo, &format, data, &len, &time, &duration);
    if (buffer_2d)
    {
        IMF2DBuffer_Unlock2D(buffer_2d);
        IMF2DBuffer_Release(buffer_2d);
    }
    else
        IMFMediaBuffer_Unlock(buffer);

    switch (ret)
    {
    case STATUS_SUCCESS:
        IMFMediaBuffer_SetCurrentLength(buffer, len);
        if (duration == INT64_MIN || !duration)
        {
            if (FAILED(IMFMediaType_GetUINT64(decoder->output_type, &MF_MT_FRAME_RATE, &frame_rate))
                    || !(UINT32)frame_rate)
                frame_rate = (UINT64)30000 << 32 | 1001;
            duration = MulDiv(10000000, (UINT32)frame_rate, frame_rate >> 32);
        }
        if (time == INT64_MIN)
            time = decoder->next_time;
        decoder->next_time = time + duration;
        IMFSample_SetSampleTime(samples->pSample, time);
        IMFSample_SetSampleDuration(samples->pSample, duration);
        TRACE("Frame at %s, duration %s.\n", wine_dbgstr_longlong(time), wine_dbgstr_longlong(duration));
        IMFSample_SetUINT32(samples->pSample, &MFSampleExtension_CleanPoint, 1);
        hr = S_OK;
        break;

    case STATUS_PENDING:
    case STATUS_END_OF_FILE:
        IMFMediaBuffer_SetCurrentLength(buffer, 0);
        hr = MF_E_TRANSFORM_NEED_MORE_INPUT;
        break;

    case STATUS_MEDIA_CHANGED:
        TRACE("Frame size changed to %ux%u.\n", format.width, format.height);
        IMFMediaBuffer_SetCurrentLength(buffer, 0);
        decoder->width = format.width;
        decoder->height = format.height;
        update_output_info_size(decoder);
        samples->dwStatus |= MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE;
        *status |= MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE;
        hr = MF_E_TRANSFORM_STREAM_CHANGE;
        break;

    case STATUS_BUFFER_TOO_SMALL:
        WARN("Output buffer of %#lx bytes too small, need %#x.\n", size, len);
        hr = MF_E_BUFFERTOOSMALL;
        break;

    default:
        ERR("Failed to decode, status %#lx.\n", ret);
        hr = E_FAIL;
        break;
    }

    IMFMediaBuffer_Release(buffer);
    return hr;
}

static const IMFTransformVtbl transform_vtbl =
{
    transform_QueryInterface,
    transform_AddRef,
    transform_Release,
    transform_GetStreamLimits,
    transform_GetStreamCount,
    transform_GetStreamIDs,
    transform_GetInputStreamInfo,
    transform_GetOutputStreamInfo,
    transform_GetAttributes,
    transform_GetInputStreamAttributes,
    transform_GetOutputStreamAttributes,
    transform_DeleteInputStream,
    transform_AddInputStreams,
    transform_GetInputAvailableType,
    transform_GetOutputAvailableType,
    transform_SetInputType,
    transform_SetOutputType,
    transform_GetInputCurrentType,
    transform_GetOutputCurrentType,
    transform_GetInputStatus,
    transform_GetOutputStatus,
    transform_SetOutputBounds,
    transform_ProcessEvent,
    transform_ProcessMessage,
    transform_ProcessInput,
    transform_ProcessOutput,
};

/* Checks that FFmpeg is there and has an H.264 decoder. */
static BOOL ffmpeg_h264_supported(void)
{
    MFVIDEOFORMAT format = {.dwSize = sizeof(format), .guidFormat = MFVideoFormat_H264};
    struct winedmo_decoder winedmo = {0};

    format.videoInfo.dwWidth = 1920;
    format.videoInfo.dwHeight = 1080;
    if (winedmo_decoder_create(&MFMediaType_Video, (union winedmo_format *)&format, &winedmo))
        return FALSE;
    winedmo_decoder_destroy(&winedmo);
    return TRUE;
}

HRESULT ffmpeg_h264_decoder_create(REFIID riid, void **out)
{
    struct h264_decoder *decoder;
    HRESULT hr;

    TRACE("riid %s, out %p.\n", debugstr_guid(riid), out);

    *out = NULL;
    if (!ffmpeg_h264_supported())
        return E_FAIL;

    if (!(decoder = calloc(1, sizeof(*decoder))))
        return E_OUTOFMEMORY;
    decoder->IMFTransform_iface.lpVtbl = &transform_vtbl;
    decoder->refcount = 1;

    if (FAILED(hr = MFCreateAttributes(&decoder->attributes, 16)))
        goto failed;
    if (FAILED(hr = MFCreateAttributes(&decoder->output_attributes, 0)))
        goto failed;
    IMFAttributes_SetUINT32(decoder->attributes, &MF_LOW_LATENCY, FALSE);
    IMFAttributes_SetUINT32(decoder->attributes, &MFT_DECODER_EXPOSE_OUTPUT_TYPES_IN_NATIVE_ORDER, FALSE);

    decoder->input_info.dwFlags = MFT_INPUT_STREAM_WHOLE_SAMPLES | MFT_INPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER
            | MFT_INPUT_STREAM_FIXED_SAMPLE_SIZE;
    decoder->input_info.cbSize = 0x1000;
    decoder->output_info.dwFlags = MFT_OUTPUT_STREAM_WHOLE_SAMPLES | MFT_OUTPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER
            | MFT_OUTPUT_STREAM_FIXED_SAMPLE_SIZE;
    update_output_info_size(decoder);

    TRACE("Created FFmpeg H.264 decoder %p.\n", decoder);
    hr = IMFTransform_QueryInterface(&decoder->IMFTransform_iface, riid, out);
    IMFTransform_Release(&decoder->IMFTransform_iface);
    return hr;

failed:
    if (decoder->output_attributes)
        IMFAttributes_Release(decoder->output_attributes);
    if (decoder->attributes)
        IMFAttributes_Release(decoder->attributes);
    free(decoder);
    return hr;
}
