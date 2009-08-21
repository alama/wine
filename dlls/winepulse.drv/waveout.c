/*
 * Wine Driver for PulseAudio - WaveOut Functionality
 * http://pulseaudio.org/
 *
 * Copyright    2009 Arthur Taylor <theycallhimart@gmail.com>
 *
 * Contains code from other wine multimedia drivers.
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

#include "config.h"

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winnls.h"
#include "winerror.h"
#include "mmddk.h"

#include <winepulse.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wave);

#if HAVE_PULSEAUDIO

/* Use this to make the infinite wait not so infinite */
#define PULSE_INFINITE 10000

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
 * | PAUSED  | reset()     | RESETTING     | PAUSED                          |
 * | (other) | reset()     | RESETTING     | STOPPED                         |
 * | (any)   | close()     | CLOSING       | CLOSED                          |
 * +---------+-------------+---------------+---------------------------------+
 */

/*
 * - It is currently unknown if pausing in a loop works the same as expected.
 */

/*======================================================================*
 *                  WAVE OUT specific PulseAudio Callbacks              *
 *======================================================================*/

/**************************************************************************
 *                  WAVEOUT_SinkInputInfoCallback               [internal]
 *
 * Called by the pulse thread. Used for wodGetVolume.
 */
static void WAVEOUT_SinkInputInfoCallback(pa_context *c, const pa_sink_input_info *i, int eol, void *userdata) {
    WINE_WAVEINST* wwo = (WINE_WAVEINST*)userdata;
    if (!eol && i) {
        for (wwo->volume.channels = 0; wwo->volume.channels != i->volume.channels; wwo->volume.channels++)
            wwo->volume.values[wwo->volume.channels] = i->volume.values[wwo->volume.channels];
        pa_threaded_mainloop_signal(PULSE_ml, 0);
    }
}

/*======================================================================*
 *                  "Low level" WAVE OUT implementation                 *
 *======================================================================*/

/**************************************************************************
 *                  wodPlayer_NotifyClient                      [internal]
 */
