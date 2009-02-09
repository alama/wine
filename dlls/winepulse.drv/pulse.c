/*
 * Wine Driver for PulseAudio
 * http://pulseaudio.org/
 *
 * Copyright	2008 Arthur Taylor <art@ified.ca>
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
#include "mmddk.h"

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

/*
 * - Need to subscribe to sink/source events and keep the WInDev and WOutDev
 *   structures updated
 */

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
 *		PULSE_InitRingMessage
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
 *		PULSE_DestroyRingMessage
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
 *		PULSE_ResetRingMessage
 *
 */
void PULSE_ResetRingMessage(PULSE_MSG_RING* omr)
{
    RESET_OMR(omr);
}

/******************************************************************
 *		PULSE_WaitRingMessage
 *
 */
void PULSE_WaitRingMessage(PULSE_MSG_RING* omr, DWORD sleep)
{
    WAIT_OMR(omr, sleep);
}

/******************************************************************
 *		PULSE_AddRingMessage
 *
 * Inserts a new message into the ring (should be called from DriverProc derived routines)
 */
int PULSE_AddRingMessage(PULSE_MSG_RING* omr, enum win_wm_message msg, DWORD param, BOOL wait)
{
    HANDLE	hEvent = INVALID_HANDLE_VALUE;

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
        if (omr->msg_toget != omr->msg_tosave && omr->messages[omr->msg_toget].msg != WINE_WM_HEADER)
            FIXME("two fast messages in the queue!!!!\n"); /* toget = %d(%s), tosave=%d(%s)\n",
                  omr->msg_toget,ALSA_getCmdString(omr->messages[omr->msg_toget].msg),
                  omr->msg_tosave,ALSA_getCmdString(omr->messages[omr->msg_tosave].msg)); */

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
 *		PULSE_RetrieveRingMessage
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
 */

/******************************************************************
 *		PULSE_setupFormat
 *
 * Checks to see if the audio format in wf is supported, and if so set up the
 * pa_sample_spec at ss to that format.
 */
BOOL PULSE_setupFormat(LPWAVEFORMATEX wf, pa_sample_spec *ss) {

    if (wf->nSamplesPerSec<DSBFREQUENCY_MIN||wf->nSamplesPerSec>DSBFREQUENCY_MAX)
        return FALSE;

    ss->channels=wf->nChannels;
    ss->rate=wf->nSamplesPerSec;

    if (wf->wFormatTag == WAVE_FORMAT_PCM) {
	if (ss->channels==1 || ss->channels==2) {
	    switch (wf->wBitsPerSample) {
		case 8:
		    ss->format = PA_SAMPLE_U8;
		    return TRUE;
		case 16:
		    ss->format = PA_SAMPLE_S16NE;
		    return TRUE;
	    }
        }
    } else if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE 	* wfex = (WAVEFORMATEXTENSIBLE *)wf;

        if (wf->cbSize == 22 &&
            (IsEqualGUID(&wfex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM))) {
            if (ss->channels>=1 && ss->channels<=6) {
                if (wf->wBitsPerSample==wfex->Samples.wValidBitsPerSample) {
		    switch (wf->wBitsPerSample) {
			case 8:
			    ss->format=PA_SAMPLE_U8;
			    return TRUE;
			case 16:
			    ss->format=PA_SAMPLE_S16NE;
			    return TRUE;
			case 43:
			    ss->format=PA_SAMPLE_S32NE;
			    return TRUE;
		    }
                } else
                    WARN("wBitsPerSample != wValidBitsPerSample not supported yet\n");
            }
	} else if (wf->cbSize == 22 &&
	(IsEqualGUID(&wfex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))) {
	    if (ss->channels>=1 && ss->channels<=6) {
		if (wf->wBitsPerSample==wfex->Samples.wValidBitsPerSample && wf->wBitsPerSample == 32) {
		    ss->format=PA_SAMPLE_FLOAT32NE;
		    return TRUE;
		}
	    }
	} else
            WARN("only KSDATAFORMAT_SUBTYPE_PCM and KSDATAFORMAT_SUBTYPE_IEEE_FLOAT "
                 "supported\n");
    } else if (wf->wFormatTag == WAVE_FORMAT_MULAW || wf->wFormatTag == WAVE_FORMAT_ALAW) {
        if (wf->wBitsPerSample==8) {
	    ss->format= (wf->wFormatTag==WAVE_FORMAT_MULAW) ? PA_SAMPLE_ULAW : PA_SAMPLE_ALAW;
            return TRUE;
	} else
            ERR("WAVE_FORMAT_MULAW and WAVE_FORMAT_ALAW wBitsPerSample must = 8\n");
    } else
        WARN("only WAVE_FORMAT_PCM, WAVE_FORMAT_MULAW, WAVE_FORMAT_ALAW and WAVE_FORMAT_EXTENSIBLE supported\n");

    return FALSE;
}

