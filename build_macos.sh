#!/bin/sh
set -eu
cd "$(dirname "$0")"
bundle="build/Mono Filter Distortion.clap"
mkdir -p "$bundle/Contents/MacOS"
cp Info.plist "$bundle/Contents/Info.plist"
xcrun clang -std=c11 -O2 -Wall -Wextra -Wpedantic -fvisibility=hidden \
  -mmacosx-version-min=11.0 -arch arm64 -arch x86_64 -bundle \
  -I vendor/clap/include source/mono_filter.c -Wl,-exported_symbol,_clap_entry \
  -o "$bundle/Contents/MacOS/MonoFilterDistortion"
codesign --force --sign - --timestamp=none "$bundle"
printf 'Built: %s\n' "$bundle"
