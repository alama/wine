/*
 * Wine Driver for PulseAudio - WaveIn Functionality
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
#include "mmddk.h"

#include <winepulse.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wave);

#if HAVE_PULSEAUDIO

/*======================================================================*
 *                  WAVE IN specific PulseAudio Callbacks               *
 *======================================================================*/

/**************************************************************************
 *                  widNotifyClient                             [internal]
*/
static DWORD widNotifyClient(WINE_WAVEINST* wwi, WORD wMsg, DWORD dwParam1, DWORD dwParam2) {
   TRACE("wMsg = 0x%04x dwParm1 = %04X dwParam2 = %04X\n", wMsg, dwParam1, dwParam2);

   switch (wMsg) {
   case WIM_OPEN:
   case WIM_CLOSE:
   case WIM_DATA:
       if (wwi->wFlags != DCB_NULL &&
           !DriverCallback(wwi->waveDesc.dwCallback, wwi->wFlags, (HDRVR)wwi->waveDesc.hWave,
                           wMsg, wwi->waveDesc.dwInstance, dwParam1, dwParam2)) {
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
 *                         widRecorder_CopyData                 [internal]
 *
 * Copys data from the fragments pulse returns to queued buffers.
 */
static void widRecorder_CopyData(WINE_WAVEINST *wwi) {
    LPWAVEHDR lpWaveHdr = wwi->lpQueuePtr;
    size_t bytes_avail;

    /* Get this value once and trust it. Note that the total available is made
     * of one _or more_ fragments. These fragments will probably not align with
     * the wavehdr buffer sizes. */
    PULSE_MainloopLock();
    bytes_avail = pa_stream_readable_size(wwi->stream);
    PULSE_MainloopUnlock();

    if (bytes_avail == -1) {
        ERR("pa_stream_readable_size() returned -1, record stream has failed.\n");
        return;
    }

    /* If there is an already peeked buffer, add it to the total */
    if (wwi->buffer)
        bytes_avail += wwi->buffer_length - wwi->buffer_read_offset;

    for (;bytes_avail && lpWaveHdr; lpWaveHdr = wwi->lpQueuePtr) {
        size_t peek_avail;

        if (!wwi->buffer) {
            PULSE_MainloopLock();
            pa_stream_peek(wwi->stream, &wwi->buffer, &wwi->buffer_length);
            PULSE_MainloopUnlock();
            wwi->buffer_read_offset = 0;

            if (!wwi->buffer || !wwi->buffer_length) {
                WARN("pa_stream_peek failed\n");
                break;
            }
        }
        
        peek_avail = min(wwi->buffer_length - wwi->buffer_read_offset,
                         lpWaveHdr->dwBufferLength - lpWaveHdr->dwBytesRecorded);

        memcpy(lpWaveHdr->lpData + lpWaveHdr->dwBytesRecorded,
               (PBYTE)wwi->buffer + wwi->buffer_read_offset,
               peek_avail);

        wwi->buffer_read_offset += peek_avail;
        lpWaveHdr->dwBytesRecorded += peek_avail;
        bytes_avail -= peek_avail;

        if (lpWaveHdr->dwBytesRecorded == lpWaveHdr->dwBufferLength) {
            lpWaveHdr->dwFlags &= ~WHDR_INQUEUE;
            lpWaveHdr->dwFlags |= WHDR_DONE;
            wwi->lpQueuePtr = lpWaveHdr->lpNext;
            widNotifyClient(wwi, WIM_DATA, (DWORD)lpWaveHdr, 0);
        }

        if (wwi->buffer_read_offset == wwi->buffer_length) {
            PULSE_MainloopLock();
            pa_stream_drop(wwi->stream);
            wwi->buffer = NULL;
            PULSE_MainloopUnlock();
        }
    } /* for(bytes_avail && lpWaveHdr) */

    return;
}

static void widRecorder_ProcessMessages(WINE_WAVEINST* wwi) {
    LPWAVEHDR          lpWaveHdr;
    enum win_wm_message msg;
    DWORD               param;
    HANDLE              ev;


    while (PULSE_RetrieveRingMessage(&wwi->msgRing, &msg, &param, &ev)) {
            TRACE("Received %s %x\n", PULSE_getCmdString(msg), param);

        switch (msg) {
        case WINE_WM_FEED:
            /* Spin the loop in widRecorder */
            SetEvent(ev);
            break;

        case WINE_WM_STARTING:
            wwi->dwLastReset = wwi->timing_info->read_index;
            PULSE_MainloopLock();
            PULSE_WaitForOperation(pa_stream_cork(wwi->stream, 0, PULSE_StreamSuccessCallback, NULL));
            PULSE_MainloopUnlock();
            wwi->state = WINE_WS_PLAYING;
            SetEvent(ev);
            break;

        case WINE_WM_HEADER:
            lpWaveHdr = (LPWAVEHDR)param;
            lpWaveHdr->lpNext = 0;
            /* insert buffer at the end of queue */
            {
                LPWAVEHDR *wh;
                for (wh = &(wwi->lpQueuePtr); *wh; wh = &((*wh)->lpNext));
                *wh = lpWaveHdr;
            }
            break;

        case WINE_WM_STOPPING:
            if (wwi->state != WINE_WS_STOPPED) {
                wwi->state = WINE_WS_STOPPED;
                PULSE_MainloopLock();
                PULSE_WaitForOperation(pa_stream_cork(wwi->stream, 1, PULSE_StreamSuccessCallback, NULL));
                if (wwi->buffer) {
                    pa_stream_drop(wwi->stream);
                    wwi->buffer = NULL;
                }
                PULSE_MainloopUnlock();

                /* return only the current buffer to app */
                if ((lpWaveHdr = wwi->lpQueuePtr)) {
                    LPWAVEHDR lpNext = lpWaveHdr->lpNext;
                    TRACE("stop %p %p\n", lpWaveHdr, lpWaveHdr->lpNext);
                    lpWaveHdr->dwFlags &= ~WHDR_INQUEUE;
                    lpWaveHdr->dwFlags |= WHDR_DONE;
                    wwi->lpQueuePtr = lpNext;
                    widNotifyClient(wwi, WIM_DATA, (DWORD)lpWaveHdr, 0);
                }
            }
            SetEvent(ev);
            break;

        case WINE_WM_RESETTING:
            if (wwi->state != WINE_WS_STOPPED) {
                wwi->state = WINE_WS_STOPPED;
                PULSE_MainloopLock();
                PULSE_WaitForOperation(pa_stream_cork(wwi->stream, 1, PULSE_StreamSuccessCallback, NULL));
                if (wwi->buffer) {
                    pa_stream_drop(wwi->stream);
                    wwi->buffer = NULL;
                }
                PULSE_MainloopUnlock();
            }

            /* return all the buffers to the app */
            lpWaveHdr = wwi->lpPlayPtr ? wwi->lpPlayPtr : wwi->lpQueuePtr;
            for (; lpWaveHdr; lpWaveHdr = wwi->lpQueuePtr) {
                lpWaveHdr->dwFlags &= ~WHDR_INQUEUE;
                lpWaveHdr->dwFlags |= WHDR_DONE;
                wwi->lpQueuePtr = lpWaveHdr->lpNext;
                widNotifyClient(wwi, WIM_DATA, (DWORD)lpWaveHdr, 0);
            }
            SetEvent(ev);
            break;

        case WINE_WM_CLOSING:
            wwi->hThread = 0;
            wwi->state = WINE_WS_CLOSED;
            SetEvent(ev);
            ExitThread(0);
            /* shouldn't go here */

        default:
            FIXME("unknown message %d\n", msg);
            break;
        }
    }
}

/**************************************************************************
 *                  widRecorder                                 [internal]
 */
static DWORD CALLBACK widRecorder(LPVOID lpParam) {
    WINE_WAVEINST   *wwi = (WINE_WAVEINST*)lpParam;

    wwi->state = WINE_WS_STOPPED;
    SetEvent(wwi->hStartUpEvent);

    for (;;) {
        PULSE_WaitRingMessage(&wwi->msgRing, INFINITE);
        widRecorder_ProcessMessages(wwi);
        if (wwi->state == WINE_WS_PLAYING && wwi->lpQueuePtr)
            widRecorder_CopyData(wwi);
    }

    return 0;
}

/**************************************************************************
 *                  widOpen                                     [internal]
 */
static DWORD widOpen(WORD wDevID, DWORD_PTR *lpdwUser, LPWAVEOPENDESC lpDesc, DWORD dwFlags) {
    WINE_WAVEDEV *wdi;
    WINE_WAVEINST *wwi = NULL;
    DWORD ret = MMSYSERR_NOERROR;

    TRACE("(%u, %p, %08X);\n", wDevID, lpDesc, dwFlags);
    if (lpDesc == NULL) {
        WARN("Invalid Parameter !\n");
        return MMSYSERR_INVALPARAM;
    }

    if (wDevID >= PULSE_WidNumDevs) {
        TRACE("Asked for device %d, but only %d known!\n", wDevID, PULSE_WidNumDevs);
        return MMSYSERR_BADDEVICEID;
    }
    wdi = &WInDev[wDevID];

    wwi = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WINE_WAVEINST));
    if (!wwi) return MMSYSERR_NOMEM;
    *lpdwUser = (DWORD_PTR)wwi;

    /* check to see if format is supported and make pa_sample_spec struct */
    if (!PULSE_SetupFormat(lpDesc->lpFormat, &wwi->sample_spec)) {
        WARN("Bad format: tag=%04X nChannels=%d nSamplesPerSec=%d !\n",
             lpDesc->lpFormat->wFormatTag, lpDesc->lpFormat->nChannels,
             lpDesc->lpFormat->nSamplesPerSec);
        ret = WAVERR_BADFORMAT;
        goto exit;
    }

    if (TRACE_ON(wave)) {
        char t[PA_SAMPLE_SPEC_SNPRINT_MAX];
        pa_sample_spec_snprint(t, sizeof(t), &wwi->sample_spec);
        TRACE("Sample spec '%s'\n", t);
    }

    if (dwFlags & WAVE_FORMAT_QUERY) {
        TRACE("Query format: tag=%04X nChannels=%d nSamplesPerSec=%d !\n",
         lpDesc->lpFormat->wFormatTag, lpDesc->lpFormat->nChannels,
             lpDesc->lpFormat->nSamplesPerSec);
        ret = MMSYSERR_NOERROR;
        goto exit;
    }

    wwi->wFlags = HIWORD(dwFlags & CALLBACK_TYPEMASK);
    wwi->waveDesc = *lpDesc;
    PULSE_InitRingMessage(&wwi->msgRing);

    wwi->stream = pa_stream_new(PULSE_context, "WaveIn", &wwi->sample_spec, NULL);
    if (!wwi->stream) {
        ret = WAVERR_BADFORMAT;
        goto exit;
    }

    pa_stream_set_state_callback(wwi->stream, PULSE_StreamStateCallback,    wwi);
    pa_stream_set_read_callback (wwi->stream, PULSE_StreamRequestCallback,  wwi);

    wwi->buffer_attr.maxlength = (uint32_t)-1;
    wwi->buffer_attr.fragsize = pa_bytes_per_second(&wwi->sample_spec) / 100;

    PULSE_MainloopLock();
    TRACE("Asking to open %s for recording.\n", wdi->device_name);
    pa_stream_connect_record(wwi->stream, wdi->device_name, &wwi->buffer_attr,
                             PA_STREAM_START_CORKED |
                             PA_STREAM_AUTO_TIMING_UPDATE |
                             PA_STREAM_ADJUST_LATENCY);

    for (;;) {
        pa_context_state_t cstate = pa_context_get_state(PULSE_context);
        pa_stream_state_t sstate = pa_stream_get_state(wwi->stream);

        if (cstate == PA_CONTEXT_FAILED || cstate == PA_CONTEXT_TERMINATED ||
            sstate == PA_STREAM_FAILED || sstate == PA_STREAM_TERMINATED) {
            ERR("Failed to connect context object: %s\n", pa_strerror(pa_context_errno(PULSE_context)));
                ret = MMSYSERR_NODRIVER;
                PULSE_MainloopUnlock();
                goto exit;
            }

            if (sstate == PA_STREAM_READY)
                break;

            PULSE_MainloopWait();
        }
    TRACE("(%p)->stream connected for recording.\n", wwi);

    PULSE_WaitForOperation(pa_stream_update_timing_info(wwi->stream, PULSE_StreamSuccessCallback, wwi));

    wwi->timing_info = pa_stream_get_timing_info(wwi->stream);
    assert(wwi->timing_info);
    PULSE_MainloopUnlock();

    wwi->hStartUpEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    wwi->hThread = CreateThread(NULL, 0, widRecorder, (LPVOID)wwi, 0, &(wwi->dwThreadID));
    if (wwi->hThread)
        SetThreadPriority(wwi->hThread, THREAD_PRIORITY_TIME_CRITICAL);
    else {
        ERR("Thread creation for the widRecorder failed!\n");
        ret = MMSYSERR_NOMEM;
        goto exit;
    }
    WaitForSingleObject(wwi->hStartUpEvent, INFINITE);
    CloseHandle(wwi->hStartUpEvent);
    wwi->hStartUpEvent = INVALID_HANDLE_VALUE;

    return widNotifyClient(wwi, WIM_OPEN, 0L, 0L);

exit:
    if (!wwi)
        return ret;

    if (wwi->hStartUpEvent != INVALID_HANDLE_VALUE)
        CloseHandle(wwi->hStartUpEvent);

    if (wwi->msgRing.ring_buffer_size > 0)
        PULSE_DestroyRingMessage(&wwi->msgRing);

    if (wwi->stream) {
        if (pa_stream_get_state(wwi->stream) == PA_STREAM_READY)
            pa_stream_disconnect(wwi->stream);
        pa_stream_unref(wwi->stream);
    }
    HeapFree(GetProcessHeap(), 0, wwi);

    return ret;
}
/**************************************************************************
 *                              widClose                        [internal]
 */
static DWORD widClose(WORD wDevID, WINE_WAVEINST *wwi) {
    DWORD ret;

    TRACE("(%u, %p);\n", wDevID, wwi);
    if (wDevID >= PULSE_WidNumDevs) {
        WARN("Asked for device %d, but only %d known!\n", wDevID, PULSE_WodNumDevs);
        return MMSYSERR_INVALHANDLE;
    } else if (!wwi) {
        WARN("Stream instance invalid.\n");
        return MMSYSERR_INVALHANDLE;
    }

    if (wwi->state != WINE_WS_FAILED) {
        if (wwi->lpQueuePtr) {
            WARN("buffers recording recording !\n");
            return WAVERR_STILLPLAYING;
        }

        PULSE_MainloopLock();
        if (pa_stream_get_state(wwi->stream) == PA_STREAM_READY)
            pa_stream_drop(wwi->stream);
            pa_stream_disconnect(wwi->stream);
        PULSE_MainloopUnlock();

        if (wwi->hThread != INVALID_HANDLE_VALUE)
            PULSE_AddRingMessage(&wwi->msgRing, WINE_WM_CLOSING, 0, TRUE);

        PULSE_DestroyRingMessage(&wwi->msgRing);
    }
    ret = widNotifyClient(wwi, WIM_CLOSE, 0L, 0L);

    pa_stream_unref(wwi->stream);
    TRACE("Deallocating record instance.\n");
    HeapFree(GetProcessHeap(), 0, wwi);
    return ret;
}

/**************************************************************************
 *                  widAddBuffer                                [internal]
 *
 */
static DWORD widAddBuffer(WINE_WAVEINST* wwi, LPWAVEHDR lpWaveHdr, DWORD dwSize) {
    TRACE("(%p, %p, %08X);\n", wwi, lpWaveHdr, dwSize);

    if (!wwi || wwi->state == WINE_WS_FAILED) {
        WARN("Stream instance invalid.\n");
        return MMSYSERR_INVALHANDLE;
    }

    if (lpWaveHdr->lpData == NULL || !(lpWaveHdr->dwFlags & WHDR_PREPARED))
        return WAVERR_UNPREPARED;

    if (lpWaveHdr->dwFlags & WHDR_INQUEUE)
        return WAVERR_STILLPLAYING;

    lpWaveHdr->dwFlags &= ~WHDR_DONE;
    lpWaveHdr->dwFlags |= WHDR_INQUEUE;
    lpWaveHdr->dwBytesRecorded = 0;
    lpWaveHdr->lpNext = 0;

    PULSE_AddRingMessage(&wwi->msgRing, WINE_WM_HEADER, (DWORD)lpWaveHdr, FALSE);

    return MMSYSERR_NOERROR;
}

/**************************************************************************
 *                              widRecorderMessage                [internal]
 */
static DWORD widRecorderMessage(WINE_WAVEINST *wwi, enum win_wm_message message) {
    if (!wwi || wwi->state == WINE_WS_FAILED) {
        WARN("Stream instance invalid.\n");
        return MMSYSERR_INVALHANDLE;
    }

    PULSE_AddRingMessage(&wwi->msgRing, message, 0, TRUE);
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 *                              widGetPosition                  [internal]
 */
static DWORD widGetPosition(WINE_WAVEINST *wwi, LPMMTIME lpTime, DWORD uSize) {

    if (!wwi || wwi->state == WINE_WS_FAILED) {
        WARN("Stream instance invalid.\n");
        return MMSYSERR_INVALHANDLE;
    }

    if (lpTime == NULL) return MMSYSERR_INVALPARAM;

    return PULSE_UsecToMMTime(pa_bytes_to_usec(wwi->timing_info->read_index - wwi->dwLastReset, &wwi->sample_spec), lpTime, &wwi->sample_spec);
}

/**************************************************************************
 *                              widGetDevCaps                   [internal]
 */
static DWORD widGetDevCaps(DWORD wDevID, LPWAVEINCAPSW lpCaps, DWORD dwSize) {
    TRACE("(%u, %p, %u);\n", wDevID, lpCaps, dwSize);

    if (lpCaps == NULL) return MMSYSERR_NOTENABLED;

    if (wDevID >= PULSE_WidNumDevs) {
        TRACE("Asked for device %d, but only %d known!\n", wDevID, PULSE_WidNumDevs);
        return MMSYSERR_INVALHANDLE;
    }

    memcpy(lpCaps, &(WInDev[wDevID].caps.in), min(dwSize, sizeof(*lpCaps)));
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 *                              widGetNumDevs                   [internal]
 * Context-sanity check here, as if we respond with 0, WINE will move on
 * to the next wavein driver.
 */
static DWORD widGetNumDevs(void) {
    if (pa_context_get_state(PULSE_context) != PA_CONTEXT_READY)
        return 0;

    return PULSE_WidNumDevs;
}

/**************************************************************************
 *                              widDevInterfaceSize             [internal]
 */
static DWORD widDevInterfaceSize(UINT wDevID, LPDWORD dwParam1) {
    TRACE("(%u, %p)\n", wDevID, dwParam1);

    *dwParam1 = MultiByteToWideChar(CP_UTF8, 0, WInDev[wDevID].interface_name, -1,
                                    NULL, 0 ) * sizeof(WCHAR);
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 *                              widDevInterface                 [internal]
 */
static DWORD widDevInterface(UINT wDevID, PWCHAR dwParam1, DWORD dwParam2) {
    if (dwParam2 >= MultiByteToWideChar(CP_UTF8, 0, WInDev[wDevID].interface_name, -1,
                                        NULL, 0 ) * sizeof(WCHAR))
    {
        MultiByteToWideChar(CP_UTF8, 0, WInDev[wDevID].interface_name, -1,
                            dwParam1, dwParam2 / sizeof(WCHAR));
        return MMSYSERR_NOERROR;
    }
    return MMSYSERR_INVALPARAM;
}

/**************************************************************************
 *                              widDsDesc                       [internal]
 */
DWORD widDsDesc(UINT wDevID, PDSDRIVERDESC desc)
{
    *desc = WInDev[wDevID].ds_desc;
    return MMSYSERR_NOERROR;
}

/**************************************************************************
 *                  widMessage (WINEPULSE.@)
 */
DWORD WINAPI PULSE_widMessage(UINT wDevID, UINT wMsg, DWORD_PTR dwUser,
                             DWORD_PTR dwParam1, DWORD_PTR dwParam2) {

    switch (wMsg) {
    case DRVM_INIT:
    case DRVM_EXIT:
    case DRVM_ENABLE:
    case DRVM_DISABLE:
        /* FIXME: Pretend this is supported */
        return 0;
    case WIDM_OPEN:         return widOpen      (wDevID, (DWORD_PTR*)dwUser, (LPWAVEOPENDESC)dwParam1, dwParam2);
    case WIDM_CLOSE:        return widClose     (wDevID, (WINE_WAVEINST*)dwUser);
    case WIDM_ADDBUFFER:    return widAddBuffer ((WINE_WAVEINST*)dwUser, (LPWAVEHDR)dwParam1, dwParam2);
    case WIDM_PREPARE:      return MMSYSERR_NOTSUPPORTED;
    case WIDM_UNPREPARE:    return MMSYSERR_NOTSUPPORTED;
    case WIDM_GETDEVCAPS:   return widGetDevCaps(wDevID, (LPWAVEINCAPSW)dwParam1, dwParam2);
    case WIDM_GETNUMDEVS:   return widGetNumDevs();
    case WIDM_GETPOS:       return widGetPosition   ((WINE_WAVEINST*)dwUser, (LPMMTIME)dwParam1, dwParam2);
    case WIDM_RESET:        return widRecorderMessage((WINE_WAVEINST*)dwUser, WINE_WM_RESETTING);
    case WIDM_START:        return widRecorderMessage((WINE_WAVEINST*)dwUser, WINE_WM_STARTING);
    case WIDM_STOP:         return widRecorderMessage((WINE_WAVEINST*)dwUser, WINE_WM_STOPPING);
    case DRV_QUERYDEVICEINTERFACESIZE: return widDevInterfaceSize(wDevID, (LPDWORD)dwParam1);
    case DRV_QUERYDEVICEINTERFACE:     return widDevInterface(wDevID, (PWCHAR)dwParam1, dwParam2);
    case DRV_QUERYDSOUNDIFACE:  return MMSYSERR_NOTSUPPORTED; /* Use emulation, as there is no advantage */
    case DRV_QUERYDSOUNDDESC:   return widDsDesc(wDevID, (PDSDRIVERDESC)dwParam1);
    default:
        FIXME("unknown message %d!\n", wMsg);
    }
    return MMSYSERR_NOTSUPPORTED;
}

#else /* HAVE_PULSEAUDIO */

/**************************************************************************
 *                  widMessage (WINEPULSE.@)
 */
DWORD WINAPI PULSE_widMessage(WORD wDevID, WORD wMsg, DWORD dwUser,
                             DWORD dwParam1, DWORD dwParam2) {
    FIXME("(%u, %04X, %08X, %08X, %08X):stub\n", wDevID, wMsg, dwUser, dwParam1, dwParam2);
    return MMSYSERR_NOTENABLED;
}

#endif /* HAVE_PULSEAUDIO */
