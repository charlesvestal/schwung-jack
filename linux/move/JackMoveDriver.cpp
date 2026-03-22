/*
Copyright (C) 2001 Paul Davis
Copyright (C) 2004 Grame
Copyright (C) 2025 Cycling '74 - Adapted for Move

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

#include "jack.h"
#include "JackMoveDriver.h"
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

const char * DISPLAY_HEADER = "MOVEDISP";
const size_t DISPLAY_HEADER_LEN = 8;
const size_t DISPLAY_DATA_LEN = DISPLAY_HEADER_LEN + ABLSPI_DISPLAY_BYTES;

namespace {
  void moveProcessMidi(void* pUserContext, AblSpiMidiEvent event) {
    JackMoveDriver * inst = reinterpret_cast<JackMoveDriver *>(pUserContext);
    inst->processMidi(event);
  }
  AblSpiUsbMidiMessage moveProvideMidi(void* pUserContext) {
    JackMoveDriver * inst = reinterpret_cast<JackMoveDriver *>(pUserContext);
    return inst->provideMidi();
  }
  void moveProcessAudio(void* pUserContext,
      const int16_t* pInputLeft, const int16_t* pInputRight,
      int16_t* pOutputLeft, int16_t* pOutputRight,
      int numSamples) {
    JackMoveDriver * inst = reinterpret_cast<JackMoveDriver *>(pUserContext);
    inst->processAudio(pInputLeft, pInputRight, pOutputLeft, pOutputRight, numSamples);
  }

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
}

void JackMoveDriver::processAudio(const int16_t* pInputLeft, const int16_t* pInputRight, int16_t* pOutputLeft, int16_t* pOutputRight, int numSamples) {
  for (int i = 0; i < numSamples; i++) {
    fInputBuffer[0][i] = fromint(pInputLeft[i]);
    fInputBuffer[1][i] = fromint(pInputRight[i]);
    pOutputLeft[i] = toint(fOutputBuffer[0][i]);
    pOutputRight[i] = toint(fOutputBuffer[1][i]);
  }
}

AblSpiUsbMidiMessage JackMoveDriver::provideMidi() {

    if (mPendingMIDIEventsEnd > mPendingMIDIEventsStart) {
      auto cur = mPendingMIDIEventsStart;
      mPendingMIDIEventsStart++;
      return mPendingMIDIQueue[cur];
    }

    uint8_t cable = 0;
    jack_midi_event_t * e = nullptr;
    size_t size = 0;
    jack_midi_data_t * data = nullptr;

    while (true) {
        e = fMIDIReadQueue.DequeueEvent();
        if (e && e->size > 0) {
            size = e->size;
            data = e->buffer;

            const auto status = data[0];
            const auto chan = status & 0x0F;
            const auto masked = status & 0xF0;

            //we need to filter out anything other than channel "16" and rewrite it to "1"
            //move only accepts notes, cc, reset and sysex
            if ((masked == 0x80 || masked == 0x90 || masked == 0xB0)) {
                if (chan == 15) {
                    //valid, rewrite chan and break;
                    data[0] = masked;
                    break;
                }
            } else if (status == 0xFF || status == 0xF0 || status == 0xF7 || (status & 0x80) == 0) {
                //otherwise, if it is a reset, sysex start, end or continue, send it along
                break;
            }
        } else {
            //external always goes out on cable 2
            cable = 2;
            e = fExternalMIDIReadQueue.DequeueEvent();
            size = e ? e->size : 0;
            data = e ? e->buffer : nullptr;
            break;
        }
    }

    AblSpiMidiMessage r = AblSpiMidiMessage{0, 0, 0, 0};

    if (data && size > 0) {
      if (size <= 3) {
        r.channel = data[0] & 0x0F;

        if (data[0] == 0xF0) {
          mSysexActive = true;
        } else if (mSysexActive && (data[0] & 0x80) != 0 && data[0] != 0xF7) {
          //incase there is some other message in here
          mSysexActive = false;
        }

        r.type = (data[0] & 0xF0) >> 4;
        if (size > 1) {
          r.data1 = data[1];
          if (size > 2) {
            r.data2 = data[2];
          }
        }

        auto cur = encodeAsUsbMidi(r, cable);

        if (mSysexActive) {
          cur.cin = 0x4;
          mSysexActive = false;

          if (data[0] == 0xF7) {
            cur.cin = 0x5; //end in 1 byte
          } else if (size > 1 && data[1] == 0xF7) {
            cur.cin = 0x6; //end in 2 byte
          } else if (size > 2 && data[2] == 0xF7) {
            cur.cin = 0x7;
          } else {
            mSysexActive = true;
          }
        }

        return cur;
      } else {
        //sysex

        mPendingMIDIEventsStart = 0;
        mPendingMIDIEventsEnd = 0;

        //skip first packet
        for (size_t i = 3; i < size; i += 3) {
          size_t len = std::min(size - i, (size_t)3);

          r.channel = data[i] & 0x0F;
          r.type = (data[i] & 0xF0) >> 4;
          r.data1 = len > 1 ? data[i + 1] : 0;
          r.data2 = len > 2 ? data[i + 2] : 0;

          uint8_t cin = 0x4;
          //last packet?
          switch (len) {
            case 1:
              cin = 0x5; //end in 1 byte
              break;
            case 2:
              cin = 0x6; //end in 2 byte
              break;
            case 3:
            default:
              if (r.data2 == 0xF7) {
                cin = 0x7;
              }
              break;
          }

          mPendingMIDIQueue[mPendingMIDIEventsEnd] = AblSpiUsbMidiMessage{ cin, cable, r };
          mPendingMIDIEventsEnd++;
        }

        {
          r.channel = data[0] & 0x0F;
          r.type = (data[0] & 0xF0) >> 4;
          r.data1 = data[1];
          r.data2 = data[2];

          auto usb = AblSpiUsbMidiMessage{ 0x4, cable, r };

          return usb;
        }
      }
    }
    return AblSpiUsbMidiMessage{ 0, 0, r };
}

void JackMoveDriver::processMidi(AblSpiMidiEvent event) {
    const auto& midi = event.message.midi;
    const auto cable = event.message.cable;

    std::array<uint8_t, 3> data = {
        static_cast<uint8_t>(midi.channel | (midi.type << 4)), //reconstruct status
        midi.data1,
        midi.data2
    };

    size_t size = 3;
    switch (event.message.cin) {
        case 0x5:
        case 0xF:
            size = 1;
            break;

        case 0x2:
        case 0x6:
        case 0xC:
        case 0xD:
            size = 2;
            break;

        case 0x3:
        case 0x4:
        case 0x8:
        case 0x9:
        case 0xA:
        case 0xB:
        case 0xE:
            size = 3;
            break;

        default:
            //shouldn't be getting these?
            return;
    }

    if (cable == 0) { //from move hardware, power status, buttons, encoders
        //rewrite to channel "16"
        switch (midi.type) {
            case 0x8:
            case 0x9:
            case 0xB:
            case 0xA:
            case 0xC:
            case 0xD:
            case 0xE:
                data[0] = static_cast<uint8_t>(15 | (midi.type << 4));
                break;
            default:
                break;
        }
        //std::cout << "sending midi on cable 0: " << (int)size << " " << (int)data[0] << " " << (int)data[1] << " " << (int)data[2] << std::endl;
        fMIDIWriteQueue.EnqueueEvent(event.timestamp, size, data.data());
    } else if (cable >= 2 && cable <= 10) { //external midi i/o
        fExternalMIDIWriteQueue.EnqueueEvent(event.timestamp, size, data.data());
    } else {
      //std::cout << "got cable event: " << (int)event.message.cable << " cin: " << (int)event.message.cin << " data: " << (int)((midi.type << 4) | midi.channel) << " " << (int)midi.data1 << " " << (int)midi.data2 << std::endl;
    }
}

int JackMoveDriver::Open(jack_nframes_t nframes,
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

    memset(fInputBuffer[0], 0, sizeof(jack_default_audio_sample_t) * ABLSPI_AUDIO_BUFFER_SIZE);
    memset(fInputBuffer[1], 0, sizeof(jack_default_audio_sample_t) * ABLSPI_AUDIO_BUFFER_SIZE);
    memset(fOutputBuffer[0], 0, sizeof(jack_default_audio_sample_t) * ABLSPI_AUDIO_BUFFER_SIZE);
    memset(fOutputBuffer[1], 0, sizeof(jack_default_audio_sample_t) * ABLSPI_AUDIO_BUFFER_SIZE);

    return 0;
}

int JackMoveDriver::Attach() {
    JackPort* port;
    jack_port_id_t port_index;
    char name[REAL_JACK_PORT_NAME_SIZE+1];
    char alias[REAL_JACK_PORT_NAME_SIZE+1];
    int i;

    jack_log("JackMoveDriver::Attach fBufferSize = %ld fSampleRate = %ld", fEngineControl->fBufferSize, fEngineControl->fSampleRate);

    for (i = 0; i < fCaptureChannels; i++) {
        snprintf(alias, sizeof(alias), "move:out%d", i + 1);
        snprintf(name, sizeof(name), "%s:capture_%d", fClientControl.fName, i + 1);
        if (fEngine->PortRegister(fClientControl.fRefNum, name, JACK_DEFAULT_AUDIO_TYPE, CaptureDriverFlags, fEngineControl->fBufferSize, &port_index) < 0) {
            jack_error("driver: cannot register port for %s", name);
            return -1;
        }
        port = fGraphManager->GetPort(port_index);
        port->SetAlias(alias);
        fCapturePortList[i] = port_index;
        jack_log("JackMoveDriver::Attach fCapturePortList[i] port_index = %ld", port_index);
    }

    for (i = 0; i < fPlaybackChannels; i++) {
        snprintf(alias, sizeof(alias), "move:in%d", i + 1);
        snprintf(name, sizeof(name), "%s:playback_%d", fClientControl.fName, i + 1);
        if (fEngine->PortRegister(fClientControl.fRefNum, name, JACK_DEFAULT_AUDIO_TYPE, PlaybackDriverFlags, fEngineControl->fBufferSize, &port_index) < 0) {
            jack_error("driver: cannot register port for %s", name);
            return -1;
        }
        port = fGraphManager->GetPort(port_index);
        port->SetAlias(alias);
        fPlaybackPortList[i] = port_index;
        jack_log("JackMoveDriver::Attach fPlaybackPortList[i] port_index = %ld", port_index);

    }

    // TODO what is the actual latency?
    jack_nframes_t latency = ABLSPI_AUDIO_BUFFER_SIZE;
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
      //TODO set metadata
      /*
         auto port = fGraphManager->GetPort(fDisplayId);
         */
    }

    UpdateLatencies();
    return ablspiInit(&fSPIDev, this);
}

