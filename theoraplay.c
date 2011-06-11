/**
 * TheoraPlay; multithreaded Ogg Theora/Ogg Vorbis decoding.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

// I wrote this with a lot of peeking at the Theora example code in
//  libtheora-1.1.1/examples/player_example.c, but this is all my own
//  code.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <assert.h>

#include "theoraplay.h"
#include "theora/theoradec.h"
#include "vorbis/codec.h"

typedef THEORAPLAY_YuvVideoItem YuvVideoItem;
typedef THEORAPLAY_PcmAudioItem PcmAudioItem;

typedef struct TheoraDecoder
{
    // Thread wrangling...
    int thread_created;
    pthread_mutex_t lock;
    volatile int halt;
    volatile unsigned int videocount;
    int thread_done;
    int decode_error;
    pthread_t worker;

    // Ogg, Vorbis, and Theora decoder state...
    int fd;

    // API state...
    unsigned int maxframes;  // Max video frames to buffer.

    YuvVideoItem *videolist;
    YuvVideoItem *videolisttail;

    PcmAudioItem *audiolist;
    PcmAudioItem *audiolisttail;
} TheoraDecoder;


static int FeedMoreOggData(const int fd, ogg_sync_state *sync)
{
    long buflen = 4096;
    char *buffer = ogg_sync_buffer(sync, buflen);
    if (buffer == NULL)
        return -1;

    while ( ((buflen = read(fd, buffer, buflen)) < 0) && (errno == EINTR) ) {}

    if (buflen <= 0)
        return 0;

    return (ogg_sync_wrote(sync, buflen) == 0) ? 1 : -1;
} // FeedMoreOggData


// This massive function is where all the effort happens.
static void WorkerThread(TheoraDecoder *ctx)
{
    // make sure we initialized the stream before using pagein, but the stream
    //  will know to ignore pages that aren't meant for it, so pass to both.
    #define queue_ogg_page(ctx) do { \
        if (tpackets) ogg_stream_pagein(&tstream, &page); \
        if (vpackets) ogg_stream_pagein(&vstream, &page); \
    } while (0)

    unsigned long audioframes = 0;
    unsigned long videoframes = 0;
    double fps = 0.0;
    int was_error = 1;  // resets to 0 at the end.
    int eos = 0;  // end of stream flag.

    // Too much Ogg/Vorbis/Theora state...
    ogg_packet packet;
    ogg_sync_state sync;
    ogg_page page;
    int vpackets = 0;
    vorbis_info vinfo;
    vorbis_comment vcomment;
    ogg_stream_state vstream;
    int vdsp_init = 0;
    vorbis_dsp_state vdsp;
    int tpackets = 0;
    th_info tinfo;
    th_comment tcomment;
    ogg_stream_state tstream;
    int vblock_init = 0;
    vorbis_block vblock;
    th_dec_ctx *tdec = NULL;
    th_setup_info *tsetup = NULL;

    ogg_sync_init(&sync);
    vorbis_info_init(&vinfo);
    vorbis_comment_init(&vcomment);
    th_comment_init(&tcomment);
    th_info_init(&tinfo);

    int bos = 1;
    while (!ctx->halt && bos)
    {
        if (FeedMoreOggData(ctx->fd, &sync) <= 0)
            goto cleanup;

        // parse out the initial header.
        while ( (!ctx->halt) && (ogg_sync_pageout(&sync, &page) > 0) )
        {
            ogg_stream_state test;

            if (!ogg_page_bos(&page))  // not a header.
            {
                queue_ogg_page(ctx);
                bos = 0;
                break;
            } // if

            ogg_stream_init(&test, ogg_page_serialno(&page));
            ogg_stream_pagein(&test, &page);
            ogg_stream_packetout(&test, &packet);

            if (!tpackets && (th_decode_headerin(&tinfo, &tcomment, &tsetup, &packet) >= 0))
            {
                memcpy(&tstream, &test, sizeof (test));
                tpackets = 1;
            } // if
            else if (!vpackets && (vorbis_synthesis_headerin(&vinfo, &vcomment, &packet) >= 0))
            {
                memcpy(&vstream, &test, sizeof (test));
                vpackets = 1;
            } // else if
            else
            {
                // whatever it is, we don't care about it
                ogg_stream_clear(&test);
            } // else
        } // while
    } // while

    // no audio OR video?
    if (ctx->halt || (!vpackets && !tpackets))
        goto cleanup;

    // apparently there are two more theora and two more vorbis headers next.
    while ((!ctx->halt) && ((tpackets && (tpackets < 3)) || (vpackets && (vpackets < 3))))
    {
        while (!ctx->halt && tpackets && (tpackets < 3))
        {
            if (ogg_stream_packetout(&tstream, &packet) != 1)
                break; // get more data?
            if (!th_decode_headerin(&tinfo, &tcomment, &tsetup, &packet))
                goto cleanup;
            tpackets++;
        } // while

        while (!ctx->halt && vpackets && (vpackets < 3))
        {
            if (ogg_stream_packetout(&vstream, &packet) != 1)
                break;  // get more data?
            if (vorbis_synthesis_headerin(&vinfo, &vcomment, &packet))
                goto cleanup;
            vpackets++;
        } // while

        // get another page, try again?
        if (ogg_sync_pageout(&sync, &page) > 0)
            queue_ogg_page(ctx);
        else if (FeedMoreOggData(ctx->fd, &sync) <= 0)
            goto cleanup;
    } // while

    // okay, now we have our streams, ready to set up decoding.
    if (!ctx->halt && tpackets)
    {
        // th_decode_alloc() docs say to check for insanely large frames yourself.
        if ((tinfo.frame_width > 99999) || (tinfo.frame_height > 99999))
            goto cleanup;

        //if (tinfo.colorspace != TH_CS_ITU_REC_470M) { assert(0); goto cleanup; } // !!! FIXME
        if (tinfo.pixel_fmt != TH_PF_420) { assert(0); goto cleanup; } // !!! FIXME

        if (tinfo.fps_denominator != 0)
            fps = ((double) tinfo.fps_numerator) / ((double) tinfo.fps_denominator);

        tdec = th_decode_alloc(&tinfo, tsetup);
        if (!tdec) goto cleanup;

        // Set decoder to maximum post-processing level.
        //  Theoretically we could try dropping this level if we're not keeping up.
        int pp_level_max = 0;
        th_decode_ctl(tdec, TH_DECCTL_GET_PPLEVEL_MAX, &pp_level_max, sizeof(pp_level_max));
        th_decode_ctl(tdec, TH_DECCTL_SET_PPLEVEL, &pp_level_max, sizeof(pp_level_max));
    } // if

    // Done with this now.
    if (tsetup != NULL)
    {
        th_setup_free(tsetup);
        tsetup = NULL;
    } // if

    if (!ctx->halt && vpackets)
    {
        vdsp_init = (vorbis_synthesis_init(&vdsp, &vinfo) == 0);
        if (!vdsp_init)
            goto cleanup;
        vblock_init = (vorbis_block_init(&vdsp, &vblock) == 0);
        if (!vblock_init)
            goto cleanup;
    } // if

    // Now we can start the actual decoding!
    // Note that audio and video don't _HAVE_ to start simultaneously.

    while (!ctx->halt && !eos)
    {
        int need_pages = 0;  // need more Ogg pages?
        int saw_video_frame = 0;

        // Try to read as much audio as we can at once. We limit the outer
        //  loop to one video frame and as much audio as we can eat.
        while (!ctx->halt && vpackets)
        {
            float **pcm = NULL;
            const int frames = vorbis_synthesis_pcmout(&vdsp, &pcm);
            if (frames > 0)
            {
                const int channels = vinfo.channels;
                int chanidx, frameidx;
                float *samples;
                PcmAudioItem *item = (PcmAudioItem *) malloc(sizeof (PcmAudioItem));
                if (item == NULL) goto cleanup;
                item->playms = (unsigned long) ((((double) audioframes) / ((double) vinfo.rate)) * 1000.0);
                item->channels = channels;
                item->freq = vinfo.rate;
                item->frames = frames;
                item->samples = (float *) malloc(sizeof (float) * frames * channels);
                item->next = NULL;

                if (item->samples == NULL)
                {
                    free(item);
                    goto cleanup;
                } // if

                // I bet this beats the crap out of the CPU cache...
                samples = item->samples;
                for (frameidx = 0; frameidx < frames; frameidx++)
                {
                    for (chanidx = 0; chanidx < channels; chanidx++)
                        *(samples++) = pcm[chanidx][frameidx];
                } // for

                vorbis_synthesis_read(&vdsp, frames);  // we ate everything.
                audioframes += frames;

                //printf("Decoded %d frames of audio.\n", (int) frames);
                pthread_mutex_lock(&ctx->lock);
                if (ctx->audiolisttail)
                {
                    assert(ctx->audiolist);
                    ctx->audiolisttail->next = item;
                } // if
                else
                {
                    assert(!ctx->audiolist);
                    ctx->audiolist = item;
                } // else
                ctx->audiolisttail = item;
                pthread_mutex_unlock(&ctx->lock);
            } // if

            else  // no audio available left in current packet?
            {
                // try to feed another packet to the Vorbis stream...
                if (ogg_stream_packetout(&vstream, &packet) <= 0)
                    break;  // we'll get more pages when the video catches up.
                else
                {
                    if (vorbis_synthesis(&vblock, &packet) == 0)
                        vorbis_synthesis_blockin(&vdsp, &vblock);
                } // else
            } // else
        } // while

        if (!ctx->halt && tpackets)
        {
            // Theora, according to example_player.c, is
            //  "one [packet] in, one [frame] out."
            if (ogg_stream_packetout(&tstream, &packet) <= 0)
                need_pages = 1;
            else
            {
                ogg_int64_t granulepos = 0;
                const int rc = th_decode_packetin(tdec, &packet, &granulepos);
                if (rc == TH_DUPFRAME)
                    videoframes++;  // nothing else to do.
                else if (rc == 0)  // new frame!
                {
                    th_ycbcr_buffer ycbcr;
                    if (th_decode_ycbcr_out(tdec, ycbcr) == 0)
                    {
                        int i;
                        const int w = tinfo.pic_width;
                        const int h = tinfo.pic_height;
                        const int yoff = (tinfo.pic_x & ~1) + ycbcr[0].stride * (tinfo.pic_y & ~1);
                        const int uvoff= (tinfo.pic_x / 2) + (ycbcr[1].stride) * (tinfo.pic_y / 2);
                        unsigned char *yuv;
                        YuvVideoItem *item = (YuvVideoItem *) malloc(sizeof (YuvVideoItem));
                        if (item == NULL) goto cleanup;
                        item->playms = (fps == 0) ? 0 : (unsigned int) ((((double) videoframes) / fps) * 1000.0);
                        item->fps = fps;
                        item->width = w;
                        item->height = h;
                        item->yuv = (unsigned char *) malloc(w * h * 2);
                        item->next = NULL;

                        if (item->yuv == NULL)
                        {
                            free(item);
                            goto cleanup;
                        } // if

                        yuv = item->yuv;
                        for (i = 0; i < h; i++, yuv += w)
                            memcpy(yuv, ycbcr[0].data + yoff + ycbcr[0].stride * i, w);
                        for (i = 0; i < (h / 2); i++, yuv += w/2)
                            memcpy(yuv, ycbcr[2].data + uvoff + ycbcr[2].stride * i, w / 2);
                        for (i = 0; i < (h / 2); i++, yuv += w/2)
                            memcpy(yuv, ycbcr[1].data + uvoff + ycbcr[1].stride * i, w / 2);

                        //printf("Decoded another video frame.\n");
                        pthread_mutex_lock(&ctx->lock);
                        if (ctx->videolisttail)
                        {
                            assert(ctx->videolist);
                            ctx->videolisttail->next = item;
                        } // if
                        else
                        {
                            assert(!ctx->videolist);
                            ctx->videolist = item;
                        } // else
                        ctx->videolisttail = item;
                        ctx->videocount++;
                        pthread_mutex_unlock(&ctx->lock);

                        saw_video_frame = 1;
                    } // if
                    videoframes++;
                } // if
            } // else
        } // if

        if (!ctx->halt && need_pages)
        {
            const int rc = FeedMoreOggData(ctx->fd, &sync);
            if (rc == 0)
                eos = 1;  // end of stream
            else if (rc < 0)
                goto cleanup;  // i/o error, etc.
            else
            {
                while (!ctx->halt && (ogg_sync_pageout(&sync, &page) > 0))
                    queue_ogg_page(ctx);
            } // else
        } // if

        // Sleep the process until we have space for more frames.
        if (saw_video_frame)
        {
            int go_on = !ctx->halt;
            //printf("Sleeping.\n");
            while (go_on)
            {
                // !!! FIXME: This is stupid. I should use a semaphore for this.
                pthread_mutex_lock(&ctx->lock);
                go_on = !ctx->halt && (ctx->videocount >= ctx->maxframes);
                pthread_mutex_unlock(&ctx->lock);
                if (go_on)
                    usleep(10000);
            } // while
            //printf("Awake!\n");
        } // if
    } // while

    was_error = 0;

cleanup:
    ctx->decode_error = (!ctx->halt && was_error);
    if (tdec != NULL) th_decode_free(tdec);
    if (tsetup != NULL) th_setup_free(tsetup);
    if (vblock_init) vorbis_block_clear(&vblock);
    if (vdsp_init) vorbis_dsp_clear(&vdsp);
    if (tpackets) ogg_stream_clear(&tstream);
    if (vpackets) ogg_stream_clear(&vstream);
    th_info_clear(&tinfo);
    th_comment_clear(&tcomment);
    vorbis_comment_clear(&vcomment);
    vorbis_info_clear(&vinfo);
    ogg_sync_clear(&sync);
    close(ctx->fd);
    ctx->thread_done = 1;
} // WorkerThread


static void *WorkerThreadEntry(void *_this)
{
    TheoraDecoder *ctx = (TheoraDecoder *) _this;
    WorkerThread(ctx);
    //printf("Worker thread is done.\n");
    return NULL;
} // WorkerThreadEntry


THEORAPLAY_Decoder *THEORAPLAY_startDecode(const char *fname,
                                           const unsigned int maxframes)
{
    TheoraDecoder *ctx = malloc(sizeof (TheoraDecoder));
    if (ctx == NULL)
        return NULL;

    memset(ctx, '\0', sizeof (TheoraDecoder));
    ctx->maxframes = maxframes;

    ctx->fd = open(fname, O_RDONLY);
    if (ctx->fd != -1)
    {
        struct stat statbuf;
        if (fstat(ctx->fd, &statbuf) != -1)
        {
            if (pthread_mutex_init(&ctx->lock, NULL) == 0)
            {
                ctx->thread_created = (pthread_create(&ctx->worker, NULL, WorkerThreadEntry, ctx) == 0);
                if (ctx->thread_created)
                    return (THEORAPLAY_Decoder *) ctx;
            } // if

            pthread_mutex_destroy(&ctx->lock);
        } // if

        close(ctx->fd);
    } // if

    free(ctx);
    return NULL;
} // THEORAPLAY_startDecode


void THEORAPLAY_stopDecode(THEORAPLAY_Decoder *decoder)
{
    TheoraDecoder *ctx = (TheoraDecoder *) decoder;

    if (ctx->thread_created)
    {
        ctx->halt = 1;
        pthread_join(ctx->worker, NULL);
        pthread_mutex_destroy(&ctx->lock);
    } // if

    YuvVideoItem *videolist = ctx->videolist;
    while (videolist)
    {
        YuvVideoItem *next = videolist->next;
        free(videolist->yuv);
        free(videolist);
        videolist = next;
    } // while

    PcmAudioItem *audiolist = ctx->audiolist;
    while (audiolist)
    {
        PcmAudioItem *next = audiolist->next;
        free(audiolist->samples);
        free(audiolist);
        audiolist = next;
    } // while

    free(ctx);
} // THEORAPLAY_stopDecode


int THEORAPLAY_isDecoding(THEORAPLAY_Decoder *decoder)
{
    const TheoraDecoder *ctx = (TheoraDecoder *) decoder;
    return ( ctx && (ctx->audiolist || ctx->videolist ||
             (ctx->thread_created && !ctx->thread_done)) );
} // THEORAPLAY_isDecoding


int THEORAPLAY_decodingError(THEORAPLAY_Decoder *decoder)
{
    const TheoraDecoder *ctx = (TheoraDecoder *) decoder;
    return (ctx && ctx->decode_error);
} // THEORAPLAY_decodingError


const PcmAudioItem *THEORAPLAY_getAudio(THEORAPLAY_Decoder *decoder)
{
    TheoraDecoder *ctx = (TheoraDecoder *) decoder;
    PcmAudioItem *retval;

    pthread_mutex_lock(&ctx->lock);
    retval = ctx->audiolist;
    if (retval)
    {
        ctx->audiolist = retval->next;
        retval->next = NULL;
        if (ctx->audiolist == NULL)
            ctx->audiolisttail = NULL;
    } // if
    pthread_mutex_unlock(&ctx->lock);

    return retval;
} // THEORAPLAY_getAudio


void THEORAPLAY_freeAudio(const PcmAudioItem *_item)
{
    PcmAudioItem *item = (PcmAudioItem *) _item;
    assert(item->next == NULL);
    free(item->samples);
    free(item);
} // THEORAPLAY_freeAudio


const YuvVideoItem *THEORAPLAY_getVideo(THEORAPLAY_Decoder *decoder)
{
    TheoraDecoder *ctx = (TheoraDecoder *) decoder;
    YuvVideoItem *retval;

    pthread_mutex_lock(&ctx->lock);
    retval = ctx->videolist;
    if (retval)
    {
        ctx->videolist = retval->next;
        retval->next = NULL;
        if (ctx->videolist == NULL)
            ctx->videolisttail = NULL;
        assert(ctx->videocount > 0);
        ctx->videocount--;
    } // if
    pthread_mutex_unlock(&ctx->lock);

    return retval;
} // THEORAPLAY_getVideo


void THEORAPLAY_freeVideo(const YuvVideoItem *_item)
{
    YuvVideoItem *item = (YuvVideoItem *) _item;
    assert(item->next == NULL);
    free(item->yuv);
    free(item);
} // THEORAPLAY_freeVideo

// end of theoraplay.cpp ...