/******************************************************************
 *		PULSE_free_wavedevs			[internal]
 *
 * Free and deallocated all the wavedevs in the array of size allocated
 */
static void PULSE_free_wavedevs(WINE_WAVEDEV *wd, DWORD *allocated) {
    DWORD y, x = *allocated;
    WINE_WAVEDEV *current_device;
    WINE_WAVEINST *current_instance;

    TRACE("%i\n", *allocated);

    for (; x>0; ) {
	current_device = &wd[--x];
	
	pa_xfree(current_device->device_name);

	for (y = 0; y < PULSE_MAX_STREAM_INSTANCES; y++) {
	    if ((current_instance = current_device->instance[y])) {
		if (current_instance->hThread != INVALID_HANDLE_VALUE && current_instance->msgRing.messages) {
		    PULSE_AddRingMessage(&current_instance->msgRing, WINE_WM_CLOSING, 1, TRUE);
		    if (current_instance->hThread != INVALID_HANDLE_VALUE) {
			/* Overkill? */
			TerminateThread(current_instance->hThread, 1);
			current_instance->hThread = INVALID_HANDLE_VALUE;
		    }
		    if (current_instance->stream)
		        pa_stream_unref(current_instance->stream);
		    PULSE_DestroyRingMessage(&current_instance->msgRing);
		    current_device->instance[current_instance->instance_ref] = NULL;
		    if (current_instance->buffer_attr)
			pa_xfree(current_instance->buffer_attr);
		    HeapFree(GetProcessHeap(), 0, current_instance);
		}
	    }
	}
    }

    HeapFree(GetProcessHeap(), 0, wd);
    *allocated = 0;
}

/******************************************************************
 *		PULSE_wait_for_operation
 *
 * Waits for pa operations to complete, ensures success and dereferences the
 * operations.
 */
void PULSE_wait_for_operation(pa_operation *o, WINE_WAVEINST *wd) {
    assert(o && wd);

    TRACE("Waiting...");

    for (;;) {

	if (!wd->stream ||
	    !PULSE_context ||
	    pa_context_get_state(PULSE_context) != PA_CONTEXT_READY ||
	    pa_stream_get_state(wd->stream) != PA_STREAM_READY) {
	    wd->state = WINE_WS_FAILED;
	    if (wd->hThread != INVALID_HANDLE_VALUE && wd->msgRing.messages)
		PULSE_AddRingMessage(&wd->msgRing, WINE_WM_CLOSING, 1, FALSE);
	    break;
	}

	if (pa_operation_get_state(o) != PA_OPERATION_RUNNING)
	    break;

	pa_threaded_mainloop_wait(PULSE_ml);
    }
    TRACE(" done\n");
    pa_operation_unref(o);
}

/**************************************************************************
 * Common Callbacks
 */

/**************************************************************************
 * 			PULSE_stream_request_callback
 *
 * Called by the pulse mainloop whenever it wants or has audio data.
 */
void PULSE_stream_request_callback(pa_stream *s, size_t nbytes, void *userdata) {
    WINE_WAVEINST *ww = (WINE_WAVEINST*)userdata;
    assert(s && ww);

    TRACE("Asking to feed.\n");

    /* Make sure that the player is running */
    if (ww->hThread != INVALID_HANDLE_VALUE && ww->msgRing.messages) {
    	PULSE_AddRingMessage(&ww->msgRing, WINE_WM_FEED, (DWORD)nbytes, FALSE);
    }
}

/******************************************************************
 *		PULSE_stream_state_callback
 *
 * Called by pulse whenever the state of the stream changes.
 */