int JackMoveDriver::Close()
{
    int res = ablspiDestroy(&fSPIDev);
    if (res == 0) {
      return JackAudioDriver::Close();
    }
    return res;
}

int JackMoveDriver::Process()
{
    // capture is a source, playback is a sink
    ablspiSendAndReceiveAudioAndMidi(&fSPIDev, moveProcessAudio, moveProcessMidi, moveProvideMidi);

    JackDriver::CycleTakeBeginTime();
    auto r = JackAudioDriver::Process();
    JackDriver::CycleTakeEndTime();
    return r;
}

int JackMoveDriver::Read()
{
    std::array<std::pair<jack_port_id_t, JackMidiAsyncQueue*>, 2> items = {
        std::make_pair(fMIDICaptureId, &fMIDIWriteQueue),
        std::make_pair(fExternalMIDICaptureId, &fExternalMIDIWriteQueue)
    };
    for (auto idqueue: items)
    {
        JackMidiBuffer * captureBuffer = reinterpret_cast<JackMidiBuffer *>(fGraphManager->GetBuffer(idqueue.first, fEngineControl->fBufferSize));
        JackMidiBufferWriteQueue writeQueue;
        writeQueue.ResetMidiBuffer(captureBuffer, fEngineControl->fBufferSize);

        while (auto e = idqueue.second->DequeueEvent()) {
            writeQueue.EnqueueEvent(e);
        }
    }

    {
        JackMidiBuffer * displayBuffer = reinterpret_cast<JackMidiBuffer *>(fGraphManager->GetBuffer(fDisplayId, fEngineControl->fBufferSize));
        JackMidiBufferReadQueue readQueue;
        readQueue.ResetMidiBuffer(displayBuffer);
        while (auto e = readQueue.DequeueEvent()) {
            if (e->size == DISPLAY_DATA_LEN) {
                //TODO check header
                ablspiUpdateDisplay(&fSPIDev, e->buffer + DISPLAY_HEADER_LEN);
            }
        }
    }

    //taken from JackPortAudioDriver
    assert(fCaptureChannels == 2);
    for (int i = 0; i < fCaptureChannels; i++) {
        memcpy(GetInputBuffer(i), fInputBuffer[i], sizeof(jack_default_audio_sample_t) * fEngineControl->fBufferSize);
    }
    return 0;
}

