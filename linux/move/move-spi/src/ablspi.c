// SPDX-FileCopyrightText: 2026 Ableton AG
// SPDX-License-Identifier: GPL-2.0-or-later

// Disclaimer: This library is experimental. The API and ABI are subject to change, and
// the implementation may be incomplete.

#include "ableton/move/ablspi.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static const char* kSpiDevice = "/dev/ablspi0.0";

static const unsigned int kPageSize = 4096;
static const unsigned int kSpiFrequency = 20000000;

static const int kNumDisplayChunks = 6;

static const int kOutputMidiOffset = 0;
static const int kOutputDisplayStatusOffset = 80;
static const int kOutputDisplayDataOffset = 84;
static const int kOutputAudioDataOffset = 256;
static const int kOutputDisplayDataLen = 172;

static const int kInputDataOffset = 2048;
static const int kInputMidiOffset = kInputDataOffset;
static const int kInputDisplayStatusOffset = kInputDataOffset + 248;
static const int kInputAudioDataOffset = kInputDataOffset + 256;

static const int kFrameSize = 768;

enum IoctlCommands
{
  ABLSPI_FILL_TX_BUFFER,
  ABLSPI_FILL_RX_BUFFER,
  ABLSPI_READ_BUFFER,
  ABLSPI_SEND_MESSAGE,
  ABLSPI_SEND_MESSAGE_AND_WAIT,
  ABLSPI_WAIT_AND_SEND_MESSAGE,
  ABLSPI_GET_STATE,
  ABLSPI_CAN_SEND,
  ABLSPI_SET_MESSAGE_SIZE,
  ABLSPI_GET_MESSAGE_SIZE,
  ABLSPI_WAIT_AND_SEND_MESSAGE_WITH_SIZE,
  ABLSPI_SET_SPEED,
  ABLSPI_GET_SPEED
};

static bool midiMessageIsEmpty(const AblSpiMidiMessage message)
{
  return message.type == 0 && message.channel == 0 && message.data1 == 0
         && message.data2 == 0;
}

static bool usbMidiMessageIsEmpty(const AblSpiUsbMidiMessage usbMidiMessage)
{
  return usbMidiMessage.cin == 0 && usbMidiMessage.cable == 0
         && midiMessageIsEmpty(usbMidiMessage.midi);
}

AblSpiUsbMidiMessage encodeAsUsbMidi(const AblSpiMidiMessage message, uint8_t cable)
{
  AblSpiUsbMidiMessage ret;

  // Cable 0:: exchanges controls-related MIDI between the Move engine/application and the
  // XMOS processor.
  // Cable 14:: is used for system-level events.
  // Cable 15:: is used for SPI-protocol bound events.
  ret.cable = cable;

  // TODO: For basic Channel Voice messages (Note Off-Pitch Bend), it's enough to copy the
  // midi type into the CIN field. For other messages (System), we need to implement the
  // Move specific CIN mapping.
  ret.cin = message.type;

  ret.midi = message;
  return ret;
}

static int waitAndTransfer(const AblSpiDevice* pDev)
{
  while (ioctl(pDev->deviceFd, ABLSPI_WAIT_AND_SEND_MESSAGE_WITH_SIZE, kFrameSize) != 0)
  {
    // EINTR happens when a signal interrupts the call. This can happen by debugging
    // Move so we try again.
    if (errno != EINTR)
    {
      return -1;
    }
  }

  return 0;
}

static void separateChannels(const int16_t* pInterleavedBuffer,
                             int16_t* pLeftBuffer,
                             int16_t* pRightBuffer,
                             const int numSamples)
{
  for (int i = 0; i < numSamples; i++)
  {
    pLeftBuffer[i] = pInterleavedBuffer[i * 2];
    pRightBuffer[i] = pInterleavedBuffer[i * 2 + 1];
  }
}

static void interleaveChannels(const int16_t* pLeftBuffer,
                               const int16_t* pRightBuffer,
                               int16_t* pInterleavedBuffer,
                               const int numSamples)
{
  for (int i = 0; i < numSamples; i++)
  {
    pInterleavedBuffer[i * 2] = pLeftBuffer[i];
    pInterleavedBuffer[i * 2 + 1] = pRightBuffer[i];
  }
}

static void handleAudio(const AblSpiDevice* pDev, AudioCallback audioCallback)
{
  int16_t inputLeft[ABLSPI_AUDIO_BUFFER_SIZE];
  int16_t inputRight[ABLSPI_AUDIO_BUFFER_SIZE];
  int16_t outputLeft[ABLSPI_AUDIO_BUFFER_SIZE];
  int16_t outputRight[ABLSPI_AUDIO_BUFFER_SIZE];

  int16_t* pDeviceAudioInputBuffer =
    (int16_t*)((uintptr_t)pDev->deviceBuffer + kInputAudioDataOffset);
  separateChannels(
    pDeviceAudioInputBuffer, inputLeft, inputRight, ABLSPI_AUDIO_BUFFER_SIZE);

  audioCallback(pDev->userContext,
                inputLeft,
                inputRight,
                outputLeft,
                outputRight,
                ABLSPI_AUDIO_BUFFER_SIZE);

  int16_t* pDeviceAudioOutputBuffer =
    (int16_t*)((uintptr_t)pDev->deviceBuffer + kOutputAudioDataOffset);
  interleaveChannels(
    outputLeft, outputRight, pDeviceAudioOutputBuffer, ABLSPI_AUDIO_BUFFER_SIZE);
}