void PULSE_stream_state_callback(pa_stream *s, void *userdata) {
    WINE_WAVEINST *wd = (WINE_WAVEINST*)userdata;
    assert(s && wd);

    switch (pa_stream_get_state(s)) {

    case PA_STREAM_READY:
	TRACE("Stream ready\n");
	break;
    case PA_STREAM_TERMINATED:
	TRACE("Stream terminated\n");
	break;
    case PA_STREAM_FAILED:
	WARN("Stream failed!\n");
	wd->state = WINE_WS_FAILED;
	if (wd->hThread != INVALID_HANDLE_VALUE && wd->msgRing.messages)
		PULSE_AddRingMessage(&wd->msgRing, WINE_WM_CLOSING, 1, FALSE);
	break;
    case PA_STREAM_UNCONNECTED:
    case PA_STREAM_CREATING:
	return;
    }
    pa_threaded_mainloop_signal(PULSE_ml, 0);
}

/**************************************************************************
 * 			PULSE_stream_sucess_callback
 */
void PULSE_stream_success_callback(pa_stream *s, int success, void *userdata) {
    if (!success)
	WARN("Stream operation failed: %s\n", pa_strerror(pa_context_errno(PULSE_context)));

    pa_threaded_mainloop_signal(PULSE_ml, 0);
}

/**************************************************************************
 * 			PULSE_context_success_callback
 */
void PULSE_context_success_callback(pa_context *c, int success, void *userdata) {
    if (!success)
	WARN("Context operation failed: %s\n", pa_strerror(pa_context_errno(c)));

    pa_threaded_mainloop_signal(PULSE_ml, 0);
}

/**************************************************************************
 * Connection management and sink / source management.
 */

/**************************************************************************
 * 				PULSE_context_state_callback    [internal]
 */
static void PULSE_context_state_callback(pa_context *c, void *userdata) {
    assert(c);

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;

	case PA_CONTEXT_READY:
	case PA_CONTEXT_TERMINATED:
	    pa_threaded_mainloop_signal(PULSE_ml, 0);
	    break;

	case PA_CONTEXT_FAILED:
	default:
	    ERR("Conneciton failure: %s\n", pa_strerror(pa_context_errno(c)));
	
	    if (PULSE_context) {
		pa_context_disconnect(PULSE_context);
	    }

	    PULSE_free_wavedevs(WOutDev, &PULSE_WodNumDevs);
	    PULSE_free_wavedevs(WInDev, &PULSE_WidNumDevs);

	    pa_threaded_mainloop_signal(PULSE_ml, 0);
    }
}

/**************************************************************************
 * 		    PULSE_add_input_device			[internal]
 *
 * Creates or adds a device to WInDev based on the pa_source_info, or if
 * pa_source_info is null, a default.
 */
static void PULSE_add_input_device(pa_source_info *i, DWORD *allocated) {
    WINE_WAVEDEV *wwi;
    const char *description;
    int x;

    if (WInDev)
	wwi = HeapReAlloc(GetProcessHeap(), 0, WInDev, sizeof(WINE_WAVEDEV) * ((*allocated)+1));
    else
	wwi = HeapAlloc(GetProcessHeap(), 0, sizeof(WINE_WAVEDEV));

    if (!wwi)
	return;
    
    WInDev = wwi;
    wwi = &WInDev[(*allocated)++];

    if (i) {
	description = i->description;
	wwi->device_name = pa_xstrdup(i->name);
	strcpy(wwi->interface_name, "winepulse: ");
	memcpy(wwi->interface_name + strlen(wwi->interface_name),
	    i->name, min(strlen(i->name),
	    sizeof(wwi->interface_name) - strlen("winepulse:   ")));
    } else {
	description = pa_xstrdup("PulseAudio Server Default");
	wwi->device_name = NULL;
	strcpy(wwi->interface_name, "winepulse: default");
    }

    memset(wwi, 0, sizeof(WINE_WAVEDEV));
    memset(&(wwi->caps.in), 0, sizeof(wwi->caps.in));
    MultiByteToWideChar(CP_ACP, 0, description, -1, wwi->caps.in.szPname, sizeof(wwi->caps.in.szPname)/sizeof(WCHAR));
    wwi->caps.in.szPname[sizeof(wwi->caps.in.szPname)/sizeof(WCHAR) - 1] = '\0';
    wwi->caps.in.wMid = MM_CREATIVE;
    wwi->caps.in.wPid = MM_CREATIVE_SBP16_WAVEOUT;
    wwi->caps.in.vDriverVersion = 0x0100;
    wwi->caps.in.wChannels = 2;
    wwi->caps.in.dwFormats |= 
        WAVE_FORMAT_1M08 |	/* Mono	    11025Hz 8-bit  */
        WAVE_FORMAT_1M16 |	/* Mono	    11025Hz 16-bit */
        WAVE_FORMAT_1S08 |	/* Stereo   11025Hz 8-bit  */
        WAVE_FORMAT_1S16 |	/* Stereo   11025Hz 16-bit */
        WAVE_FORMAT_2M08 |	/* Mono	    22050Hz 8-bit  */
        WAVE_FORMAT_2M16 |	/* Mono	    22050Hz 16-bit */
        WAVE_FORMAT_2S08 |	/* Stereo   22050Hz 8-bit  */
	WAVE_FORMAT_2S16 |	/* Stereo   22050Hz 16-bit */
        WAVE_FORMAT_4M08 |	/* Mono	    44100Hz 8-bit  */
	WAVE_FORMAT_4M16 |	/* Mono	    44100Hz 16-bit */
        WAVE_FORMAT_4S08 |	/* Stereo   44100Hz 8-bit  */
	WAVE_FORMAT_4S16 |	/* Stereo   44100Hz 16-bit */
        WAVE_FORMAT_48M08 |	/* Mono	    48000Hz 8-bit  */
        WAVE_FORMAT_48S08 |	/* Stereo   48000Hz 8-bit  */
        WAVE_FORMAT_48M16 |	/* Mono	    48000Hz 16-bit */
        WAVE_FORMAT_48S16 |	/* Stereo   48000Hz 16-bit */
	WAVE_FORMAT_96M08 |	/* Mono	    96000Hz 8-bit  */
	WAVE_FORMAT_96S08 |	/* Stereo   96000Hz 8-bit  */
        WAVE_FORMAT_96M16 |	/* Mono	    96000Hz 16-bit */
	WAVE_FORMAT_96S16 ;	/* Stereo   96000Hz 16-bit */

    /* NULL out the instance pointers */
    for (x = 0; x < PULSE_MAX_STREAM_INSTANCES; x++) wwi->instance[x] = NULL;
}

