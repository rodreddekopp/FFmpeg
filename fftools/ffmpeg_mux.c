/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "ffmpeg.h"
#include "ffmpeg_mux.h"
#include "ffmpeg_utils.h"
#include "sync_queue.h"

#include "libavutil/avstring.h"
#include "libavutil/base64.h"
#include "libavutil/error.h"
#include "libavutil/fifo.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/time.h"
#include "libavutil/timestamp.h"

#include "libavcodec/packet.h"

#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#if (defined(CONFIG_MOV_MUXER)  && CONFIG_MOV_MUXER)  || \
    (defined(CONFIG_MP4_MUXER)  && CONFIG_MP4_MUXER)  || \
    (defined(CONFIG_ISMV_MUXER) && CONFIG_ISMV_MUXER) || \
    (defined(CONFIG_M4A_MUXER)  && CONFIG_M4A_MUXER)  || \
    (defined(CONFIG_3GP_MUXER)  && CONFIG_3GP_MUXER)  || \
    (defined(CONFIG_3G2_MUXER)  && CONFIG_3G2_MUXER)  || \
    (defined(CONFIG_MJ2_MUXER)  && CONFIG_MJ2_MUXER)  || \
    (defined(CONFIG_ISM_MUXER)  && CONFIG_ISM_MUXER)
#define ENABLE_MOV_RECOVERY 1
#else
#define ENABLE_MOV_RECOVERY 0
#endif

#if ENABLE_MOV_RECOVERY
#include "libavformat/movenc.h"
#endif

typedef struct MuxThreadContext {
    AVPacket *pkt;
    AVPacket *fix_sub_duration_pkt;
} MuxThreadContext;

static Muxer *mux_from_of(OutputFile *of)
{
    return (Muxer*)of;
}

static int64_t filesize(AVIOContext *pb)
{
    int64_t ret = -1;

    if (pb) {
        ret = avio_size(pb);
        if (ret <= 0) // FIXME improve avio_size() so it works with non seekable output too
            ret = avio_tell(pb);
    }

    return ret;
}

#if ENABLE_MOV_RECOVERY

#define MOV_RECOVERY_VERSION 1

typedef struct MovCheckpointEntry {
    uint64_t pos;
    int64_t  dts;
    int64_t  pts;
    uint32_t size;
    uint32_t stsd_index;
    uint32_t samples_in_chunk;
    uint32_t chunk_num;
    uint32_t entries;
    int32_t  cts;
    uint32_t flags;
    int64_t  prft_wallclock;
    int32_t  prft_flags;
} MovCheckpointEntry;

struct MovRecoveryIndex {
    unsigned version;
    int      stream_index;

    uint32_t mode;
    uint32_t flags;
    uint32_t timecode_flags;
    int      has_keyframes;
    int      has_disposable;
    int      last_sample_is_subtitle_end;
    int      end_reliable;
    int      frag_discont;

    uint32_t default_sample_flags;
    uint32_t default_size;

    unsigned timescale;

    uint64_t entry;
    uint64_t entry_written;
    uint64_t entries_flushed;
    uint64_t sample_count;
    uint64_t sample_size;
    uint64_t chunk_count;
    int64_t  time;
    int64_t  track_duration;
    int64_t  start_dts;
    int64_t  start_cts;
    int64_t  end_pts;
    int64_t  dts_shift;
    int64_t  default_duration;
    int64_t  data_offset;

    unsigned nb_written;
    unsigned nb_pending;
    MovCheckpointEntry *written;
    MovCheckpointEntry *pending;
};

static void mov_recovery_index_free(struct MovRecoveryIndex **pidx)
{
    struct MovRecoveryIndex *idx = pidx ? *pidx : NULL;

    if (!idx)
        return;

    av_freep(&idx->written);
    av_freep(&idx->pending);
    av_freep(pidx);
}

static void mov_recovery_write_entry(AVIOContext *pb, const MOVIentry *entry)
{
    avio_wb64(pb, entry->pos);
    avio_wb64(pb, entry->dts);
    avio_wb64(pb, entry->pts);
    avio_wb32(pb, entry->size);
    avio_wb32(pb, entry->stsd_index);
    avio_wb32(pb, entry->samples_in_chunk);
    avio_wb32(pb, entry->chunkNum);
    avio_wb32(pb, entry->entries);
    avio_wb32(pb, entry->cts);
    avio_wb32(pb, entry->flags);
    avio_wb64(pb, entry->prft.wallclock);
    avio_wb32(pb, entry->prft.flags);
}

static void mov_recovery_fill_entry(MOVIentry *dst,
                                    const MovCheckpointEntry *src)
{
    dst->pos              = src->pos;
    dst->dts              = src->dts;
    dst->pts              = src->pts;
    dst->size             = src->size;
    dst->stsd_index       = src->stsd_index;
    dst->samples_in_chunk = src->samples_in_chunk;
    dst->chunkNum         = src->chunk_num;
    dst->entries          = src->entries;
    dst->cts              = src->cts;
    dst->flags            = src->flags;
    dst->prft.wallclock   = src->prft_wallclock;
    dst->prft.flags       = src->prft_flags;
}

static int mov_recovery_supported(const AVFormatContext *fc)
{
    if (!fc || !fc->oformat)
        return 0;

    return av_match_name(fc->oformat->name,
                         "mov,mp4,m4a,3gp,3g2,mj2,ism,ismv");
}

