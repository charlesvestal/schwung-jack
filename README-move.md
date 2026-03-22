# building with a rnbo cloud compiler docker image, from jack2 dir

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

