/*
 * Wine Driver for PulseAudio
 * http://pulseaudio.org/
 *
 * Copyright    2009 Arthur Taylor <theycallhimart@gmail.com>
 *
 * Contains code from other wine sound drivers.
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
#include <stdio.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winreg.h"
#include "mmddk.h"
#include "ks.h"
#include "ksguid.h"
#include "ksmedia.h"

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <poll.h>

#ifdef HAVE_PULSEAUDIO

#include "wine/unicode.h"
#include "wine/debug.h"
#include "wine/library.h"

#include <winepulse.h>
#include <pulse/pulseaudio.h>
WINE_DEFAULT_DEBUG_CHANNEL(wave);

/* These strings used only for tracing */
const char * PULSE_getCmdString(enum win_wm_message msg) {
    static char unknown[32];
#define MSG_TO_STR(x) case x: return #x
    switch(msg) {
    MSG_TO_STR(WINE_WM_PAUSING);
    MSG_TO_STR(WINE_WM_RESTARTING);
    MSG_TO_STR(WINE_WM_RESETTING);
    MSG_TO_STR(WINE_WM_HEADER);
    MSG_TO_STR(WINE_WM_BREAKLOOP);
    MSG_TO_STR(WINE_WM_CLOSING);
    MSG_TO_STR(WINE_WM_STARTING);
    MSG_TO_STR(WINE_WM_STOPPING);
    MSG_TO_STR(WINE_WM_XRUN);
    MSG_TO_STR(WINE_WM_FEED);
    }
#undef MSG_TO_STR
    sprintf(unknown, "UNKNOWN(0x%08x)", msg);
    return unknown;
}

/*======================================================================*
 *          Ring Buffer Functions - copied from winealsa.drv            *
 *======================================================================*/

/* unless someone makes a wineserver kernel module, Unix pipes are faster than win32 events */
#define USE_PIPE_SYNC

#ifdef USE_PIPE_SYNC
#define INIT_OMR(omr) do { if (pipe(omr->msg_pipe) < 0) { omr->msg_pipe[0] = omr->msg_pipe[1] = -1; } } while (0)
#define CLOSE_OMR(omr) do { close(omr->msg_pipe[0]); close(omr->msg_pipe[1]); } while (0)
#define SIGNAL_OMR(omr) do { int x = 0; write((omr)->msg_pipe[1], &x, sizeof(x)); } while (0)
#define CLEAR_OMR(omr) do { int x = 0; read((omr)->msg_pipe[0], &x, sizeof(x)); } while (0)
#define RESET_OMR(omr) do { } while (0)
#define WAIT_OMR(omr, sleep) \
  do { struct pollfd pfd; pfd.fd = (omr)->msg_pipe[0]; \
       pfd.events = POLLIN; poll(&pfd, 1, sleep); } while (0)
#else
#define INIT_OMR(omr) do { omr->msg_event = CreateEventW(NULL, FALSE, FALSE, NULL); } while (0)
#define CLOSE_OMR(omr) do { CloseHandle(omr->msg_event); } while (0)
#define SIGNAL_OMR(omr) do { SetEvent((omr)->msg_event); } while (0)
#define CLEAR_OMR(omr) do { } while (0)
#define RESET_OMR(omr) do { ResetEvent((omr)->msg_event); } while (0)
#define WAIT_OMR(omr, sleep) \
  do { WaitForSingleObject((omr)->msg_event, sleep); } while (0)
#endif

#define PULSE_RING_BUFFER_INCREMENT      64

/******************************************************************
 *                  PULSE_InitRingMessage
 *
 * Initialize the ring of messages for passing between driver's caller
 * and playback/record thread
 */
int PULSE_InitRingMessage(PULSE_MSG_RING* omr)
{
    omr->msg_toget = 0;
    omr->msg_tosave = 0;
    INIT_OMR(omr);
    omr->ring_buffer_size = PULSE_RING_BUFFER_INCREMENT;
    omr->messages = HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,omr->ring_buffer_size * sizeof(PULSE_MSG));

    InitializeCriticalSection(&omr->msg_crst);
    omr->msg_crst.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": PULSE_MSG_RING.msg_crst");
    return 0;
}

