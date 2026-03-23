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
# Deploy the shadow driver
scp build/jack_shadow.so ableton@move.local:/data/UserData/rnbo/lib/jack/

# Start JACK with the shadow driver
ssh ableton@move.local
export LD_LIBRARY_PATH=/data/UserData/rnbo/lib:$LD_LIBRARY_PATH
jackd -d shadow
```

### Building the shadow driver

```bash
# Cross-compiles for ARM64 via Docker (builds image automatically on first run)
./scripts/build.sh
# Output: build/jack_shadow.so
```

---

# Building the full JACK2 suite

## building with the rnbo cloud compiler docker image (for full JACK2, not just the shadow driver)

```
docker pull registry.digitalocean.com/c74-private/rnbo.cloud-compiler-worker:2.6.0
docker run --user root -v $(pwd):/build --platform linux/amd64 registry.digitalocean.com/c74-private/rnbo.cloud-compiler-worker:2.6.0 /build/build-aos.sh
```

check `destdir` for the build products

This is built with the intention of it going into `/data/UserData/jack2`
that should be reflected in the `destdir` contents

## more manually

```
docker run -it --user root -v $(pwd):/build --platform linux/amd64 rnbo.cloud-compiler-worker:2.6.0 /bin/bash

cd /build/
apt-get -y install python2
su node
unset LD_LIBRARY_PATH
source /usr/local/oecore-x86_64/environment-setup-cortexa72-oe-linux
python2 ./waf configure --prefix=/data/UserData/jack2/
python2 ./waf build
python2 ./waf install --destdir=$(pwd)/destdir
exit
exit
```


## building DB

https://github.com/berkeleydb/libdb/releases/download/v5.3.28/db-5.3.28.tar.gz

extract somewhere, in vm go to `build_unix`
/build/db/db-5.3.28/build_unix$
CFLAGS="$CFLAGS -fPIC" ./dist/configure --enable-static --disable-shared --disable-debug --disable-doc --disable-maintainer-mode --prefix=/data/UserData/db/ --host aarch64-linux-gnu ac_cv_host=aarch64-linux-gnu --build=$(uname -m)-linux-gnu ac_cv_build=$(uname -m)-linux-gnu --disable-java --disable-replication --disable-sql --disable-tcl
make -j8
DESTDIR=/tmp/foo make install

