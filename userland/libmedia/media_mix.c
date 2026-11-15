// media_mix.c — several sounds in one stream (os64/media_mix.h). No library
// calls: the host drives it as it is.

#include "os64/media_mix.h"

void os64_media_voice_reset(os64_media_voice_t *v)
{
    v->anchors = 0;
}

bool os64_media_voice_room(const os64_media_voice_t *v)
{
    return v->anchors < OS64_MEDIA_MIX_ANCHORS;
}

void os64_media_voice_wrote(os64_media_voice_t *v, uint64_t stream, int64_t media, uint32_t frames)
{
    if (frames == 0)
        return;
    if (v->anchors > 0) {
        os64_media_mix_anchor_t *last = &v->anchor[v->anchors - 1];
        if (last->stream + last->frames == stream && last->media + last->frames == media &&
            last->frames <= UINT32_MAX - frames) {
            last->frames += frames;
            return;
        }
    }
    if (v->anchors == OS64_MEDIA_MIX_ANCHORS) {
        for (uint32_t i = 1; i < OS64_MEDIA_MIX_ANCHORS; i++)
            v->anchor[i - 1] = v->anchor[i];
        v->anchors--;
    }
    v->anchor[v->anchors++] = (os64_media_mix_anchor_t){stream, media, frames};
}

void os64_media_voice_prune(os64_media_voice_t *v, uint64_t passed)
{
    uint32_t gone = 0;
    while (gone + 1 < v->anchors && v->anchor[gone + 1].stream <= passed)
        gone++;
    if (gone == 0)
        return;
    for (uint32_t i = gone; i < v->anchors; i++)
        v->anchor[i - gone] = v->anchor[i];
    v->anchors -= gone;
}

int64_t os64_media_voice_heard(const os64_media_voice_t *v, uint64_t heard)
{
    if (v->anchors == 0)
        return -1;
    uint32_t i = v->anchors;
    while (i > 1 && v->anchor[i - 1].stream > heard)
        i--;
    const os64_media_mix_anchor_t *a = &v->anchor[i - 1];
    if (heard <= a->stream)
        return a->media;
    uint64_t into = heard - a->stream;
    return a->media + (int64_t)(into < a->frames ? into : a->frames);
}

uint64_t os64_media_voice_stream_end(const os64_media_voice_t *v)
{
    if (v->anchors == 0)
        return 0;
    const os64_media_mix_anchor_t *a = &v->anchor[v->anchors - 1];
    return a->stream + a->frames;
}

int64_t os64_media_voice_media_end(const os64_media_voice_t *v)
{
    if (v->anchors == 0)
        return -1;
    const os64_media_mix_anchor_t *a = &v->anchor[v->anchors - 1];
    return a->media + a->frames;
}

void os64_media_mix_add(int32_t *acc, const int16_t *pcm, uint32_t frames, double gain)
{
    if (!(gain > 0))
        return;
    if (gain >= 1) {
        for (uint32_t i = 0; i < frames * 2; i++)
            acc[i] += pcm[i];
        return;
    }
    int32_t g = (int32_t)(gain * 65536.0 + 0.5);
    for (uint32_t i = 0; i < frames * 2; i++)
        acc[i] += ((int32_t)pcm[i] * g) >> 16;
}

void os64_media_mix_out(const int32_t *acc, int16_t *out, uint32_t frames)
{
    for (uint32_t i = 0; i < frames * 2; i++)
        out[i] = (int16_t)(acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);
}