static DWORD wodPlayer_NotifyClient(WINE_WAVEINST* wwo, WORD wMsg, DWORD dwParam1, DWORD dwParam2) {
    /* TRACE("wMsg = 0x%04x dwParm1 = %04X dwParam2 = %04X\n", wMsg, dwParam1, dwParam2); */

    switch (wMsg) {
    case WOM_OPEN:
    case WOM_CLOSE:
    case WOM_DONE:
        if (wwo->wFlags != DCB_NULL &&
            !DriverCallback(wwo->waveDesc.dwCallback, wwo->wFlags, (HDRVR)wwo->waveDesc.hWave,
                            wMsg, wwo->waveDesc.dwInstance, dwParam1, dwParam2)) {
            WARN("can't notify client !\n");
            return MMSYSERR_ERROR;
        }
        break;
    default:
        FIXME("Unknown callback message %u\n", wMsg);
        return MMSYSERR_INVALPARAM;
    }
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 *                  wodPlayer_BeginWaveHdr                      [internal]
 *
 * Makes the specified lpWaveHdr the currently playing wave header.
 * If the specified wave header is a begin loop and we're not already in
 * a loop, setup the loop.
 */
static void wodPlayer_BeginWaveHdr(WINE_WAVEINST* wwo, LPWAVEHDR lpWaveHdr) {
    wwo->lpPlayPtr = lpWaveHdr;

    if (!lpWaveHdr) return;

    if (lpWaveHdr->dwFlags & WHDR_BEGINLOOP) {
        if (wwo->lpLoopPtr) {
            WARN("Already in a loop. Discarding loop on this header (%p)\n", lpWaveHdr);
        } else {
            TRACE("Starting loop (%dx) with %p\n", lpWaveHdr->dwLoops, lpWaveHdr);
            wwo->lpLoopPtr = lpWaveHdr;
            /* Windows does not touch WAVEHDR.dwLoops,
             * so we need to make an internal copy */
            wwo->dwLoops = lpWaveHdr->dwLoops;
        }
    }
    wwo->dwPartialOffset = 0;
}

/**************************************************************************
 *                  wodPlayer_PlayPtrNext                       [internal]
 *
 * Advance the play pointer to the next waveheader, looping if required.
 */
static LPWAVEHDR wodPlayer_PlayPtrNext(WINE_WAVEINST* wwo) {
    LPWAVEHDR lpWaveHdr = wwo->lpPlayPtr;

    wwo->dwPartialOffset = 0;
    if ((lpWaveHdr->dwFlags & WHDR_ENDLOOP) && wwo->lpLoopPtr) {
        /* We're at the end of a loop, loop if required */
        if (--wwo->dwLoops > 0) {
            wwo->lpPlayPtr = wwo->lpLoopPtr;
        } else {
            /* Handle overlapping loops correctly */
            if (wwo->lpLoopPtr != lpWaveHdr && (lpWaveHdr->dwFlags & WHDR_BEGINLOOP)) {
                FIXME("Correctly handled case ? (ending loop buffer also starts a new loop)\n");
                /* shall we consider the END flag for the closing loop or for
                 * the opening one or for both ???
                 * code assumes for closing loop only
                 */
            } else {
                lpWaveHdr = lpWaveHdr->lpNext;
            }
            wwo->lpLoopPtr = NULL;
            wodPlayer_BeginWaveHdr(wwo, lpWaveHdr);
        }
    } else {
        /* We're not in a loop.  Advance to the next wave header */
        wodPlayer_BeginWaveHdr(wwo, lpWaveHdr = lpWaveHdr->lpNext);
    }

    return lpWaveHdr;
}

/**************************************************************************
 *                  wodPlayer_CheckReleasing                    [internal]
 *
 * Check to make sure that playback has not stalled. If stalled ask to reduce
 * the size of the buffer on the pulse server side.
 */
static void wodPlayer_CheckReleasing(WINE_WAVEINST *wwo) {
    LPWAVEHDR lpWaveHdr;

    if (wwo->buffer_attr.tlength == -1) {
        pa_threaded_mainloop_lock(PULSE_ml);
        if (!wwo->timing_info->playing) {

            /* Calculate how large a buffer the application has made so far */
            wwo->buffer_attr.tlength = 0;
	    wwo->buffer_attr.minreq = wwo->lpQueuePtr->dwBufferLength;
            for (lpWaveHdr = wwo->lpQueuePtr; lpWaveHdr; lpWaveHdr = lpWaveHdr->lpNext)
                wwo->buffer_attr.tlength += lpWaveHdr->dwBufferLength;

            WARN("Asking for new buffer target length of %llums (%u bytes)\n",
                pa_bytes_to_usec(wwo->buffer_attr.tlength, &wwo->sample_spec) / 1000,
                wwo->buffer_attr.tlength);

            /* Try and adjust the buffer attributes so that playback can start.
             * Because of bugs pa_stream_set_buffer_attr() does not work on started
             * streams for server version 0.9.11 to 0.9.14 */
            PULSE_WaitForOperation(pa_stream_set_buffer_attr(wwo->stream, &wwo->buffer_attr, PULSE_StreamSuccessCallback, wwo));
	    TRACE("Triggering stream and hoping for the best\n");
	    PULSE_WaitForOperation(pa_stream_trigger(wwo->stream, PULSE_StreamSuccessCallback, wwo));
        }
        pa_threaded_mainloop_unlock(PULSE_ml);
    }
}

/**************************************************************************
 *                  wodPlayer_NotifyCompletions                 [internal]
 *
 * Notifies the client of wavehdr completion starting from lpQueuePtr and
 * stopping when hitting an unwritten wavehdr, the beginning of a loop or a
 * wavehdr that has not been played, when referenced to the time parameter.
 */
static DWORD wodPlayer_NotifyCompletions(WINE_WAVEINST* wwo, BOOL force, pa_usec_t time) {
    LPWAVEHDR lpWaveHdr = wwo->lpQueuePtr;
    pa_usec_t wait;

    while (lpWaveHdr) {
        if (!force) {
            /* Start from lpQueuePtr and keep notifying until:
             * - we hit an unwritten wavehdr
             * - we hit the beginning of a running loop
             * - we hit a wavehdr which hasn't finished playing
             */
            if (lpWaveHdr == wwo->lpLoopPtr) { TRACE("loop %p\n", lpWaveHdr); return PULSE_INFINITE; }
            if (lpWaveHdr == wwo->lpPlayPtr) { TRACE("play %p\n", lpWaveHdr); return PULSE_INFINITE; }

            /* See if this data has been played, and if not, return when it will have been */
            wait = pa_bytes_to_usec(lpWaveHdr->reserved + lpWaveHdr->dwBufferLength, &wwo->sample_spec);
            if (wait >= time) {
                wait = ((wait - time) + (pa_usec_t)999) / (pa_usec_t)1000;
                return wait ?: 1;
            }
        }
	TRACE("Returning %p.[%i]\n", lpWaveHdr, (DWORD)lpWaveHdr->reserved);

        /* return the wavehdr */
        wwo->lpQueuePtr = lpWaveHdr->lpNext;
        lpWaveHdr->dwFlags &= ~WHDR_INQUEUE;
        lpWaveHdr->dwFlags |= WHDR_DONE;

        wodPlayer_NotifyClient(wwo, WOM_DONE, (DWORD)lpWaveHdr, 0);
        lpWaveHdr = wwo->lpQueuePtr;
    }
    /* No more wavehdrs */
    TRACE("Empty queue\n");
    return PULSE_INFINITE;
}

/**************************************************************************
 *                  wodPlayer_WriteMax                          [internal]
 *
 * Write either how much free space or how much data we have, depending on
 * which is less
 */
static DWORD wodPlayer_WriteMax(WINE_WAVEINST *wwo, size_t *space) {
    LPWAVEHDR lpWaveHdr = wwo->lpPlayPtr;
    size_t nbytes;

    nbytes = min(lpWaveHdr->dwBufferLength - wwo->dwPartialOffset, *space);

    TRACE("Writing wavehdr %p.%u[%u]\n", lpWaveHdr, wwo->dwPartialOffset, lpWaveHdr->dwBufferLength);
    pa_stream_write(wwo->stream, lpWaveHdr->lpData + wwo->dwPartialOffset, nbytes, NULL, 0, PA_SEEK_RELATIVE);

    /* Check to see if we wrote all of the wavehdr */
    if ((wwo->dwPartialOffset += nbytes) >= lpWaveHdr->dwBufferLength)
        wodPlayer_PlayPtrNext(wwo);

    *space -= nbytes;

    return nbytes;
}

/**************************************************************************
 *                  wodPlayer_Feed                              [internal]
 *
 * Feed as much sound data as we can into pulse using wodPlayer_WriteMax.
 * size_t space _must_ have come from either pa_stream_writable_size() or
 * the value from a stream write callback, as if it isn't you run the risk
 * of a buffer overflow in which audio data will be lost.
 */
static void wodPlayer_Feed(WINE_WAVEINST* wwo, size_t space) {

    if (!space || !wwo->stream || !PULSE_context ||
        pa_context_get_state(PULSE_context) != PA_CONTEXT_READY ||
        pa_stream_get_state(wwo->stream) != PA_STREAM_READY)
        return;

    pa_threaded_mainloop_lock(PULSE_ml);
    /* Feed from a partial wavehdr */
    if (wwo->lpPlayPtr && wwo->dwPartialOffset != 0)
        wodPlayer_WriteMax(wwo, &space);

    /* Feed wavehdrs until we run out of wavehdrs or buffer space */
    if (wwo->dwPartialOffset == 0 && wwo->lpPlayPtr) {
        do {
            wwo->lpPlayPtr->reserved = wwo->timing_info->write_index;
        } while (wodPlayer_WriteMax(wwo, &space) && wwo->lpPlayPtr && space > 0);
    }

    pa_threaded_mainloop_unlock(PULSE_ml);
}

/**************************************************************************
 *                  wodPlayer_Reset                             [internal]
 *
 * wodPlayer helper. Resets current output stream.
 */
static void wodPlayer_Reset(WINE_WAVEINST* wwo) {
    enum win_wm_message msg;
    DWORD param;
    HANDLE ev;

    TRACE("(%p)\n", wwo);

    /* Remove any buffer */
    wodPlayer_NotifyCompletions(wwo, TRUE, 0);

    wwo->lpPlayPtr = wwo->lpQueuePtr = wwo->lpLoopPtr = NULL;
    if (wwo->state != WINE_WS_PAUSED)
        wwo->state = WINE_WS_STOPPED;

    wwo->dwPartialOffset = 0;

    if (!wwo->stream ||
        !PULSE_context ||
        pa_context_get_state(PULSE_context) != PA_CONTEXT_READY ||
        pa_stream_get_state(wwo->stream) != PA_STREAM_READY) {
        return;
    }

    pa_threaded_mainloop_lock(PULSE_ml);

    /* Flush the output buffer of written data*/
    PULSE_WaitForOperation(pa_stream_flush(wwo->stream, PULSE_StreamSuccessCallback, NULL));

    /* Reset the written byte count as some data may have been flushed */
    if (wwo->timing_info->write_index_corrupt)
        PULSE_WaitForOperation(pa_stream_update_timing_info(wwo->stream, PULSE_StreamSuccessCallback, wwo));

    wwo->dwLastReset = wwo->timing_info->write_index;

    /* Return all pending headers in queue */
    EnterCriticalSection(&wwo->msgRing.msg_crst);
    while (PULSE_RetrieveRingMessage(&wwo->msgRing, &msg, &param, &ev)) {
        if (msg != WINE_WM_HEADER) {
            SetEvent(ev);
            continue;
        }
        ((LPWAVEHDR)param)->dwFlags &= ~WHDR_INQUEUE;
        ((LPWAVEHDR)param)->dwFlags |= WHDR_DONE;
        wodPlayer_NotifyClient(wwo, WOM_DONE, param, 0);
    }
    PULSE_ResetRingMessage(&wwo->msgRing);
    LeaveCriticalSection(&wwo->msgRing.msg_crst);

    pa_threaded_mainloop_unlock(PULSE_ml);
}

/**************************************************************************
 *                  wodPlayer_GetStreamTime                     [internal]
 *
 * Returns how many microseconds into the playback the audio stream is. Does
 * not reset to 0 on Reset() calls. Better than pa_stream_get_time() as it is
 * more constant.
 */
static pa_usec_t wodPlayer_GetStreamTime(WINE_WAVEINST *wwo) {
    pa_usec_t time, temp;
    const pa_timing_info *t;

    t = wwo->timing_info;

    pa_threaded_mainloop_lock(PULSE_ml);

    time = pa_bytes_to_usec(t->read_index, &wwo->sample_spec);
    if (t->read_index_corrupt) {
        WARN("Read index corrupt?!\n");
        pa_threaded_mainloop_unlock(PULSE_ml);
        return time;
    }

    if (t->playing) {
        time += pa_timeval_age(&t->timestamp);
        temp = t->transport_usec + t->configured_sink_usec;
        if (temp > wwo->buffer_attr.tlength) temp = wwo->buffer_attr.tlength;
        if (time > temp) time -= temp; else time = 0;
    }

    /* Make sure we haven't claimed to have played more than we have written */
    temp = pa_bytes_to_usec(t->write_index, &wwo->sample_spec);
    if (time > temp) time = temp;

    /* No queued buffer shows an underrun, so we lie */
    if (!wwo->lpQueuePtr) time = temp;

    pa_threaded_mainloop_unlock(PULSE_ml);

    return time;
}

/**************************************************************************
 *                  wodPlayer_ProcessMessages                   [internal]
 */
static void wodPlayer_ProcessMessages(WINE_WAVEINST* wwo) {
    LPWAVEHDR           lpWaveHdr;
    enum win_wm_message msg;
    DWORD               param;
    HANDLE              ev;

    while (PULSE_RetrieveRingMessage(&wwo->msgRing, &msg, &param, &ev)) {
        TRACE("Received %s %x\n", PULSE_getCmdString(msg), param);

        switch (msg) {
        case WINE_WM_PAUSING:
            wwo->state = WINE_WS_PAUSED;
            pa_threaded_mainloop_lock(PULSE_ml);
            PULSE_WaitForOperation(pa_stream_cork(wwo->stream, 1, PULSE_StreamSuccessCallback, wwo));
            pa_threaded_mainloop_unlock(PULSE_ml);
            SetEvent(ev);
            break;

        case WINE_WM_RESTARTING:
            if (wwo->state == WINE_WS_PAUSED) {
                wwo->state = WINE_WS_PLAYING;
                pa_threaded_mainloop_lock(PULSE_ml);
                PULSE_WaitForOperation(pa_stream_cork(wwo->stream, 0, PULSE_StreamSuccessCallback, wwo));
                /* If the serverside buffer was near full before pausing, we
                 * need to have space to write soon, so force playback start */
                PULSE_WaitForOperation(pa_stream_trigger(wwo->stream, PULSE_StreamSuccessCallback, wwo));
                pa_threaded_mainloop_unlock(PULSE_ml);
            }
            SetEvent(ev);
            break;

        case WINE_WM_HEADER:
            lpWaveHdr = (LPWAVEHDR)param;
            /* insert buffer at the end of queue */
            {
                LPWAVEHDR *wh;
                for (wh = &(wwo->lpQueuePtr); *wh; wh = &((*wh)->lpNext));
                *wh = lpWaveHdr;
            }

            if (!wwo->lpPlayPtr)
                wodPlayer_BeginWaveHdr(wwo,lpWaveHdr);
            if (wwo->state == WINE_WS_STOPPED)
                wwo->state = WINE_WS_PLAYING;

            wodPlayer_Feed(wwo, pa_stream_writable_size(wwo->stream));
            SetEvent(ev);
            break;

        case WINE_WM_RESETTING:
            wodPlayer_Reset(wwo);
            SetEvent(ev);
            break;

        case WINE_WM_BREAKLOOP:
            if (wwo->state == WINE_WS_PLAYING && wwo->lpLoopPtr != NULL)
                /* ensure exit at end of current loop */
                wwo->dwLoops = 1;
            SetEvent(ev);
            break;

        case WINE_WM_FEED: /* Sent by the pulse thread */
            wodPlayer_Feed(wwo, pa_stream_writable_size(wwo->stream));
            SetEvent(ev);
            break;

        case WINE_WM_XRUN: /* Sent by the pulse thread */
            WARN("Trying to recover from underrun.\n");
            /* Return all the queued wavehdrs, so the app will send more data */
            wodPlayer_NotifyCompletions(wwo, FALSE, (pa_usec_t)-1);

            SetEvent(ev);
            break;

        case WINE_WM_CLOSING:
            wwo->hThread = NULL;
            wwo->state = WINE_WS_CLOSED;
            /* sanity check: this should not happen since the device must have been reset before */
            if (wwo->lpQueuePtr || wwo->lpPlayPtr) ERR("out of sync\n");
            SetEvent(ev);
            TRACE("Thread exiting.\n");
            ExitThread(0);
            /* shouldn't go here */

        default:
            FIXME("unknown message %d\n", msg);
            break;
        }
    }
}

/**************************************************************************
 *                  wodPlayer                                   [internal]
 *
 *  The thread which is responsible for returning WaveHdrs via DriverCallback,
 *  the writing of queued WaveHdrs, and all pause / reset stream management.
 */
static DWORD CALLBACK wodPlayer(LPVOID lpParam) {
    WINE_WAVEINST *wwo = (WINE_WAVEINST*)lpParam;
    DWORD         dwSleepTime = INFINITE;
    int64_t       delta_write;

    wwo->state = WINE_WS_STOPPED;
    SetEvent(wwo->hStartUpEvent);

    /* Wait for the shortest time before an action is required. If there are
     * no pending actions, wait forever for a command. */
    for (;;) {
        TRACE("Waiting %u ms\n", dwSleepTime);
        PULSE_WaitRingMessage(&wwo->msgRing, dwSleepTime);

        delta_write = wwo->timing_info->write_index;
        wodPlayer_ProcessMessages(wwo);

        /* Check for a stall situaiton */
        if (delta_write == wwo->timing_info->write_index
            && wwo->lpQueuePtr && !wwo->lpPlayPtr
            && wwo->state != WINE_WS_STOPPED)
            wodPlayer_CheckReleasing(wwo);

        /* If there is audio playing, return headers and get next timeout */
        if (wwo->state == WINE_WS_PLAYING) {
            dwSleepTime = wodPlayer_NotifyCompletions(wwo, FALSE, wodPlayer_GetStreamTime(wwo));
        } else
            dwSleepTime = INFINITE;
    }

    return 0;
}

/**************************************************************************
 *                              wodOpen                         [internal]
 *
 * Create a new pa_stream and connect it to a sink while creating a new
 * WINE_WAVEINST to represent the device to the windows application.
 */
static DWORD wodOpen(WORD wDevID, DWORD_PTR lpdwUser, LPWAVEOPENDESC lpDesc, DWORD dwFlags) {
    WINE_WAVEDEV *wdo;
    WINE_WAVEINST *wwo = NULL;
    DWORD ret = MMSYSERR_NOERROR;

    TRACE("(%u, %p, %08X);\n", wDevID, lpDesc, dwFlags);
    if (lpDesc == NULL) {
        WARN("Invalid Parameter!\n");
        return MMSYSERR_INVALPARAM;
    }

    if (wDevID >= PULSE_WodNumDevs) {
        WARN("Asked for device %d, but only %d known!\n", wDevID, PULSE_WodNumDevs);
        return MMSYSERR_BADDEVICEID;
    }
    wdo = &WOutDev[wDevID];

    wwo = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WINE_WAVEINST));
    if (!wwo) {
        WARN("Out of memory?!\n");
        return MMSYSERR_NOMEM;
    }
    *(WINE_WAVEINST**)lpdwUser = wwo;

    /* check to see if format is supported and make pa_sample_spec struct */
    if (!PULSE_SetupFormat(lpDesc->lpFormat, &wwo->sample_spec)) {
        WARN("Bad format: tag=%04X nChannels=%d nSamplesPerSec=%d !\n",
             lpDesc->lpFormat->wFormatTag, lpDesc->lpFormat->nChannels,
             lpDesc->lpFormat->nSamplesPerSec);
        ret = WAVERR_BADFORMAT;
        goto exit;
    }

    /* Check to see if this was just a query */
    if (dwFlags & WAVE_FORMAT_QUERY) {
        TRACE("Query format: tag=%04X nChannels=%d nSamplesPerSec=%d !\n",
             lpDesc->lpFormat->wFormatTag, lpDesc->lpFormat->nChannels,
             lpDesc->lpFormat->nSamplesPerSec);
        ret = MMSYSERR_NOERROR;
        goto exit;
    }

    if (TRACE_ON(wave)) {
        char t[PA_SAMPLE_SPEC_SNPRINT_MAX];
        pa_sample_spec_snprint(t, sizeof(t), &wwo->sample_spec);
        TRACE("Sample spec '%s'\n", t);
    }

    wwo->wFlags = HIWORD(dwFlags & CALLBACK_TYPEMASK);
    wwo->waveDesc = *lpDesc;
    PULSE_InitRingMessage(&wwo->msgRing);

    wwo->stream = pa_stream_new(PULSE_context, "WaveOut", &wwo->sample_spec, NULL);
    /* If server doesn't support sample_spec, it will error out here (re: 24bit) */
    if (!wwo->stream) {
        ret = WAVERR_BADFORMAT;
        goto exit;
    }

    /* Setup callbacks */
    pa_stream_set_write_callback        (wwo->stream, PULSE_StreamRequestCallback,      wwo);
    pa_stream_set_state_callback        (wwo->stream, PULSE_StreamStateCallback,        wwo);
    pa_stream_set_underflow_callback    (wwo->stream, PULSE_StreamUnderflowCallback,    wwo);
    pa_stream_set_moved_callback        (wwo->stream, PULSE_StreamMovedCallback,        wwo);
    pa_stream_set_suspended_callback    (wwo->stream, PULSE_StreamSuspendedCallback,    wwo);

    /* Blank Buffer Attributes */
    wwo->buffer_attr.prebuf = (uint32_t)-1;
    wwo->buffer_attr.tlength = (uint32_t)-1;
    wwo->buffer_attr.minreq = (uint32_t)-1;
    wwo->buffer_attr.maxlength = (uint32_t)-1;

    /* Try and connect */
    TRACE("Connecting stream for playback on %s.\n", wdo->device_name);
    pa_threaded_mainloop_lock(PULSE_ml);
    pa_stream_connect_playback(wwo->stream, wdo->device_name, &wwo->buffer_attr, PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_ADJUST_LATENCY, NULL, NULL);

    /* Wait for connection */
    for (;;) {
        pa_context_state_t cstate = pa_context_get_state(PULSE_context);
        pa_stream_state_t sstate = pa_stream_get_state(wwo->stream);

        if (cstate == PA_CONTEXT_FAILED || cstate == PA_CONTEXT_TERMINATED ||
            sstate == PA_STREAM_FAILED || sstate == PA_STREAM_TERMINATED) {
            ERR("Failed to connect stream context object: %s\n", pa_strerror(pa_context_errno(PULSE_context)));
            pa_threaded_mainloop_unlock(PULSE_ml);
            ret = MMSYSERR_NODRIVER;
            goto exit;
        }

        if (sstate == PA_STREAM_READY)
            break;

        pa_threaded_mainloop_wait(PULSE_ml);
    }
    TRACE("(%p)->stream connected for playback.\n", wwo);

    /* Get the pa_timing_info structure */
    PULSE_WaitForOperation(pa_stream_update_timing_info(wwo->stream, PULSE_StreamSuccessCallback, wwo));
    wwo->timing_info = pa_stream_get_timing_info(wwo->stream);
    assert(wwo->timing_info);
    pa_threaded_mainloop_unlock(PULSE_ml);

    /* Create and start the wodPlayer() thread to manage playback */
    wwo->hStartUpEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    wwo->hThread = CreateThread(NULL, 0, wodPlayer, (LPVOID)wwo, 0, &(wwo->dwThreadID));
    if (wwo->hThread)
        SetThreadPriority(wwo->hThread, THREAD_PRIORITY_TIME_CRITICAL);
    else {
        ERR("Thread creation for the wodPlayer failed!\n");
        ret = MMSYSERR_NOMEM;
        goto exit;
    }
    WaitForSingleObject(wwo->hStartUpEvent, INFINITE);
    CloseHandle(wwo->hStartUpEvent);
    wwo->hStartUpEvent = INVALID_HANDLE_VALUE;

    return wodPlayer_NotifyClient (wwo, WOM_OPEN, 0L, 0L);