/**************************************************************************
 * 		    PULSE_add_output_device			[internal]
 *
 * Creates or adds a device to WOutDev based on the pa_sinl_info, or if
 * pa_sink_info is null, a default.
 */
static void PULSE_add_output_device(pa_sink_info *i, DWORD *allocated) {
    WINE_WAVEDEV *wwo;
    const char *description;
    int x;

    if (WOutDev)
        wwo = HeapReAlloc(GetProcessHeap(), 0, WOutDev, sizeof(WINE_WAVEDEV) * ((*allocated)+1));
    else
        wwo = HeapAlloc(GetProcessHeap(), 0, sizeof(WINE_WAVEDEV));
    
    if (!wwo)
	return;

    WOutDev = wwo;
    wwo = &WOutDev[(*allocated)++];

    if (i) {
	description = i->description;
	wwo->device_name = pa_xstrdup(i->name);
	strcpy(wwo->interface_name, "winepulse: ");
	memcpy(wwo->interface_name + strlen(wwo->interface_name),
	    wwo->device_name, min(strlen(wwo->device_name),
	    sizeof(wwo->interface_name) - strlen("winepulse:   ")));
    } else {
	description = pa_xstrdup("PulseAudio Server Default");
	wwo->device_name = NULL;
	strcpy(wwo->interface_name, "winepulse: default");
    }

    memset(wwo, 0, sizeof(WINE_WAVEDEV));
    memset(&(wwo->caps.out), 0, sizeof(wwo->caps.out));
    MultiByteToWideChar(CP_ACP, 0, description, -1, wwo->caps.out.szPname, sizeof(wwo->caps.out.szPname)/sizeof(WCHAR));
    wwo->caps.out.szPname[sizeof(wwo->caps.out.szPname)/sizeof(WCHAR) - 1] = '\0';
    wwo->caps.out.wMid = MM_CREATIVE;
    wwo->caps.out.wPid = MM_CREATIVE_SBP16_WAVEOUT;
    wwo->caps.out.vDriverVersion = 0x0100;
    wwo->caps.out.dwSupport |= WAVECAPS_VOLUME | WAVECAPS_LRVOLUME;
    wwo->caps.out.wChannels = 2;
    wwo->caps.out.dwFormats |= 
        WAVE_FORMAT_1M08 |	/* Mono	    11025Hz 8-bit  */
        WAVE_FORMAT_1M16 |	/* Mono	    11025Hz 16-bit */
        WAVE_FORMAT_1S08 |	/* Stereo   11025Hz 8-bit  */
        WAVE_FORMAT_1S16 |	/* Stereo   11025Hz 16-bit */
        WAVE_FORMAT_2M08 |	/* Mono	    22050Hz 8-bit  */
        WAVE_FORMAT_2M16 |	/* Mono	    22050Hz 16-bit */
        WAVE_FORMAT_2S08 |	/* Stereo   22050Hz 8-bit  */
        WAVE_FORMAT_2S16 |	/* Stereo   22050Hz 16-bit */
        WAVE_FORMAT_4M08 |	/* Mono	    44100Hz 8-bit  */
        WAVE_FORMAT_4M16 |	/* Mono	    44100Hz 16-bit */
        WAVE_FORMAT_4S08 |	/* Stereo   44100Hz 8-bit  */
        WAVE_FORMAT_4S16 |	/* Stereo   44100Hz 16-bit */
        WAVE_FORMAT_48M08 |	/* Mono	    48000Hz 8-bit  */
        WAVE_FORMAT_48S08 |	/* Stereo   48000Hz 8-bit  */
        WAVE_FORMAT_48M16 |	/* Mono	    48000Hz 16-bit */
        WAVE_FORMAT_48S16 |	/* Stereo   48000Hz 16-bit */
        WAVE_FORMAT_96M08 |	/* Mono	    96000Hz 8-bit  */
        WAVE_FORMAT_96S08 |	/* Stereo   96000Hz 8-bit  */
	WAVE_FORMAT_96M16 |	/* Mono	    96000HZ 16-bit */
        WAVE_FORMAT_96S16 ;	/* Stereo   96000Hz 16-bit */

    wwo->left_vol = PA_VOLUME_NORM;
    wwo->right_vol = PA_VOLUME_NORM;

    /* NULL out the instance pointers */
    for (x = 0; x < PULSE_MAX_STREAM_INSTANCES; x++) wwo->instance[x] = NULL;
}

