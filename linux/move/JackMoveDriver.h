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

#ifndef __JackMoveDriver__
#define __JackMoveDriver__

#include "../linux/driver.h"
#include "JackAudioDriver.h"
#include "JackThreadedDriver.h"
#include "JackTime.h"
#include "JackMidiAsyncQueue.h"
#include "ableton/move/ablspi.h"
#include <array>

namespace Jack
{

/*!
\brief The Move driver.
*/

class JackMoveDriver : public JackAudioDriver
{

    private:

        jack_driver_t* fDriver;
        jack_native_thread_t fReservationLoopThread;

        AblSpiDevice fSPIDev;

        jack_default_audio_sample_t fInputBuffer[2][ABLSPI_AUDIO_BUFFER_SIZE];
        jack_default_audio_sample_t fOutputBuffer[2][ABLSPI_AUDIO_BUFFER_SIZE];

        jack_port_id_t fMIDICaptureId;
        jack_port_id_t fMIDIPlaybackId;

        jack_port_id_t fExternalMIDICaptureId;
        jack_port_id_t fExternalMIDIPlaybackId;

        jack_port_id_t fDisplayId;

        JackMidiAsyncQueue fMIDIReadQueue;
        JackMidiAsyncQueue fMIDIWriteQueue;

        JackMidiAsyncQueue fExternalMIDIReadQueue;
        JackMidiAsyncQueue fExternalMIDIWriteQueue;

        //for spi events
        std::array<AblSpiUsbMidiMessage, ABLSPI_MAX_MIDI_OUT_PER_TRANSFER> mPendingMIDIQueue;
        size_t mPendingMIDIEventsStart = 0;
        size_t mPendingMIDIEventsEnd = 0;
        bool mSysexActive = false;
    public:

        JackMoveDriver(const char* name, const char* alias, JackLockedEngine* engine, JackSynchro* table)
          : JackAudioDriver(name, alias, engine, table),fDriver(NULL)
        {}
        virtual ~JackMoveDriver()
        {}

        int Open(jack_nframes_t buffer_size,
                        jack_nframes_t samplerate,
                        bool capturing,
                        bool playing,
                        int inchannels,
                        int outchannels,
                        bool monitor,
                        const char* capture_driver_name,
                        const char* playback_driver_name,
                        jack_nframes_t capture_latency,
                        jack_nframes_t playback_latency);

        int Attach();
        int Close();

        int Process();
        int Read();
        int Write();

        // BufferSize can not be changed
        bool IsFixedBufferSize()
        {
            return true;
        }

        // JACK API emulation for the midi driver
        int is_realtime() const;

        //called by driver
        void processAudio(const int16_t* pInputLeft, const int16_t* pInputRight, int16_t* pOutputLeft, int16_t* pOutputRight, int numSamples);
        AblSpiUsbMidiMessage provideMidi();
        void processMidi(AblSpiMidiEvent);

};

} // end of namespace

#endif