static int mov_recovery_collect_track(AVIOContext *pb, const MOVTrack *track,
                                      int stream_index)
{
    avio_wb32(pb, MOV_RECOVERY_VERSION);
    avio_wb32(pb, stream_index);

    avio_wb32(pb, track->mode);
    avio_wb32(pb, track->flags);
    avio_wb32(pb, track->timecode_flags);
    avio_wb32(pb, track->has_keyframes);
    avio_wb32(pb, track->has_disposable);
    avio_wb32(pb, track->last_sample_is_subtitle_end);
    avio_wb32(pb, track->end_reliable);
    avio_wb32(pb, track->frag_discont);

    avio_wb32(pb, track->default_sample_flags);
    avio_wb32(pb, track->default_size);

    avio_wb32(pb, track->timescale);

    avio_wb64(pb, track->entry);
    avio_wb64(pb, track->entry_written);
    avio_wb64(pb, track->entries_flushed);
    avio_wb64(pb, track->sample_count);
    avio_wb64(pb, track->sample_size);
    avio_wb64(pb, track->chunkCount);
    avio_wb64(pb, track->time);
    avio_wb64(pb, track->track_duration);
    avio_wb64(pb, track->start_dts);
    avio_wb64(pb, track->start_cts);
    avio_wb64(pb, track->end_pts);
    avio_wb64(pb, track->dts_shift);
    avio_wb64(pb, track->default_duration);
    avio_wb64(pb, track->data_offset);

    avio_wb32(pb, track->entry_written);
    for (uint64_t i = 0; i < track->entry_written; i++)
        mov_recovery_write_entry(pb, &track->cluster_written[i]);

    avio_wb32(pb, track->entry);
    for (uint64_t i = 0; i < track->entry; i++)
        mov_recovery_write_entry(pb, &track->cluster[i]);

    return 0;
}

static int mov_recovery_collect(OutputFile *of, AVBPrint *bp)
{
    Muxer *mux = mux_from_of(of);
    AVFormatContext *fc = mux ? mux->fc : NULL;
    MOVMuxContext *mov;
    AVIOContext *dyn = NULL;
    uint8_t *payload = NULL;
    char *encoded = NULL;
    int size;
    int ret;

    if (!fc || !mov_recovery_supported(fc))
        return 0;

    mov = fc->priv_data;
    if (!mov)
        return 0;

    av_bprintf(bp, "MOVF %d %"PRIu64"\n", MOV_RECOVERY_VERSION, mov->mdat_size);

    for (int i = 0; i < mov->nb_streams && i < of->nb_streams; i++) {
        MOVTrack *track = &mov->tracks[i];

        ret = avio_open_dyn_buf(&dyn);
        if (ret < 0)
            goto fail;

        ret = mov_recovery_collect_track(dyn, track, i);
        if (ret < 0)
            goto fail;

        size = avio_close_dyn_buf(dyn, &payload);
        dyn = NULL;
        if (size < 0) {
            ret = size;
            goto fail;
        }

        encoded = av_malloc(AV_BASE64_SIZE(size));
        if (!encoded) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        av_base64_encode(encoded, AV_BASE64_SIZE(size), payload, size);
        av_bprintf(bp, "MOVT %d %s\n", i, encoded);

        av_freep(&payload);
        av_freep(&encoded);
    }

    ret = 0;
fail:
    if (dyn)
        avio_close_dyn_buf(dyn, &payload);
    av_freep(&payload);
    av_freep(&encoded);
    return ret;
}

static int mov_checkpoint_publish(OutputFile *of, int64_t append_pos)
{
    Muxer *mux = mux_from_of(of);
    AVFormatContext *fc = mux ? mux->fc : NULL;
    int ret;
    int64_t seek_ret;

    if (!fc || !fc->pb || (fc->oformat->flags & AVFMT_NOFILE))
        return 0;

    ret = ff_mov_checkpoint_trailer(fc);
    if (ret < 0) {
        av_log(of, AV_LOG_WARNING,
               "Failed to finalize MOV recovery checkpoint for %s: %s\n",
               fc->url ? fc->url : "output", av_err2str(ret));
        return ret;
    }

    avio_flush(fc->pb);

    seek_ret = avio_seek(fc->pb, append_pos, SEEK_SET);
    if (seek_ret < 0) {
        av_log(of, AV_LOG_WARNING,
               "Failed to restore MOV write position for %s: %s\n",
               fc->url ? fc->url : "output", av_err2str((int)seek_ret));
        return (int)seek_ret;
    }

    return 0;
}

static int mov_recovery_read_entry(MovCheckpointEntry *entry,
                                   const uint8_t **pp, const uint8_t *end)
{
    const uint8_t *p = *pp;

    if (end - p < 8 * 3 + 4 * 7 + 8 + 4)
        return AVERROR_INVALIDDATA;

    entry->pos              = AV_RB64(p); p += 8;
    entry->dts              = AV_RB64(p); p += 8;
    entry->pts              = AV_RB64(p); p += 8;
    entry->size             = AV_RB32(p); p += 4;
    entry->stsd_index       = AV_RB32(p); p += 4;
    entry->samples_in_chunk = AV_RB32(p); p += 4;
    entry->chunk_num        = AV_RB32(p); p += 4;
    entry->entries          = AV_RB32(p); p += 4;
    entry->cts              = AV_RB32(p); p += 4;
    entry->flags            = AV_RB32(p); p += 4;
    entry->prft_wallclock   = AV_RB64(p); p += 8;
    entry->prft_flags       = AV_RB32(p); p += 4;

    *pp = p;
    return 0;
}

static int mov_recovery_parse_track(OutputFile *of, int stream_index,
                                     const uint8_t *data, const uint8_t *end)
{
    struct MovRecoveryIndex *idx = NULL;
    OutputStream *ost;
    int ret;
    uint32_t version;

    if (stream_index < 0 || stream_index >= of->nb_streams)
        return AVERROR_INVALIDDATA;

    ost = of->streams[stream_index];
    if (!ost)
        return AVERROR_INVALIDDATA;

    if (end - data < 4)
        return AVERROR_INVALIDDATA;

    version = AV_RB32(data); data += 4;
    if (version != MOV_RECOVERY_VERSION)
        return AVERROR_INVALIDDATA;

    if (end - data < 4)
        return AVERROR_INVALIDDATA;

    if ((int)AV_RB32(data) != stream_index)
        return AVERROR_INVALIDDATA;
    data += 4;

    idx = av_mallocz(sizeof(*idx));
    if (!idx)
        return AVERROR(ENOMEM);

    idx->version = version;
    idx->stream_index = stream_index;

#define READ32(field) do { \
        if (end - data < 4) { ret = AVERROR_INVALIDDATA; goto fail; } \
        field = AV_RB32(data); \
        data += 4; \
    } while (0)