/**************************************************************************
 * 		    PULSE_source_info_callback			[internal]
 *
 * Calls PULSE_add_input_device to add the source to the list of devices
 */
static void PULSE_source_info_callback(pa_context *c, pa_source_info *i, int eol, void *userdata) {
    assert(c);
    if (!eol && i)
	if (i->monitor_of_sink == PA_INVALID_INDEX) /* For now only add non-montior sources */
	PULSE_add_input_device(i, (DWORD*)userdata);

    pa_threaded_mainloop_signal(PULSE_ml, 0);
}

/**************************************************************************
 * 		    PULSE_sink_info_callback			[internal]
 *
 * Calls PULSE_add_output_device to add the sink to the list of devices
 */
static void PULSE_sink_info_callback(pa_context *c, pa_sink_info *i, int eol, void *userdata) {
    assert(c);
    if (!eol && i)
	PULSE_add_output_device(i, (DWORD*)userdata);

    pa_threaded_mainloop_signal(PULSE_ml, 0);
}

/**************************************************************************
 * 				PULSE_WaveInit                  [internal]
 *
 * Connects to the pulseaudio server, tries to discover sinks and sources and
 * allocates the WaveIn/WaveOut devices.
 */
LONG PULSE_WaveInit(void) {
    pa_operation *o = NULL;
    DWORD allocated;

    WOutDev = NULL;
    WInDev = NULL;
    PULSE_WodNumDevs = 0;
    PULSE_WidNumDevs = 0;
    PULSE_context = NULL;
    PULSE_ml = NULL;

    if (!(PULSE_ml = pa_threaded_mainloop_new())) {
	WARN("Failed to create mainloop object.");
	return -1;
    }
    
    pa_threaded_mainloop_start(PULSE_ml);
    
    /* FIXME: better name? */
    PULSE_context = pa_context_new(pa_threaded_mainloop_get_api(PULSE_ml), "Wine Application");
    assert(PULSE_context);
    
    pa_context_set_state_callback(PULSE_context, PULSE_context_state_callback, NULL);
    
    if (pa_context_get_state(PULSE_context) != PA_CONTEXT_UNCONNECTED)
	return 0;
    
    pa_threaded_mainloop_lock(PULSE_ml);

    TRACE("Attempting to connect to pulseaudio server.\n");
    if (pa_context_connect(PULSE_context, NULL, 0, NULL) < 0) {
        WARN("failed to connect context object %s\n", pa_strerror(pa_context_errno(PULSE_context)));
        return -1;
    }

    /* Wait for connection */
    for (;;) {
	pa_context_state_t state = pa_context_get_state(PULSE_context);

	if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
	    ERR("Failed to connect to pulseaudio server.\n");
	    pa_context_unref(PULSE_context);
	    pa_threaded_mainloop_unlock(PULSE_ml);
	    pa_threaded_mainloop_stop(PULSE_ml);
	    pa_threaded_mainloop_free(PULSE_ml);
	    return -1;
	}

	if (state == PA_CONTEXT_READY) {
	    TRACE("Connection succeeded!\n");
	    break;
	}
    
	pa_threaded_mainloop_wait(PULSE_ml);
    }

    /* Ask for all the sinks and create objects in the WOutDev array */
    allocated=0;
    /* add a fake output (server default sink) */
    PULSE_add_output_device(NULL, &allocated);
    o = pa_context_get_sink_info_list(PULSE_context, (pa_sink_info_cb_t)PULSE_sink_info_callback, &allocated);
    assert(o);
    while (pa_operation_get_state(o) != PA_OPERATION_DONE)
	pa_threaded_mainloop_wait(PULSE_ml);
    pa_operation_unref(o);
    pa_threaded_mainloop_unlock(PULSE_ml);
    TRACE("Allocated %i output device(s).\n",allocated);
    PULSE_WodNumDevs=allocated;
    if (PULSE_WodNumDevs == 1) /* Only the fake sink exists */
	PULSE_free_wavedevs(WOutDev, &PULSE_WodNumDevs);

    /* Repeate for all the sources */
    allocated=0;
    /* add a fake input (server default source) */
    PULSE_add_input_device(NULL, &allocated);
    pa_threaded_mainloop_lock(PULSE_ml);
    o = pa_context_get_source_info_list(PULSE_context, (pa_source_info_cb_t)PULSE_source_info_callback, &allocated);
    assert(o);
    while (pa_operation_get_state(o) != PA_OPERATION_DONE)
	pa_threaded_mainloop_wait(PULSE_ml);
    pa_operation_unref(o);
    pa_threaded_mainloop_unlock(PULSE_ml);
    TRACE("Allocated %i input device(s).\n", allocated);
    PULSE_WidNumDevs=allocated;
    if (PULSE_WidNumDevs == 1) /* Only the fake source exists */
	PULSE_free_wavedevs(WInDev, &PULSE_WidNumDevs);

    return 1;
}

