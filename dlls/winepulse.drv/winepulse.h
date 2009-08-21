/* Definitions for PulseAudio Wine Driver
 *
 * Copyright    2009 Arthur Taylor <theycallhimart@gmail.com>
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

#ifndef __WINE_CONFIG_H
# error You must include config.h to use this header
#endif

#if defined(HAVE_PULSEAUDIO) && !defined(__WINEPULSE_H)
#define __WINEPULSE_H

#include "mmreg.h"
#include "dsound.h"
#include "dsdriver.h"

#include "ks.h"
#include "ksmedia.h"
#include "ksguid.h"

#include <pulse/pulseaudio.h>

/* state diagram for waveOut writing:
 *
 * +---------+-------------+---------------+---------------------------------+
 * |  state  |  function   |     event     |            new state            |
 * +---------+-------------+---------------+---------------------------------+
 * |         | open()      |               | STOPPED                         |
 * | PAUSED  | write()     |               | PAUSED                          |
 * | STOPPED | write()     | <thrd create> | PLAYING                         |
 * | PLAYING | write()     | HEADER        | PLAYING                         |
 * | (other) | write()     | <error>       |                                 |
 * | (any)   | pause()     | PAUSING       | PAUSED                          |
 * | PAUSED  | restart()   | RESTARTING    | PLAYING (if no thrd => STOPPED) |
 * | (any)   | reset()     | RESETTING     | STOPPED                         |
 * | (any)   | close()     | CLOSING       | CLOSED                          |
 * +---------+-------------+---------------+---------------------------------+
 */

/* states of the playing device */
#define WINE_WS_PLAYING         1
#define WINE_WS_PAUSED          2
#define WINE_WS_STOPPED         3
#define WINE_WS_CLOSED          4
#define WINE_WS_FAILED          5

#define PULSE_ALL_FORMATS \
        WAVE_FORMAT_1M08 |      /* Mono     11025Hz 8-bit  */\
        WAVE_FORMAT_1M16 |      /* Mono     11025Hz 16-bit */\
        WAVE_FORMAT_1S08 |      /* Stereo   11025Hz 8-bit  */\
        WAVE_FORMAT_1S16 |      /* Stereo   11025Hz 16-bit */\
        WAVE_FORMAT_2M08 |      /* Mono     22050Hz 8-bit  */\
        WAVE_FORMAT_2M16 |      /* Mono     22050Hz 16-bit */\
        WAVE_FORMAT_2S08 |      /* Stereo   22050Hz 8-bit  */\
        WAVE_FORMAT_2S16 |      /* Stereo   22050Hz 16-bit */\
        WAVE_FORMAT_4M08 |      /* Mono     44100Hz 8-bit  */\
        WAVE_FORMAT_4M16 |      /* Mono     44100Hz 16-bit */\
        WAVE_FORMAT_4S08 |      /* Stereo   44100Hz 8-bit  */\
        WAVE_FORMAT_4S16 |      /* Stereo   44100Hz 16-bit */\
        WAVE_FORMAT_48M08 |     /* Mono     48000Hz 8-bit  */\
        WAVE_FORMAT_48S08 |     /* Stereo   48000Hz 8-bit  */\
        WAVE_FORMAT_48M16 |     /* Mono     48000Hz 16-bit */\
        WAVE_FORMAT_48S16 |     /* Stereo   48000Hz 16-bit */\
        WAVE_FORMAT_96M08 |     /* Mono     96000Hz 8-bit  */\
        WAVE_FORMAT_96S08 |     /* Stereo   96000Hz 8-bit  */\
        WAVE_FORMAT_96M16 |     /* Mono     96000Hz 16-bit */\
        WAVE_FORMAT_96S16       /* Stereo   96000Hz 16-bit */

/* events to be sent to device */
enum win_wm_message {
    WINE_WM_PAUSING = WM_USER + 1, WINE_WM_RESTARTING, WINE_WM_RESETTING, WINE_WM_HEADER,
    WINE_WM_BREAKLOOP, WINE_WM_CLOSING, WINE_WM_STARTING, WINE_WM_STOPPING, WINE_WM_XRUN, WINE_WM_FEED
};

typedef struct {
    enum win_wm_message msg;    /* message identifier */
    DWORD               param;  /* parameter for this message */
    HANDLE              hEvent; /* if message is synchronous, handle of event for synchro */
} PULSE_MSG;

/* implement an in-process message ring for better performance
 * (compared to passing thru the server)
 * this ring will be used by the input (resp output) record (resp playback) routine
 */
