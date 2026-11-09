/*
 * Time stretching for the streaming audio renderer
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

/* Waveform similarity overlap-add (WSOLA). Output is built from overlapping
 * Hann-windowed segments of the input, placed every "hop" output frames.
 * Segments are read from the input every hop * rate frames, so playback is
 * faster or slower while the pitch stays the same. Each segment's exact
 * position is searched within a small window to best continue the waveform of
 * the previous one, which avoids the phasing artefacts of plain overlap-add. */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "mf_private.h"

struct tempo
{
    unsigned int channels;
    unsigned int sample_rate;
    unsigned int window;      /* segment length, in frames */
    unsigned int hop;         /* output frames per segment, window / 2 */
    unsigned int tolerance;   /* search range around the ideal position */
    float rate;
    float *hann;              /* first half of the window, the second is 1 - w */

    float *input;             /* interleaved input frames not yet consumed */
    unsigned int input_len, input_size;
    LONGLONG start_time;      /* presentation time of the first frame pushed */
    UINT64 dropped;           /* frames consumed since then; input[0] follows them */
    double ideal;             /* ideal position of the next segment in input */
    int previous;             /* position of the previous segment, or -1 */
    float *tail;              /* windowed second half of the previous segment */
};

struct tempo *tempo_create(unsigned int channels, unsigned int sample_rate)
{
    struct tempo *tempo;
    unsigned int i;

    if (!channels || !sample_rate || !(tempo = calloc(1, sizeof(*tempo))))
        return NULL;

    tempo->channels = channels;
    tempo->sample_rate = sample_rate;
    /* 30 ms segments, searched within 8 ms: suits speech and music alike. */
    tempo->hop = max(sample_rate * 15 / 1000, 16);
    tempo->window = tempo->hop * 2;
    tempo->tolerance = sample_rate * 8 / 1000;
    tempo->rate = 1.0f;

    if (!(tempo->hann = malloc(tempo->hop * sizeof(float)))
            || !(tempo->tail = calloc(tempo->hop * channels, sizeof(float))))
    {
        tempo_destroy(tempo);
        return NULL;
    }
    for (i = 0; i < tempo->hop; ++i)
        tempo->hann[i] = 0.5f - 0.5f * cosf(M_PI * i / tempo->hop);

    tempo_reset(tempo);
    return tempo;
}

void tempo_destroy(struct tempo *tempo)
{
    if (!tempo) return;
    free(tempo->hann);
    free(tempo->tail);
    free(tempo->input);
    free(tempo);
}

void tempo_reset(struct tempo *tempo)
{
    tempo->input_len = 0;
    tempo->ideal = 0.0;
    tempo->previous = -1;
    memset(tempo->tail, 0, tempo->hop * tempo->channels * sizeof(float));
}

void tempo_set_rate(struct tempo *tempo, float rate)
{
    tempo->rate = rate;
}

BOOL tempo_is_empty(const struct tempo *tempo)
{
    return !tempo->input_len && tempo->previous < 0;
}

HRESULT tempo_push(struct tempo *tempo, const float *data, unsigned int frames, LONGLONG time)
{
    unsigned int needed = tempo->input_len + frames;
    float *input;

    if (tempo_is_empty(tempo))
    {
        tempo->start_time = time;
        tempo->dropped = 0;
    }

    if (needed > tempo->input_size)
    {
        unsigned int size = max(needed, tempo->input_size * 2);

        if (!(input = realloc(tempo->input, size * tempo->channels * sizeof(float))))
            return E_OUTOFMEMORY;
        tempo->input = input;
        tempo->input_size = size;
    }

    memcpy(tempo->input + tempo->input_len * tempo->channels, data, frames * tempo->channels * sizeof(float));
    tempo->input_len += frames;
    return S_OK;
}

/* Returns the position within [start, end] whose first "hop" frames best match
 * those at "target", using normalised cross-correlation of the channel sum. */
static unsigned int tempo_search(const struct tempo *tempo, unsigned int target, unsigned int start, unsigned int end)
{
    unsigned int pos, best = target, i, c, channels = tempo->channels;
    const float *ref = tempo->input + target * channels;
    double best_score = -2.0;

    for (pos = start; pos <= end; ++pos)
    {
        const float *cand = tempo->input + pos * channels;
        double dot = 0.0, energy = 0.0;

        /* Every other frame is plenty for finding the alignment. */
        for (i = 0; i < tempo->hop; i += 2)
        {
            float a = 0.0f, b = 0.0f;
            for (c = 0; c < channels; ++c)
            {
                a += ref[i * channels + c];
                b += cand[i * channels + c];
            }
            dot += a * b;
            energy += b * b;
        }

        if (energy > 0.0)
        {
            double score = dot / sqrt(energy);
            if (score > best_score)
            {
                best_score = score;
                best = pos;
            }
        }
        else if (best_score < -1.0)
        {
            best_score = -1.0;
            best = pos;
        }
    }

    return best;
}

unsigned int tempo_output(struct tempo *tempo, float *out, unsigned int max_frames, LONGLONG *time)
{
    unsigned int channels = tempo->channels, hop = tempo->hop, frames = 0, i, c, pos, drop;
    BOOL have_time = FALSE;

    while (frames + hop <= max_frames)
    {
        unsigned int ideal = (unsigned int)(tempo->ideal + 0.5);
        unsigned int start = ideal > tempo->tolerance ? ideal - tempo->tolerance : 0;
        unsigned int end = ideal + tempo->tolerance;
        const float *seg;
        float *dst = out + frames * channels;

        if (end + tempo->window > tempo->input_len)
            break;

        if (tempo->previous < 0)
            pos = ideal;
        else
            pos = tempo_search(tempo, tempo->previous + hop, start, end);

        if (!have_time)
        {
            *time = tempo->start_time + (LONGLONG)((tempo->dropped + ideal) * 10000000 / tempo->sample_rate);
            have_time = TRUE;
        }

        /* Fade the new segment in over the faded-out tail of the previous one. */
        seg = tempo->input + pos * channels;
        for (i = 0; i < hop; ++i)
        {
            for (c = 0; c < channels; ++c)
            {
                dst[i * channels + c] = tempo->tail[i * channels + c] + seg[i * channels + c] * tempo->hann[i];
                tempo->tail[i * channels + c] = seg[(i + hop) * channels + c] * (1.0f - tempo->hann[i]);
            }
        }
        frames += hop;

        tempo->previous = pos;
        tempo->ideal += hop * tempo->rate;

        /* Drop input that no later segment or search can use. */
        ideal = (unsigned int)(tempo->ideal + 0.5);
        drop = min(tempo->previous, ideal > tempo->tolerance ? ideal - tempo->tolerance : 0);
        if (drop)
        {
            tempo->input_len -= drop;
            memmove(tempo->input, tempo->input + drop * channels, tempo->input_len * channels * sizeof(float));
            tempo->dropped += drop;
            tempo->ideal -= drop;
            tempo->previous -= drop;
        }
    }

    return frames;
}

unsigned int tempo_max_output(const struct tempo *tempo, unsigned int input_frames)
{
    /* Output per call is at most what the buffered input allows at the slowest rate, plus a hop. */
    double frames = (tempo->input_len + input_frames) / max(tempo->rate, 0.1f);
    return (unsigned int)frames + tempo->hop * 2;
}
