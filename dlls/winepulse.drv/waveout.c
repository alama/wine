/*
 * Wine Driver for PulseAudio - WaveOut Functionality
 * http://pulseaudio.org/
 *
 * Copyright 2002	Eric Pouech
 *	     2002	Marco Pietrobono
 *	     2003	Christian Costa
 *	     2006-2007	Maarten Lankhorst
 *	     2008	Arthur Taylor (PulseAudio version)
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

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winnls.h"
#include "winerror.h"
#include "mmddk.h"
#include "mmreg.h"
#include "ks.h"
#include "ksguid.h"
#include "ksmedia.h"
#include "dsound.h"
#include "dsdriver.h"

#include <winepulse.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wave);

#if HAVE_PULSEAUDIO

/* state diagram for waveOut writing:
 *
 * +---------+-------------+---------------+---------------------------------+
 * |  state  |  function   |     event     |            new state	     |
 * +---------+-------------+---------------+---------------------------------+
 * |	     | open()	   |		   | STOPPED		       	     |
 * | PAUSED  | write()	   | 		   | PAUSED		       	     |
 * | STOPPED | write()	   | <thrd create> | PLAYING		  	     |
 * | PLAYING | write()	   | HEADER        | PLAYING		  	     |
 * | (other) | write()	   | <error>       |		       		     |
 * | (any)   | pause()	   | PAUSING	   | PAUSED		       	     |
 * | PAUSED  | restart()   | RESTARTING    | PLAYING (if no thrd => STOPPED) |
 * | PAUSED  | reset()     | RESETTING     | PAUSED                          |
 * | (other) | reset()	   | RESETTING     | STOPPED		      	     |
 * | (any)   | close()	   | CLOSING	   | CLOSED		      	     |
 * +---------+-------------+---------------+---------------------------------+
 */

/*
 * - It is currently unknown if pausing in a loop works the same as expected.
 */

/*======================================================================*
 *                  WAVE OUT specific PulseAudio Callbacks		*
 *======================================================================*/

#if HAVE_PULSEAUDIO_0_9_11
/**************************************************************************
 * 			PULSE_started_callback			[internal]
 *
 * Called by the pulse mainloop whenever stream playback resumes after an
 * underflow or an initial start
 */
static void PULSE_started_callback(pa_stream *s, void *userdata) {
    WINE_WAVEINST *wwo = (WINE_WAVEINST*)userdata;
    assert(s && wwo);

    TRACE("Audio flowing.\n");

    if (wwo->hThread != INVALID_HANDLE_VALUE && wwo->msgRing.ring_buffer_size) {
	PULSE_AddRingMessage(&wwo->msgRing, WINE_WM_STARTING, 0, FALSE);
    }
}
#else /* HAVE_PULSEAUDIO_0_9_11 */
/**************************************************************************
 * 			PULSE_timing_info_update_callback	[internal]
 *
 * Called by the pulse mainloop whenever the timing info gets updated, we
 * use this to send the started signal */
static void PULSE_timing_info_update_callback(pa_stream *s, void *userdata) {
    WINE_WAVEINST *wwo = (WINE_WAVEINST*)userdata;
    assert(s && wwo);

    if (wwo->is_buffering && wwo->timing_info && wwo->timing_info->playing) {
        TRACE("Audio flowing.\n");

        if (wwo->hThread != INVALID_HANDLE_VALUE && wwo->msgRing.ring_buffer_size)
	    PULSE_AddRingMessage(&wwo->msgRing, WINE_WM_STARTING, 0, FALSE);
    }
}
#endif

/**************************************************************************
 * 			PULSE_suspended_callback		[internal]
 *
 * Called by the pulse mainloop any time stream playback is intentionally
 * suspended or resumed from being suspended.
 */
static void PULSE_suspended_callback(pa_stream *s, void *userdata) {
    WINE_WAVEINST *wwo = (WINE_WAVEINST*)userdata;
    assert(s && wwo);

    /* Currently we handle this kinda like an underrun. Perhaps we should
     * tell the client somehow so it doesn't just hang? */

    if (!pa_stream_is_suspended(s) && wwo->hThread != INVALID_HANDLE_VALUE && wwo->msgRing.ring_buffer_size > 0)
	PULSE_AddRingMessage(&wwo->msgRing, WINE_WM_XRUN, 0, FALSE);
}

/**************************************************************************
 * 			PULSE_underrun_callback			[internal]
 *
 * Called by the pulse mainloop when the prebuf runs out of data.
 */
static void PULSE_underrun_callback(pa_stream *s, void *userdata) {
    WINE_WAVEINST *wwo = (WINE_WAVEINST*)userdata;
    assert(s && wwo);

    /* If we aren't playing, don't care ^_^ */
    if (wwo->state != WINE_WS_PLAYING) return;

    TRACE("Underrun occurred.\n");

    if (wwo->hThread != INVALID_HANDLE_VALUE && wwo->msgRing.ring_buffer_size > 0);
        PULSE_AddRingMessage(&wwo->msgRing, WINE_WM_XRUN, 0, FALSE);
}

/*======================================================================*
 *                  "Low level" WAVE OUT implementation			*
 *======================================================================*/

/**************************************************************************
 * 			wodNotifyClient			[internal]
 */