/******************************************************************
 *                  PULSE_DestroyRingMessage
 *
 */
int PULSE_DestroyRingMessage(PULSE_MSG_RING* omr)
{
    CLOSE_OMR(omr);
    HeapFree(GetProcessHeap(),0,omr->messages);
    omr->messages = NULL;
    omr->ring_buffer_size = PULSE_RING_BUFFER_INCREMENT;
    omr->msg_crst.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection(&omr->msg_crst);
    return 0;
}
/******************************************************************
 *                  PULSE_ResetRingMessage
 *
 */
void PULSE_ResetRingMessage(PULSE_MSG_RING* omr)
{
    RESET_OMR(omr);
}

/******************************************************************
 *                  PULSE_WaitRingMessage
 *
 */
void PULSE_WaitRingMessage(PULSE_MSG_RING* omr, DWORD sleep)
{
    WAIT_OMR(omr, sleep);
}

/******************************************************************
 *                  PULSE_AddRingMessage
 *
 * Inserts a new message into the ring (should be called from DriverProc derived routines)
 */
int PULSE_AddRingMessage(PULSE_MSG_RING* omr, enum win_wm_message msg, DWORD param, BOOL wait)
{
    HANDLE      hEvent = INVALID_HANDLE_VALUE;

    EnterCriticalSection(&omr->msg_crst);
    if ((omr->msg_toget == ((omr->msg_tosave + 1) % omr->ring_buffer_size)))
    {
        int old_ring_buffer_size = omr->ring_buffer_size;
        omr->ring_buffer_size += PULSE_RING_BUFFER_INCREMENT;
        omr->messages = HeapReAlloc(GetProcessHeap(),0,omr->messages, omr->ring_buffer_size * sizeof(PULSE_MSG));
        /* Now we need to rearrange the ring buffer so that the new
           buffers just allocated are in between omr->msg_tosave and
           omr->msg_toget.
        */
        if (omr->msg_tosave < omr->msg_toget)
        {
            memmove(&(omr->messages[omr->msg_toget + PULSE_RING_BUFFER_INCREMENT]),
                    &(omr->messages[omr->msg_toget]),
                    sizeof(PULSE_MSG)*(old_ring_buffer_size - omr->msg_toget)
                    );
            omr->msg_toget += PULSE_RING_BUFFER_INCREMENT;
        }
    }
    if (wait)
    {
        hEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (hEvent == INVALID_HANDLE_VALUE)
        {
            ERR("can't create event !?\n");
            LeaveCriticalSection(&omr->msg_crst);
            return 0;
        }
        /* fast messages have to be added at the start of the queue */
        omr->msg_toget = (omr->msg_toget + omr->ring_buffer_size - 1) % omr->ring_buffer_size;

        omr->messages[omr->msg_toget].msg = msg;
        omr->messages[omr->msg_toget].param = param;
        omr->messages[omr->msg_toget].hEvent = hEvent;
    }
    else
    {
        omr->messages[omr->msg_tosave].msg = msg;
        omr->messages[omr->msg_tosave].param = param;
        omr->messages[omr->msg_tosave].hEvent = INVALID_HANDLE_VALUE;
        omr->msg_tosave = (omr->msg_tosave + 1) % omr->ring_buffer_size;
    }
    LeaveCriticalSection(&omr->msg_crst);
    /* signal a new message */
    SIGNAL_OMR(omr);
    if (wait)
    {
        /* wait for playback/record thread to have processed the message */
        WaitForSingleObject(hEvent, INFINITE);
        CloseHandle(hEvent);
    }
    return 1;
}

/******************************************************************
 *                  PULSE_RetrieveRingMessage
 *
 * Get a message from the ring. Should be called by the playback/record thread.
 */
int PULSE_RetrieveRingMessage(PULSE_MSG_RING* omr,
                                   enum win_wm_message *msg, DWORD *param, HANDLE *hEvent)
{
    EnterCriticalSection(&omr->msg_crst);

    if (omr->msg_toget == omr->msg_tosave) /* buffer empty ? */
    {
        LeaveCriticalSection(&omr->msg_crst);
        return 0;
    }