int JackMoveDriver::Write()
{
    std::array<std::pair<jack_port_id_t, JackMidiAsyncQueue*>, 2> items = {
        std::make_pair(fMIDIPlaybackId, &fMIDIReadQueue),
        std::make_pair(fExternalMIDIPlaybackId, &fExternalMIDIReadQueue)
    };
    for (auto idqueue : items)
    {
        JackMidiBuffer * playbackBuffer = reinterpret_cast<JackMidiBuffer *>(fGraphManager->GetBuffer(idqueue.first, fEngineControl->fBufferSize));
        JackMidiBufferReadQueue readQueue;
        readQueue.ResetMidiBuffer(playbackBuffer);

        while (auto e = readQueue.DequeueEvent()) {
            idqueue.second->EnqueueEvent(e);
        }
    }

    //taken from JackPortAudioDriver
    assert(fPlaybackChannels == 2);
    for (int i = 0; i < fPlaybackChannels; i++) {
        memcpy(fOutputBuffer[i], GetOutputBuffer(i), sizeof(jack_default_audio_sample_t) * fEngineControl->fBufferSize);
    }
    return 0;
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

    desc = jack_driver_descriptor_construct("move", JackDriverMaster, "Linux AOS Move API based audio backend", &filler);

    return desc;
}

static Jack::JackMoveDriver* g_move_driver = nullptr;

