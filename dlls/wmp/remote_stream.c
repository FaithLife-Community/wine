/*
 * IStream on an http(s) URL, streamed into memory as it is read
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

#define COBJMACROS
#include "wmp_private.h"
#include "wininet.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wmp);

/* The whole file is downloaded into memory in the background, never into the cache.
 * Reads further ahead than this are fetched with a range request instead of waiting
 * for the download to get there (seeking, tags at the end of the file). */
#define RANGE_THRESHOLD (512 * 1024)
#define CHUNK_SIZE (64 * 1024)

struct remote_stream
{
    IStream IStream_iface;
    LONG refcount;

    WCHAR *url;
    WCHAR *content_type;
    HINTERNET session, request;
    HANDLE thread;
    BYTE *data;
    ULONGLONG size, available, position;
    BOOL done, shutdown;
    HRESULT status;
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
};

static struct remote_stream *impl_from_IStream(IStream *iface)
{
    return CONTAINING_RECORD(iface, struct remote_stream, IStream_iface);
}

static HINTERNET open_url(struct remote_stream *stream, const WCHAR *headers, DWORD expect_status)
{
    DWORD status = 0, size = sizeof(status);
    HINTERNET request;

    if (!(request = InternetOpenUrlW(stream->session, stream->url, headers, headers ? -1 : 0,
            INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_COOKIES, 0)))
    {
        WARN("Failed to open %s, error %lu.\n", debugstr_w(stream->url), GetLastError());
        return NULL;
    }
    if (!HttpQueryInfoW(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, NULL)
            || status != expect_status)
    {
        WARN("Got HTTP status %lu for %s, expected %lu.\n", status, debugstr_w(stream->url), expect_status);
        InternetCloseHandle(request);
        return NULL;
    }
    return request;
}

static DWORD CALLBACK download_thread(void *arg)
{
    struct remote_stream *stream = arg;
    HRESULT status = S_OK;
    DWORD read;

    SetThreadDescription(GetCurrentThread(), L"wine_wmp_remote_stream");

    while (!stream->shutdown && stream->available < stream->size)
    {
        if (!InternetReadFile(stream->request, stream->data + stream->available,
                min(stream->size - stream->available, CHUNK_SIZE), &read))
        {
            status = HRESULT_FROM_WIN32(GetLastError());
            break;
        }
        if (!read)
        {
            status = E_FAIL;
            break;
        }
        EnterCriticalSection(&stream->cs);
        stream->available += read;
        WakeAllConditionVariable(&stream->cv);
        LeaveCriticalSection(&stream->cs);
    }

    if (FAILED(status))
        WARN("Download of %s stopped at %I64u of %I64u bytes, hr %#lx.\n",
                debugstr_w(stream->url), stream->available, stream->size, status);
    else
        TRACE("Downloaded %I64u bytes.\n", stream->available);

    EnterCriticalSection(&stream->cs);
    stream->done = TRUE;
    stream->status = status;
    WakeAllConditionVariable(&stream->cv);
    LeaveCriticalSection(&stream->cs);
    return 0;
}

static HRESULT read_range(struct remote_stream *stream, ULONGLONG offset, ULONG length, BYTE *buffer)
{
    WCHAR headers[64];
    HINTERNET request;
    DWORD read, total = 0;
    HRESULT hr = S_OK;

    TRACE("Fetching range %#I64x-%#I64x.\n", offset, offset + length - 1);

    swprintf(headers, ARRAY_SIZE(headers), L"Range: bytes=%I64u-%I64u\r\n", offset, offset + length - 1);
    if (!(request = open_url(stream, headers, 206)))
        return E_FAIL;
    while (total < length)
    {
        if (!InternetReadFile(request, buffer + total, length - total, &read))
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            break;
        }
        if (!read)
        {
            hr = E_FAIL;
            break;
        }
        total += read;
    }
    InternetCloseHandle(request);
    return hr;
}