    *msg = omr->messages[omr->msg_toget].msg;
    omr->messages[omr->msg_toget].msg = 0;
    *param = omr->messages[omr->msg_toget].param;
    *hEvent = omr->messages[omr->msg_toget].hEvent;
    omr->msg_toget = (omr->msg_toget + 1) % omr->ring_buffer_size;
    CLEAR_OMR(omr);
    LeaveCriticalSection(&omr->msg_crst);
    return 1;
}

/**************************************************************************
 * Utility Functions
 *************************************************************************/

/******************************************************************
 *                  PULSE_SetupFormat
 *
 * Checks to see if the audio format in wf is supported, and if so set up the
 * pa_sample_spec at ss to that format.
 */
BOOL PULSE_SetupFormat(LPWAVEFORMATEX wf, pa_sample_spec *ss) {
    WAVEFORMATEXTENSIBLE *wfex;

    ss->channels = wf->nChannels;
    ss->rate = wf->nSamplesPerSec;
    ss->format = PA_SAMPLE_INVALID;

    if (ss->rate < DSBFREQUENCY_MIN || ss->rate > DSBFREQUENCY_MAX) return FALSE;

    switch (wf->wFormatTag) {
        case WAVE_FORMAT_PCM:
            /* MSDN says that for WAVE_FORMAT_PCM, nChannels must be 1 or 2 and
             * wBitsPerSample must be 8 or 16, yet other values are used by some
             * applications in the wild for surround. */
            if (ss->channels > 6 || ss->channels < 1) return FALSE;
            ss->format = wf->wBitsPerSample == 8  ? PA_SAMPLE_U8
                       : wf->wBitsPerSample == 16 ? PA_SAMPLE_S16NE
                       : wf->wBitsPerSample == 32 ? PA_SAMPLE_S32NE
                       : PA_SAMPLE_INVALID;
        break;

        case WAVE_FORMAT_MULAW:
            if (wf->wBitsPerSample == 8) ss->format = PA_SAMPLE_ULAW;
            break;

        case WAVE_FORMAT_ALAW:
            if (wf->wBitsPerSample == 8) ss->format = PA_SAMPLE_ALAW;
            break;

        case WAVE_FORMAT_EXTENSIBLE:
            if (wf->cbSize > 22) return FALSE;
            if (ss->channels < 1 || ss->channels > 6) return FALSE;
            wfex = (WAVEFORMATEXTENSIBLE *)wf;
            if (IsEqualGUID(&wfex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM)) {
                if (wf->wBitsPerSample == wfex->Samples.wValidBitsPerSample) {
                    ss->format = wf->wBitsPerSample == 8  ? PA_SAMPLE_U8
                               : wf->wBitsPerSample == 16 ? PA_SAMPLE_S16NE
                               : wf->wBitsPerSample == 32 ? PA_SAMPLE_S32NE
                               : PA_SAMPLE_INVALID;
                } else {
                    return FALSE;
                }
            } else if (wf->wBitsPerSample != wfex->Samples.wValidBitsPerSample) {
                return FALSE;
            } else if ((IsEqualGUID(&wfex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))) {
                ss->format = PA_SAMPLE_FLOAT32NE;
            } else {
                WARN("only KSDATAFORMAT_SUBTYPE_PCM and KSDATAFORMAT_SUBTYPE_IEEE_FLOAT of WAVE_FORMAT_EXTENSIBLE supported\n");
                return FALSE;
            }
            break;

        default:
            WARN("only WAVE_FORMAT_PCM, WAVE_FORMAT_MULAW, WAVE_FORMAT_ALAW and WAVE_FORMAT_EXTENSIBLE supported\n");
            return FALSE;
    }

    if (!pa_sample_spec_valid(ss)) return FALSE;
    if (wf->nBlockAlign != pa_frame_size(ss)) {
        ERR("wf->nBlockAlign != the format frame size!\n");
        return FALSE;
    }

    return TRUE;
}

/******************************************************************
 *                  PULSE_SetupFormat
 *
 * Converts the current time to a MMTIME structure. lpTime shold be validated
 * before calling.
 */
