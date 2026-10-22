#!/bin/sh
# Build CorePatch-Zygisk into a flashable Magisk module zip.
#
# Local requirements:
#   - JDK 17
#   - Android SDK with: platform 34, build-tools 34.0.0, NDK 26.1.10909125, CMake 3.22.1
#
# Set ANDROID_HOME (and optionally ANDROID_NDK_HOME), or provide local.properties.
set -e
cd "$(dirname "$0")"

if [ ! -f local.properties ]; then
  : "${ANDROID_HOME:?set ANDROID_HOME or create local.properties (sdk.dir=...)}"
  {
    echo "sdk.dir=$ANDROID_HOME"
    if [ -n "${ANDROID_NDK_HOME:-}" ]; then
      echo "ndk.dir=$ANDROID_NDK_HOME"
    fi
  } > local.properties
fi

./gradlew :runtime:packageModule "$@"

echo
echo "Flashable module: $(pwd)/runtime/build/module_out/CorePatch-Zygisk.zip"
