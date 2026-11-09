/*
 * Copyright 2024 Rémi Bernon for CodeWeavers
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

#include <stddef.h>
#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"

#include "mfapi.h"
#include "mfidl.h"
#include "rpcproxy.h"
#include "winreg.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dmo);

#include "initguid.h"

DEFINE_MEDIATYPE_GUID(MFAudioFormat_RAW_AAC,WAVE_FORMAT_RAW_AAC1);

extern HRESULT ffmpeg_aac_decoder_create(REFIID riid, void **out);

/* FFmpeg decoders are used when Wine's FFmpeg media source is enabled, or
 * when they are asked for explicitly; GStreamer stays the fallback. */
static BOOL use_ffmpeg_decoders(void)
{
    DWORD value, size = sizeof(value);

    if (!RegGetValueW(HKEY_CURRENT_USER, L"Software\\Wine\\MediaFoundation", L"DisableGstDecoders",
            RRF_RT_REG_DWORD, NULL, &value, &size))
        return !!value;
    size = sizeof(value);
    if (!RegGetValueW(HKEY_CURRENT_USER, L"Software\\Wine\\MediaFoundation", L"DisableGstByteStreamHandler",
            RRF_RT_REG_DWORD, NULL, &value, &size))
        return !!value;
    return FALSE;
}

static HRESULT WINAPI aac_decoder_factory_CreateInstance(IClassFactory *iface, IUnknown *outer,
        REFIID riid, void **out)
{
    static const GUID CLSID_wg_aac_decoder = {0xe7889a8a,0x2083,0x4844,{0x83,0x70,0x5e,0xe3,0x49,0xb1,0x45,0x03}};
    BOOL ffmpeg = use_ffmpeg_decoders();
    HRESULT hr;

    TRACE("outer %p, riid %s, out %p, ffmpeg %u.\n", outer, debugstr_guid(riid), out, ffmpeg);

    if (ffmpeg && !outer && SUCCEEDED(hr = ffmpeg_aac_decoder_create(riid, out)))
        return hr;
    if (SUCCEEDED(hr = CoCreateInstance(&CLSID_wg_aac_decoder, outer, CLSCTX_INPROC_SERVER, riid, out))
            || ffmpeg || outer)
        return hr;
    /* GStreamer or its AAC plugin is missing, try FFmpeg */
    return ffmpeg_aac_decoder_create(riid, out);
}

static HRESULT WINAPI class_factory_QueryInterface(IClassFactory *iface, REFIID riid, void **out)
{
    *out = IsEqualGUID(riid, &IID_IClassFactory) || IsEqualGUID(riid, &IID_IUnknown) ? iface : NULL;
    return *out ? S_OK : E_NOINTERFACE;
}
static ULONG WINAPI class_factory_AddRef(IClassFactory *iface)
{
    return 2;
}
static ULONG WINAPI class_factory_Release(IClassFactory *iface)
{
    return 1;
}
static HRESULT WINAPI class_factory_LockServer(IClassFactory *iface, BOOL dolock)
{
    return S_OK;
}

static const IClassFactoryVtbl aac_decoder_factory_vtbl =
{
    class_factory_QueryInterface,
    class_factory_AddRef,
    class_factory_Release,
    aac_decoder_factory_CreateInstance,
    class_factory_LockServer,
};

static IClassFactory aac_decoder_factory = {&aac_decoder_factory_vtbl};

/***********************************************************************
 *              DllGetClassObject (msauddecmft.@)
 */
HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
    if (IsEqualGUID(clsid, &CLSID_MSAACDecMFT))
        return IClassFactory_QueryInterface(&aac_decoder_factory, riid, out);

    *out = NULL;
    FIXME("Unknown clsid %s.\n", debugstr_guid(clsid));
    return CLASS_E_CLASSNOTAVAILABLE;
}

/***********************************************************************
 *              DllRegisterServer (msauddecmft.@)
 */
HRESULT WINAPI DllRegisterServer(void)
{
    MFT_REGISTER_TYPE_INFO aac_decoder_mft_inputs[] =
    {
        {MFMediaType_Audio, MFAudioFormat_AAC},
        {MFMediaType_Audio, MFAudioFormat_RAW_AAC},
        {MFMediaType_Audio, MFAudioFormat_ADTS},
    };
    MFT_REGISTER_TYPE_INFO aac_decoder_mft_outputs[] =
    {
        {MFMediaType_Audio, MFAudioFormat_Float},
        {MFMediaType_Audio, MFAudioFormat_PCM},
    };
    HRESULT hr;

    TRACE("\n");

    if (FAILED(hr = __wine_register_resources()))
        return hr;
    if (FAILED(hr = MFTRegister(CLSID_MSAACDecMFT, MFT_CATEGORY_AUDIO_DECODER,
            (WCHAR *)L"Microsoft AAC Audio Decoder MFT", MFT_ENUM_FLAG_SYNCMFT,
            ARRAY_SIZE(aac_decoder_mft_inputs), aac_decoder_mft_inputs,
            ARRAY_SIZE(aac_decoder_mft_outputs), aac_decoder_mft_outputs, NULL)))
        return hr;

    return S_OK;
}

/***********************************************************************
 *              DllUnregisterServer (msauddecmft.@)
 */
HRESULT WINAPI DllUnregisterServer(void)
{
    HRESULT hr;

    TRACE("\n");

    if (FAILED(hr = __wine_unregister_resources()))
        return hr;
    if (FAILED(hr = MFTUnregister(CLSID_MSAACDecMFT)))
        return hr;

    return S_OK;
}