HRESULT PULSE_UsecToMMTime(pa_usec_t time, LPMMTIME lpTime, const pa_sample_spec *ss) {
    pa_usec_t temp;
    size_t bytes;

    /* Convert to milliseconds */
    time /= 1000;

    bytes = time * pa_bytes_per_second(ss) / 1000;
    /* Align to frame size */
    bytes -= bytes % pa_frame_size(ss);

    switch (lpTime->wType) {
    case TIME_SAMPLES:
        lpTime->u.sample = bytes / pa_frame_size(ss);
        TRACE("TIME_SAMPLES=%u\n", lpTime->u.sample);
        break;
    case TIME_MS:
        lpTime->u.ms = time;
        TRACE("TIME_MS=%u\n", lpTime->u.ms);
        break;
    case TIME_SMPTE:
        lpTime->u.smpte.fps = 30;
        temp = bytes / pa_frame_size(ss);
        temp += ss->rate / lpTime->u.smpte.fps - 1;
        lpTime->u.smpte.sec = time/1000;
        temp -= lpTime->u.smpte.sec * ss->rate;
        lpTime->u.smpte.min = lpTime->u.smpte.sec / 60;
        lpTime->u.smpte.sec -= 60 * lpTime->u.smpte.min;
        lpTime->u.smpte.hour = lpTime->u.smpte.min / 60;
        lpTime->u.smpte.min -= 60 * lpTime->u.smpte.hour;
        lpTime->u.smpte.frame = temp  * lpTime->u.smpte.fps / ss->rate;
        TRACE("TIME_SMPTE=%02u:%02u:%02u:%02u\n",
              lpTime->u.smpte.hour, lpTime->u.smpte.min,
              lpTime->u.smpte.sec, lpTime->u.smpte.frame);
        break;
    default:
        WARN("Format %d not supported, using TIME_BYTES !\n", lpTime->wType);
        lpTime->wType = TIME_BYTES;
        /* fall through */
    case TIME_BYTES:
        lpTime->u.cb = bytes;
        TRACE("TIME_BYTES=%u\n", lpTime->u.cb);
        break;
    }

    return MMSYSERR_NOERROR;
}

/**************************************************************************
 *                  PULSE_WaitForOperation
 *
 * Waits for pa operations to complete, and dereferences the operation.
 */
void PULSE_WaitForOperation(pa_operation *o) {
    if (!o) return;

    for (;;) {
        if (pa_operation_get_state(o) != PA_OPERATION_RUNNING)
            break;
        pa_threaded_mainloop_wait(PULSE_ml);
    }
    pa_operation_unref(o);
}

/**************************************************************************
 * Common Callbacks
 */

/**************************************************************************
 *                  PULSE_StreamRequestCallback
 *
 * Called by the pulse mainloop whenever it wants/has audio data.
 */
void PULSE_StreamRequestCallback(pa_stream *s, size_t nbytes, void *userdata) {
    WINE_WAVEINST *ww = (WINE_WAVEINST*)userdata;

    TRACE("Server has %u bytes\n", nbytes);

    /* Make sure that the player/recorder is running */
    if (ww->hThread != INVALID_HANDLE_VALUE && ww->msgRing.messages) {
        PULSE_AddRingMessage(&ww->msgRing, WINE_WM_FEED, (DWORD)nbytes, FALSE);
    }
}


/**************************************************************************
 *                  PULSE_StreamSuspendedCallback               [internal]
 *
 * Called by the pulse mainloop any time stream playback is intentionally
 * suspended or resumed from being suspended.
 */
void PULSE_StreamSuspendedCallback(pa_stream *s, void *userdata) {
    WINE_WAVEINST *wwo = (WINE_WAVEINST*)userdata;
    assert(s && wwo);

    /* Currently we handle this kinda like an underrun. Perhaps we should
     * tell the client somehow so it doesn't just hang? */

    if (!pa_stream_is_suspended(s) && wwo->hThread != INVALID_HANDLE_VALUE && wwo->msgRing.ring_buffer_size > 0)
        PULSE_AddRingMessage(&wwo->msgRing, WINE_WM_XRUN, 0, FALSE);
}

/**************************************************************************
 *                  PULSE_StreamUnderflowCallback               [internal]
 *
 * Called by the pulse mainloop when the prebuf runs out of data.
 */
