/*
 * jni_bridge.cpp - JNI natives exposed to the Java runtime
 * (org.lsposed.corepatch.zygisk.NativeRuntime).
 *
 * These are registered explicitly from module.cpp right after the runtime dex
 * is injected, because Zygisk dlopens our .so directly (no System.loadLibrary,
 * so JNI_OnLoad is not guaranteed to run).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jni.h>
#include <android/log.h>

#include "art_hooker.h"

#define LOG_TAG "CorePatchZygisk"

namespace {

// NativeRuntime.init() -> boolean
jboolean nInit(JNIEnv* env, jclass clazz, jclass bridgeClass) {
    return corepatch::art::init(env, bridgeClass) ? JNI_TRUE : JNI_FALSE;
}

// NativeRuntime.hook(Method) -> long
jlong nHook(JNIEnv* env, jclass, jobject method) {
    return static_cast<jlong>(corepatch::art::hook(env, method));
}

// NativeRuntime.unhook(long) -> boolean
jboolean nUnhook(JNIEnv* env, jclass, jlong handle) {
    return corepatch::art::unhook(env, handle) ? JNI_TRUE : JNI_FALSE;
}

// NativeRuntime.deoptimize(Method) -> boolean
jboolean nDeoptimize(JNIEnv* env, jclass, jobject method) {
    return corepatch::art::deoptimize(env, method) ? JNI_TRUE : JNI_FALSE;
}

// NativeRuntime.invokeOrigin(long, Object thiz, Object[] args) -> Object
jobject nInvokeOrigin(JNIEnv* env, jclass, jlong handle, jobject thiz,
                      jobjectArray args) {
    return corepatch::art::invokeOrigin(env, handle, thiz, args);
}

// NativeRuntime.log(int level, String tag, String msg, Throwable t)
void nLog(JNIEnv* env, jclass, jint level, jstring jtag, jstring jmsg,
          jthrowable jt) {
    int lvl = (level >= ANDROID_LOG_VERBOSE && level <= ANDROID_LOG_FATAL)
                  ? level
                  : ANDROID_LOG_INFO;
    const char* tag = jtag ? env->GetStringUTFChars(jtag, nullptr) : "CorePatch";
    const char* msg = jmsg ? env->GetStringUTFChars(jmsg, nullptr) : "";
    __android_log_print(lvl, tag, "%s", msg);
    if (jt != nullptr) {
        jclass tcls = env->GetObjectClass(jt);
        jmethodID toString = env->GetMethodID(tcls, "toString", "()Ljava/lang/String;");
        jstring jts = static_cast<jstring>(env->CallObjectMethod(jt, toString));
        const char* cts = jts ? env->GetStringUTFChars(jts, nullptr) : "";
        __android_log_print(ANDROID_LOG_ERROR, tag, "%s", cts);
        if (jts) env->ReleaseStringUTFChars(jts, cts);
        env->DeleteLocalRef(tcls);
    }
    if (jtag) env->ReleaseStringUTFChars(jtag, tag);
    if (jmsg) env->ReleaseStringUTFChars(jmsg, msg);
}

const JNINativeMethod kMethods[] = {
    {"init", "(Ljava/lang/Class;)Z", reinterpret_cast<void*>(nInit)},
    {"hook", "(Ljava/lang/Object;)J", reinterpret_cast<void*>(nHook)},
    {"unhook", "(J)Z", reinterpret_cast<void*>(nUnhook)},
    {"deoptimize", "(Ljava/lang/Object;)Z", reinterpret_cast<void*>(nDeoptimize)},
    {"invokeOrigin", "(JLjava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",
     reinterpret_cast<void*>(nInvokeOrigin)},
    {"log", "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)V",
     reinterpret_cast<void*>(nLog)},
};

}  // namespace

extern "C" int corepatch_register_natives(JNIEnv* env, jclass nativeRuntime) {
    return env->RegisterNatives(nativeRuntime, kMethods,
                                sizeof(kMethods) / sizeof(kMethods[0]));
}
