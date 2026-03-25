# JACK2 Shadow Driver for Ableton Move

Fork of JACK2 with a shadow driver that shares audio, MIDI, and display with Move's firmware via [Schwung](https://github.com/charlesvestal/schwung)'s shared memory bridge.

## Shadow Driver

The `shadow` driver (`linux/shadow/JackShadowDriver.cpp`) connects to Schwung's SPI shim via `/dev/shm/schwung_jack`. Any JACK client gets:

| Port | Direction | Description |
|------|-----------|-------------|
| `system:capture_1/2` | in | Move audio input (stereo) |
| `system:playback_1/2` | out | Move audio output (mixed with Move's own audio) |
| `system:midi_capture` | in | Hardware MIDI — pads, knobs, buttons (raw, no scale mapping) |
| `system:midi_playback` | out | MIDI to hardware — LEDs, sysex |
| `system:ext_midi_capture` | in | External MIDI input |
| `system:ext_midi_playback` | out | External MIDI output |
| `system:display` | out | Display override (sysex display chunks) |

### MIDI notes

- Channel messages on `midi_playback` must use **channel 16** (rewritten to ch0 for hardware)
- Sysex passes through directly (Ableton format: `F0 00 21 1D 01 01 ...`)
- LED colors use Move's palette (0–127 velocity on note-on)

### Prerequisites

- [Schwung](https://github.com/charlesvestal/schwung) installed on Move (provides the SPI shim)
- JACK binaries (included with RNBO at `/data/UserData/rnbo/bin/`, or build from this repo)

### Quick start

```bash
./scripts/build.sh     # cross-compiles jack_shadow.so for ARM64 via Docker
./scripts/install.sh   # deploys to Move over SSH

# Start JACK with the shadow driver
ssh ableton@move.local
export LD_LIBRARY_PATH=/data/UserData/rnbo/lib:$LD_LIBRARY_PATH
jackd -d shadow
```