/**************************************************************************
 * 				PULSE_WaveClose                 [internal]
 *
 * Disconnect from the server, deallocated the WaveIn/WaveOut devices, stop and
 * free the mainloop.
 */

LONG PULSE_WaveClose(void) {
    pa_threaded_mainloop_lock(PULSE_ml);
    if (PULSE_context) {
	pa_context_disconnect(PULSE_context);
	pa_context_unref(PULSE_context);
	PULSE_context = NULL;
    }

    PULSE_free_wavedevs(WOutDev, &PULSE_WodNumDevs);
    PULSE_free_wavedevs(WInDev, &PULSE_WidNumDevs);
    
    pa_threaded_mainloop_unlock(PULSE_ml);
    pa_threaded_mainloop_stop(PULSE_ml);
    pa_threaded_mainloop_free(PULSE_ml);
    
    return 1;
}

#endif /* HAVE_PULSEAUDIO */

/**************************************************************************
 * 				DriverProc (WINEPULSE.@)
 */
LRESULT CALLBACK PULSE_DriverProc(DWORD_PTR dwDevID, HDRVR hDriv, UINT wMsg,
                                 LPARAM dwParam1, LPARAM dwParam2) {

    switch(wMsg) {
#ifdef HAVE_PULSEAUDIO
    case DRV_LOAD:		PULSE_WaveInit();
				return 1;
    case DRV_FREE:		return PULSE_WaveClose();
    case DRV_OPEN:		return 1;
    case DRV_CLOSE:		return 1;
    case DRV_ENABLE:		return 1;
    case DRV_DISABLE:		return 1;
    case DRV_QUERYCONFIGURE:	return 1;
    case DRV_CONFIGURE:		MessageBoxA(0, "PulseAudio MultiMedia Driver !", "PulseAudio Driver", MB_OK);	return 1;
    case DRV_INSTALL:		return DRVCNF_RESTART;
    case DRV_REMOVE:		return DRVCNF_RESTART;
#endif
    default:
	return DefDriverProc(dwDevID, hDriv, wMsg, dwParam1, dwParam2);
    }
}
