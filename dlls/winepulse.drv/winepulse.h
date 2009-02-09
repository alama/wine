/* Definitions for PulseAudio Wine Driver
 *
 * Copyright	2008 Arthur Taylor
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

#undef PULSE_VERBOSE

/* states of the playing device */
#define WINE_WS_PLAYING         1
#define WINE_WS_PAUSED          2
#define WINE_WS_STOPPED         3
#define WINE_WS_CLOSED          4
#define WINE_WS_FAILED          5

#define PULSE_MAX_STREAM_INSTANCES 16 /* Sixteen streams per device should be good enough? */

/* events to be sent to device */
enum win_wm_message {
    WINE_WM_PAUSING = WM_USER + 1, WINE_WM_RESTARTING, WINE_WM_RESETTING, WINE_WM_HEADER,
    WINE_WM_BREAKLOOP, WINE_WM_CLOSING, WINE_WM_STARTING, WINE_WM_STOPPING, WINE_WM_XRUN, WINE_WM_FEED
};

typedef struct {
    enum win_wm_message 	msg;	/* message identifier */
    DWORD	                param;  /* parameter for this message */
    HANDLE	                hEvent;	/* if message is synchronous, handle of event for synchro */
} PULSE_MSG;

/* implement an in-process message ring for better performance
 * (compared to passing thru the server)
 * this ring will be used by the input (resp output) record (resp playback) routine
 */
typedef struct {
    PULSE_MSG			* messages;
    int                         ring_buffer_size;
    int				msg_tosave;
    int				msg_toget;
/* Either pipe or event is used, but that is defined in pulse.c,
 * since this is a global header we define both here */
    int                         msg_pipe[2];
    HANDLE                      msg_event;
    CRITICAL_SECTION		msg_crst;
} PULSE_MSG_RING;

/* Per-playback/record instance */
typedef struct {
    int			instance_ref;	    /* The array index of WINE_WAVEDEV->instance[] that this is */
    volatile int        state;		    /* one of the WINE_WS_ manifest constants */
    WAVEOPENDESC        waveDesc;
    WORD		wFlags;
    pa_stream		*stream;	    /* The PulseAudio stream */
    const pa_timing_info	*timing_info;
    pa_buffer_attr	*buffer_attr;
    pa_sample_spec	sample_spec;	    /* Sample spec of this stream / device */
    pa_cvolume		volume;

    /* WavaHdr information */
    LPWAVEHDR		lpQueuePtr;	    /* start of queued WAVEHDRs (waiting to be notified) */
    LPWAVEHDR		lpPlayPtr;	    /* start of not yet fully written buffers */
    DWORD		dwPartialOffset;    /* Offset of not yet written bytes in lpPlayPtr */
    LPWAVEHDR		lpLoopPtr;          /* pointer of first buffer in loop, if any */
    DWORD		dwLoops;	    /* private copy of loop counter */

    /* Virtual stream positioning information */
    DWORD		last_reset;	    /* When the last reset occured, as pa stream time doesn't reset */
    BOOL		is_buffering;	    /* !is_playing */
    struct timeval	last_header;	    /* When the last wavehdr was received, only updated when is_buffering is true */
    BOOL		is_releasing;	    /* Whether we are releasing wavehdrs, may not always be the inverse of is_buffering */
    struct timeval	started_releasing;  /* When wavehdr releasing started, for comparison to queued written wavehdrs */
    DWORD		releasing_offset;   /* How much audio has been released prior when releasing started in this instance */

    /* Thread communication and synchronization stuff */
    HANDLE		hStartUpEvent;
    HANDLE		hThread;
    DWORD		dwThreadID;
    PULSE_MSG_RING	msgRing;
} WINE_WAVEINST;

/* Per-playback/record device */
typedef struct {
    char		*device_name;	    /* Name of the device used as an identifer on the server */
    char                interface_name[MAXPNAMELEN * 2];

    pa_volume_t		left_vol;	    /* Volume is per device and always stereo */
    pa_volume_t		right_vol;

    union {
        WAVEOUTCAPSW	out;
	WAVEINCAPSW	in;
    } caps;

    WINE_WAVEINST	*instance[PULSE_MAX_STREAM_INSTANCES];
} WINE_WAVEDEV;

/* We establish one context per instance, so make it global to the lib */
pa_context		*PULSE_context;   /* Connection Context */
pa_threaded_mainloop	*PULSE_ml;        /* PA Runtime information */

/* WaveIn / WaveOut devices */
WINE_WAVEDEV *WOutDev;
WINE_WAVEDEV *WInDev;
DWORD PULSE_WodNumDevs;
DWORD PULSE_WidNumDevs;

/* pulse.c */
void	PULSE_wait_for_operation(pa_operation *o, WINE_WAVEINST *ww);
void	PULSE_stream_success_callback(pa_stream *s, int success, void *userdata);
void	PULSE_stream_state_callback(pa_stream *s, void *userdata);
void	PULSE_context_success_callback(pa_context *c, int success, void *userdata);
void	PULSE_stream_request_callback(pa_stream *s, size_t n, void *userdata);
BOOL	PULSE_setupFormat(LPWAVEFORMATEX wf, pa_sample_spec *ss);
int	PULSE_InitRingMessage(PULSE_MSG_RING* omr);
int	PULSE_DestroyRingMessage(PULSE_MSG_RING* omr);
void	PULSE_ResetRingMessage(PULSE_MSG_RING* omr);
void	PULSE_WaitRingMessage(PULSE_MSG_RING* omr, DWORD sleep);
int	PULSE_AddRingMessage(PULSE_MSG_RING* omr, enum win_wm_message msg, DWORD param, BOOL wait);
int	PULSE_RetrieveRingMessage(PULSE_MSG_RING* omr, enum win_wm_message *msg, DWORD *param, HANDLE *hEvent);
const char * PULSE_getCmdString(enum win_wm_message msg);
#endif