void PULSE_StreamUnderflowCallback(pa_stream *s, void *userdata) {
    WINE_WAVEINST *wwo = (WINE_WAVEINST*)userdata;
    assert(s && wwo);

    /* If we aren't playing, don't care ^_^ */
    if (wwo->state != WINE_WS_PLAYING) return;

    TRACE("Underrun occurred.\n");

    if (wwo->hThread != INVALID_HANDLE_VALUE && wwo->msgRing.ring_buffer_size > 0);
        PULSE_AddRingMessage(&wwo->msgRing, WINE_WM_XRUN, 0, FALSE);
}

/**************************************************************************
 *                  PULSE_StreamMovedCallback                   [internal]
 *
 * Called by the pulse mainloop when the stream gets moved, resulting in
 * possibly different metrics.
 */
void PULSE_StreamMovedCallback(pa_stream *s, void *userdata) {
    FIXME("stub");
}


/******************************************************************
 *                  PULSE_StreamStateCallback
 *
 * Called by pulse whenever the state of the stream changes.
 */
void PULSE_StreamStateCallback(pa_stream *s, void *userdata) {
    assert(s);

    switch (pa_stream_get_state(s)) {
        case PA_STREAM_READY:
            TRACE("Stream %p ready\n", userdata);
            break;

        case PA_STREAM_TERMINATED: /* Stream closed normally */
            TRACE("Stream %p terminated\n", userdata);
            break;

        case PA_STREAM_FAILED: /* Stream closed not-normally */
            ERR("Stream %p failed!\n", userdata);
            break;

        case PA_STREAM_UNCONNECTED:
        case PA_STREAM_CREATING:
            return;
    }
    pa_threaded_mainloop_signal(PULSE_ml, 0);
}

/**************************************************************************
 *                  PULSE_StreamSucessCallback
 */
void PULSE_StreamSuccessCallback(pa_stream *s, int success, void *userdata) {
    if (!success)
        WARN("Stream %p operation failed: %s\n", userdata, pa_strerror(pa_context_errno(PULSE_context)));

    pa_threaded_mainloop_signal(PULSE_ml, 0);
}

/**************************************************************************
 *                  PULSE_ContextSuccessCallback
 */
void PULSE_ContextSuccessCallback(pa_context *c, int success, void *userdata) {
    if (!success) ERR("Context operation failed: %s\n", pa_strerror(pa_context_errno(c)));
    pa_threaded_mainloop_signal(PULSE_ml, 0);
}

/**************************************************************************
 * Connection management and sink / source management.
 */

/**************************************************************************
 *                  PULSE_ContextStateCallback                  [internal]
 */
static void PULSE_ContextStateCallback(pa_context *c, void *userdata) {
    assert(c);

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;

        case PA_CONTEXT_READY:
        case PA_CONTEXT_TERMINATED:
            pa_threaded_mainloop_signal(PULSE_ml, 0);
            break;

        case PA_CONTEXT_FAILED:
            ERR("Context failed: %s\n", pa_strerror(pa_context_errno(c)));
            pa_threaded_mainloop_signal(PULSE_ml, 0);
            break;
    }
}

/**************************************************************************
 *                  PULSE_AllocateWaveinDevice                  [internal]
 *
 * Creates or adds a device to WInDev based on the pa_source_info.
 */
