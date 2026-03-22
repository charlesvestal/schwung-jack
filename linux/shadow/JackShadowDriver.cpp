/*
Copyright (C) 2001 Paul Davis
Copyright (C) 2004 Grame
Copyright (C) 2025 Cycling '74 - Adapted for Move
Copyright (C) 2026 Schwung - Shadow driver (shared memory + futex)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#define __STDC_FORMAT_MACROS   // For inttypes.h to work in C++

#include <iostream>
#include <math.h>
#include <stdio.h>
#include <memory.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <string.h>
#include <limits>
#include <algorithm>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <fcntl.h>

#include "jack.h"
#include "JackShadowDriver.h"
#include "JackEngineControl.h"
#include "JackClientControl.h"
#include "JackPort.h"
#include "JackGraphManager.h"
#include "JackLockedEngine.h"
#include "JackPosixThread.h"
#include "JackCompilerDeps.h"
#include "JackServerGlobals.h"
#include "JackMidiBufferReadQueue.h"
#include "JackMidiBufferWriteQueue.h"

namespace Jack
{

const char * SHADOW_DISPLAY_HEADER = "MOVEDISP";
const size_t SHADOW_DISPLAY_HEADER_LEN = 8;
const size_t SHADOW_DISPLAY_DATA_LEN = SHADOW_DISPLAY_HEADER_LEN + SCHWUNG_JACK_DISPLAY_SIZE;

namespace {
  const jack_default_audio_sample_t mul = std::min(static_cast<jack_default_audio_sample_t>(std::numeric_limits<int16_t>::max()), static_cast<jack_default_audio_sample_t>(std::abs(std::numeric_limits<int16_t>::min())));

  inline jack_default_audio_sample_t clamp(jack_default_audio_sample_t v, jack_default_audio_sample_t min, jack_default_audio_sample_t max) {
    if (v < min)
      return min;
    if (v > max)
      return max;
    return v;
  }

  inline int16_t toint(jack_default_audio_sample_t v) {
    return static_cast<int16_t>(clamp(v, -1.0, 1.0) * mul);
  }

  inline jack_default_audio_sample_t fromint(int16_t v) {
    return static_cast<jack_default_audio_sample_t>(v) / mul;
  }

  static int futex_wait(uint32_t* addr, uint32_t val, const struct timespec* timeout) {
    return syscall(SYS_futex, addr, FUTEX_WAIT, val, timeout, NULL, 0);
  }
}

int JackShadowDriver::Open(jack_nframes_t nframes,
    jack_nframes_t samplerate,
    bool capturing,
    bool playing,
    int inchannels,
    int outchannels,
    bool monitor,
    const char* capture_driver_name,
    const char* playback_driver_name,
    jack_nframes_t capture_latency,
    jack_nframes_t playback_latency)
{
    // Generic JackAudioDriver Open
    if (JackAudioDriver::Open(nframes, samplerate, capturing, playing,
                              inchannels, outchannels, monitor, capture_driver_name, playback_driver_name,
                              capture_latency, playback_latency) != 0) {
        return -1;
    }

    memset(fInputBuffer[0], 0, sizeof(jack_default_audio_sample_t) * SCHWUNG_JACK_AUDIO_FRAMES);
    memset(fInputBuffer[1], 0, sizeof(jack_default_audio_sample_t) * SCHWUNG_JACK_AUDIO_FRAMES);
    memset(fOutputBuffer[0], 0, sizeof(jack_default_audio_sample_t) * SCHWUNG_JACK_AUDIO_FRAMES);
    memset(fOutputBuffer[1], 0, sizeof(jack_default_audio_sample_t) * SCHWUNG_JACK_AUDIO_FRAMES);

    return 0;
}

int JackShadowDriver::Attach() {
    JackPort* port;
    jack_port_id_t port_index;
    char name[REAL_JACK_PORT_NAME_SIZE+1];
    char alias[REAL_JACK_PORT_NAME_SIZE+1];
    int i;

    jack_log("JackShadowDriver::Attach fBufferSize = %ld fSampleRate = %ld", fEngineControl->fBufferSize, fEngineControl->fSampleRate);

    for (i = 0; i < fCaptureChannels; i++) {
        snprintf(alias, sizeof(alias), "shadow:out%d", i + 1);
        snprintf(name, sizeof(name), "%s:capture_%d", fClientControl.fName, i + 1);
        if (fEngine->PortRegister(fClientControl.fRefNum, name, JACK_DEFAULT_AUDIO_TYPE, CaptureDriverFlags, fEngineControl->fBufferSize, &port_index) < 0) {
            jack_error("driver: cannot register port for %s", name);
            return -1;
        }
        port = fGraphManager->GetPort(port_index);
        port->SetAlias(alias);
        fCapturePortList[i] = port_index;
        jack_log("JackShadowDriver::Attach fCapturePortList[i] port_index = %ld", port_index);
    }

    for (i = 0; i < fPlaybackChannels; i++) {
        snprintf(alias, sizeof(alias), "shadow:in%d", i + 1);
        snprintf(name, sizeof(name), "%s:playback_%d", fClientControl.fName, i + 1);
        if (fEngine->PortRegister(fClientControl.fRefNum, name, JACK_DEFAULT_AUDIO_TYPE, PlaybackDriverFlags, fEngineControl->fBufferSize, &port_index) < 0) {
            jack_error("driver: cannot register port for %s", name);
            return -1;
        }
        port = fGraphManager->GetPort(port_index);
        port->SetAlias(alias);
        fPlaybackPortList[i] = port_index;
        jack_log("JackShadowDriver::Attach fPlaybackPortList[i] port_index = %ld", port_index);
    }

    jack_nframes_t latency = SCHWUNG_JACK_AUDIO_FRAMES;
    jack_latency_range_t latency_range;
    latency_range.max = latency;
    latency_range.min = latency;

    // register MIDI
    {
      auto buf = "system:midi_playback";
      if (fEngine->PortRegister(fClientControl.fRefNum, buf,
            JACK_DEFAULT_MIDI_TYPE,
            PlaybackDriverFlags,
            fEngineControl->fBufferSize, &fMIDIPlaybackId) < 0) {
        jack_error("driver: cannot register port for %s", buf);
        return -1;
      }
      port = fGraphManager->GetPort(fMIDIPlaybackId);
      port->SetLatencyRange(JackPlaybackLatency, &latency_range);
    }
    {
      auto buf = "system:midi_capture";
      if (fEngine->PortRegister(fClientControl.fRefNum, buf,
            JACK_DEFAULT_MIDI_TYPE,
            CaptureDriverFlags,
            fEngineControl->fBufferSize, &fMIDICaptureId) < 0) {
        jack_error("driver: cannot register port for %s", buf);
        return -1;
      }
      port = fGraphManager->GetPort(fMIDICaptureId);
      port->SetLatencyRange(JackCaptureLatency, &latency_range);
    }

    //external midi i/o
    {
      auto buf = "system:midi_playback_ext";
      if (fEngine->PortRegister(fClientControl.fRefNum, buf,
            JACK_DEFAULT_MIDI_TYPE,
            PlaybackDriverFlags,
            fEngineControl->fBufferSize, &fExternalMIDIPlaybackId) < 0) {
        jack_error("driver: cannot register port for %s", buf);
        return -1;
      }
      port = fGraphManager->GetPort(fExternalMIDIPlaybackId);
      port->SetLatencyRange(JackPlaybackLatency, &latency_range);
    }
    {
      auto buf = "system:midi_capture_ext";
      if (fEngine->PortRegister(fClientControl.fRefNum, buf,
            JACK_DEFAULT_MIDI_TYPE,
            CaptureDriverFlags,
            fEngineControl->fBufferSize, &fExternalMIDICaptureId) < 0) {
        jack_error("driver: cannot register port for %s", buf);
        return -1;
      }
      port = fGraphManager->GetPort(fExternalMIDICaptureId);
      port->SetLatencyRange(JackCaptureLatency, &latency_range);
    }

    // register display
    {
      auto buf = "system:display";
      if (fEngine->PortRegister(fClientControl.fRefNum, buf,
            JACK_DEFAULT_MIDI_TYPE,
            PlaybackDriverFlags,
            fEngineControl->fBufferSize, &fDisplayId) < 0) {
        jack_error("driver: cannot register port for %s", buf);
        return -1;
      }
    }

    UpdateLatencies();

    // Open shared memory
    int fd = shm_open(SCHWUNG_JACK_SHM_PATH, O_RDWR, 0666);
    if (fd < 0) {
        jack_error("JackShadowDriver::Attach: shm_open(%s) failed: %s", SCHWUNG_JACK_SHM_PATH, strerror(errno));
        return -1;
    }

    void* ptr = mmap(NULL, SCHWUNG_JACK_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (ptr == MAP_FAILED) {
        jack_error("JackShadowDriver::Attach: mmap failed: %s", strerror(errno));
        return -1;
    }

    fShm = reinterpret_cast<SchwungJackShm*>(ptr);

    if (fShm->magic != SCHWUNG_JACK_MAGIC) {
        jack_error("JackShadowDriver::Attach: bad magic 0x%08x (expected 0x%08x)", fShm->magic, SCHWUNG_JACK_MAGIC);
        munmap(fShm, SCHWUNG_JACK_SHM_SIZE);
        fShm = NULL;
        return -1;
    }

    if (fShm->version != SCHWUNG_JACK_VERSION) {
        jack_error("JackShadowDriver::Attach: version mismatch %u (expected %u)", fShm->version, SCHWUNG_JACK_VERSION);
        munmap(fShm, SCHWUNG_JACK_SHM_SIZE);
        fShm = NULL;
        return -1;
    }

    fLastFrameCounter = fShm->frame_counter;
    jack_log("JackShadowDriver::Attach: shared memory attached successfully");

    return 0;
}

int JackShadowDriver::Close()
{
    if (fShm) {
        munmap(fShm, SCHWUNG_JACK_SHM_SIZE);
        fShm = NULL;
    }
    return JackAudioDriver::Close();
}

int JackShadowDriver::Process()
{
    // Wait for the shim to signal a new frame via futex
    struct timespec timeout;
    timeout.tv_sec = 0;
    timeout.tv_nsec = 10000000; // 10ms

    uint32_t expected = fLastFrameCounter;
    futex_wait(&fShm->frame_counter, expected, &timeout);
    fLastFrameCounter = fShm->frame_counter;

    JackDriver::CycleTakeBeginTime();
    auto r = JackAudioDriver::Process();
    JackDriver::CycleTakeEndTime();

    // Signal completion
    fShm->jack_frame_done = fShm->frame_counter;

    return r;
}

int JackShadowDriver::Read()
{
    // Deinterleave int16 audio from shared memory into float input buffers
    const int16_t* audio_in = fShm->audio_in;
    for (int i = 0; i < SCHWUNG_JACK_AUDIO_FRAMES; i++) {
        fInputBuffer[0][i] = fromint(audio_in[i * 2]);
        fInputBuffer[1][i] = fromint(audio_in[i * 2 + 1]);
    }

    // Copy float input buffers to JACK capture ports
    assert(fCaptureChannels == 2);
    for (int i = 0; i < fCaptureChannels; i++) {
        memcpy(GetInputBuffer(i), fInputBuffer[i], sizeof(jack_default_audio_sample_t) * fEngineControl->fBufferSize);
    }

    // Process MIDI from shm -> JACK capture (cable 0, rewrite to channel 16)
    {
        JackMidiBuffer * captureBuffer = reinterpret_cast<JackMidiBuffer *>(fGraphManager->GetBuffer(fMIDICaptureId, fEngineControl->fBufferSize));
        JackMidiBufferWriteQueue writeQueue;
        writeQueue.ResetMidiBuffer(captureBuffer, fEngineControl->fBufferSize);

        for (uint8_t idx = 0; idx < fShm->midi_to_jack_count; idx++) {
            const auto& event = fShm->midi_to_jack[idx];
            const auto& midi = event.message.midi;

            std::array<uint8_t, 3> data = {
                static_cast<uint8_t>(midi.channel | (midi.type << 4)),
                midi.data1,
                midi.data2
            };

            size_t size = 3;
            switch (event.message.cin) {
                case 0x5: case 0xF: size = 1; break;
                case 0x2: case 0x6: case 0xC: case 0xD: size = 2; break;
                case 0x3: case 0x4: case 0x8: case 0x9: case 0xA: case 0xB: case 0xE: size = 3; break;
                default: continue;
            }

            // Rewrite cable 0 events to channel 16
            if (event.message.cable == 0) {
                switch (midi.type) {
                    case 0x8: case 0x9: case 0xB: case 0xA: case 0xC: case 0xD: case 0xE:
                        data[0] = static_cast<uint8_t>(15 | (midi.type << 4));
                        break;
                    default:
                        break;
                }
            }

            fMIDIWriteQueue.EnqueueEvent(event.timestamp, size, data.data());
        }

        while (auto e = fMIDIWriteQueue.DequeueEvent()) {
            writeQueue.EnqueueEvent(e);
        }
    }

    // Process external MIDI from shm -> JACK ext capture
    {
        JackMidiBuffer * captureBuffer = reinterpret_cast<JackMidiBuffer *>(fGraphManager->GetBuffer(fExternalMIDICaptureId, fEngineControl->fBufferSize));
        JackMidiBufferWriteQueue writeQueue;
        writeQueue.ResetMidiBuffer(captureBuffer, fEngineControl->fBufferSize);

        for (uint8_t idx = 0; idx < fShm->ext_midi_to_jack_count; idx++) {
            const auto& event = fShm->ext_midi_to_jack[idx];
            const auto& midi = event.message.midi;

            std::array<uint8_t, 3> data = {
                static_cast<uint8_t>(midi.channel | (midi.type << 4)),
                midi.data1,
                midi.data2
            };

            size_t size = 3;
            switch (event.message.cin) {
                case 0x5: case 0xF: size = 1; break;
                case 0x2: case 0x6: case 0xC: case 0xD: size = 2; break;
                case 0x3: case 0x4: case 0x8: case 0x9: case 0xA: case 0xB: case 0xE: size = 3; break;
                default: continue;
            }

            fExternalMIDIWriteQueue.EnqueueEvent(event.timestamp, size, data.data());
        }

        while (auto e = fExternalMIDIWriteQueue.DequeueEvent()) {
            writeQueue.EnqueueEvent(e);
        }
    }

    // Read display port and copy to shared memory
    {
        JackMidiBuffer * displayBuffer = reinterpret_cast<JackMidiBuffer *>(fGraphManager->GetBuffer(fDisplayId, fEngineControl->fBufferSize));
        JackMidiBufferReadQueue readQueue;
        readQueue.ResetMidiBuffer(displayBuffer);
        while (auto e = readQueue.DequeueEvent()) {
            if (e->size == SHADOW_DISPLAY_DATA_LEN) {
                // Strip "MOVEDISP" header, copy 1024 bytes of display data
                memcpy(fShm->display_data, e->buffer + SHADOW_DISPLAY_HEADER_LEN, SCHWUNG_JACK_DISPLAY_SIZE);
                fShm->display_active = 1;
            }
        }
    }

    return 0;
}

int JackShadowDriver::Write()
{
    // Read JACK playback ports into float output buffers
    assert(fPlaybackChannels == 2);
    for (int i = 0; i < fPlaybackChannels; i++) {
        memcpy(fOutputBuffer[i], GetOutputBuffer(i), sizeof(jack_default_audio_sample_t) * fEngineControl->fBufferSize);
    }

    // Interleave float output buffers to int16 in shared memory
    int16_t* audio_out = fShm->audio_out;
    for (int i = 0; i < SCHWUNG_JACK_AUDIO_FRAMES; i++) {
        audio_out[i * 2] = toint(fOutputBuffer[0][i]);
        audio_out[i * 2 + 1] = toint(fOutputBuffer[1][i]);
    }

    // Read JACK MIDI playback -> shm midi_from_jack (cable 0, ch16 only)
    {
        JackMidiBuffer * playbackBuffer = reinterpret_cast<JackMidiBuffer *>(fGraphManager->GetBuffer(fMIDIPlaybackId, fEngineControl->fBufferSize));
        JackMidiBufferReadQueue readQueue;
        readQueue.ResetMidiBuffer(playbackBuffer);

        uint8_t count = 0;
        while (auto e = readQueue.DequeueEvent()) {
            if (count >= SCHWUNG_JACK_MIDI_OUT_MAX) break;
            if (e->size == 0 || e->size > 3) continue;

            const uint8_t status = e->buffer[0];
            const uint8_t chan = status & 0x0F;
            const uint8_t masked = status & 0xF0;

            // Filter: only channel 16 (index 15) for cable 0, plus sysex/reset
            bool valid = false;
            if ((masked == 0x80 || masked == 0x90 || masked == 0xB0) && chan == 15) {
                valid = true;
            } else if (status == 0xFF || status == 0xF0 || status == 0xF7 || (status & 0x80) == 0) {
                valid = true;
            }
            if (!valid) continue;

            SchwungJackMidiMsg midi;
            midi.channel = e->buffer[0] & 0x0F;
            midi.type = (e->buffer[0] & 0xF0) >> 4;
            midi.data1 = e->size > 1 ? e->buffer[1] : 0;
            midi.data2 = e->size > 2 ? e->buffer[2] : 0;

            // Rewrite channel 16 back to channel 1 for cable 0
            if (chan == 15) {
                midi.channel = 0;
            }

            SchwungJackUsbMidiMsg usb;
            usb.cable = 0;
            usb.midi = midi;

            // Determine CIN from message type
            switch (e->size) {
                case 1: usb.cin = 0x5; break;
                case 2: usb.cin = (masked == 0xC0 || masked == 0xD0) ? 0xC : 0x6; break;
                case 3: usb.cin = (midi.type & 0x08) ? midi.type : 0x4; break;
                default: usb.cin = 0; break;
            }

            fShm->midi_from_jack[count++] = usb;
        }
        fShm->midi_from_jack_count = count;
    }

    // Read JACK external MIDI playback -> shm ext_midi_from_jack
    {
        JackMidiBuffer * playbackBuffer = reinterpret_cast<JackMidiBuffer *>(fGraphManager->GetBuffer(fExternalMIDIPlaybackId, fEngineControl->fBufferSize));
        JackMidiBufferReadQueue readQueue;
        readQueue.ResetMidiBuffer(playbackBuffer);

        uint8_t count = 0;
        while (auto e = readQueue.DequeueEvent()) {
            if (count >= SCHWUNG_JACK_MIDI_OUT_MAX) break;
            if (e->size == 0 || e->size > 3) continue;

            SchwungJackMidiMsg midi;
            midi.channel = e->buffer[0] & 0x0F;
            midi.type = (e->buffer[0] & 0xF0) >> 4;
            midi.data1 = e->size > 1 ? e->buffer[1] : 0;
            midi.data2 = e->size > 2 ? e->buffer[2] : 0;

            SchwungJackUsbMidiMsg usb;
            usb.cable = 2;
            usb.midi = midi;

            switch (e->size) {
                case 1: usb.cin = 0x5; break;
                case 2: {
                    uint8_t masked = e->buffer[0] & 0xF0;
                    usb.cin = (masked == 0xC0 || masked == 0xD0) ? 0xC : 0x6;
                    break;
                }
                case 3: usb.cin = (midi.type & 0x08) ? midi.type : 0x4; break;
                default: usb.cin = 0; break;
            }

            fShm->ext_midi_from_jack[count++] = usb;
        }
        fShm->ext_midi_from_jack_count = count;
    }

    return 0;
}

int JackShadowDriver::is_realtime() const
{
    return 1;
}

} // end of namespace


#ifdef __cplusplus
extern "C"
{
#endif

SERVER_EXPORT const jack_driver_desc_t* driver_get_descriptor ()
{
    jack_driver_desc_t * desc;
    jack_driver_desc_filler_t filler;

    desc = jack_driver_descriptor_construct("shadow", JackDriverMaster, "Schwung shadow driver (shared memory with Move shim)", &filler);

    return desc;
}

static Jack::JackShadowDriver* g_shadow_driver = nullptr;

SERVER_EXPORT Jack::JackDriverClientInterface* driver_initialize(Jack::JackLockedEngine* engine, Jack::JackSynchro* table, const JSList* params)
{
    jack_nframes_t nframes = SCHWUNG_JACK_AUDIO_FRAMES;
    jack_nframes_t samplerate = 44100;
    bool capturing = true;
    bool playing = true;
    int inchannels = 2;
    int outchannels = 2;
    bool monitor = false;
    const char* capture_driver_name = "shadow";
    const char* playback_driver_name = "shadow";

    jack_nframes_t capture_latency = 0;
    jack_nframes_t playback_latency = 0;

    g_shadow_driver = new Jack::JackShadowDriver("system", "shadow", engine, table);
    Jack::JackDriverClientInterface* threaded_driver = new Jack::JackThreadedDriver(g_shadow_driver);
    if (g_shadow_driver->Open(nframes, samplerate, capturing, playing, inchannels, outchannels, monitor, capture_driver_name, playback_driver_name, capture_latency, playback_latency) == 0) {
        return threaded_driver;
    } else {
        delete threaded_driver;
        return NULL;
    }

}

#ifdef __cplusplus
}
#endif
