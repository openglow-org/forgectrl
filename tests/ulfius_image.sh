#!/bin/sh
# ulfius_image.sh - the image's ulfius, for the host tests that need it
# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# SPDX-License-Identifier: MIT
#
# Builds libulfius at the revision the forgefirm layer's recipe pins,
# with the patches the recipe applies, into <out-dir>/lib. The image's
# patch makes a request's client_address the whole peer sockaddr: the
# distribution's library copies 16 bytes of the sockaddr_in6 its
# dual-stack listener reports, so on it forgectrl never sees a loopback
# peer. The build keeps the distribution's features (its ulfius-cfg.h),
# so the library stands in for the distribution's at run time
# (LD_LIBRARY_PATH). Needs git, cmake, and the libulfius-dev
# dependencies; build.yml runs it:
#
#   sh tests/ulfius_image.sh <recipe-dir> <out-dir>
set -e
R=$(cd "${1:?recipe dir}" && pwd)
OUT=${2:?out dir}
BB=$(ls "$R"/ulfius_*.bb)
rev=$(sed -n 's/^SRCREV = "\([0-9a-f]*\)"$/\1/p' "$BB")
[ -n "$rev" ] || { echo "no SRCREV in $BB"; exit 1; }
rm -rf "$OUT"
mkdir -p "$OUT/lib"
git init -q "$OUT/src"
git -C "$OUT/src" fetch -q --depth 1 https://github.com/babelouest/ulfius "$rev"
git -C "$OUT/src" checkout -q FETCH_HEAD
for p in $(sed -n 's|.*file://\([^ "]*\.patch\).*|\1|p' "$BB"); do
    patch -s -d "$OUT/src" -p1 < "$R/ulfius/$p"
    echo "applied $p"
done
cmake -S "$OUT/src" -B "$OUT/build" -DCMAKE_BUILD_TYPE=Release -DWITH_JOURNALD=OFF \
      -DBUILD_UWSC=OFF -DBUILD_STATIC=OFF > "$OUT/cmake.log" 2>&1 \
    || { tail -20 "$OUT/cmake.log"; exit 1; }
cmake --build "$OUT/build" -j"$(nproc)" > "$OUT/build.log" 2>&1 || { tail -20 "$OUT/build.log"; exit 1; }
# The features the library was built with must be the distribution's:
# they decide the layout of the structures forgectrl was compiled with.
grep -h '^/\* #undef U_\|^#define U_' "$OUT/build/ulfius-cfg.h" > "$OUT/cfg.built"
grep -h '^/\* #undef U_\|^#define U_' /usr/include/ulfius-cfg.h > "$OUT/cfg.installed"
cmp -s "$OUT/cfg.built" "$OUT/cfg.installed" || { echo "ulfius-cfg.h differs from the installed one"; diff "$OUT/cfg.built" "$OUT/cfg.installed"; exit 1; }
cp -P "$OUT"/build/libulfius.so* "$OUT/lib/"
echo "libulfius at $rev in $OUT/lib: $(ls "$OUT/lib")"