static void PULSE_AllocateWaveinDevice(const char *name, const char *device, const char *description, const pa_cvolume *v) {
    WINE_WAVEDEV *wdi;

    if (WInDev)
        wdi = HeapReAlloc(GetProcessHeap(), 0, WInDev, sizeof(WINE_WAVEDEV) * (PULSE_WidNumDevs + 1));
    else
        wdi = HeapAlloc(GetProcessHeap(), 0, sizeof(WINE_WAVEDEV));

    if (!wdi) return;

    WInDev = wdi;
    wdi = &WInDev[PULSE_WidNumDevs++];
    memset(wdi, 0, sizeof(WINE_WAVEDEV));
    memset(&(wdi->caps.in), 0, sizeof(wdi->caps.in));
    snprintf(wdi->interface_name, MAXPNAMELEN * 2, "winepulse: %s", name);
    wdi->device_name = pa_xstrdup(device);
    MultiByteToWideChar(CP_UTF8, 0, description, -1, wdi->caps.in.szPname, sizeof(wdi->caps.in.szPname)/sizeof(WCHAR));
    wdi->caps.in.szPname[sizeof(wdi->caps.in.szPname)/sizeof(WCHAR) - 1] = '\0';
    wdi->caps.in.wMid = MM_CREATIVE;
    wdi->caps.in.wPid = MM_CREATIVE_SBP16_WAVEOUT;
    wdi->caps.in.vDriverVersion = 0x0100;
    wdi->caps.in.wChannels = v->channels == 1 ? 1 : 2;
    wdi->caps.in.dwFormats = PULSE_ALL_FORMATS;
    memset(&wdi->ds_desc, 0, sizeof(DSDRIVERDESC));
    memcpy(wdi->ds_desc.szDesc, description, min(sizeof(wdi->ds_desc.szDesc) - 1, strlen(description)));
    memcpy(wdi->ds_desc.szDrvname, "winepulse.drv", 14);
    wdi->ds_caps.dwMinSecondarySampleRate = DSBFREQUENCY_MIN;
    wdi->ds_caps.dwMaxSecondarySampleRate = DSBFREQUENCY_MAX;
    wdi->ds_caps.dwPrimaryBuffers = 1;
    wdi->ds_caps.dwFlags = \
        DSCAPS_PRIMARYMONO |
        DSCAPS_PRIMARYSTEREO |
        DSCAPS_PRIMARY8BIT |
        DSCAPS_PRIMARY16BIT |
        DSCAPS_SECONDARYMONO |
        DSCAPS_SECONDARYSTEREO |
        DSCAPS_SECONDARY8BIT |
        DSCAPS_SECONDARY16BIT |
        DSCCAPS_MULTIPLECAPTURE;
}

/**************************************************************************
 *                  PULSE_AllocateWaveoutDevice                 [internal]
 *
 *  Creates or adds a sink to the WOutDev array.
 */
static void PULSE_AllocateWaveoutDevice(const char *name, const char *device, const char *description, const pa_cvolume *v) {
    WINE_WAVEDEV *wdo;
    int x;

    if (WOutDev)
        wdo = HeapReAlloc(GetProcessHeap(), 0, WOutDev, sizeof(WINE_WAVEDEV) * (PULSE_WodNumDevs + 1));
    else
        wdo = HeapAlloc(GetProcessHeap(), 0, sizeof(WINE_WAVEDEV));

    if (!wdo) return;

    WOutDev = wdo;
    wdo = &WOutDev[PULSE_WodNumDevs++];
    memset(wdo, 0, sizeof(WINE_WAVEDEV));

    wdo->device_name = pa_xstrdup(device);
    wdo->volume.channels = v->channels;
    for (x = 0; x < v->channels; x++) wdo->volume.values[x] = v->values[x];
    snprintf(wdo->interface_name, MAXPNAMELEN * 2, "winepulse: %s", name);
    MultiByteToWideChar(CP_UTF8, 0, description, -1, wdo->caps.out.szPname, sizeof(wdo->caps.out.szPname)/sizeof(WCHAR));
    wdo->caps.out.szPname[sizeof(wdo->caps.out.szPname)/sizeof(WCHAR) - 1] = '\0';
    wdo->caps.out.wMid = MM_CREATIVE;
    wdo->caps.out.wPid = MM_CREATIVE_SBP16_WAVEOUT;
    wdo->caps.out.vDriverVersion = 0x0100;
    wdo->caps.out.dwSupport = WAVECAPS_VOLUME | WAVECAPS_SAMPLEACCURATE;// | WAVECAPS_DIRECTSOUND;
    if (v->channels >= 2) {
        wdo->caps.out.wChannels = 2;
        wdo->caps.out.dwSupport |= WAVECAPS_LRVOLUME;
    } else
        wdo->caps.out.wChannels = 1;
    wdo->caps.out.dwFormats = PULSE_ALL_FORMATS;
    memset(&wdo->ds_desc, 0, sizeof(DSDRIVERDESC));
    memcpy(wdo->ds_desc.szDesc, description, min(sizeof(wdo->ds_desc.szDesc) - 1, strlen(description)));
    memcpy(wdo->ds_desc.szDrvname, "winepulse.drv", 14);
    wdo->ds_caps.dwMinSecondarySampleRate = DSBFREQUENCY_MIN;
    wdo->ds_caps.dwMaxSecondarySampleRate = DSBFREQUENCY_MAX;
    wdo->ds_caps.dwPrimaryBuffers = 1;
    wdo->ds_caps.dwFlags = \
        DSCAPS_PRIMARYMONO |
        DSCAPS_PRIMARYSTEREO |
        DSCAPS_PRIMARY8BIT |
        DSCAPS_PRIMARY16BIT |
        DSCAPS_SECONDARYMONO |
        DSCAPS_SECONDARYSTEREO |
        DSCAPS_SECONDARY8BIT |
        DSCAPS_SECONDARY16BIT |
        DSCAPS_CONTINUOUSRATE;
}