#define READ64(field) do { \
        if (end - data < 8) { ret = AVERROR_INVALIDDATA; goto fail; } \
        field = AV_RB64(data); \
        data += 8; \
    } while (0)

    READ32(idx->mode);
    READ32(idx->flags);
    READ32(idx->timecode_flags);
    READ32(idx->has_keyframes);
    READ32(idx->has_disposable);
    READ32(idx->last_sample_is_subtitle_end);
    READ32(idx->end_reliable);
    READ32(idx->frag_discont);

    READ32(idx->default_sample_flags);
    READ32(idx->default_size);

    READ32(idx->timescale);

    READ64(idx->entry);
    READ64(idx->entry_written);
    READ64(idx->entries_flushed);
    READ64(idx->sample_count);
    READ64(idx->sample_size);
    READ64(idx->chunk_count);
    READ64(idx->time);
    READ64(idx->track_duration);
    READ64(idx->start_dts);
    READ64(idx->start_cts);
    READ64(idx->end_pts);
    READ64(idx->dts_shift);
    READ64(idx->default_duration);
    READ64(idx->data_offset);

    READ32(idx->nb_written);
    if (idx->nb_written) {
        idx->written = av_malloc_array(idx->nb_written, sizeof(*idx->written));
        if (!idx->written) { ret = AVERROR(ENOMEM); goto fail; }
        for (unsigned i = 0; i < idx->nb_written; i++) {
            ret = mov_recovery_read_entry(&idx->written[i], &data, end);
            if (ret < 0)
                goto fail;
        }
    }

    READ32(idx->nb_pending);
    if (idx->nb_pending) {
        idx->pending = av_malloc_array(idx->nb_pending, sizeof(*idx->pending));
        if (!idx->pending) { ret = AVERROR(ENOMEM); goto fail; }
        for (unsigned i = 0; i < idx->nb_pending; i++) {
            ret = mov_recovery_read_entry(&idx->pending[i], &data, end);
            if (ret < 0)
                goto fail;
        }
    }

    if (data != end) {
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }

#undef READ32
#undef READ64

    ffmpeg_mux_recovery_ost_reset(ost);
    ost->recovery.mov = idx;

    return 0;

fail:
    mov_recovery_index_free(&idx);
    return ret;
}

static int mov_recovery_parse(OutputFile *of, const char *line)
{
    const char *arg;

    if (av_strstart(line, "MOVF ", &arg)) {
        int version;
        uint64_t mdat_size;
        if (sscanf(arg, "%d %"SCNu64, &version, &mdat_size) != 2)
            return AVERROR_INVALIDDATA;
        if (version != MOV_RECOVERY_VERSION)
            return AVERROR_INVALIDDATA;
        of->recovery.have_mov_mdat_size = 1;
        of->recovery.mov_mdat_size = mdat_size;
        return 1;
    }

    if (av_strstart(line, "MOVT ", &arg)) {
        int stream_index;
        const char *payload;
        uint8_t *decoded = NULL;
        int decoded_size;
        int consumed;
        int ret;
        int payload_len;
        int max_decoded;

        if (sscanf(arg, "%d %n", &stream_index, &consumed) != 1)
            return AVERROR_INVALIDDATA;

        payload = arg + consumed;
        while (*payload == ' ')
            payload++;

        payload_len = strlen(payload);
        max_decoded = payload_len ? (payload_len / 4) * 3 + 3 : 1;
        decoded = av_malloc(max_decoded);
        if (!decoded)
            return AVERROR(ENOMEM);

        decoded_size = av_base64_decode(decoded, payload, max_decoded);
        if (decoded_size < 0) {
            av_free(decoded);
            return AVERROR_INVALIDDATA;
        }

        ret = mov_recovery_parse_track(of, stream_index, decoded,
                                       decoded + decoded_size);
        av_free(decoded);
        if (ret < 0)
            return ret;

        return 1;
    }

    return 0;
}

static int mov_recovery_apply(OutputFile *of)
{
    Muxer *mux = mux_from_of(of);
    AVFormatContext *fc = mux ? mux->fc : NULL;
    MOVMuxContext *mov;

    if (!fc || !mov_recovery_supported(fc))
        return 0;

    mov = fc->priv_data;
    if (!mov)
        return 0;

    if (of->recovery.have_mov_mdat_size)
        mov->mdat_size = of->recovery.mov_mdat_size;

    for (int i = 0; i < of->nb_streams && i < mov->nb_streams; i++) {
        OutputStream *ost = of->streams[i];
        MOVTrack *track = &mov->tracks[i];
        struct MovRecoveryIndex *idx = ost ? ost->recovery.mov : NULL;
        MOVIentry *cluster = NULL;
        MOVIentry *cluster_written = NULL;

        if (!idx)
            continue;

        if (idx->nb_pending) {
            cluster = av_malloc_array(idx->nb_pending, sizeof(*cluster));
            if (!cluster)
                return AVERROR(ENOMEM);
            for (unsigned j = 0; j < idx->nb_pending; j++)
                mov_recovery_fill_entry(&cluster[j], &idx->pending[j]);
        }

        if (idx->nb_written) {
            cluster_written = av_malloc_array(idx->nb_written,
                                              sizeof(*cluster_written));
            if (!cluster_written) {
                av_freep(&cluster);
                return AVERROR(ENOMEM);
            }
            for (unsigned j = 0; j < idx->nb_written; j++)
                mov_recovery_fill_entry(&cluster_written[j], &idx->written[j]);
        }

        av_freep(&track->cluster);
        av_freep(&track->cluster_written);

        track->cluster           = cluster;
        track->cluster_written   = cluster_written;
        track->cluster_capacity  = idx->nb_pending;
        track->entry             = idx->nb_pending;
        track->entry_written     = idx->nb_written;
        track->entries_flushed   = idx->entries_flushed;
        track->sample_count      = idx->sample_count;
        track->sample_size       = idx->sample_size;
        track->chunkCount        = idx->chunk_count;
        track->time              = idx->time;
        track->track_duration    = idx->track_duration;
        track->start_dts         = idx->start_dts;
        track->start_cts         = idx->start_cts;
        track->end_pts           = idx->end_pts;
        track->dts_shift         = idx->dts_shift;
        track->default_duration  = idx->default_duration;
        track->data_offset       = idx->data_offset;

        track->mode                    = idx->mode;
        track->flags                   = idx->flags;
        track->timecode_flags          = idx->timecode_flags;
        track->has_keyframes           = idx->has_keyframes;
        track->has_disposable          = idx->has_disposable;
        track->last_sample_is_subtitle_end = idx->last_sample_is_subtitle_end;
        track->end_reliable            = idx->end_reliable;
        track->frag_discont            = idx->frag_discont;
        track->default_sample_flags    = idx->default_sample_flags;
        track->default_size            = idx->default_size;
        if (idx->timescale)
            track->timescale = idx->timescale;

        mov_recovery_index_free(&ost->recovery.mov);
    }

    return 0;
}