exit:
    if (!wwo)
        return ret;

    if (wwo->hStartUpEvent != INVALID_HANDLE_VALUE)
        CloseHandle(wwo->hStartUpEvent);

    if (wwo->msgRing.ring_buffer_size > 0)
            PULSE_DestroyRingMessage(&wwo->msgRing);

    if (wwo->stream) {
        if (pa_stream_get_state(wwo->stream) == PA_STREAM_READY)
            pa_stream_disconnect(wwo->stream);
        pa_stream_unref(wwo->stream);
        wwo->stream = NULL;
    }
    HeapFree(GetProcessHeap(), 0, wwo);

    return ret;
}

/**************************************************************************
 *                              wodClose                        [internal]
 */
static DWORD wodClose(WINE_WAVEINST *wwo) {
    DWORD ret;

    TRACE("(%p);\n", wwo);
    if (!wwo) {
        WARN("Stream instance invalid.\n");
        return MMSYSERR_INVALHANDLE;
    }

    if (wwo->state != WINE_WS_FAILED) {
        if (wwo->lpQueuePtr && wwo->lpPlayPtr) {
            WARN("buffers still playing !\n");
            return WAVERR_STILLPLAYING;
        }

        pa_threaded_mainloop_lock(PULSE_ml);
        PULSE_WaitForOperation(pa_stream_drain(wwo->stream, PULSE_StreamSuccessCallback, NULL));
        pa_stream_disconnect(wwo->stream);
        pa_threaded_mainloop_unlock(PULSE_ml);

        if (wwo->hThread != INVALID_HANDLE_VALUE)
            PULSE_AddRingMessage(&wwo->msgRing, WINE_WM_CLOSING, 0, TRUE);

        PULSE_DestroyRingMessage(&wwo->msgRing);
    }

    if (wwo->stream)
        pa_stream_unref(wwo->stream);
    ret = wodPlayer_NotifyClient(wwo, WOM_CLOSE, 0L, 0L);

    HeapFree(GetProcessHeap(), 0, wwo);

    return ret;
}