/**************************************************************************
 *                  PULSE_SourceInfoCallback                    [internal]
 */
static void PULSE_SourceInfoCallback(pa_context *c, const pa_source_info *i, int eol, void *userdata) {

    if (!eol && i)
        PULSE_AllocateWaveinDevice(i->name, i->name, i->description, &i->volume);

    pa_threaded_mainloop_signal(PULSE_ml, 0);
}

/**************************************************************************
 *                  PULSE_SinkInfoCallback                      [internal]
 */
static void PULSE_SinkInfoCallback(pa_context *c, const pa_sink_info *i, int eol, void *userdata) {

    if (!eol && i)
        PULSE_AllocateWaveoutDevice(i->name, i->name, i->description, &i->volume);

    pa_threaded_mainloop_signal(PULSE_ml, 0);
}

/**************************************************************************
 *                  PULSE_ContextNotifyCallback                 [internal]
 */
static void PULSE_ContextNotifyCallback(pa_context *c, void *userdata) {
    pa_threaded_mainloop_signal(PULSE_ml, 0);
}

/**************************************************************************
 *                  PULSE_WaveClose                             [internal]
 *
 * Disconnect from the server, deallocated the WaveIn/WaveOut devices, stop and
 * free the mainloop.
 */
static LONG PULSE_WaveClose(void) {
    int x;
    TRACE("()\n");
    if (!PULSE_ml) return DRV_FAILURE;

    pa_threaded_mainloop_lock(PULSE_ml);
    /* device_name is allocated with pa_xstrdup, free with pa_xfree */
    for (x = 0; x < PULSE_WodNumDevs; x++) pa_xfree(WOutDev[x].device_name);
    for (x = 0; x < PULSE_WidNumDevs; x++) pa_xfree(WInDev[x].device_name);
    HeapFree(GetProcessHeap(), 0, WOutDev);
    HeapFree(GetProcessHeap(), 0, WInDev);
    if (PULSE_context) {
        PULSE_WaitForOperation(pa_context_drain(PULSE_context, PULSE_ContextNotifyCallback, NULL));
        pa_context_disconnect(PULSE_context);
        pa_context_unref(PULSE_context);
        PULSE_context = NULL;
    }

    pa_threaded_mainloop_unlock(PULSE_ml);
    pa_threaded_mainloop_stop(PULSE_ml);
    pa_threaded_mainloop_free(PULSE_ml);
    PULSE_ml = NULL;

    return DRV_SUCCESS;
}

/**************************************************************************
 *                  PULSE_WaveInit                              [internal]
 *
 * Connects to the pulseaudio server, tries to discover sinks and sources and
 * allocates the WaveIn/WaveOut devices.
 */