#endif /* MOV muxers */

int ffmpeg_mux_recovery_parse(OutputFile *of, const char *line)
{
#if ENABLE_MOV_RECOVERY
    int ret = mov_recovery_parse(of, line);
    if (ret != 0)
        return ret;
#endif
    return 0;
}

int ffmpeg_mux_recovery_serialize(OutputFile *of, AVBPrint *bp)
{
#if ENABLE_MOV_RECOVERY
    int ret = mov_recovery_collect(of, bp);
    if (ret < 0)
        return ret;
#endif
    return 0;
}

int ffmpeg_mux_recovery_apply(OutputFile *of)
{
#if ENABLE_MOV_RECOVERY
    int ret = mov_recovery_apply(of);
    if (ret < 0)
        return ret;
#endif
    return 0;
}

void ffmpeg_mux_recovery_ost_reset(OutputStream *ost)
{
#if ENABLE_MOV_RECOVERY
    mov_recovery_index_free(&ost->recovery.mov);
#else
    ost->recovery.mov = NULL;
#endif
}

int ffmpeg_mux_checkpoint_flush(OutputFile *of)
{
    Muxer *mux = mux_from_of(of);
    AVFormatContext *fc;

    if (!mux)
        return 0;

    fc = mux->fc;
    if (!fc || !fc->pb || (fc->oformat->flags & AVFMT_NOFILE))
        return 0;

    avio_flush(fc->pb);
    atomic_store(&mux->last_filesize, filesize(fc->pb));

    return 0;
}

int ffmpeg_mux_checkpoint_publish(OutputFile *of, int64_t append_pos)
{
    Muxer *mux = mux_from_of(of);
    AVFormatContext *fc = mux ? mux->fc : NULL;

    if (!mux || !fc || append_pos < 0)
        return 0;

#if ENABLE_MOV_RECOVERY
    if (mov_recovery_supported(fc))
        return mov_checkpoint_publish(of, append_pos);
#endif

    return 0;
}

static void mux_log_debug_ts(OutputStream *ost, const AVPacket *pkt)
{
    static const char *desc[] = {
        [LATENCY_PROBE_DEMUX]       = "demux",
        [LATENCY_PROBE_DEC_PRE]     = "decode",
        [LATENCY_PROBE_DEC_POST]    = "decode",
        [LATENCY_PROBE_FILTER_PRE]  = "filter",
        [LATENCY_PROBE_FILTER_POST] = "filter",
        [LATENCY_PROBE_ENC_PRE]     = "encode",
        [LATENCY_PROBE_ENC_POST]    = "encode",
        [LATENCY_PROBE_NB]          = "mux",
    };

    char latency[512];

    *latency = 0;
    if (pkt->opaque_ref) {
        const FrameData *fd = (FrameData*)pkt->opaque_ref->data;
        int64_t         now = av_gettime_relative();
        int64_t       total = INT64_MIN;

        int next;

        for (unsigned i = 0; i < FF_ARRAY_ELEMS(fd->wallclock); i = next) {
            int64_t val = fd->wallclock[i];

            next = i + 1;

            if (val == INT64_MIN)
                continue;

            if (total == INT64_MIN) {
                total = now - val;
                snprintf(latency, sizeof(latency), "total:%gms", total / 1e3);
            }

            // find the next valid entry
            for (; next <= FF_ARRAY_ELEMS(fd->wallclock); next++) {
                int64_t val_next = (next == FF_ARRAY_ELEMS(fd->wallclock)) ?
                                   now : fd->wallclock[next];
                int64_t diff;

                if (val_next == INT64_MIN)
                    continue;
                diff = val_next - val;

                // print those stages that take at least 5% of total
                if (100. * diff > 5. * total) {
                    av_strlcat(latency, ", ", sizeof(latency));

                    if (!strcmp(desc[i], desc[next]))
                        av_strlcat(latency, desc[i], sizeof(latency));
                    else
                        av_strlcatf(latency, sizeof(latency), "%s-%s:",
                                    desc[i], desc[next]);

                    av_strlcatf(latency, sizeof(latency), " %gms/%d%%",
                                diff / 1e3, (int)(100. * diff / total));
                }

                break;
            }

        }
    }

    av_log(ost, AV_LOG_INFO, "muxer <- pts:%s pts_time:%s dts:%s dts_time:%s "
           "duration:%s duration_time:%s size:%d latency(%s)\n",
           av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, &ost->st->time_base),
           av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, &ost->st->time_base),
           av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, &ost->st->time_base),
           pkt->size, *latency ? latency : "N/A");
}