/**************************************************************************
 *                              wodWrite                        [internal]
 */
static DWORD wodWrite(WINE_WAVEINST *wwo, LPWAVEHDR lpWaveHdr, DWORD dwSize) {
    if (!wwo || wwo->state == WINE_WS_FAILED) {
        WARN("Stream instance invalid.\n");
        return MMSYSERR_INVALHANDLE;
    }

    if (lpWaveHdr->lpData == NULL || !(lpWaveHdr->dwFlags & WHDR_PREPARED))
        return WAVERR_UNPREPARED;

    if (lpWaveHdr->dwFlags & WHDR_INQUEUE)
        return WAVERR_STILLPLAYING;

    lpWaveHdr->dwFlags &= ~WHDR_DONE;
    lpWaveHdr->dwFlags |= WHDR_INQUEUE;
    lpWaveHdr->lpNext = 0;
    lpWaveHdr->reserved = 0;

    PULSE_AddRingMessage(&wwo->msgRing, WINE_WM_HEADER, (DWORD)lpWaveHdr, FALSE);
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 *                              wodPause                        [internal]
 */
static DWORD wodPause(WINE_WAVEINST *wwo) {
    if (!wwo || wwo->state == WINE_WS_FAILED) {
        WARN("Stream instance invalid.\n");
        return MMSYSERR_INVALHANDLE;
    }

    PULSE_AddRingMessage(&wwo->msgRing, WINE_WM_PAUSING, 0, TRUE);
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 *                              wodGetPosition                  [internal]
 */
static DWORD wodGetPosition(WINE_WAVEINST *wwo, LPMMTIME lpTime, DWORD uSize) {
    pa_usec_t   time, temp;

    if (!wwo || wwo->state == WINE_WS_FAILED) {
        WARN("Stream instance invalid.\n");
        return MMSYSERR_INVALHANDLE;
    }

    if (lpTime == NULL) return MMSYSERR_INVALPARAM;

    time = wodPlayer_GetStreamTime(wwo);

    temp = pa_bytes_to_usec(wwo->dwLastReset, &wwo->sample_spec);
    if (time > temp) time -= temp; else time = 0;

    return PULSE_UsecToMMTime(time, lpTime, &wwo->sample_spec);
}
/**************************************************************************
 *                              wodBreakLoop                    [internal]
 */
static DWORD wodBreakLoop(WINE_WAVEINST *wwo) {
    if (!wwo || wwo->state == WINE_WS_FAILED) {
        WARN("Stream instance invalid.\n");
        return MMSYSERR_INVALHANDLE;
    }

    PULSE_AddRingMessage(&wwo->msgRing, WINE_WM_BREAKLOOP, 0, TRUE);
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 *                              wodGetDevCaps                   [internal]
 */
static DWORD wodGetDevCaps(DWORD wDevID, LPWAVEOUTCAPSW lpCaps, DWORD dwSize) {
    TRACE("(%u, %p, %u);\n", wDevID, lpCaps, dwSize);

    if (lpCaps == NULL) return MMSYSERR_NOTENABLED;

    if (wDevID >= PULSE_WodNumDevs) {
        TRACE("Asked for device %d, but only %d known!\n", wDevID, PULSE_WodNumDevs);
        return MMSYSERR_INVALHANDLE;
    }

    memcpy(lpCaps, &(WOutDev[wDevID].caps.out), min(dwSize, sizeof(*lpCaps)));
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 *                              wodGetNumDevs                   [internal]
 * Context-sanity check here, as if we respond with 0, WINE will move on
 * to the next waveout driver.
 */
static DWORD wodGetNumDevs(void) {
    if (!PULSE_ml || !PULSE_context || pa_context_get_state(PULSE_context) != PA_CONTEXT_READY)
        return 0;

    return PULSE_WodNumDevs;
}

/**************************************************************************
 *                              wodGetVolume                    [internal]
 */
static DWORD wodGetVolume(WINE_WAVEINST *wwo, LPDWORD lpdwVol) {
    double   value1, value2;
    DWORD   wleft, wright;

    if (!wwo || wwo->state == WINE_WS_FAILED) {
        WARN("Stream instance invalid.\n");
        return MMSYSERR_INVALHANDLE;
    }

    TRACE("(%p, %p);\n", wwo, lpdwVol);

    if (lpdwVol == NULL)
        return MMSYSERR_NOTENABLED;

    pa_threaded_mainloop_lock(PULSE_ml);
    if (wwo->stream && PULSE_context && pa_context_get_state(PULSE_context) == PA_CONTEXT_READY &&
        pa_stream_get_state(wwo->stream) == PA_STREAM_READY) {
        PULSE_WaitForOperation(pa_context_get_sink_input_info(PULSE_context, pa_stream_get_index(wwo->stream), WAVEOUT_SinkInputInfoCallback, wwo));
    }
    pa_threaded_mainloop_unlock(PULSE_ml);


    if (wwo->volume.channels == 2) {
        value1 = pa_sw_volume_to_linear(wwo->volume.values[0]);
        value2 = pa_sw_volume_to_linear(wwo->volume.values[1]);
    } else {
        value1 = pa_sw_volume_to_linear(pa_cvolume_avg(&wwo->volume));
        value2 = value1;
    }

    wleft = 0xFFFFl * value1;
    wright = 0xFFFFl * value2;

    if (wleft > 0xFFFFl)
        wleft = 0xFFFFl;
    if (wright > 0xFFFFl)
        wright = 0xFFFFl;

    *lpdwVol = (WORD)wleft + (WORD)(wright << 16);

    return MMSYSERR_NOERROR;
}

/**************************************************************************
 *                              wodSetVolume                    [internal]
 */
static DWORD wodSetVolume(WINE_WAVEINST *wwo, DWORD dwParam1) {
    double value1, value2;

    TRACE("(%p, %08X);\n", wwo, dwParam1);
    if (!wwo || wwo->state == WINE_WS_FAILED) {
        WARN("Stream instance invalid.\n");
        return MMSYSERR_INVALHANDLE;
    }

    value1 = (double)LOWORD(dwParam1)/(double)0xFFFFl;
    value2 = (double)HIWORD(dwParam1)/(double)0xFFFFl;

    if (wwo->sample_spec.channels == 2) {
        wwo->volume.channels = 2;
        wwo->volume.values[0] = pa_sw_volume_from_linear(value1);
        wwo->volume.values[1] = pa_sw_volume_from_linear(value2);
    } else {
        if (value1 != value2) FIXME("Non-stereo streams can't pan!\n");
        wwo->volume.channels = wwo->sample_spec.channels;
        pa_cvolume_set(&wwo->volume, wwo->volume.channels, pa_sw_volume_from_linear(value1 > value2 ? value1 : value2));
    }

    if (TRACE_ON(wave)) {
        char s[PA_CVOLUME_SNPRINT_MAX];
        pa_cvolume_snprint(s, PA_CVOLUME_SNPRINT_MAX, &wwo->volume);
        TRACE("%s\n", s);
    }

    pa_threaded_mainloop_lock(PULSE_ml);
    if (!wwo->stream || !PULSE_context || pa_context_get_state(PULSE_context) != PA_CONTEXT_READY ||
        pa_stream_get_state(wwo->stream) != PA_STREAM_READY || !pa_cvolume_valid(&wwo->volume)) {
        pa_threaded_mainloop_unlock(PULSE_ml);
        return MMSYSERR_NOERROR;
    }

    PULSE_WaitForOperation(pa_context_set_sink_input_volume(PULSE_context,
            pa_stream_get_index(wwo->stream), &wwo->volume,
            PULSE_ContextSuccessCallback, wwo));
    pa_threaded_mainloop_unlock(PULSE_ml);
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 *                              wodRestart                      [internal]
 */
static DWORD wodRestart(WINE_WAVEINST *wwo) {
    if (!wwo || wwo->state == WINE_WS_FAILED) {
        WARN("Stream instance invalid.\n");
        return MMSYSERR_INVALHANDLE;
    }

    if (wwo->state == WINE_WS_PAUSED)
        PULSE_AddRingMessage(&wwo->msgRing, WINE_WM_RESTARTING, 0, TRUE);
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 *                              wodReset                        [internal]
 */
static DWORD wodReset(WINE_WAVEINST *wwo) {
    if (!wwo || wwo->state == WINE_WS_FAILED) {
        WARN("Stream instance invalid.\n");
        return MMSYSERR_INVALHANDLE;
    }

    PULSE_AddRingMessage(&wwo->msgRing, WINE_WM_RESETTING, 0, TRUE);
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 *                  wodDevInterfaceSize                         [internal]
 */
static DWORD wodDevInterfaceSize(UINT wDevID, LPDWORD dwParam1) {

    *dwParam1 = MultiByteToWideChar(CP_UTF8, 0, WOutDev[wDevID].interface_name, -1, NULL, 0) * sizeof(WCHAR);
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 *                  wodDevInterface                             [internal]
 */
static DWORD wodDevInterface(UINT wDevID, PWCHAR dwParam1, DWORD dwParam2) {
    if (dwParam2 >= MultiByteToWideChar(CP_UTF8, 0, WOutDev[wDevID].interface_name, -1,
                                        NULL, 0 ) * sizeof(WCHAR))
    {
        MultiByteToWideChar(CP_UTF8, 0, WOutDev[wDevID].interface_name, -1,
                            dwParam1, dwParam2 / sizeof(WCHAR));
        return MMSYSERR_NOERROR;
    }
    return MMSYSERR_INVALPARAM;
}

DWORD wodDsDesc(UINT wDevID, PDSDRIVERDESC desc) {
    TRACE("(%u, %p)\n", wDevID, desc);
    *desc = WOutDev[wDevID].ds_desc;
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 *                  wodMessage (WINEPULSE.@)
 */
DWORD WINAPI PULSE_wodMessage(UINT wDevID, UINT wMsg, DWORD_PTR dwUser, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {

    switch (wMsg) {

    case DRVM_INIT:
    case DRVM_EXIT:
    case DRVM_ENABLE:
    case DRVM_DISABLE:
        return 0;

    /* WaveOut Playback related functions */
    case WODM_OPEN:         return wodOpen          (wDevID, dwUser, (LPWAVEOPENDESC)dwParam1, dwParam2);
    case WODM_CLOSE:        return wodClose         ((WINE_WAVEINST*)dwUser);
    case WODM_WRITE:        return wodWrite         ((WINE_WAVEINST*)dwUser, (LPWAVEHDR)dwParam1, dwParam2);
    case WODM_PAUSE:        return wodPause         ((WINE_WAVEINST*)dwUser);
    case WODM_GETPOS:       return wodGetPosition   ((WINE_WAVEINST*)dwUser, (LPMMTIME)dwParam1, dwParam2);
    case WODM_BREAKLOOP:    return wodBreakLoop     ((WINE_WAVEINST*)dwUser);
    case WODM_RESTART:      return wodRestart       ((WINE_WAVEINST*)dwUser);
    case WODM_RESET:        return wodReset         ((WINE_WAVEINST*)dwUser);

    case WODM_GETVOLUME:    return wodGetVolume     ((WINE_WAVEINST*)dwUser, (LPDWORD)dwParam1);
    case WODM_SETVOLUME:    return wodSetVolume     ((WINE_WAVEINST*)dwUser, dwParam1);

    case WODM_PREPARE:
    case WODM_UNPREPARE:

    case WODM_GETPITCH:
    case WODM_SETPITCH:

    case WODM_GETPLAYBACKRATE:
    case WODM_SETPLAYBACKRATE:
        return MMSYSERR_NOTSUPPORTED;

    /* Device enumeration, directsound and capabilities */
    case WODM_GETDEVCAPS:   return wodGetDevCaps    (wDevID, (LPWAVEOUTCAPSW)dwParam1, dwParam2);
    case WODM_GETNUMDEVS:   return wodGetNumDevs    ();
    case DRV_QUERYDEVICEINTERFACESIZE: return wodDevInterfaceSize   (wDevID, (LPDWORD)dwParam1);
    case DRV_QUERYDEVICEINTERFACE:     return wodDevInterface       (wDevID, (PWCHAR)dwParam1, dwParam2);
    case DRV_QUERYDSOUNDIFACE:  return MMSYSERR_NOTSUPPORTED;
    case DRV_QUERYDSOUNDDESC:   return wodDsDesc    (wDevID, (PDSDRIVERDESC)dwParam1);

    default:
        FIXME("unknown message %d!\n", wMsg);
    }
    return MMSYSERR_NOTSUPPORTED;
}

#else /* !HAVE_PULSEAUDIO */

/**************************************************************************
 *                  wodMessage (WINEPULSE.@)
 */
DWORD WINAPI PULSE_wodMessage(WORD wDevID, WORD wMsg, DWORD dwUser,
                              DWORD dwParam1, DWORD dwParam2) {
    FIXME("(%u, %04X, %08X, %08X, %08X):stub\n", wDevID, wMsg, dwUser,
          dwParam1, dwParam2);
    return MMSYSERR_NOTENABLED;
}

#endif /* HAVE_PULSEAUDIO */