/* Blocks until the requested bytes have arrived. */
static HRESULT stream_read_at(struct remote_stream *stream, ULONGLONG offset, ULONG length, BYTE *buffer, ULONG *ret)
{
    HRESULT hr;

    *ret = 0;
    if (offset >= stream->size || !length)
        return S_OK;
    if (offset + length > stream->size)
        length = stream->size - offset;

    EnterCriticalSection(&stream->cs);
    for (;;)
    {
        if (stream->shutdown)
        {
            hr = E_ABORT;
            break;
        }
        if (offset + length <= stream->available)
        {
            memcpy(buffer, stream->data + offset, length);
            *ret = length;
            hr = S_OK;
            break;
        }
        if (stream->done || offset > stream->available + RANGE_THRESHOLD)
        {
            LeaveCriticalSection(&stream->cs);
            if (SUCCEEDED(hr = read_range(stream, offset, length, buffer)))
                *ret = length;
            return hr;
        }
        SleepConditionVariableCS(&stream->cv, &stream->cs, INFINITE);
    }
    LeaveCriticalSection(&stream->cs);
    return hr;
}

static HRESULT WINAPI stream_QueryInterface(IStream *iface, REFIID riid, void **out)
{
    TRACE("iface %p, riid %s, out %p.\n", iface, debugstr_guid(riid), out);

    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ISequentialStream) || IsEqualIID(riid, &IID_IStream))
    {
        *out = iface;
        IStream_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI stream_AddRef(IStream *iface)
{
    struct remote_stream *stream = impl_from_IStream(iface);
    return InterlockedIncrement(&stream->refcount);
}

static ULONG WINAPI stream_Release(IStream *iface)
{
    struct remote_stream *stream = impl_from_IStream(iface);
    ULONG refcount = InterlockedDecrement(&stream->refcount);

    if (!refcount)
    {
        EnterCriticalSection(&stream->cs);
        stream->shutdown = TRUE;
        WakeAllConditionVariable(&stream->cv);
        LeaveCriticalSection(&stream->cs);
        if (stream->thread)
        {
            WaitForSingleObject(stream->thread, INFINITE);
            CloseHandle(stream->thread);
        }
        if (stream->request) InternetCloseHandle(stream->request);
        if (stream->session) InternetCloseHandle(stream->session);
        DeleteCriticalSection(&stream->cs);
        free(stream->data);
        free(stream->url);
        free(stream->content_type);
        free(stream);
    }
    return refcount;
}

static HRESULT WINAPI stream_Read(IStream *iface, void *buffer, ULONG size, ULONG *ret)
{
    struct remote_stream *stream = impl_from_IStream(iface);
    ULONG read;
    HRESULT hr;

    TRACE("iface %p, buffer %p, size %lu, ret %p.\n", iface, buffer, size, ret);

    if (SUCCEEDED(hr = stream_read_at(stream, stream->position, size, buffer, &read)))
        stream->position += read;
    if (ret)
        *ret = read;
    if (SUCCEEDED(hr) && read < size)
        hr = S_FALSE;
    return hr;
}

static HRESULT WINAPI stream_Write(IStream *iface, const void *buffer, ULONG size, ULONG *ret)
{
    return STG_E_ACCESSDENIED;
}

static HRESULT WINAPI stream_Seek(IStream *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *ret)
{
    struct remote_stream *stream = impl_from_IStream(iface);
    LONGLONG position;

    TRACE("iface %p, move %I64d, origin %lu, ret %p.\n", iface, move.QuadPart, origin, ret);

    switch (origin)
    {
        case STREAM_SEEK_SET: position = move.QuadPart; break;
        case STREAM_SEEK_CUR: position = stream->position + move.QuadPart; break;
        case STREAM_SEEK_END: position = stream->size + move.QuadPart; break;
        default: return STG_E_INVALIDFUNCTION;
    }
    if (position < 0)
        return STG_E_INVALIDFUNCTION;
    stream->position = position;
    if (ret)
        ret->QuadPart = position;
    return S_OK;
}

static HRESULT WINAPI stream_SetSize(IStream *iface, ULARGE_INTEGER size)
{
    return STG_E_ACCESSDENIED;
}

static HRESULT WINAPI stream_CopyTo(IStream *iface, IStream *dest, ULARGE_INTEGER size,
        ULARGE_INTEGER *read, ULARGE_INTEGER *written)
{
    FIXME("iface %p, stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_Commit(IStream *iface, DWORD flags)
{
    return S_OK;
}

static HRESULT WINAPI stream_Revert(IStream *iface)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_LockRegion(IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type)
{
    return STG_E_INVALIDFUNCTION;
}

static HRESULT WINAPI stream_UnlockRegion(IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type)
{
    return STG_E_INVALIDFUNCTION;
}

static HRESULT WINAPI stream_Stat(IStream *iface, STATSTG *stat, DWORD flags)
{
    struct remote_stream *stream = impl_from_IStream(iface);

    TRACE("iface %p, stat %p, flags %#lx.\n", iface, stat, flags);

    memset(stat, 0, sizeof(*stat));
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = stream->size;
    stat->grfMode = STGM_READ;
    if (!(flags & STATFLAG_NONAME))
    {
        if ((stat->pwcsName = CoTaskMemAlloc((wcslen(stream->url) + 1) * sizeof(WCHAR))))
            wcscpy(stat->pwcsName, stream->url);
    }
    return S_OK;
}

static HRESULT WINAPI stream_Clone(IStream *iface, IStream **out)
{
    FIXME("iface %p, stub!\n", iface);
    return E_NOTIMPL;
}

static const IStreamVtbl remote_stream_vtbl =
{
    stream_QueryInterface,
    stream_AddRef,
    stream_Release,
    stream_Read,
    stream_Write,
    stream_Seek,
    stream_SetSize,
    stream_CopyTo,
    stream_Commit,
    stream_Revert,
    stream_LockRegion,
    stream_UnlockRegion,
    stream_Stat,
    stream_Clone,
};

BOOL is_remote_url(const WCHAR *url)
{
    return url && (!wcsnicmp(url, L"http://", 7) || !wcsnicmp(url, L"https://", 8));
}

HRESULT create_remote_stream(const WCHAR *url, IStream **out, WCHAR **content_type)
{
    WCHAR length_str[32], type[256];
    DWORD size = sizeof(length_str);
    struct remote_stream *stream;

    if (!(stream = calloc(1, sizeof(*stream))))
        return E_OUTOFMEMORY;
    stream->IStream_iface.lpVtbl = &remote_stream_vtbl;
    stream->refcount = 1;
    InitializeCriticalSection(&stream->cs);
    InitializeConditionVariable(&stream->cv);

    if (!(stream->url = wcsdup(url))
            || !(stream->session = InternetOpenW(L"Windows-Media-Player/12.0", INTERNET_OPEN_TYPE_PRECONFIG,
                NULL, NULL, 0))
            || !(stream->request = open_url(stream, NULL, 200)))
        goto fail;

    if (!HttpQueryInfoW(stream->request, HTTP_QUERY_CONTENT_LENGTH, length_str, &size, NULL)
            || !(stream->size = _wcstoui64(length_str, NULL, 10)))
    {
        FIXME("No content length for %s, streams of unknown size are not supported.\n", debugstr_w(url));
        goto fail;
    }
    /* FIXME: the whole file is held in memory while it plays. */
    if (stream->size > (ULONGLONG)1024 * 1024 * 1024 || !(stream->data = malloc(stream->size)))
    {
        WARN("Cannot buffer %I64u bytes.\n", stream->size);
        goto fail;
    }

    size = sizeof(type);
    if (HttpQueryInfoW(stream->request, HTTP_QUERY_CONTENT_TYPE, type, &size, NULL))
    {
        WCHAR *params = wcschr(type, ';');
        if (params) *params = 0;
        *content_type = wcsdup(type);
    }
    else
        *content_type = NULL;

    TRACE("Streaming %s, %I64u bytes, type %s.\n", debugstr_w(url), stream->size, debugstr_w(*content_type));

    if (!(stream->thread = CreateThread(NULL, 0, download_thread, stream, 0, NULL)))
    {
        free(*content_type);
        *content_type = NULL;
        goto fail;
    }

    *out = &stream->IStream_iface;
    return S_OK;

fail:
    IStream_Release(&stream->IStream_iface);
    return HRESULT_FROM_WIN32(ERROR_INTERNET_CANNOT_CONNECT);
}
