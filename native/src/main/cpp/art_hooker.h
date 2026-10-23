/*
 * art_hooker.h - public interface of the ART method hooking engine.
 *
 * This is the piece that replaces what LSPosed normally provides.  It hooks
 * Java methods (typically inside system_server) by swapping the target's ART
 * entry point for a native trampoline that marshals arguments and calls back
 * into the Java runtime (org.lsposed.corepatch.zygisk.JavaBridge).
 *
 * IMPORTANT (honest scope): ART internal layout differs across Android
 * versions/devices.  The entry-point offset is auto-calibrated at runtime
 * (with a per-version default fallback), but the trampoline ABI and offsets
 * MUST be validated and tuned on real hardware for each supported Android
 * version.  See README.md "Bring-up".
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <jni.h>
#include <cstdint>

namespace corepatch::art {

// Opaque handle returned by hook(); 0 means failure.
using HookHandle = uint64_t;

// Initialize the engine. `bridgeClass` is the Java class that receives
// intercepted calls. Returns false if the class/methods are missing.
bool init(JNIEnv* env, jclass bridgeClass);

// Hook `javaMember` (a java.lang.reflect.Method/Constructor). The intercepted
// calls are delivered to JavaBridge.onEnter/onExit. Returns a handle.
HookHandle hook(JNIEnv* env, jobject javaMember);

// Restore the original entry point of a previously hooked method.
bool unhook(JNIEnv* env, HookHandle handle);

// Ask ART to run `javaMember` in the interpreter (deoptimize).
bool deoptimize(JNIEnv* env, jobject javaMember);

// Invoke the ORIGINAL implementation of a hooked method, passing `thiz`
// (null for statics) and `args`. Used by the runtime for after-hooks that
// need to read the original result, and by the trampoline for proceed().
jobject invokeOrigin(JNIEnv* env, HookHandle handle, jobject thiz,
                     jobjectArray args);

// Whether a handle is currently live.
bool isHooked(HookHandle handle);

}  // namespace corepatch::art