static LONG PULSE_WaveInit(void) {
    char *app_name;
    char path[PATH_MAX];
    char *offset = NULL;
    pa_cvolume fake_cvolume;

    WOutDev = NULL;
    WInDev = NULL;
    PULSE_WodNumDevs = 0;
    PULSE_WidNumDevs = 0;
    PULSE_context = NULL;
    PULSE_ml = NULL;

    if (!(PULSE_ml = pa_threaded_mainloop_new())) {
        ERR("Failed to create mainloop object.");
        return DRV_FAILURE;
    }

    /* Application name giving to pulse should be unique to the binary so that
     * pulse-*-restore can be useful */

    /* Get binary path, and remove path a-la strrchr */
    if (GetModuleFileNameA(NULL, path, PATH_MAX))
        offset = strrchr(path, '\\');

    if (offset && ++offset && offset < path + PATH_MAX) {
        app_name = pa_xmalloc(strlen(offset) + 8);
        snprintf(app_name, strlen(offset) + 8, "WINE [%s]", offset);
    } else
        app_name = pa_xstrdup("WINE Application");

    TRACE("App name is \"%s\"\n", app_name);

    pa_threaded_mainloop_start(PULSE_ml);
    PULSE_context = pa_context_new(pa_threaded_mainloop_get_api(PULSE_ml), app_name);
    assert(PULSE_context);
    pa_xfree(app_name);

    pa_context_set_state_callback(PULSE_context, PULSE_ContextStateCallback, NULL);

    pa_threaded_mainloop_lock(PULSE_ml);

    TRACE("libpulse protocol version: %u. API Version %u\n", pa_context_get_protocol_version(PULSE_context), PA_API_VERSION);
    if (pa_context_connect(PULSE_context, NULL, 0, NULL) < 0)
        goto fail;

    /* Wait for connection */
    for (;;) {
        pa_context_state_t state = pa_context_get_state(PULSE_context);

        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED)
            goto fail;

        if (state == PA_CONTEXT_READY)
            break;

        pa_threaded_mainloop_wait(PULSE_ml);
    }

    TRACE("Connected to server %s with protocol version: %i.\n",
        pa_context_get_server(PULSE_context),
        pa_context_get_server_protocol_version(PULSE_context));

    fake_cvolume.channels = 2;
    pa_cvolume_reset(&fake_cvolume, 2);
    /* FIXME Translations? */
    PULSE_AllocateWaveoutDevice("default", NULL, "Default", &fake_cvolume);
    PULSE_AllocateWaveinDevice("default", NULL, "Default", &fake_cvolume);
    PULSE_WaitForOperation(pa_context_get_sink_info_list(PULSE_context, PULSE_SinkInfoCallback, &PULSE_WodNumDevs));
    PULSE_WaitForOperation(pa_context_get_source_info_list(PULSE_context, PULSE_SourceInfoCallback, &PULSE_WidNumDevs));
    TRACE("Found %u output and %u input device(s).\n", PULSE_WodNumDevs - 1, PULSE_WidNumDevs - 1);

    pa_threaded_mainloop_unlock(PULSE_ml);

    return DRV_SUCCESS;

fail:
    pa_threaded_mainloop_unlock(PULSE_ml);
    /* Only warn, because if we failed wine may still choose the next driver */
    WARN("Failed to connect to server\n");
    return DRV_FAILURE;
}

#endif /* HAVE_PULSEAUDIO */

/**************************************************************************
 *                  DriverProc (WINEPULSE.@)
 */
LRESULT CALLBACK PULSE_DriverProc(DWORD_PTR dwDevID, HDRVR hDriv, UINT wMsg,
                                 LPARAM dwParam1, LPARAM dwParam2) {

    switch(wMsg) {
#ifdef HAVE_PULSEAUDIO
    case DRV_LOAD:              return PULSE_WaveInit();
    case DRV_FREE:              return PULSE_WaveClose();
    case DRV_OPEN:              return 1;
    case DRV_CLOSE:             return 1;
    case DRV_ENABLE:            return 1;
    case DRV_DISABLE:           return 1;
    case DRV_QUERYCONFIGURE:    return 1;
    case DRV_CONFIGURE:         MessageBoxA(0, "PulseAudio MultiMedia Driver !", "PulseAudio Driver", MB_OK); return 1;
    case DRV_INSTALL:           return DRVCNF_RESTART;
    case DRV_REMOVE:            return DRVCNF_RESTART;
#endif
    default:
        return DefDriverProc(dwDevID, hDriv, wMsg, dwParam1, dwParam2);
    }
}
