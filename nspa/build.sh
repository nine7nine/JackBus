#!/bin/sh
# Build JackBus with the same compiler flags and waf options Arch's makepkg
# uses for jack2, into ./build.
#
# Usage: nspa/build.sh [extra waf configure args]
#   NSPA_LTO=1    add -flto=auto (Arch builds jack2 with !lto, upstream #485)
#   NSPA_DEBUG=1  add -g
#   NSPA_SANITIZE=address,undefined | thread   sanitizer build (adds -g -O1)
set -eu
cd "$(dirname "$0")/.."

CFLAGS="-march=x86-64 -mtune=generic -O2 -pipe -fno-plt -fexceptions \
 -Wp,-D_FORTIFY_SOURCE=3 -Wformat -Werror=format-security \
 -fstack-clash-protection -fcf-protection \
 -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer"
CXXFLAGS="$CFLAGS -Wp,-D_GLIBCXX_ASSERTIONS"
LDFLAGS="-Wl,-O1 -Wl,--sort-common -Wl,--as-needed -Wl,-z,relro -Wl,-z,now -Wl,-z,pack-relative-relocs"
if [ "${NSPA_LTO:-0}" = 1 ]; then
    CFLAGS="$CFLAGS -flto=auto"; CXXFLAGS="$CXXFLAGS -flto=auto"; LDFLAGS="$LDFLAGS -flto=auto"
fi
if [ "${NSPA_DEBUG:-0}" = 1 ]; then
    CFLAGS="$CFLAGS -g"; CXXFLAGS="$CXXFLAGS -g"
fi
if [ -n "${NSPA_SANITIZE:-}" ]; then
    # _FORTIFY_SOURCE does not mix well with sanitizers; -O1 keeps reports readable
    SAN="-fsanitize=$NSPA_SANITIZE -fno-omit-frame-pointer -g -O1 -Wp,-U_FORTIFY_SOURCE"
    CFLAGS="$CFLAGS $SAN"; CXXFLAGS="$CXXFLAGS $SAN"; LDFLAGS="$LDFLAGS -fsanitize=$NSPA_SANITIZE"
fi
export CFLAGS CXXFLAGS LDFLAGS LINKFLAGS="$LDFLAGS"

python3 ./waf configure --prefix=/usr --htmldir=/usr/share/doc/jack2/html \
    --autostart=none --systemd-unit --classic --dbus "$@"
python3 ./waf build -j"$(nproc)"