static int mux_fixup_ts(Muxer *mux, MuxStream *ms, AVPacket *pkt)
{
    OutputStream *ost = &ms->ost;

#if FFMPEG_OPT_VSYNC_DROP
    if (ost->type == AVMEDIA_TYPE_VIDEO && ms->ts_drop)
        pkt->pts = pkt->dts = AV_NOPTS_VALUE;
#endif

    // rescale timestamps to the stream timebase
    if (ost->type == AVMEDIA_TYPE_AUDIO && !ost->enc) {
        // use av_rescale_delta() for streamcopying audio, to preserve
        // accuracy with coarse input timebases
        int duration = av_get_audio_frame_duration2(ost->st->codecpar, pkt->size);

        if (!duration)
            duration = ost->st->codecpar->frame_size;

        pkt->dts = av_rescale_delta(pkt->time_base, pkt->dts,
                                    (AVRational){1, ost->st->codecpar->sample_rate}, duration,
                                    &ms->ts_rescale_delta_last, ost->st->time_base);
        pkt->pts = pkt->dts;

        pkt->duration = av_rescale_q(pkt->duration, pkt->time_base, ost->st->time_base);
    } else
        av_packet_rescale_ts(pkt, pkt->time_base, ost->st->time_base);
    pkt->time_base = ost->st->time_base;

    if (!(mux->fc->oformat->flags & AVFMT_NOTIMESTAMPS)) {
        if (pkt->dts != AV_NOPTS_VALUE &&
            pkt->pts != AV_NOPTS_VALUE &&
            pkt->dts > pkt->pts) {
            av_log(ost, AV_LOG_WARNING, "Invalid DTS: %"PRId64" PTS: %"PRId64", replacing by guess\n",
                   pkt->dts, pkt->pts);
            pkt->pts =
            pkt->dts = pkt->pts + pkt->dts + ms->last_mux_dts + 1
                     - FFMIN3(pkt->pts, pkt->dts, ms->last_mux_dts + 1)
                     - FFMAX3(pkt->pts, pkt->dts, ms->last_mux_dts + 1);
        }
        if ((ost->type == AVMEDIA_TYPE_AUDIO || ost->type == AVMEDIA_TYPE_VIDEO || ost->type == AVMEDIA_TYPE_SUBTITLE) &&
            pkt->dts != AV_NOPTS_VALUE &&
            ms->last_mux_dts != AV_NOPTS_VALUE) {
            int64_t max = ms->last_mux_dts + !(mux->fc->oformat->flags & AVFMT_TS_NONSTRICT);
            if (pkt->dts < max) {
                int loglevel = max - pkt->dts > 2 || ost->type == AVMEDIA_TYPE_VIDEO ? AV_LOG_WARNING : AV_LOG_DEBUG;
                if (exit_on_error)
                    loglevel = AV_LOG_ERROR;
                av_log(ost, loglevel, "Non-monotonic DTS; "
                       "previous: %"PRId64", current: %"PRId64"; ",
                       ms->last_mux_dts, pkt->dts);
                if (exit_on_error) {
                    return AVERROR(EINVAL);
                }

                av_log(ost, loglevel, "changing to %"PRId64". This may result "
                       "in incorrect timestamps in the output file.\n",
                       max);
                if (pkt->pts >= pkt->dts)
                    pkt->pts = FFMAX(pkt->pts, max);
                pkt->dts = max;
            }
        }
    }
    ms->last_mux_dts = pkt->dts;

    if (debug_ts)
        mux_log_debug_ts(ost, pkt);

    return 0;
}

static int write_packet(Muxer *mux, OutputStream *ost, AVPacket *pkt)
{
    MuxStream *ms = ms_from_ost(ost);
    AVFormatContext *s = mux->fc;
    int64_t fs;
    uint64_t frame_num;
    int ret;

    fs = filesize(s->pb);
    atomic_store(&mux->last_filesize, fs);
    if (fs >= mux->limit_filesize) {
        ret = AVERROR_EOF;
        goto fail;
    }

    ret = mux_fixup_ts(mux, ms, pkt);
    if (ret < 0)
        goto fail;

    ms->data_size_mux += pkt->size;
    frame_num = atomic_fetch_add(&ost->packets_written, 1);

    pkt->stream_index = ost->index;

    if (ms->stats.io)
        enc_stats_write(ost, &ms->stats, NULL, pkt, frame_num);

    ret = av_interleaved_write_frame(s, pkt);
    if (ret < 0) {
        av_log(ost, AV_LOG_ERROR,
               "Error submitting a packet to the muxer: %s\n",
               av_err2str(ret));
        goto fail;
    }

    return 0;
fail:
    av_packet_unref(pkt);
    return ret;
}

static int sync_queue_process(Muxer *mux, MuxStream *ms, AVPacket *pkt, int *stream_eof)
{
    OutputFile *of = &mux->of;

    if (ms->sq_idx_mux >= 0) {
        int ret = sq_send(mux->sq_mux, ms->sq_idx_mux, SQPKT(pkt));
        if (ret < 0) {
            if (ret == AVERROR_EOF)
                *stream_eof = 1;

            return ret;
        }

        while (1) {
            ret = sq_receive(mux->sq_mux, -1, SQPKT(mux->sq_pkt));
            if (ret < 0) {
                /* n.b.: We forward EOF from the sync queue, terminating muxing.
                 * This assumes that if a muxing sync queue is present, then all
                 * the streams use it. That is true currently, but may change in
                 * the future, then this code needs to be revisited.
                 */
                return ret == AVERROR(EAGAIN) ? 0 : ret;
            }

            ret = write_packet(mux, of->streams[ret],
                               mux->sq_pkt);
            if (ret < 0)
                return ret;
        }
    } else if (pkt)
        return write_packet(mux, &ms->ost, pkt);

    return 0;
}

static int of_streamcopy(OutputFile *of, OutputStream *ost, AVPacket *pkt);

/* apply the output bitstream filters */
static int mux_packet_filter(Muxer *mux, MuxThreadContext *mt,
                             OutputStream *ost, AVPacket *pkt, int *stream_eof)
{
    MuxStream *ms = ms_from_ost(ost);
    const char *err_msg;
    int ret;

    if (pkt && !ost->enc) {
        ret = of_streamcopy(&mux->of, ost, pkt);
        if (ret == AVERROR(EAGAIN))
            return 0;
        else if (ret == AVERROR_EOF) {
            av_packet_unref(pkt);
            pkt = NULL;
            *stream_eof = 1;
        } else if (ret < 0)
            goto fail;
    }

    // emit heartbeat for -fix_sub_duration;
    // we are only interested in heartbeats on on random access points.
    if (pkt && (pkt->flags & AV_PKT_FLAG_KEY)) {
        mt->fix_sub_duration_pkt->opaque    = (void*)(intptr_t)PKT_OPAQUE_FIX_SUB_DURATION;
        mt->fix_sub_duration_pkt->pts       = pkt->pts;
        mt->fix_sub_duration_pkt->time_base = pkt->time_base;

        ret = sch_mux_sub_heartbeat(mux->sch, mux->sch_idx, ms->sch_idx,
                                    mt->fix_sub_duration_pkt);
        if (ret < 0)
            goto fail;
    }

    if (ms->bsf_ctx) {
        int bsf_eof = 0;

        if (pkt)
            av_packet_rescale_ts(pkt, pkt->time_base, ms->bsf_ctx->time_base_in);

        ret = av_bsf_send_packet(ms->bsf_ctx, pkt);
        if (ret < 0) {
            err_msg = "submitting a packet for bitstream filtering";
            goto fail;
        }

        while (!bsf_eof) {
            ret = av_bsf_receive_packet(ms->bsf_ctx, ms->bsf_pkt);
            if (ret == AVERROR(EAGAIN))
                return 0;
            else if (ret == AVERROR_EOF)
                bsf_eof = 1;
            else if (ret < 0) {
                av_log(ost, AV_LOG_ERROR,
                       "Error applying bitstream filters to a packet: %s",
                       av_err2str(ret));
                if (exit_on_error)
                    return ret;
                continue;
            }

            if (!bsf_eof)
                ms->bsf_pkt->time_base = ms->bsf_ctx->time_base_out;

            ret = sync_queue_process(mux, ms, bsf_eof ? NULL : ms->bsf_pkt, stream_eof);
            if (ret < 0)
                goto mux_fail;
        }
        *stream_eof = 1;
    } else {
        ret = sync_queue_process(mux, ms, pkt, stream_eof);
        if (ret < 0)
            goto mux_fail;
    }

    return *stream_eof ? AVERROR_EOF : 0;

mux_fail:
    err_msg = "submitting a packet to the muxer";

fail:
    if (ret != AVERROR_EOF)
        av_log(ost, AV_LOG_ERROR, "Error %s: %s\n", err_msg, av_err2str(ret));
    return ret;
}

