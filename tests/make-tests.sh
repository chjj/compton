#!/bin/bash

# Test script for GNU make build

BASE_DIR=$(dirname "$0")/..
. "${BASE_DIR}/functions.sh"

OPTIONS=( NO_XINERAMA NO_LIBCONFIG NO_REGEX_PCRE NO_REGEX_PCRE_JIT
  NO_VSYNC_DRM NO_DBUS NO_XSYNC NO_C2 )

for o in "${OPTIONS[@]}"; do
  einfo Building with $o
  make "${o}=1" -B "${@}" || die
  einfo Build completed.
done
