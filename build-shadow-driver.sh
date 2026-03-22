#!/bin/bash
set -e
cd /build/
apt-get -y install python2 2>/dev/null || true
su -c '
    unset LD_LIBRARY_PATH
    source /usr/local/oecore-x86_64/environment-setup-cortexa72-oe-linux
    python2 ./waf configure --prefix=/data/UserData/jack2/
    python2 ./waf build
' node
mkdir -p destdir
cp build/jack_shadow.so destdir/
echo "Built: destdir/jack_shadow.so"
