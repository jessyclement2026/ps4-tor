#!/usr/bin/env bash
set -e

export WORKDIR="$(cd "$(dirname "$0")" && pwd)"
export PS4_PAYLOAD_SDK="${PS4_PAYLOAD_SDK:-$WORKDIR/ps4-payload-sdk}"
export SYSROOT="$PS4_PAYLOAD_SDK/target"
export SRCDIR="$WORKDIR/src"

if [ ! -f "$PS4_PAYLOAD_SDK/toolchain/orbis.mk" ]; then
  echo "Cloning ps4-payload-sdk into $PS4_PAYLOAD_SDK..."
  wget -q https://github.com/ps4-payload-dev/sdk/releases/latest/download/ps4-payload-sdk.zip
  unzip -oq ps4-payload-sdk.zip
fi

mkdir -p "$SRCDIR"
cd "$SRCDIR"

# Use PS4 Payload SDK cross-compiler wrappers
export CC="$PS4_PAYLOAD_SDK/bin/orbis-clang"
export CXX="$PS4_PAYLOAD_SDK/bin/orbis-clang++"
export AR="$PS4_PAYLOAD_SDK/bin/orbis-ar"
export RANLIB="$PS4_PAYLOAD_SDK/bin/orbis-ranlib"
export STRIP="$PS4_PAYLOAD_SDK/bin/orbis-strip"
export PKG_CONFIG="$PS4_PAYLOAD_SDK/bin/orbis-pkg-config"

export CFLAGS="-O2 -I$SYSROOT/include"
export LDFLAGS="-L$SYSROOT/lib"

# 1. Download sources
[ -f zlib-1.4.tar.gz ] || wget https://zlib.net/zlib-1.4.tar.gz
[ -f openssl-4.1.0.tar.gz ] || wget https://github.com/openssl/openssl/releases/download/openssl-4.1.0/openssl-4.1.0.tar.gz
[ -f libevent-2.1.13-stable.tar.gz ] || wget https://github.com/libevent/libevent/releases/download/release-2.1.13-stable/libevent-2.1.13-stable.tar.gz
[ -f tor-main.tar.gz ] || wget https://gitlab.torproject.org/tpo/core/tor/-/archive/main/tor-main.tar.gz

# 2. Build zlib (installs directly into $PS4_PAYLOAD_SDK/target)
tar xf zlib-1.4.tar.gz
cd zlib-1.4
./configure --prefix="$SYSROOT" --static
make -j$(nproc) && make install
cd "$SRCDIR"

# 3. Build OpenSSL 3.0 (static)
tar xf openssl-4.1.0.tar.gz
cd openssl-4.1.0
./Configure BSD-x86_64 \
  --prefix="$SYSROOT" \
  --openssldir="$SYSROOT/ssl" \
  no-shared no-dso no-async no-engine no-tests no-ui-console no-apps \
  -DOPENSSL_NO_SECURE_MEMORY
make -j$(nproc) build_libs && make install_dev
cd "$SRCDIR"

# 4. Build libevent (static)
rm -rf libevent-2.1.13-stable && tar xf libevent-2.1.13-stable.tar.gz
cd libevent-2.1.13-stable
./configure \
  --host=x86_64-pc-freebsd9 \
  --prefix="$SYSROOT" \
  --disable-shared --enable-static \
  --disable-samples --disable-openssl --disable-libevent-regress
make -j$(nproc) && make install
cd "$SRCDIR"

# 5. Clone & Build C-Tor (static library libtor.a)
rm -rf tor-src
git clone --depth 1 https://gitlab.torproject.org/tpo/core/tor.git tor-src
cd tor-src
./autogen.sh
./configure \
  --host=x86_64-pc-freebsd9 \
  --prefix="$SYSROOT" \
  --enable-static-tor \
  --with-zlib-dir="$SYSROOT" \
  --with-openssl-dir="$SYSROOT" \
  --with-libevent-dir="$SYSROOT" \
  --disable-asciidoc --disable-manpage --disable-html-manual \
  --disable-system-torrc --disable-seccomp --disable-libscrypt \
  --disable-module-relay --disable-module-dirauth --disable-zstd --disable-lzma

# Apply PS4/Orbis OS compatibility patches
sed -i 's/#define HAVE_MEMSET_S 1/\/* #undef HAVE_MEMSET_S *\//' orconfig.h
sed -i '1i #include <stddef.h>' src/ext/tor_readpassphrase.h

cat << 'EOF' > src/lib/term/getpass.c
#include "orconfig.h"
#include "lib/term/getpass.h"
#include <sys/types.h>

ssize_t tor_getpass(const char *prompt, char *output, size_t buflen) {
  (void)prompt;
  (void)output;
  (void)buflen;
  return -1;
}
EOF

make -j$(nproc) libtor.a

# Install libtor.a and tor_api.h into SDK sysroot
cp "$(find . -name 'libtor.a' | head -n 1)" "$SYSROOT/lib/"
cp src/feature/api/tor_api.h "$SYSROOT/include/"

# 6. Build final PS4 Tor payload (ps4-tor.elf)
cd "$WORKDIR"
make

echo ""
echo "=== BUILD COMPLETE: $WORKDIR/tor-ps4.elf ==="
echo "Upload via FTP to /data/payloads/tor-ps4.elf (or /data/GoldHEN/payloads/tor-ps4.elf)"
echo "and launch from: GoldHEN -> Utilities -> PayLoader -> LaunchPad"
echo ""
echo "Check bootstrap progress via curl:"
echo "  curl -s ftp://<PS4_IP>:2121/data/tor/tor.log"