static void thread_set_name(Muxer *mux)
{
    char name[16];
    snprintf(name, sizeof(name), "mux%d:%s",
             mux->of.index, mux->fc->oformat->name);
    ff_thread_setname(name);
}

static void mux_thread_uninit(MuxThreadContext *mt)
{
    av_packet_free(&mt->pkt);
    av_packet_free(&mt->fix_sub_duration_pkt);

    memset(mt, 0, sizeof(*mt));
}

static int mux_thread_init(MuxThreadContext *mt)
{
    memset(mt, 0, sizeof(*mt));

    mt->pkt = av_packet_alloc();
    if (!mt->pkt)
        goto fail;

    mt->fix_sub_duration_pkt = av_packet_alloc();
    if (!mt->fix_sub_duration_pkt)
        goto fail;

    return 0;

fail:
    mux_thread_uninit(mt);
    return AVERROR(ENOMEM);
}

int muxer_thread(void *arg)
{
    Muxer     *mux = arg;
    OutputFile *of = &mux->of;

    MuxThreadContext mt;

    int        ret = 0;

    ret = mux_thread_init(&mt);
    if (ret < 0)
        goto finish;

    thread_set_name(mux);

    while (1) {
        OutputStream *ost;
        int stream_idx, stream_eof = 0;

        ret = sch_mux_receive(mux->sch, of->index, mt.pkt);
        stream_idx = mt.pkt->stream_index;
        if (stream_idx < 0) {
            av_log(mux, AV_LOG_VERBOSE, "All streams finished\n");
            ret = 0;
            break;
        }

        ost = of->streams[mux->sch_stream_idx[stream_idx]];
        mt.pkt->stream_index = ost->index;
        mt.pkt->flags       &= ~AV_PKT_FLAG_TRUSTED;

        ret = mux_packet_filter(mux, &mt, ost, ret < 0 ? NULL : mt.pkt, &stream_eof);
        av_packet_unref(mt.pkt);
        if (ret == AVERROR_EOF) {
            if (stream_eof) {
                sch_mux_receive_finish(mux->sch, of->index, stream_idx);
            } else {
                av_log(mux, AV_LOG_VERBOSE, "Muxer returned EOF\n");
                ret = 0;
                break;
            }
        } else if (ret < 0) {
            av_log(mux, AV_LOG_ERROR, "Error muxing a packet\n");
            break;
        }
    }

finish:
    mux_thread_uninit(&mt);

    return ret;
}

static int of_streamcopy(OutputFile *of, OutputStream *ost, AVPacket *pkt)
{
    MuxStream  *ms = ms_from_ost(ost);
    FrameData  *fd = pkt->opaque_ref ? (FrameData*)pkt->opaque_ref->data : NULL;
    int64_t      dts = fd ? fd->dts_est : AV_NOPTS_VALUE;
    int64_t start_time = (of->start_time == AV_NOPTS_VALUE) ? 0 : of->start_time;
    int64_t ts_offset;

    if (of->recording_time != INT64_MAX &&
        dts >= of->recording_time + start_time)
        return AVERROR_EOF;

    if (!ms->streamcopy_started && !(pkt->flags & AV_PKT_FLAG_KEY) &&
        !ms->copy_initial_nonkeyframes)
        return AVERROR(EAGAIN);

    if (!ms->streamcopy_started) {
        if (!ms->copy_prior_start &&
            (pkt->pts == AV_NOPTS_VALUE ?
             dts < ms->ts_copy_start :
             pkt->pts < av_rescale_q(ms->ts_copy_start, AV_TIME_BASE_Q, pkt->time_base)))
            return AVERROR(EAGAIN);

        if (of->start_time != AV_NOPTS_VALUE && dts < of->start_time)
            return AVERROR(EAGAIN);
    }

    ts_offset = av_rescale_q(start_time, AV_TIME_BASE_Q, pkt->time_base);

    if (pkt->pts != AV_NOPTS_VALUE)
        pkt->pts -= ts_offset;

    if (pkt->dts == AV_NOPTS_VALUE) {
        pkt->dts = av_rescale_q(dts, AV_TIME_BASE_Q, pkt->time_base);
    } else if (ost->st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        pkt->pts = pkt->dts - ts_offset;
    }

    pkt->dts -= ts_offset;

    ms->streamcopy_started = 1;

    return 0;
}

int print_sdp(const char *filename);

