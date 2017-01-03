/*
 * Blackmagic Devices Decklink C wrapper
 * Copyright (c) 2013 Luca Barbato.
 *
 * This file is part of libbmd.
 *
 * libbmd is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * libbmd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with libbmd; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>

#include <DeckLinkAPIVersion.h>
#include <DeckLinkAPIDispatch.cpp>
#include <DeckLinkAPI.h>

extern "C" {
#include "decklink_capture.h"
}

struct DecklinkCapture {
    IDeckLinkIterator            *it;
    IDeckLink                    *dl;
    IDeckLinkInput               *in;
    IDeckLinkDisplayModeIterator *dm_it;
    IDeckLinkDisplayMode         *dm;
    IDeckLinkConfiguration       *conf;
};

class CaptureDelegate : public IDeckLinkInputCallback
{
public:
    CaptureDelegate(void *context,
                    int64_t time_base,
                    decklink_video_cb video,
                    decklink_audio_cb audio);
    ~CaptureDelegate();

    virtual HRESULT STDMETHODCALLTYPE
        QueryInterface(REFIID iid, LPVOID *ppv) { return E_NOINTERFACE; }
    virtual ULONG STDMETHODCALLTYPE
        AddRef(void);
    virtual ULONG STDMETHODCALLTYPE
        Release(void);
    virtual HRESULT STDMETHODCALLTYPE
        VideoInputFormatChanged(BMDVideoInputFormatChangedEvents,
                                IDeckLinkDisplayMode*,
                                BMDDetectedVideoInputFormatFlags);
    virtual HRESULT STDMETHODCALLTYPE
        VideoInputFrameArrived(IDeckLinkVideoInputFrame*,
                               IDeckLinkAudioInputPacket*);

private:
    ULONG ref_count;
    pthread_mutex_t mutex;

// callbacks
    void *ctx;
    int64_t timebase;
    decklink_video_cb video_cb;
    decklink_audio_cb audio_cb;
};

CaptureDelegate::CaptureDelegate(void *context,
                                 int64_t time_base,
                                 decklink_video_cb video,
                                 decklink_audio_cb audio) : ref_count(0)
{
    video_cb = video;
    audio_cb = audio;
    timebase = time_base;
    ctx = context;

    pthread_mutex_init(&mutex, NULL);
}

CaptureDelegate::~CaptureDelegate()
{
    pthread_mutex_destroy(&mutex);
}

ULONG CaptureDelegate::AddRef(void)
{
    pthread_mutex_lock(&mutex);
    ref_count++;
    pthread_mutex_unlock(&mutex);

    return (ULONG)ref_count;
}

ULONG CaptureDelegate::Release(void)
{
    pthread_mutex_lock(&mutex);
    ref_count--;
    pthread_mutex_unlock(&mutex);

    if (!ref_count) {
        delete this;
        return 0;
    }

    return (ULONG)ref_count;
}

HRESULT
CaptureDelegate::VideoInputFrameArrived(IDeckLinkVideoInputFrame  *v_frame,
                                        IDeckLinkAudioInputPacket *a_frame)
{
    uint8_t *frame_bytes;
    BMDTimeValue timestamp;
    BMDTimeValue duration;

    // Handle Video Frame
    if (v_frame) {
        if (v_frame->GetFlags() & bmdFrameHasNoInputSource) {
        // log
            return S_OK;
        } else {
            v_frame->GetBytes((void **)&frame_bytes);
            v_frame->GetStreamTime(&timestamp, &duration, timebase);

            video_cb(ctx, frame_bytes,
                     v_frame->GetWidth(),
                     v_frame->GetHeight(),
                     v_frame->GetRowBytes(),
                     timestamp,
                     duration, 0);
        }
    }

    // Handle Audio Frame
    if (a_frame) {
        BMDTimeValue audio_pts;

        a_frame->GetSampleFrameCount();
        a_frame->GetBytes((void **)&frame_bytes);
        a_frame->GetPacketTime(&timestamp, 48000);

        audio_cb(ctx, frame_bytes,
                 a_frame->GetSampleFrameCount(),
                 timestamp, 0);

    }
    return S_OK;
}

HRESULT
CaptureDelegate::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents ev,
                                         IDeckLinkDisplayMode *mode,
                                         BMDDetectedVideoInputFormatFlags)
{
    //FIXME
    return S_OK;
}


void decklink_capture_free(DecklinkCapture *capture)
{
    if (!capture)
        return;

    if (capture->dm_it) {
        capture->dm_it->Release();
        capture->dm_it = NULL;
    }

    if (capture->in) {
        capture->in->Release();
        capture->in = NULL;
    }

    if (capture->dl) {
        capture->dl->Release();
        capture->dl = NULL;
    }

    if (capture->it)
        capture->it->Release();

    free(capture);
}

DecklinkCapture *decklink_capture_connect(DecklinkConf *c)
{
    DecklinkCapture   *capture     = (DecklinkCapture *)calloc(1, sizeof(*capture));
    BMDPixelFormat    pix[]        = { bmdFormat8BitYUV, bmdFormat10BitYUV,
                                       bmdFormat8BitARGB, bmdFormat10BitRGB,
                                       bmdFormat8BitBGRA };
    BMDDisplayMode    display_mode;
    CaptureDelegate   *delegate;
    HRESULT           ret;
    int               i            = 0;

    if (!capture)
        return NULL;

    capture->it = CreateDeckLinkIteratorInstance();

    if (!capture->it)
        goto fail;

    switch (c->audio_channels) {
    case  0:
        c->audio_channels = 2;
    case  2:
    case  8:
    case 16:
        break;
    default:
        goto fail;
    }

    switch (c->audio_sample_depth) {
    case  0:
        c->audio_sample_depth = 16;
    case 16:
    case 32:
        break;
    default:
        goto fail;
    }

    if (c->pixel_format >= sizeof(pix))
        goto fail;

    do {
        ret = capture->it->Next(&capture->dl);
    } while (i++ < c->instance);

    if (ret != S_OK)
        goto fail;

    ret = capture->dl->QueryInterface(IID_IDeckLinkInput,
                                      (void**)&capture->in);
    if (ret != S_OK)
        goto fail;

    ret = capture->dl->QueryInterface(IID_IDeckLinkConfiguration,
                                      (void**)&capture->conf);

    switch (c->audio_connection) {
    case 1:
        ret = capture->conf->SetInt(bmdDeckLinkConfigAudioInputConnection,
                                    bmdAudioConnectionAnalog);
        break;
    case 2:
        ret = capture->conf->SetInt(bmdDeckLinkConfigAudioInputConnection,
                                    bmdAudioConnectionEmbedded);
        break;
    default:
        // do not change it
        break;
    }

    if (ret != S_OK) {
        goto fail;
    }

    switch (c->video_connection) {
    case 1:
        ret = capture->conf->SetInt(bmdDeckLinkConfigVideoInputConnection,
                                    bmdVideoConnectionComposite);
        break;
    case 2:
        ret = capture->conf->SetInt(bmdDeckLinkConfigVideoInputConnection,
                                    bmdVideoConnectionComponent);
        break;
    case 3:
        ret = capture->conf->SetInt(bmdDeckLinkConfigVideoInputConnection,
                                    bmdVideoConnectionHDMI);
        break;
    case 4:
        ret = capture->conf->SetInt(bmdDeckLinkConfigVideoInputConnection,
                                    bmdVideoConnectionSDI);
        break;
    default:
        // do not change it
        break;
    }

    if (ret != S_OK) {
        goto fail;
    }

    ret = capture->in->GetDisplayModeIterator(&capture->dm_it);

    if (ret != S_OK) {
        goto fail;
    }

    switch (c->video_mode) {
        case 0: display_mode = bmdModeNTSC; break;
        case 1: display_mode = bmdModeNTSC2398; break;
        case 2: display_mode = bmdModePAL; break;
        case 14: display_mode = bmdModeNTSCp; break;
        case 15: display_mode = bmdModePALp; break;

        case 3: display_mode = bmdModeHD1080p2398; break;
        case 4: display_mode = bmdModeHD1080p24; break;
        case 5: display_mode = bmdModeHD1080p25; break;
        case 6: display_mode = bmdModeHD1080p2997; break;
        case 7: display_mode = bmdModeHD1080p30; break;
        case 8: display_mode = bmdModeHD1080i50; break;
        case 9: display_mode = bmdModeHD1080i5994; break;
        case 10: display_mode = bmdModeHD1080i6000; break;
        case 16: display_mode = bmdModeHD1080p50; break;
        case 17: display_mode = bmdModeHD1080p5994; break;
        case 18: display_mode = bmdModeHD1080p6000; break;

        case 11: display_mode = bmdModeHD720p50; break;
        case 12: display_mode = bmdModeHD720p5994; break;
        case 13: display_mode = bmdModeHD720p60; break;

        case 19: display_mode = bmdMode2k2398; break;
        case 20: display_mode = bmdMode2k24; break;
        case 21: display_mode = bmdMode2k25; break;

        case 22: display_mode = bmdMode2kDCI2398; break;
        case 23: display_mode = bmdMode2kDCI24; break;
        case 24: display_mode = bmdMode2kDCI25; break;

        case 25: display_mode = bmdMode4K2160p2398; break;
        case 26: display_mode = bmdMode4K2160p24; break;
        case 27: display_mode = bmdMode4K2160p25; break;
        case 28: display_mode = bmdMode4K2160p2997; break;
        case 29: display_mode = bmdMode4K2160p30; break;

#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0a030100
        case 30: display_mode = bmdMode4K2160p50; break;
        case 31: display_mode = bmdMode4K2160p5994; break;
        case 32: display_mode = bmdMode4K2160p60; break;
#endif

        case 33: display_mode = bmdMode4kDCI2398; break;
        case 34: display_mode = bmdMode4kDCI24; break;
        case 35: display_mode = bmdMode4kDCI25; break;

        default: display_mode = bmdModeUnknown; break;
    }

    while ((ret = capture->dm_it->Next(&capture->dm)) == S_OK) {
        if (display_mode == bmdModeUnknown ||
            display_mode == capture->dm->GetDisplayMode())
            break;
        else
            capture->dm->Release();
    }

    if (ret != S_OK)
        goto fail;

    c->width      = capture->dm->GetWidth();
    c->height     = capture->dm->GetHeight();
    switch (capture->dm->GetFieldDominance()) {
    case bmdUnknownFieldDominance:
        c->field_mode = 0;
        break;
    case bmdLowerFieldFirst:
        c->field_mode = 1;
        break;
    case bmdUpperFieldFirst:
        c->field_mode = 2;
        break;
    case bmdProgressiveFrame:
        c->field_mode = 3;
        break;
    case bmdProgressiveSegmentedFrame:
        c->field_mode = 4;
        break;
    default:
        goto fail;
    }

    capture->dm->GetFrameRate(&c->tb_num, &c->tb_den);

    delegate = new CaptureDelegate(c->priv, c->tb_den,
                                   c->video_cb, c->audio_cb);

    if (!delegate)
        goto fail;

    capture->in->SetCallback(delegate);

    ret = capture->in->EnableVideoInput(capture->dm->GetDisplayMode(),
                                        pix[c->pixel_format], 0);

    ret = capture->in->EnableAudioInput(bmdAudioSampleRate48kHz,
                                        c->audio_sample_depth,
                                        c->audio_channels);

    return capture;
fail:
    decklink_capture_free(capture);
    return NULL;
}

class QueryDelegate : public IDeckLinkInputCallback
{
public:
    QueryDelegate(BMDDisplayMode display_mode);
    
    virtual HRESULT STDMETHODCALLTYPE
        QueryInterface(REFIID iid, LPVOID *ppv) { return E_NOINTERFACE; }
    virtual ULONG STDMETHODCALLTYPE
        AddRef(void);
    virtual ULONG STDMETHODCALLTYPE
        Release(void);
    
    virtual HRESULT STDMETHODCALLTYPE
        VideoInputFormatChanged(BMDVideoInputFormatChangedEvents,
                                IDeckLinkDisplayMode*,
                                BMDDetectedVideoInputFormatFlags);
    
    virtual HRESULT STDMETHODCALLTYPE
        VideoInputFrameArrived(IDeckLinkVideoInputFrame*,
                               IDeckLinkAudioInputPacket*);
    
    BMDDisplayMode GetDisplayMode() const { return display_mode; }
    bool isDone() const { return done; }
                   
    private:
        BMDDisplayMode display_mode;
        bool done;
        ULONG ref_count;
};

QueryDelegate::QueryDelegate(BMDDisplayMode display_mode): ref_count(0), display_mode(display_mode)
{
    done = false;
}

ULONG QueryDelegate::AddRef(void)
{
    ref_count++;
    return ref_count;
}

ULONG QueryDelegate::Release(void)
{
    ref_count--;

    if (!ref_count) {
        delete this;
        return 0;
    }

    return ref_count;
}

HRESULT
QueryDelegate::VideoInputFrameArrived(IDeckLinkVideoInputFrame  *v_frame,
                                        IDeckLinkAudioInputPacket *a_frame)
{
    if (v_frame->GetFlags() & bmdFrameHasNoInputSource) {
        return S_OK;
    }
    else {
        done = true;
    }
    
    return S_OK;
}

HRESULT
QueryDelegate::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents ev,
                                         IDeckLinkDisplayMode *mode,
                                         BMDDetectedVideoInputFormatFlags)
{
    display_mode = mode->GetDisplayMode();
    done = true;
    return S_OK;
}

int query_display_mode(DecklinkConf *c)
{
    DecklinkCapture   *capture     = (DecklinkCapture *)calloc(1, sizeof(*capture));
    BMDPixelFormat    pix[]        = { bmdFormat8BitYUV, bmdFormat10BitYUV,
                                       bmdFormat8BitARGB, bmdFormat10BitRGB,
                                       bmdFormat8BitBGRA };
    BMDDisplayMode    display_mode = -1;
    QueryDelegate     *delegate;
    HRESULT           ret;
    int               i            = 0;
    int               result       = -1;
    IDeckLinkAttributes* deckLinkAttributes;
    bool			  formatDetectionSupported;

    if (!capture)
        goto fail;

    capture->it = CreateDeckLinkIteratorInstance();

    if (!capture->it)
        goto fail;

    switch (c->audio_channels) {
    case  0:
        c->audio_channels = 2;
    case  2:
    case  8:
    case 16:
        break;
    default:
        goto fail;
    }

    switch (c->audio_sample_depth) {
    case  0:
        c->audio_sample_depth = 16;
    case 16:
    case 32:
        break;
    default:
        goto fail;
    }

    if (c->pixel_format >= sizeof(pix))
        goto fail;

    do {
        ret = capture->it->Next(&capture->dl);
    } while (i++ < c->instance);

    if (ret != S_OK)
        goto fail;

    ret = capture->dl->QueryInterface(IID_IDeckLinkAttributes, (void**)&deckLinkAttributes);
    
    if (ret != S_OK) {
        goto fail;
    }
    
    ret = deckLinkAttributes->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &formatDetectionSupported);
    
    if (ret != S_OK) {
        goto fail;
    }
    
    if (!formatDetectionSupported) {
        goto fail;
    }
    
    ret = capture->dl->QueryInterface(IID_IDeckLinkInput,
                                      (void**)&capture->in);
    if (ret != S_OK)
        goto fail;

    ret = capture->dl->QueryInterface(IID_IDeckLinkConfiguration,
                                      (void**)&capture->conf);

    switch (c->audio_connection) {
    case 1:
        ret = capture->conf->SetInt(bmdDeckLinkConfigAudioInputConnection,
                                    bmdAudioConnectionAnalog);
        break;
    case 2:
        ret = capture->conf->SetInt(bmdDeckLinkConfigAudioInputConnection,
                                    bmdAudioConnectionEmbedded);
        break;
    default:
        // do not change it
        break;
    }

    if (ret != S_OK) {
        goto fail;
    }

    switch (c->video_connection) {
    case 1:
        ret = capture->conf->SetInt(bmdDeckLinkConfigVideoInputConnection,
                                    bmdVideoConnectionComposite);
        break;
    case 2:
        ret = capture->conf->SetInt(bmdDeckLinkConfigVideoInputConnection,
                                    bmdVideoConnectionComponent);
        break;
    case 3:
        ret = capture->conf->SetInt(bmdDeckLinkConfigVideoInputConnection,
                                    bmdVideoConnectionHDMI);
        break;
    case 4:
        ret = capture->conf->SetInt(bmdDeckLinkConfigVideoInputConnection,
                                    bmdVideoConnectionSDI);
        break;
    default:
        // do not change it
        break;
    }

    if (ret != S_OK) {
        goto fail;
    }

    ret = capture->in->GetDisplayModeIterator(&capture->dm_it);

    if (ret != S_OK) {
        goto fail;
    }

    i = 0;
    while (true) {
    
        if (capture->dm_it->Next(&capture->dm) != S_OK) {
            capture->dm->Release();
        }
        else {
            result = i;
            break;
        }
        i++;
    }
    
    if (result == -1) {
        return -1;
    }

    c->width      = capture->dm->GetWidth();
    c->height     = capture->dm->GetHeight();
    switch (capture->dm->GetFieldDominance()) {
    case bmdUnknownFieldDominance:
        c->field_mode = 0;
        break;
    case bmdLowerFieldFirst:
        c->field_mode = 1;
        break;
    case bmdUpperFieldFirst:
        c->field_mode = 2;
        break;
    case bmdProgressiveFrame:
        c->field_mode = 3;
        break;
    case bmdProgressiveSegmentedFrame:
        c->field_mode = 4;
        break;
    default:
        goto fail;
    }

    capture->dm->GetFrameRate(&c->tb_num, &c->tb_den);

    delegate = new QueryDelegate(capture->dm->GetDisplayMode());

    if (!delegate)
        goto fail;

    capture->in->SetCallback(delegate);

    ret = capture->in->EnableVideoInput(capture->dm->GetDisplayMode(),
                                        pix[c->pixel_format],
                                        bmdVideoInputEnableFormatDetection);

    ret = capture->in->StartStreams();
    
    if (ret != S_OK) {
        goto fail;
    }
    
    while (!delegate->isDone())
    {
        usleep(20000);
    }
    
    ret = capture->in->StopStreams();
    
    if (ret != S_OK) {
        goto fail;
    }
    
    switch (delegate->GetDisplayMode())
    {
        case bmdModeNTSC: result = 0; break;
        case bmdModeNTSC2398: result = 1; break;
        case bmdModePAL: result = 2; break;
        case bmdModeNTSCp: result = 14; break;
        case bmdModePALp: result = 15; break;

        case bmdModeHD1080p2398: result = 3; break;
        case bmdModeHD1080p24: result = 4; break;
        case bmdModeHD1080p25: result = 5; break;
        case bmdModeHD1080p2997: result = 6; break;
        case bmdModeHD1080p30: result = 7; break;
        case bmdModeHD1080i50: result = 8; break;
        case bmdModeHD1080i5994: result = 9; break;
        case bmdModeHD1080i6000: result = 10; break;
        case bmdModeHD1080p50: result = 16; break;
        case bmdModeHD1080p5994: result = 17; break;
        case bmdModeHD1080p6000: result = 18; break;

        case bmdModeHD720p50: result = 11; break;
        case bmdModeHD720p5994: result = 12; break;
        case bmdModeHD720p60: result = 13; break;

        case bmdMode2k2398: result = 19; break;
        case bmdMode2k24: result = 20; break;
        case bmdMode2k25: result = 21; break;

        case bmdMode2kDCI2398: result = 22; break;
        case bmdMode2kDCI24: result = 23; break;
        case bmdMode2kDCI25: result = 24; break;

        case bmdMode4K2160p2398: result = 25; break;
        case bmdMode4K2160p24: result = 26; break;
        case bmdMode4K2160p25: result = 27; break;
        case bmdMode4K2160p2997: result = 28; break;
        case bmdMode4K2160p30: result = 29; break;

#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0a030100
        case bmdMode4K2160p50: result = 30; break;
        case bmdMode4K2160p5994: result = 31; break;
        case bmdMode4K2160p60: result = 32; break;
#endif

        case bmdMode4kDCI2398: result = 33; break;
        case bmdMode4kDCI24: result = 34; break;
        case bmdMode4kDCI25: result = 35; break;

        default: result = -1; break;
    }

fail:
    decklink_capture_free(capture);
    
    return result;
}

DecklinkCapture *decklink_capture_alloc(DecklinkConf *c)
{
    if (c->video_mode == -1)
    {
        c->video_mode = query_display_mode(c);
    }
    
    return decklink_capture_connect(c);
}

int decklink_capture_start(DecklinkCapture *capture)
{
    return capture->in->StartStreams();
}

int decklink_capture_stop(DecklinkCapture *capture)
{
    return capture->in->StopStreams();
}