static DWORD wodNotifyClient(WINE_WAVEINST* wwo, WORD wMsg, DWORD dwParam1, DWORD dwParam2) {
    TRACE("wMsg = 0x%04x dwParm1 = %04X dwParam2 = %04X\n", wMsg, dwParam1, dwParam2);

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
 * 				wodPlayer_BeginWaveHdr          [internal]
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
 * 				wodPlayer_PlayPtrNext	        [internal]
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
 * 				wodPlayer_CheckReleasing	[internal]
 *
 * Check to see if data has stopped being fed to us before actual playback
 * starts. In this case the app wants a smaller buffer than pulse currently is
 * offering. We ignore this and just start the releasing reference. The
 * downside is that there is a latency unknown to the app. The upside is that
 * pulse is good at managing latencies
 */
static void wodPlayer_CheckReleasing(WINE_WAVEINST *wwo) {
    LPWAVEHDR lpWaveHdr = wwo->lpQueuePtr;

    /* If we aren't playing, (only valid on pulse >= 0.9.11) and we have 
     * queued data and we aren't relasing, start releasing if either:
     * - We have stopped being given wavehdrs, or
     * - We have 2s worth of audio built up.*/
    if (wwo->is_buffering && lpWaveHdr && !wwo->is_releasing &&
	(pa_bytes_to_usec(lpWaveHdr->dwBufferLength, &wwo->sample_spec)/2 < pa_timeval_age(&wwo->last_header)||
	wwo->timing_info->write_index - lpWaveHdr->reserved > pa_bytes_per_second(&wwo->sample_spec)*2)) {

	pa_gettimeofday(&wwo->started_releasing);
	wwo->is_releasing = TRUE;
	wwo->releasing_offset = wwo->lpQueuePtr->reserved;
	TRACE("Starting to release early: %u\n", wwo->releasing_offset);
    }
}

/**************************************************************************
 * 				wodPlayer_NotifyCompletions	[internal]
 *
 * Notifies and remove from queue all wavehdrs which have been played to
 * the speaker based on a reference time of (theoretical) playback start. If
 * force is true, we notify all wavehdrs and remove them all from the queue
 * even if they are unplayed or part of a loop. We return the time to wait
 * until the next wavehdr needs to be freed, or INFINITE if there are no more
 * wavehdrs.
 */
static DWORD wodPlayer_NotifyCompletions(WINE_WAVEINST* wwo, BOOL force) {
    LPWAVEHDR	lpWaveHdr;
    pa_usec_t	time;
    pa_usec_t	wait;

    time = pa_bytes_to_usec(wwo->releasing_offset, &wwo->sample_spec);
    if (wwo->is_releasing)
	time += pa_timeval_age(&wwo->started_releasing);

    for (lpWaveHdr = wwo->lpQueuePtr; lpWaveHdr; lpWaveHdr = wwo->lpQueuePtr) {
	if (!force) {
	    /* Start from lpQueuePtr and keep notifying until:
	     * - we hit an unwritten wavehdr
	     * - we hit the beginning of a running loop
	     * - we hit a wavehdr which hasn't finished playing
	     */
            if (lpWaveHdr == wwo->lpPlayPtr) { TRACE("play %p\n", lpWaveHdr); return INFINITE; }
	    if (lpWaveHdr == wwo->lpLoopPtr) { TRACE("loop %p\n", lpWaveHdr); return INFINITE; }

	    /* See if this data has been played, and if not, return when it will have been */
	    wait = pa_bytes_to_usec(lpWaveHdr->reserved + lpWaveHdr->dwBufferLength, &wwo->sample_spec);
	    if (wait >= time) {
		wait = (wait - time) / 1000;
	        return wait ?: 1;
	    }
	}

	/* return the wavehdr */
	wwo->lpQueuePtr = lpWaveHdr->lpNext;
	lpWaveHdr->dwFlags &= ~WHDR_INQUEUE;
	lpWaveHdr->dwFlags |= WHDR_DONE;

	wodNotifyClient(wwo, WOM_DONE, (DWORD)lpWaveHdr, 0);
    }
    /* No more wavehdrs */
    TRACE("Empty queue\n");
    return INFINITE;
}

/**************************************************************************
 * 			     wodPlayer_WriteMax               [internal]
 * Writes either space or the wavehdr's size into pulse's buffer, and
 * returning how much data was written.
 */
static int wodPlayer_WriteMax(WINE_WAVEINST *wwo, size_t *space) {
    LPWAVEHDR lpWaveHdr = wwo->lpPlayPtr;
    size_t toWrite = min(lpWaveHdr->dwBufferLength - wwo->dwPartialOffset, *space);
    size_t written = 0;

    if (!wwo->stream ||
        !PULSE_context ||
        pa_context_get_state(PULSE_context) != PA_CONTEXT_READY ||
        pa_stream_get_state(wwo->stream) != PA_STREAM_READY) {
	return 0;
    }
	
    if (toWrite > 0 &&
	pa_stream_write(wwo->stream, lpWaveHdr->lpData + wwo->dwPartialOffset, toWrite, NULL, 0, PA_SEEK_RELATIVE) >= 0) {
	TRACE("Writing wavehdr %p.%u[%u]\n", lpWaveHdr, wwo->dwPartialOffset, lpWaveHdr->dwBufferLength);
	written = toWrite;
    }

    /* Check to see if we wrote all of the wavehdr */
    if ((wwo->dwPartialOffset += written) >= lpWaveHdr->dwBufferLength)
        wodPlayer_PlayPtrNext(wwo);
    *space -= written;
    
    return written;
}

/**************************************************************************
 * 			     wodPlayer_Feed			[internal]
 *
 * Feed as much sound data as we can into pulse using wodPlayer_WriteMax
 */
static void wodPlayer_Feed(WINE_WAVEINST* wwo, size_t space) {
    /* no more room... no need to try to feed */
    if (space > 0) {
        /* Feed from a partial wavehdr */
        if (wwo->lpPlayPtr && wwo->dwPartialOffset != 0)
            wodPlayer_WriteMax(wwo, &space);

        /* Feed wavehdrs until we run out of wavehdrs or buffer space */
        if (wwo->dwPartialOffset == 0 && wwo->lpPlayPtr) {
            do {
                wwo->lpPlayPtr->reserved = wwo->timing_info->write_index;
            } while (wodPlayer_WriteMax(wwo, &space) > 0 && wwo->lpPlayPtr && space > 0);
	}
    }
}

/**************************************************************************
 * 				wodPlayer_Reset			[internal]
 *
 * wodPlayer helper. Resets current output stream.
 */
static void wodPlayer_Reset(WINE_WAVEINST* wwo) {
    enum win_wm_message	    msg;
    DWORD		    param;
    HANDLE		    ev;
    pa_operation	    *o;

    TRACE("(%p)\n", wwo);

    /* remove any buffer */
    wodPlayer_NotifyCompletions(wwo, TRUE);
    
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

    /* flush the output buffer of written data*/
    if ((o = pa_stream_flush(wwo->stream, PULSE_stream_success_callback, NULL)))
        PULSE_wait_for_operation(o, wwo);

    /* Ask for the timing info to be updated (sanity, I don't know if we _have_ to) */
    if ((o = pa_stream_update_timing_info(wwo->stream, PULSE_stream_success_callback, wwo)))
        PULSE_wait_for_operation(o, wwo);

    /* Reset the written byte count as some data may have been flushed */
    wwo->releasing_offset = wwo->last_reset = wwo->timing_info->write_index;
    if (wwo->is_releasing)
        pa_gettimeofday(&wwo->started_releasing);
	
    /* return all pending headers in queue */
    EnterCriticalSection(&wwo->msgRing.msg_crst);
    while (PULSE_RetrieveRingMessage(&wwo->msgRing, &msg, &param, &ev)) {
	if (msg != WINE_WM_HEADER) {
	    SetEvent(ev);
	    continue;
	}
	((LPWAVEHDR)param)->dwFlags &= ~WHDR_INQUEUE;
	((LPWAVEHDR)param)->dwFlags |= WHDR_DONE;
	wodNotifyClient(wwo, WOM_DONE, param, 0);
    }
    PULSE_ResetRingMessage(&wwo->msgRing);
    LeaveCriticalSection(&wwo->msgRing.msg_crst); 

    pa_threaded_mainloop_unlock(PULSE_ml);
}

/**************************************************************************
 * 				wodPlayer_Underrun		[internal]
 *
 * wodPlayer helper. Deal with a stream underrun.
 */
static void wodPlayer_Underrun(WINE_WAVEINST* wwo) {
    size_t space;
    pa_operation *o;

    wwo->is_buffering = TRUE;

    pa_threaded_mainloop_lock(PULSE_ml);

    if (wwo->lpPlayPtr) {
	TRACE("There is queued data. Trying to recover.\n");	
	space = pa_stream_writable_size(wwo->stream);
	
	if (!space) {
	    TRACE("No space to feed. Flushing.\n");
	    if ((o = pa_stream_flush(wwo->stream, PULSE_stream_success_callback, wwo)))
		PULSE_wait_for_operation(o, wwo);
	    space = pa_stream_writable_size(wwo->stream);
	}
	wodPlayer_Feed(wwo, space);
    }

    /* Ask for a timing update */
    if ((o = pa_stream_update_timing_info(wwo->stream, PULSE_stream_success_callback, wwo)))
	PULSE_wait_for_operation(o, wwo);
    
    pa_threaded_mainloop_unlock(PULSE_ml);

    if (wwo->timing_info->playing) {
	TRACE("Recovered.\n");
	wwo->is_buffering = FALSE;
    } else {
	ERR("Stream underrun! %i\n", wwo->instance_ref);
	wwo->is_releasing = FALSE;
	wwo->releasing_offset = wwo->timing_info->write_index;
    }
    
    wwo->releasing_offset = wwo->timing_info->write_index;
}


/**************************************************************************
 * 		      wodPlayer_ProcessMessages			[internal]
 */
static void wodPlayer_ProcessMessages(WINE_WAVEINST* wwo) {
    LPWAVEHDR           lpWaveHdr;
    enum win_wm_message	msg;
    DWORD		param;
    HANDLE		ev;
    pa_operation	*o;

    while (PULSE_RetrieveRingMessage(&wwo->msgRing, &msg, &param, &ev)) {
	TRACE("Received %s %x\n", PULSE_getCmdString(msg), param);
	
	switch (msg) {
	case WINE_WM_PAUSING:
	    wwo->state = WINE_WS_PAUSED;
	    pa_threaded_mainloop_lock(PULSE_ml);
	    if ((o = pa_stream_cork(wwo->stream, 1, PULSE_stream_success_callback, NULL)))
		PULSE_wait_for_operation(o, wwo);

	    /* save how far we are, as releasing will restart from here */
	    if (wwo->is_releasing)
		wwo->releasing_offset = wwo->timing_info->write_index;

	    wwo->is_buffering = TRUE;	    
	    pa_threaded_mainloop_unlock(PULSE_ml);
	    
	    SetEvent(ev);
	    break;

	case WINE_WM_RESTARTING:
            if (wwo->state == WINE_WS_PAUSED) {
		wwo->state = WINE_WS_PLAYING;
		if (wwo->is_releasing)
		    pa_gettimeofday(&wwo->started_releasing);
		pa_threaded_mainloop_lock(PULSE_ml);
		if ((o = pa_stream_cork(wwo->stream, 0, PULSE_stream_success_callback, NULL)))
		    PULSE_wait_for_operation(o, wwo);
		pa_threaded_mainloop_unlock(PULSE_ml);    
	    }
	    SetEvent(ev);
	    break;

	case WINE_WM_HEADER:
	    lpWaveHdr = (LPWAVEHDR)param;
	    /* insert buffer at the end of queue */
	    {
		LPWAVEHDR*	wh;
		for (wh = &(wwo->lpQueuePtr); *wh; wh = &((*wh)->lpNext));
		*wh = lpWaveHdr;
	    }

            if (!wwo->lpPlayPtr)
                wodPlayer_BeginWaveHdr(wwo,lpWaveHdr);
	    /* Try and feed now, as we may have missed when pulse first asked */
	    wodPlayer_Feed(wwo, pa_stream_writable_size(wwo->stream));
	    if (wwo->state == WINE_WS_STOPPED)
		wwo->state = WINE_WS_PLAYING;
	    if (wwo->is_buffering && !wwo->is_releasing)
	        pa_gettimeofday(&wwo->last_header);
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
	    wodPlayer_Feed(wwo, (size_t)param);
	    SetEvent(ev);
	    break;

	case WINE_WM_XRUN: /* Sent by the pulse thread */
	    wodPlayer_Underrun(wwo);
	    SetEvent(ev);
	    break;

	case WINE_WM_STARTING: /* Set by the pulse thread */
	     wwo->is_buffering = FALSE;
	    /* Start releasing wavehdrs if we haven't already */
	    if (!wwo->is_releasing) {
		wwo->is_releasing = TRUE;
		pa_gettimeofday(&wwo->started_releasing);
	    }
	    SetEvent(ev);
	    break;

	case WINE_WM_CLOSING: /* If param = 1, close because of a failure */
	    wwo->hThread = NULL;
	    if ((DWORD)param == 1) {
		/* If we are here, the stream has failed */
		wwo->state = WINE_WS_FAILED;
		SetEvent(ev);
		PULSE_DestroyRingMessage(&wwo->msgRing);
		wodPlayer_NotifyCompletions(wwo, TRUE);
		wodNotifyClient(wwo, WOM_CLOSE, 0L, 0L);
		wwo->lpPlayPtr = wwo->lpQueuePtr = wwo->lpLoopPtr = NULL;
		pa_threaded_mainloop_lock(PULSE_ml);
		pa_stream_disconnect(wwo->stream);
		pa_threaded_mainloop_unlock(PULSE_ml);
		TRACE("Thread exiting because of failure.\n");
		ExitThread(1);
	    }
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
 * 				wodPlayer			[internal]
 */
static DWORD CALLBACK wodPlayer(LPVOID lpParam) {
    WINE_WAVEINST *wwo = (WINE_WAVEINST*)lpParam;
    DWORD         dwSleepTime = INFINITE;

    wwo->state = WINE_WS_STOPPED;
    SetEvent(wwo->hStartUpEvent);

    /* Wait for the shortest time before an action is required.  If there are
     * no pending actions, wait forever for a command. */
    for (;;) {
	TRACE("Waiting %u ms\n", dwSleepTime);
        PULSE_WaitRingMessage(&wwo->msgRing, dwSleepTime);
	wodPlayer_ProcessMessages(wwo);
	if (wwo->state == WINE_WS_PLAYING) {
	    wodPlayer_CheckReleasing(wwo);
	    dwSleepTime = wodPlayer_NotifyCompletions(wwo, FALSE);
	} else
	    dwSleepTime = INFINITE;
    }
}

/**************************************************************************
 *                              wodOpen                         [internal]
 */
static DWORD wodOpen(WORD wDevID, LPDWORD lpdwUser, LPWAVEOPENDESC lpDesc, DWORD dwFlags) {
    WINE_WAVEDEV *wdo;
    WINE_WAVEINST *wwo = NULL;
    pa_operation *o;
    DWORD x, ret = MMSYSERR_NOERROR;
    pa_sample_spec test;
   
    TRACE("(%u, %p, %08X);\n", wDevID, lpDesc, dwFlags);
    if (lpDesc == NULL) {
	WARN("Invalid Parameter !\n");
	return MMSYSERR_INVALPARAM;
    }

    if (wDevID >= PULSE_WodNumDevs) {
	TRACE("Asked for device %d, but only %d known!\n", wDevID, PULSE_WodNumDevs);
	return MMSYSERR_BADDEVICEID;
    }
    wdo = &WOutDev[wDevID];

    /* check to see if format is supported and make pa_sample_spec struct */
    if (!PULSE_setupFormat(lpDesc->lpFormat, &test) &&
	!pa_sample_spec_valid(&test)) {
	WARN("Bad format: tag=%04X nChannels=%d nSamplesPerSec=%d !\n",
	     lpDesc->lpFormat->wFormatTag, lpDesc->lpFormat->nChannels,
	     lpDesc->lpFormat->nSamplesPerSec);
	return WAVERR_BADFORMAT;
    }

    if (TRACE_ON(wave)) {
    	char t[PA_SAMPLE_SPEC_SNPRINT_MAX];
    	pa_sample_spec_snprint(t, sizeof(t), &test);
    	TRACE("Sample spec '%s'\n", t);
    }

    if (dwFlags & WAVE_FORMAT_QUERY) {
	TRACE("Query format: tag=%04X nChannels=%d nSamplesPerSec=%d !\n",
	     lpDesc->lpFormat->wFormatTag, lpDesc->lpFormat->nChannels,
	     lpDesc->lpFormat->nSamplesPerSec);
	return MMSYSERR_NOERROR;
    }

    for (x = 0; x < PULSE_MAX_STREAM_INSTANCES; x++) {
	if (wdo->instance[x] == NULL) {
	    if (!(wdo->instance[x] = wwo = HeapAlloc(GetProcessHeap(), 0, sizeof(WINE_WAVEINST))))
		return MMSYSERR_NOMEM;
	    TRACE("Allocated new playback instance %u on device %u.\n", x, wDevID);
	    break;
	}
    }
    if (x >= PULSE_MAX_STREAM_INSTANCES) return MMSYSERR_ALLOCATED;

    *lpdwUser = (DWORD)wwo;
    wwo->instance_ref = x;
    wwo->sample_spec.format =	test.format;
    wwo->sample_spec.rate =	test.rate;
    wwo->sample_spec.channels = test.channels;
    if (test.channels == 2) {
	wwo->volume.channels = 2;
	wwo->volume.values[0] = wdo->left_vol;
	wwo->volume.values[1] = wdo->right_vol;
    } else
	pa_cvolume_set(&wwo->volume, test.channels, (wdo->left_vol + wdo->right_vol)/2);
    wwo->wFlags = HIWORD(dwFlags & CALLBACK_TYPEMASK);
    wwo->waveDesc = *lpDesc;
    wwo->lpQueuePtr = wwo->lpPlayPtr = wwo->lpLoopPtr = NULL;
    wwo->dwPartialOffset = 0;
    wwo->is_buffering = TRUE;
    wwo->is_releasing = FALSE;
    wwo->releasing_offset = 0;
    wwo->timing_info = NULL;
    PULSE_InitRingMessage(&wwo->msgRing);

    /* FIXME Find a better name for the stream */
    wwo->stream = pa_stream_new(PULSE_context, "Wine Playback", &wwo->sample_spec, NULL);
    assert(wwo->stream);
   
    pa_stream_set_state_callback(wwo->stream, PULSE_stream_state_callback, wwo);
    pa_stream_set_write_callback(wwo->stream, PULSE_stream_request_callback, wwo);
    pa_stream_set_underflow_callback(wwo->stream, PULSE_underrun_callback, wwo);
    pa_stream_set_suspended_callback(wwo->stream, PULSE_suspended_callback, wwo);
#if HAVE_PULSEAUDIO_0_9_11
    pa_stream_set_started_callback(wwo->stream, PULSE_started_callback, wwo);
#else
    pa_stream_set_latency_update_callback(wwo->stream, PULSE_timing_info_update_callback, wwo);
#endif

    /* I don't like this, but... */
    wwo->buffer_attr = pa_xmalloc(sizeof(pa_buffer_attr));
    wwo->buffer_attr->maxlength = (uint32_t) -1;
    wwo->buffer_attr->tlength = pa_bytes_per_second(&wwo->sample_spec)/5;
    wwo->buffer_attr->prebuf = (uint32_t) -1;
    wwo->buffer_attr->minreq = (uint32_t) -1;

    TRACE("Connecting stream for playback.\n");
    pa_threaded_mainloop_lock(PULSE_ml);
#if HAVE_PULSEAUDIO_0_9_11
    pa_stream_connect_playback(wwo->stream, wdo->device_name, wwo->buffer_attr, PA_STREAM_ADJUST_LATENCY, &wwo->volume, NULL);
#else
    pa_stream_connect_playback(wwo->stream, wdo->device_name, wwo->buffer_attr, PA_STREAM_AUTO_TIMING_UPDATE, &wwo->volume, NULL);
#endif

    for (;;) {
        pa_context_state_t cstate = pa_context_get_state(PULSE_context);
        pa_stream_state_t sstate = pa_stream_get_state(wwo->stream);

	if (cstate == PA_CONTEXT_FAILED || cstate == PA_CONTEXT_TERMINATED ||
	    sstate == PA_STREAM_FAILED || sstate == PA_STREAM_TERMINATED) {
	    ERR("Failed to connect context object: %s\n", pa_strerror(pa_context_errno(PULSE_context)));
	    pa_threaded_mainloop_unlock(PULSE_ml);
	    ret = MMSYSERR_NODRIVER;
	    goto err;
	}

	if (sstate == PA_STREAM_READY)
	    break;

	pa_threaded_mainloop_wait(PULSE_ml);
    }
    TRACE("Stream connected for playback.\n");

    if ((o = pa_stream_update_timing_info(wwo->stream, PULSE_stream_success_callback, wwo)))
	PULSE_wait_for_operation(o, wwo);

    wwo->timing_info = pa_stream_get_timing_info(wwo->stream);
    assert(wwo->timing_info);
    pa_threaded_mainloop_unlock(PULSE_ml);

    wwo->hStartUpEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    wwo->hThread = CreateThread(NULL, 0, wodPlayer, (LPVOID)wwo, 0, &(wwo->dwThreadID));
    if (wwo->hThread)
        SetThreadPriority(wwo->hThread, THREAD_PRIORITY_TIME_CRITICAL);
    else {
        ERR("Thread creation for the wodPlayer failed!\n");
        ret = MMSYSERR_NOMEM;
	goto err;
    }
    WaitForSingleObject(wwo->hStartUpEvent, INFINITE);
    CloseHandle(wwo->hStartUpEvent);
    wwo->hStartUpEvent = INVALID_HANDLE_VALUE;


    return wodNotifyClient (wwo, WOM_OPEN, 0L, 0L);

err:
    TRACE("Bailing out...\n");
    wdo->instance[x] = NULL;
    if (!wwo)
	return ret;

    if (wwo->hStartUpEvent != INVALID_HANDLE_VALUE)
	CloseHandle(wwo->hStartUpEvent);

    if (wwo->msgRing.ring_buffer_size > 0)
	    PULSE_DestroyRingMessage(&wwo->msgRing);
    
    if (pa_stream_get_state(wwo->stream) == PA_STREAM_READY)
	pa_stream_disconnect(wwo->stream);
    pa_stream_unref(wwo->stream);
    if (wwo->buffer_attr)
	pa_xfree(wwo->buffer_attr);
    HeapFree(GetProcessHeap(), 0, wwo);

    return ret;
}

/**************************************************************************
 *                              wodClose                        [internal]
 */
static DWORD wodClose(WORD wDevID, WINE_WAVEINST *wwo) {
    pa_operation *o;
    DWORD ret;

    TRACE("(%u, %p);\n", wDevID, wwo);
    if (wDevID >= PULSE_WodNumDevs) {
	WARN("Asked for device %d, but only %d known!\n", wDevID, PULSE_WodNumDevs);
	return MMSYSERR_INVALHANDLE;
    } else if (!wwo) {
	WARN("Stream instance invalid.\n");
	return MMSYSERR_INVALHANDLE;
    }

    if (wwo->state != WINE_WS_FAILED) {
	if (wwo->lpQueuePtr && wwo->lpPlayPtr) {
	    WARN("buffers still playing !\n");
	    return WAVERR_STILLPLAYING;
	}

	pa_threaded_mainloop_lock(PULSE_ml);
	if ((o = pa_stream_drain(wwo->stream, PULSE_stream_success_callback, NULL)))
	    PULSE_wait_for_operation(o, wwo);
	pa_stream_disconnect(wwo->stream);
	pa_threaded_mainloop_unlock(PULSE_ml);

	if (wwo->hThread != INVALID_HANDLE_VALUE)
	    PULSE_AddRingMessage(&wwo->msgRing, WINE_WM_CLOSING, 0, TRUE);

	PULSE_DestroyRingMessage(&wwo->msgRing);

	pa_threaded_mainloop_lock(PULSE_ml);
	pa_stream_disconnect(wwo->stream);
	pa_threaded_mainloop_unlock(PULSE_ml);
    }
    ret = wodNotifyClient(wwo, WOM_CLOSE, 0L, 0L);

    if (wwo->buffer_attr)
	pa_xfree(wwo->buffer_attr);
    pa_stream_unref(wwo->stream);
    WOutDev[wDevID].instance[wwo->instance_ref] = NULL;
    TRACE("Deallocating playback instance %u on device %u.\n", wwo->instance_ref, wDevID);
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
    if (!wwo ||	wwo->state == WINE_WS_FAILED) {
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
    pa_usec_t	time;

    if (!wwo ||	wwo->state == WINE_WS_FAILED) {
	WARN("Stream instance invalid.\n");
	return MMSYSERR_INVALHANDLE;
    }

    if (lpTime == NULL)	return MMSYSERR_INVALPARAM;

    time = pa_bytes_to_usec(wwo->releasing_offset, &wwo->sample_spec);

    if (wwo->is_releasing)
	time += pa_timeval_age(&wwo->started_releasing); 

    if (wwo->last_reset > wwo->releasing_offset)
	wwo->last_reset = 0;

    time -= pa_bytes_to_usec(wwo->last_reset, &wwo->sample_spec);
    time /= 1000;

    switch (lpTime->wType) {
    case TIME_SAMPLES:
        lpTime->u.sample = (time * wwo->sample_spec.rate) / 1000;
        break;
    case TIME_MS:
        lpTime->u.ms = time;
	break;
    case TIME_SMPTE:
        lpTime->u.smpte.fps = 30;
        lpTime->u.smpte.sec = time/1000;
        lpTime->u.smpte.min = lpTime->u.smpte.sec / 60;
        lpTime->u.smpte.sec -= 60 * lpTime->u.smpte.min;
        lpTime->u.smpte.hour = lpTime->u.smpte.min / 60;
        lpTime->u.smpte.min -= 60 * lpTime->u.smpte.hour;
        lpTime->u.smpte.fps = 30;
        lpTime->u.smpte.frame = time / lpTime->u.smpte.fps * 1000;
        TRACE("TIME_SMPTE=%02u:%02u:%02u:%02u\n",
              lpTime->u.smpte.hour, lpTime->u.smpte.min,
              lpTime->u.smpte.sec, lpTime->u.smpte.frame);
        break;
    default:
        WARN("Format %d not supported, using TIME_BYTES !\n", lpTime->wType);
        lpTime->wType = TIME_BYTES;
        /* fall through */
    case TIME_BYTES:
        lpTime->u.cb = (pa_bytes_per_second(&wwo->sample_spec)*time) / 1000;
        TRACE("TIME_BYTES=%u\n", lpTime->u.cb);
        break;
    }

    return MMSYSERR_NOERROR;
}
/**************************************************************************
 *                              wodBreakLoop                    [internal]
 */
static DWORD wodBreakLoop(WINE_WAVEINST *wwo) {
    if (!wwo ||	wwo->state == WINE_WS_FAILED) {
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
 *                              wodGetVolume                    [internal]
 */
static DWORD wodGetVolume(WORD wDevID, LPDWORD lpdwVol) {
    DWORD	    wleft, wright;
    WINE_WAVEDEV*   wdo;

    TRACE("(%u, %p);\n", wDevID, lpdwVol);

    if (wDevID >= PULSE_WodNumDevs) {
	TRACE("Asked for device %d, but only %d known!\n", wDevID, PULSE_WodNumDevs);
	return MMSYSERR_INVALHANDLE;
    }

    if (lpdwVol == NULL)
	return MMSYSERR_NOTENABLED;

    wdo = &WOutDev[wDevID];

    wleft=(long int)(pa_sw_volume_to_linear(wdo->left_vol)*0xFFFFl);
    wright=(long int)(pa_sw_volume_to_linear(wdo->right_vol)*0xFFFFl);

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
static DWORD wodSetVolume(WORD wDevID, DWORD dwParam1) {
    WINE_WAVEDEV *wdo;
    WINE_WAVEINST *current;
    pa_operation *o;
    DWORD x;

    TRACE("(%u, %08X);\n", wDevID, dwParam1);

    if (wDevID >= PULSE_WodNumDevs) {
	TRACE("Asked for device %d, but only %d known!\n", wDevID, PULSE_WodNumDevs);
	return MMSYSERR_INVALHANDLE;
    }

    wdo = &WOutDev[wDevID];

    wdo->left_vol  = pa_sw_volume_from_linear((double)LOWORD(dwParam1)/0xFFFFl);
    wdo->right_vol = pa_sw_volume_from_linear((double)HIWORD(dwParam1)/0xFFFFl);

    /* Windows assumes that this volume is for the entire device, so we have to
     * hunt down all current streams of the device and set their volumes.
     * Streams which are not stereo (channels!=2) don't pan correctly. */
    for (x = 0; (current = wdo->instance[x]); x++) {
	switch (current->sample_spec.channels) {
	    case 2:
		current->volume.channels = 2;
		current->volume.values[0] = wdo->left_vol;
		current->volume.values[1] = wdo->right_vol;
		break;
	    case 1:
		current->volume.channels = 1;
		current->volume.values[0] = (wdo->left_vol + wdo->right_vol)/2; /* Is this right? */
		break;
	    default:
		/* FIXME How does more than stereo work? */
		pa_cvolume_set(&current->volume, current->sample_spec.channels, (wdo->left_vol + wdo->right_vol)/2);
	}

	if (!current->stream ||
	    !PULSE_context ||
	    pa_context_get_state(PULSE_context) != PA_CONTEXT_READY ||
	    pa_stream_get_state(current->stream) != PA_STREAM_READY ||
	    !pa_cvolume_valid(&current->volume))
	    continue;

	pa_threaded_mainloop_lock(PULSE_ml);
	if ((o = pa_context_set_sink_input_volume(PULSE_context,
	    pa_stream_get_index(current->stream), &current->volume,
	    PULSE_context_success_callback, current)))
	    PULSE_wait_for_operation(o, current);
	pa_threaded_mainloop_unlock(PULSE_ml);
    }
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 *                              wodRestart                      [internal]
 */
static DWORD wodRestart(WINE_WAVEINST *wwo) {
    if (!wwo ||	wwo->state == WINE_WS_FAILED) {
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
    if (!wwo ||	wwo->state == WINE_WS_FAILED) {
	WARN("Stream instance invalid.\n");
	return MMSYSERR_INVALHANDLE;
    }

    PULSE_AddRingMessage(&wwo->msgRing, WINE_WM_RESETTING, 0, TRUE);
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 *                              wodDevInterfaceSize             [internal]
 */
static DWORD wodDevInterfaceSize(UINT wDevID, LPDWORD dwParam1) {

    *dwParam1 = MultiByteToWideChar(CP_ACP, 0, WOutDev[wDevID].interface_name, -1, NULL, 0) * sizeof(WCHAR);
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 *                              wodDevInterface                 [internal]
 */
static DWORD wodDevInterface(UINT wDevID, PWCHAR dwParam1, DWORD dwParam2) {
    if (dwParam2 >= MultiByteToWideChar(CP_ACP, 0, WOutDev[wDevID].interface_name, -1,
                                        NULL, 0 ) * sizeof(WCHAR))
    {
        MultiByteToWideChar(CP_ACP, 0, WOutDev[wDevID].interface_name, -1,
                            dwParam1, dwParam2 / sizeof(WCHAR));
	return MMSYSERR_NOERROR;
    }
    return MMSYSERR_INVALPARAM;
}

/*======================================================================*
 *                  Low level DSOUND implementation			*
 *======================================================================*/
static DWORD wodDsCreate(UINT wDevID, PIDSDRIVER* drv) {
    /* Is this possible ?*/
    return MMSYSERR_NOTSUPPORTED;
}

static DWORD wodDsDesc(UINT wDevID, PDSDRIVERDESC desc) {
    memset(desc, 0, sizeof(*desc));
    strcpy(desc->szDesc, "Wine PulseAudio DirectSound Driver");
    strcpy(desc->szDrvname, "winepulse.drv");
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				wodMessage (WINEPULSE.@)
 */
DWORD WINAPI PULSE_wodMessage(UINT wDevID, UINT wMsg, DWORD dwUser,
			    DWORD dwParam1, DWORD dwParam2) {
/*
    TRACE("(%u, %s, %08X, %08X, %08X);\n",
	  wDevID, PULSE_getMessage(wMsg), dwUser, dwParam1, dwParam2);
*/
    switch (wMsg) {
    case DRVM_INIT:
    case DRVM_EXIT:
    case DRVM_ENABLE:
    case DRVM_DISABLE:
	/* FIXME: Pretend this is supported */
	return 0;
    case WODM_OPEN:	 	return wodOpen		(wDevID, (LPDWORD)dwUser, (LPWAVEOPENDESC)dwParam1,	dwParam2);
    case WODM_CLOSE:	 	return wodClose		(wDevID, (WINE_WAVEINST*)dwUser);
    case WODM_WRITE:	 	return wodWrite		((WINE_WAVEINST*)dwUser,    (LPWAVEHDR)dwParam1,		dwParam2);
    case WODM_PAUSE:	 	return wodPause		((WINE_WAVEINST*)dwUser);
    case WODM_GETPOS:	 	return wodGetPosition	((WINE_WAVEINST*)dwUser,    (LPMMTIME)dwParam1, 		dwParam2);
    case WODM_BREAKLOOP: 	return wodBreakLoop     ((WINE_WAVEINST*)dwUser);
    case WODM_PREPARE:	 	return MMSYSERR_NOTSUPPORTED;
    case WODM_UNPREPARE: 	return MMSYSERR_NOTSUPPORTED;
    case WODM_GETDEVCAPS:	return wodGetDevCaps	(wDevID, (LPWAVEOUTCAPSW)dwParam1,	dwParam2);
    case WODM_GETNUMDEVS:	return PULSE_WodNumDevs;
    case WODM_GETPITCH:	 	return MMSYSERR_NOTSUPPORTED;
    case WODM_SETPITCH:	 	return MMSYSERR_NOTSUPPORTED;
    case WODM_GETPLAYBACKRATE:	return MMSYSERR_NOTSUPPORTED; /* support if theoretically possible */
    case WODM_SETPLAYBACKRATE:	return MMSYSERR_NOTSUPPORTED; /* since pulseaudio 0.9.8 */
    case WODM_GETVOLUME:	return wodGetVolume	(wDevID, (LPDWORD)dwParam1);
    case WODM_SETVOLUME:	return wodSetVolume	(wDevID, dwParam1);
    case WODM_RESTART:		return wodRestart	((WINE_WAVEINST*)dwUser);
    case WODM_RESET:		return wodReset		((WINE_WAVEINST*)dwUser);
    case DRV_QUERYDEVICEINTERFACESIZE: return wodDevInterfaceSize      (wDevID, (LPDWORD)dwParam1);
    case DRV_QUERYDEVICEINTERFACE:     return wodDevInterface          (wDevID, (PWCHAR)dwParam1, dwParam2);
    case DRV_QUERYDSOUNDIFACE:	return wodDsCreate	(wDevID, (PIDSDRIVER*)dwParam1);
    case DRV_QUERYDSOUNDDESC:	return wodDsDesc	(wDevID, (PDSDRIVERDESC)dwParam1);
    default:
	FIXME("unknown message %d!\n", wMsg);
    }
    return MMSYSERR_NOTSUPPORTED;
}

#else /* !HAVE_PULSEAUDIO */

/**************************************************************************
 * 				wodMessage (WINEPULSE.@)
 */
DWORD WINAPI PULSE_wodMessage(WORD wDevID, WORD wMsg, DWORD dwUser,
			      DWORD dwParam1, DWORD dwParam2) {
    FIXME("(%u, %04X, %08X, %08X, %08X):stub\n", wDevID, wMsg, dwUser,
          dwParam1, dwParam2);
    return MMSYSERR_NOTENABLED;
}

#endif /* HAVE_PULSEAUDIO */