static void handleMidiOutput(const AblSpiDevice* pDev,
                             MidiOutputCallback midiOutputCallback)
{
  AblSpiUsbMidiMessage* pDeviceMidiOutputBuffer =
    (AblSpiUsbMidiMessage*)((uintptr_t)pDev->deviceBuffer + kOutputMidiOffset);

  int i = 0;
  while (i < ABLSPI_MAX_MIDI_OUT_PER_TRANSFER)
  {
    const AblSpiUsbMidiMessage outputMessage = midiOutputCallback(pDev->userContext);
    if (midiMessageIsEmpty(outputMessage.midi))
    {
      break;
    }

    pDeviceMidiOutputBuffer[i] = outputMessage;
    i++;
  }

  static const AblSpiUsbMidiMessage kEmptyUsbMidiMessage = {
    .midi = {0, 0, 0, 0}, .cable = 0, .cin = 0};
  while (i < ABLSPI_MAX_MIDI_OUT_PER_TRANSFER)
  {
    pDeviceMidiOutputBuffer[i] = kEmptyUsbMidiMessage;
    i++;
  }
}

static void handleDisplayOutput(AblSpiDevice* pDev)
{
  const uint32_t* pDeviceInputDisplayStatusWord =
    (uint32_t*)((uintptr_t)pDev->deviceBuffer + kInputDisplayStatusOffset);

  uint32_t* pDeviceOutputDisplayStatusWord =
    (uint32_t*)((uintptr_t)pDev->deviceBuffer + kOutputDisplayStatusOffset);

  const uint32_t index = *pDeviceInputDisplayStatusWord;
  *pDeviceOutputDisplayStatusWord = index;
  if (index != pDev->displayIndexLast)
  {
    const bool shouldSwapBuffers = index == 1 && pDev->displayDirty;
    if (shouldSwapBuffers)
    {
      pDev->displayBufferIndex ^= 1;
      pDev->displayDirty = 0;
    }

    pDev->displayIndexLast = (uint8_t)index;
    uint8_t* pDestChunk = (uint8_t*)pDev->deviceBuffer + kOutputDisplayDataOffset;
    if (index == kNumDisplayChunks)
    {
      memcpy(pDestChunk,
             pDev->displayBuffer[pDev->displayBufferIndex]
               + (kNumDisplayChunks - 1) * kOutputDisplayDataLen,
             ABLSPI_DISPLAY_BYTES - (kNumDisplayChunks - 1) * kOutputDisplayDataLen);
    }
    else if (index > 0 && index < kNumDisplayChunks)
    {
      memcpy(pDestChunk,
             pDev->displayBuffer[pDev->displayBufferIndex]
               + (index - 1) * kOutputDisplayDataLen,
             kOutputDisplayDataLen);
    }
  }
}

static void handleMidiInput(const AblSpiDevice* pDev, MidiInputCallback midiInputCallback)
{
  AblSpiMidiEvent* pDeviceMidiInputBuffer =
    (AblSpiMidiEvent*)((uintptr_t)pDev->deviceBuffer + kInputMidiOffset);

  for (size_t i = 0; i < ABLSPI_MAX_MIDI_IN_PER_TRANSFER
                     && !usbMidiMessageIsEmpty(pDeviceMidiInputBuffer[i].message);
       i++)
  {
    midiInputCallback(pDev->userContext, pDeviceMidiInputBuffer[i]);
  }
}

int ablspiInit(AblSpiDevice* pDev, void* pUserContext)
{
  int ret = 0;

  int spiDeviceFd = open(kSpiDevice, O_RDWR);
  if (spiDeviceFd < 0)
  {
    ret = -1;
    goto fail_open;
  }

  char* pBuffer =
    mmap(NULL, kPageSize, PROT_READ | PROT_WRITE, MAP_SHARED, spiDeviceFd, 0);
  if (pBuffer == MAP_FAILED)
  {
    ret = -2;
    goto fail_mmap;
  }
  memset(pBuffer, 0, kPageSize);

  if (ioctl(spiDeviceFd, ABLSPI_SET_SPEED, kSpiFrequency))
  {
    ret = -3;
    goto fail_ioctl;
  }

  pDev->deviceFd = spiDeviceFd;
  pDev->deviceBuffer = pBuffer;
  pDev->userContext = pUserContext;

  pDev->displayIndexLast = 0;
  pDev->displayBufferIndex = 0;
  pDev->displayDirty = 0;
  memset(pDev->displayBuffer[0], 0, ABLSPI_DISPLAY_BYTES);
  memset(pDev->displayBuffer[1], 0, ABLSPI_DISPLAY_BYTES);

  return ret;

fail_ioctl:
  munmap(pBuffer, kPageSize);

fail_mmap:
  close(spiDeviceFd);

fail_open:
  return ret;
}

int ablspiDestroy(const AblSpiDevice* pDev)
{
  close(pDev->deviceFd);
  munmap(pDev->deviceBuffer, kPageSize);
  return 0;
}

void ablspiUpdateDisplay(AblSpiDevice* pDev, const uint8_t* data)
{
  size_t inactiveBufferIndex = pDev->displayBufferIndex ^ 1;
  memcpy(pDev->displayBuffer[inactiveBufferIndex], data, ABLSPI_DISPLAY_BYTES);
  pDev->displayDirty = 1;
}

int ablspiSendAndReceiveAudioAndMidi(AblSpiDevice* pDev,
                                     AudioCallback audioCallback,
                                     MidiInputCallback midiInputCallback,
                                     MidiOutputCallback midiOutputCallback)
{
  handleAudio(pDev, audioCallback);
  handleMidiOutput(pDev, midiOutputCallback);
  handleDisplayOutput(pDev);

  handleMidiInput(pDev, midiInputCallback);

  return waitAndTransfer(pDev);
}