SERVER_EXPORT Jack::JackDriverClientInterface* driver_initialize(Jack::JackLockedEngine* engine, Jack::JackSynchro* table, const JSList* params)
{
    jack_nframes_t nframes = ABLSPI_AUDIO_BUFFER_SIZE;
    jack_nframes_t samplerate = ABLSPI_AUDIO_SAMPLE_RATE_HZ;
    bool capturing = true;
    bool playing = true;
    int inchannels = 2;
    int outchannels = 2;
    bool monitor = false;
    const char* capture_driver_name = "move";
    const char* playback_driver_name = "move";

    // TODO
    jack_nframes_t capture_latency = 0;
    jack_nframes_t playback_latency = 0;

    g_move_driver = new Jack::JackMoveDriver("system", "move", engine, table);
    Jack::JackDriverClientInterface* threaded_driver = new Jack::JackThreadedDriver(g_move_driver);
    // Special open for Move driver...
    if (g_move_driver->Open(nframes, samplerate, capturing, playing, inchannels, outchannels, monitor, capture_driver_name, playback_driver_name, capture_latency, playback_latency) == 0) {
        return threaded_driver;
    } else {
        delete threaded_driver; // Delete the decorated driver
        return NULL;
    }

}

#ifdef __cplusplus
}
#endif