int print_sdp(const char *filename)
{
    char sdp[16384];
    int j = 0, ret;
    AVIOContext *sdp_pb;
    AVFormatContext **avc;

    avc = av_malloc_array(nb_output_files, sizeof(*avc));
    if (!avc)
        return AVERROR(ENOMEM);
    for (int i = 0; i < nb_output_files; i++) {
        Muxer *mux = mux_from_of(output_files[i]);

        if (!strcmp(mux->fc->oformat->name, "rtp")) {
            avc[j] = mux->fc;
            j++;
        }
    }

    if (!j) {
        av_log(NULL, AV_LOG_ERROR, "No output streams in the SDP.\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }

    ret = av_sdp_create(avc, j, sdp, sizeof(sdp));
    if (ret < 0)
        goto fail;

    if (!filename) {
        printf("SDP:\n%s\n", sdp);
        fflush(stdout);
    } else {
        ret = avio_open2(&sdp_pb, filename, AVIO_FLAG_WRITE, &int_cb, NULL);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to open sdp file '%s'\n", filename);
            goto fail;
        }

        avio_print(sdp_pb, sdp);
        avio_closep(&sdp_pb);
    }

fail:
    av_freep(&avc);
    return ret;
}

int mux_check_init(void *arg)
{
    Muxer     *mux = arg;
    OutputFile *of = &mux->of;
    AVFormatContext *fc = mux->fc;
    int ret;

    ret = avformat_write_header(fc, &mux->opts);
    if (ret < 0) {
        av_log(mux, AV_LOG_ERROR, "Could not write header (incorrect codec "
               "parameters ?): %s\n", av_err2str(ret));
        return ret;
    }
    //assert_avoptions(of->opts);
    mux->header_written = 1;

    if (of->recovery.append && fc->pb && !(fc->oformat->flags & AVFMT_NOFILE)) {
        int64_t target = of->recovery.file_size;
        int trunc_ret;
        int64_t seek_ret;

        avio_flush(fc->pb);
        seek_ret = avio_seek(fc->pb, target, SEEK_SET);
        if (seek_ret < 0) {
            av_log(mux, AV_LOG_ERROR,
                   "Seeking to recovery offset failed for %s: %s\n",
                   fc->url, av_err2str((int)seek_ret));
            return (int)seek_ret;
        }

        trunc_ret = AVERROR(ENOSYS);
        if (fc->url && fc->url[0])
            trunc_ret = ffmpeg_truncate_output_tail(fc->url, target);

        if (trunc_ret < 0 && trunc_ret != AVERROR(ENOSYS)) {
            av_log(mux, AV_LOG_WARNING,
                   "Unable to truncate %s to %"PRId64" bytes after header rewrite: %s\n",
                   fc->url, target, av_err2str(trunc_ret));
        }
    }

    if (of->recovery.append) {
        ret = ffmpeg_mux_recovery_apply(of);
        if (ret < 0)
            return ret;
    }

    av_dump_format(fc, of->index, fc->url, 1);
    atomic_fetch_add(&nb_output_dumped, 1);

    return 0;
}

static int bsf_init(MuxStream *ms)
{
    OutputStream *ost = &ms->ost;
    AVBSFContext *ctx = ms->bsf_ctx;
    int ret;

    if (!ctx)
        return avcodec_parameters_copy(ost->st->codecpar, ms->par_in);

    ret = avcodec_parameters_copy(ctx->par_in, ms->par_in);
    if (ret < 0)
        return ret;

    ctx->time_base_in = ost->st->time_base;

    ret = av_bsf_init(ctx);
    if (ret < 0) {
        av_log(ms, AV_LOG_ERROR, "Error initializing bitstream filter: %s\n",
               ctx->filter->name);
        return ret;
    }

    ret = avcodec_parameters_copy(ost->st->codecpar, ctx->par_out);
    if (ret < 0)
        return ret;
    ost->st->time_base = ctx->time_base_out;

    ms->bsf_pkt = av_packet_alloc();
    if (!ms->bsf_pkt)
        return AVERROR(ENOMEM);

    return 0;
}

int of_stream_init(OutputFile *of, OutputStream *ost,
                   const AVCodecContext *enc_ctx)
{
    Muxer *mux = mux_from_of(of);
    MuxStream *ms = ms_from_ost(ost);
    int ret;

    if (enc_ctx) {
        // use upstream time base unless it has been overridden previously
        if (ost->st->time_base.num <= 0 || ost->st->time_base.den <= 0)
            ost->st->time_base = av_add_q(enc_ctx->time_base, (AVRational){0, 1});

        ost->st->avg_frame_rate = enc_ctx->framerate;
        ost->st->sample_aspect_ratio = enc_ctx->sample_aspect_ratio;

        ret = avcodec_parameters_from_context(ms->par_in, enc_ctx);
        if (ret < 0) {
            av_log(ost, AV_LOG_FATAL,
                   "Error initializing the output stream codec parameters.\n");
            return ret;
        }
    }

    /* initialize bitstream filters for the output stream
     * needs to be done here, because the codec id for streamcopy is not
     * known until now */
    ret = bsf_init(ms);
    if (ret < 0)
        return ret;

    if (ms->stream_duration) {
        ost->st->duration = av_rescale_q(ms->stream_duration, ms->stream_duration_tb,
                                         ost->st->time_base);
    }

    if (ms->sch_idx >= 0)
        return sch_mux_stream_ready(mux->sch, of->index, ms->sch_idx);

    return 0;
}

static int check_written(OutputFile *of)
{
    int64_t total_packets_written = 0;
    int pass1_used = 1;
    int ret = 0;

    for (int i = 0; i < of->nb_streams; i++) {
        OutputStream *ost = of->streams[i];
        uint64_t packets_written = atomic_load(&ost->packets_written);

        total_packets_written += packets_written;

        if (ost->enc &&
            (ost->enc->enc_ctx->flags & (AV_CODEC_FLAG_PASS1 | AV_CODEC_FLAG_PASS2))
             != AV_CODEC_FLAG_PASS1)
            pass1_used = 0;

        if (!packets_written &&
            (abort_on_flags & ABORT_ON_FLAG_EMPTY_OUTPUT_STREAM)) {
            av_log(ost, AV_LOG_FATAL, "Empty output stream\n");
            ret = err_merge(ret, AVERROR(EINVAL));
        }
    }

    if (!total_packets_written) {
        int level = AV_LOG_WARNING;

        if (abort_on_flags & ABORT_ON_FLAG_EMPTY_OUTPUT) {
            ret = err_merge(ret, AVERROR(EINVAL));
            level = AV_LOG_FATAL;
        }

        av_log(of, level, "Output file is empty, nothing was encoded%s\n",
               pass1_used ? "" : "(check -ss / -t / -frames parameters if used)");
    }

    return ret;
}

static void mux_final_stats(Muxer *mux)
{
    OutputFile *of = &mux->of;
    uint64_t total_packets = 0, total_size = 0;
    uint64_t video_size = 0, audio_size = 0, subtitle_size = 0,
             extra_size = 0, other_size = 0;

    uint8_t overhead[16] = "unknown";
    int64_t file_size = of_filesize(of);

    av_log(of, AV_LOG_VERBOSE, "Output file #%d (%s):\n",
           of->index, of->url);

    for (int j = 0; j < of->nb_streams; j++) {
        OutputStream *ost = of->streams[j];
        MuxStream     *ms = ms_from_ost(ost);
        const AVCodecParameters *par = ost->st->codecpar;
        const  enum AVMediaType type = par->codec_type;
        const uint64_t s = ms->data_size_mux;

        switch (type) {
        case AVMEDIA_TYPE_VIDEO:    video_size    += s; break;
        case AVMEDIA_TYPE_AUDIO:    audio_size    += s; break;
        case AVMEDIA_TYPE_SUBTITLE: subtitle_size += s; break;
        default:                    other_size    += s; break;
        }

        extra_size    += par->extradata_size;
        total_size    += s;
        total_packets += atomic_load(&ost->packets_written);

        av_log(of, AV_LOG_VERBOSE, "  Output stream #%d:%d (%s): ",
               of->index, j, av_get_media_type_string(type));
        if (ost->enc) {
            av_log(of, AV_LOG_VERBOSE, "%"PRIu64" frames encoded",
                   ost->enc->frames_encoded);
            if (type == AVMEDIA_TYPE_AUDIO)
                av_log(of, AV_LOG_VERBOSE, " (%"PRIu64" samples)", ost->enc->samples_encoded);
            av_log(of, AV_LOG_VERBOSE, "; ");
        }

        av_log(of, AV_LOG_VERBOSE, "%"PRIu64" packets muxed (%"PRIu64" bytes); ",
               atomic_load(&ost->packets_written), s);

        av_log(of, AV_LOG_VERBOSE, "\n");
    }

    av_log(of, AV_LOG_VERBOSE, "  Total: %"PRIu64" packets (%"PRIu64" bytes) muxed\n",
           total_packets, total_size);

    if (total_size && file_size > 0 && file_size >= total_size) {
        snprintf(overhead, sizeof(overhead), "%f%%",
                 100.0 * (file_size - total_size) / total_size);
    }

    av_log(of, AV_LOG_INFO,
           "video:%1.0fKiB audio:%1.0fKiB subtitle:%1.0fKiB other streams:%1.0fKiB "
           "global headers:%1.0fKiB muxing overhead: %s\n",
           video_size    / 1024.0,
           audio_size    / 1024.0,
           subtitle_size / 1024.0,
           other_size    / 1024.0,
           extra_size    / 1024.0,
           overhead);
}

int of_write_trailer(OutputFile *of)
{
    Muxer *mux = mux_from_of(of);
    AVFormatContext *fc = mux->fc;
    int ret, mux_result = 0;

    if (!mux->header_written) {
        av_log(mux, AV_LOG_ERROR,
               "Nothing was written into output file, because "
               "at least one of its streams received no packets.\n");
        return AVERROR(EINVAL);
    }

    ret = av_write_trailer(fc);
    if (ret < 0) {
        av_log(mux, AV_LOG_ERROR, "Error writing trailer: %s\n", av_err2str(ret));
        mux_result = err_merge(mux_result, ret);
    }

    mux->last_filesize = filesize(fc->pb);

    if (!(fc->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_closep(&fc->pb);
        if (ret < 0) {
            av_log(mux, AV_LOG_ERROR, "Error closing file: %s\n", av_err2str(ret));
            mux_result = err_merge(mux_result, ret);
        }
    }

    mux_final_stats(mux);

    // check whether anything was actually written
    ret = check_written(of);
    mux_result = err_merge(mux_result, ret);

    return mux_result;
}

static void enc_stats_uninit(EncStats *es)
{
    for (int i = 0; i < es->nb_components; i++)
        av_freep(&es->components[i].str);
    av_freep(&es->components);

    if (es->lock_initialized)
        pthread_mutex_destroy(&es->lock);
    es->lock_initialized = 0;
}

static void ost_free(OutputStream **post)
{
    OutputStream *ost = *post;
    MuxStream *ms;

    if (!ost)
        return;
    ms = ms_from_ost(ost);

    enc_free(&ost->enc);
    fg_free(&ost->fg_simple);

    if (ost->logfile) {
        if (fclose(ost->logfile))
            av_log(ms, AV_LOG_ERROR,
                   "Error closing logfile, loss of information possible: %s\n",
                   av_err2str(AVERROR(errno)));
        ost->logfile = NULL;
    }

    avcodec_parameters_free(&ms->par_in);

    av_bsf_free(&ms->bsf_ctx);
    av_packet_free(&ms->bsf_pkt);

    av_packet_free(&ms->pkt);

    av_freep(&ost->kf.pts);
    av_expr_free(ost->kf.pexpr);

    av_freep(&ost->logfile_prefix);

    av_freep(&ost->attachment_filename);

    enc_stats_uninit(&ost->enc_stats_pre);
    enc_stats_uninit(&ost->enc_stats_post);
    enc_stats_uninit(&ms->stats);

    ffmpeg_mux_recovery_ost_reset(ost);

    av_freep(post);
}

static void fc_close(AVFormatContext **pfc)
{
    AVFormatContext *fc = *pfc;

    if (!fc)
        return;

    if (!(fc->oformat->flags & AVFMT_NOFILE))
        avio_closep(&fc->pb);
    avformat_free_context(fc);

    *pfc = NULL;
}

void of_free(OutputFile **pof)
{
    OutputFile *of = *pof;
    Muxer *mux;

    if (!of)
        return;
    mux = mux_from_of(of);

    sq_free(&mux->sq_mux);

    for (int i = 0; i < of->nb_streams; i++)
        ost_free(&of->streams[i]);
    av_freep(&of->streams);

    av_freep(&mux->sch_stream_idx);

    av_dict_free(&mux->opts);
    av_dict_free(&mux->enc_opts_used);

    av_packet_free(&mux->sq_pkt);

    fc_close(&mux->fc);

    av_freep(&of->recovery_path);

    av_freep(pof);
}

int64_t of_filesize(OutputFile *of)
{
    Muxer *mux = mux_from_of(of);
    return atomic_load(&mux->last_filesize);
}
