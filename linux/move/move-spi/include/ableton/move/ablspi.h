// SPDX-FileCopyrightText: 2026 Ableton AG
// SPDX-License-Identifier: GPL-2.0-or-later

// Disclaimer: This library is experimental. The API and ABI are subject to change, and
// the implementation may be incomplete.

#pragma once

#include <stddef.h>
#include <stdint.h>

#define ABLSPI_AUDIO_BUFFER_SIZE 128
#define ABLSPI_AUDIO_SAMPLE_RATE_HZ 44100
#define ABLSPI_MAX_MIDI_IN_PER_TRANSFER 31
#define ABLSPI_MAX_MIDI_OUT_PER_TRANSFER 20

#define ABLSPI_DISPLAY_BYTES 1024

typedef struct
{
  int deviceFd;
  char* deviceBuffer;
  void* userContext;
  uint8_t displayBufferIndex;
  uint8_t displayBuffer[2][ABLSPI_DISPLAY_BYTES];
  uint8_t displayIndexLast;
  uint8_t displayDirty;
} AblSpiDevice;

typedef struct
{
  uint8_t channel : 4;
  uint8_t type : 4;

  uint8_t data1;

  // MSB is always 0. Can be different things for different inputs:
  // Buttons: 127 (press), 0 (release)
  // Pads: # (velocity), 0 (release)
  // Encoder: # ticks, signed 7 bit integer.
  //   Positive = Clockwise
  //   Negative = Counterclockwise
  uint8_t data2;
} AblSpiMidiMessage;

typedef struct
{
  uint8_t cin : 4;
  uint8_t cable : 4;
  AblSpiMidiMessage midi;
} AblSpiUsbMidiMessage;

typedef struct __attribute__((packed))
{
  AblSpiUsbMidiMessage message;
  uint32_t timestamp;
} AblSpiMidiEvent;

#ifdef __cplusplus
extern "C"
{
#endif

  typedef void (*AudioCallback)(void* pUserContext,
                                const int16_t* pInputLeft,
                                const int16_t* pInputRight,
                                int16_t* pOutputLeft,
                                int16_t* pOutputRight,
                                int numSamples);
  typedef void (*MidiInputCallback)(void* pUserContext, AblSpiMidiEvent midiEvent);
  typedef AblSpiUsbMidiMessage (*MidiOutputCallback)(void* pUserContext);

  // Initialize the device.
  // @pContext is passed into the callbacks.
  int ablspiInit(AblSpiDevice* pDev, void* pContext);

  // Free resources allocated for the device.
  int ablspiDestroy(const AblSpiDevice* pDev);

  // Call callbacks to process current transfer frame input data, and prepare output data
  // for next transfer. Then block until next transfer time, and perform one duplex
  // transaction with the hardware.
  // To be called from the real-time thread.
  int ablspiSendAndReceiveAudioAndMidi(AblSpiDevice* pDev,
                                       AudioCallback audioCallback,
                                       MidiInputCallback midiInputCallback,
                                       MidiOutputCallback midiOutputCallback);

  // Submit new display data for next display update. Data buffer must be
  // ABLSPI_DISPLAY_BYTES long.
  // To be called from the main thread.
  void ablspiUpdateDisplay(AblSpiDevice* pDev, const uint8_t* data);

  // Encode a MIDI message as a USB MIDI message with the given cable number.
  AblSpiUsbMidiMessage encodeAsUsbMidi(AblSpiMidiMessage message, uint8_t cable);
#ifdef __cplusplus
}
#endif
