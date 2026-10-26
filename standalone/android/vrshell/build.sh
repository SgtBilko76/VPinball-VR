#!/bin/bash
# Builds libvrshell.so (the VR menu shell for Quest) and puts it next to the other native
# libraries in build/android-arm64-v8a, where the Gradle copyLibs task picks it up.
set -e
cd "$(dirname "$0")"
ROOT="$(cd ../../.. && pwd)"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-/home/bilko/workspace/android-ndk-r30}}"
CXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang++"
OUT="$ROOT/build/android-arm64-v8a"
mkdir -p "$OUT"
"$CXX" -std=c++17 -O2 -fPIC -shared -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter \
   -I"$ROOT/third-party/include" \
   vrshell.cpp \
   -L"$ROOT/third-party/runtime-libs/android-arm64-v8a" -lopenxr_loader \
   -lEGL -lGLESv3 -llog -landroid \
   -static-libstdc++ -Wl,--no-undefined -Wl,-z,max-page-size=16384 \
   -o "$OUT/libvrshell.so"
echo "Built $OUT/libvrshell.so"
