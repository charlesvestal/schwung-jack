# Schwung JACK

JACK2 shadow driver for [Schwung](https://github.com/charlesvestal/schwung) on Ableton Move.

Fork of [JACK2](https://jackaudio.org/) with a single addition: the `shadow` driver (`linux/shadow/JackShadowDriver.cpp`), which connects JACK clients to Move's hardware (audio, MIDI, display) via Schwung's shared-memory bridge.

## Build and install

```bash
./scripts/build.sh     # cross-compiles jack_shadow.so for ARM64 via Docker
./scripts/install.sh   # deploys to Move over SSH
```

See [README-move.md](README-move.md) for full details: driver ports, MIDI notes, prerequisites, and building the complete JACK2 suite.