typedef struct {
    PULSE_MSG                   * messages;
    int                         ring_buffer_size;
    int                         msg_tosave;
    int                         msg_toget;
/* Either pipe or event is used, but that is defined in pulse.c,
 * since this is a global header we define both here */
    int                         msg_pipe[2];
    HANDLE                      msg_event;
    CRITICAL_SECTION            msg_crst;
} PULSE_MSG_RING;

typedef struct WINE_WAVEDEV WINE_WAVEDEV;
typedef struct WINE_WAVEINST WINE_WAVEINST;

/* Per-playback/record device */
struct WINE_WAVEDEV {
    char                interface_name[MAXPNAMELEN * 2];
    char                *device_name;
    pa_cvolume          volume;

    union {
        WAVEOUTCAPSW    out;
        WAVEINCAPSW     in;
    } caps;
    
    /* DirectSound stuff */
    DSDRIVERDESC                ds_desc;
    DSDRIVERCAPS                ds_caps;
};

/* Per-playback/record instance */
struct WINE_WAVEINST {
    INT                 state;              /* one of the WINE_WS_ manifest constants */
    WAVEOPENDESC        waveDesc;
    WORD                wFlags;

    /* PulseAudio specific data */
    pa_stream           *stream;            /* The PulseAudio stream */
    const pa_timing_info *timing_info;      /* The timing info structure for the stream */
    pa_sample_spec      sample_spec;        /* Sample spec of this stream / device */
    pa_cvolume          volume;             /* Software volume of the stream */
    pa_buffer_attr      buffer_attr;        /* Buffer attribute, may not be used */

    /* waveIn / waveOut wavaHdr */
    LPWAVEHDR           lpQueuePtr;         /* Start of queued WAVEHDRs (waiting to be notified) */
    LPWAVEHDR           lpPlayPtr;          /* Start of not yet fully written buffers */
    DWORD               dwPartialOffset;    /* Offset of not yet written bytes in lpPlayPtr */
    LPWAVEHDR           lpLoopPtr;          /* Pointer of first buffer in loop, if any */
    DWORD               dwLoops;            /* Private copy of loop counter */
    DWORD               dwLastReset;        /* When the last reset occured, as pa stream time doesn't reset */

    /* waveIn specific */
    const void          *buffer;            /* Pointer to the latest data fragment for recording streams */
    DWORD               buffer_length;      /* How large the latest data fragment is */
    DWORD               buffer_read_offset; /* How far into latest data fragment we last read */

    /* Thread communication and synchronization stuff */
    HANDLE              hStartUpEvent;
    HANDLE              hThread;
    DWORD               dwThreadID;
    PULSE_MSG_RING      msgRing;
};

/* We establish one context per instance, so make it global to the lib */
pa_context              *PULSE_context;   /* Connection Context */
pa_threaded_mainloop    *PULSE_ml;        /* PA Runtime information */

/* WaveIn / WaveOut devices */
WINE_WAVEDEV *WOutDev;
WINE_WAVEDEV *WInDev;
DWORD PULSE_WodNumDevs;
DWORD PULSE_WidNumDevs;

/* pulse.c: PulseAudio Async Callbacks */
void    PULSE_StreamRequestCallback(pa_stream *s, size_t nbytes, void *userdata);
void    PULSE_StreamSuccessCallback(pa_stream *s, int success, void *userdata);
void    PULSE_StreamStateCallback(pa_stream *s, void *userdata);
void    PULSE_StreamUnderflowCallback(pa_stream *s, void *userdata);
void    PULSE_StreamSuspendedCallback(pa_stream *s, void *userdata);
void    PULSE_StreamMovedCallback(pa_stream *s, void *userdata);
void    PULSE_ContextSuccessCallback(pa_context *c, int success, void *userdata);

/* pulse.c: General Functions */
void    PULSE_WaitForOperation(pa_operation *o);
BOOL    PULSE_SetupFormat(LPWAVEFORMATEX wf, pa_sample_spec *ss);
HRESULT PULSE_UsecToMMTime(pa_usec_t time, LPMMTIME lpTime, const pa_sample_spec *ss);

/* pulse.c: Message Ring */
int     PULSE_InitRingMessage(PULSE_MSG_RING* omr);
int     PULSE_DestroyRingMessage(PULSE_MSG_RING* omr);
void    PULSE_ResetRingMessage(PULSE_MSG_RING* omr);
void    PULSE_WaitRingMessage(PULSE_MSG_RING* omr, DWORD sleep);
int     PULSE_AddRingMessage(PULSE_MSG_RING* omr, enum win_wm_message msg, DWORD param, BOOL wait);
int     PULSE_RetrieveRingMessage(PULSE_MSG_RING* omr, enum win_wm_message *msg, DWORD *param, HANDLE *hEvent);

/* pulse.c: Tracing */
const char * PULSE_getCmdString(enum win_wm_message msg);
#endif
