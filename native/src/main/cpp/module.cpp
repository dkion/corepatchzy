/*
 * module.cpp - CorePatch-Zygisk native Zygisk entry.
 *
 * Responsibilities:
 *   1. Export the `zygisk_api_entry` symbol so the Zygisk runtime loads us.
 *   2. Resolve our module directory (where classes.dex and the config live).
 *   3. When the *system_server* process is forked & specialized, inject the
 *      bundled runtime dex (org.lsposed.corepatch.zygisk.Bootstrap) into the
 *      system classloader and run it.  The runtime then installs the CorePatch
 *      ART hooks natively (see art_hooker.cpp / jni_bridge.cpp).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jni.h>
#include <cstring>
#include <string>
#include <android/log.h>

#include "zygisk.hpp"
#include "art_hooker.h"

// Register the Java natives (defined in jni_bridge.cpp).
extern "C" int corepatch_register_natives(JNIEnv* env, jclass nativeRuntime);

#define LOG_TAG "CorePatchZygisk"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// Filled once at zygisk_api_entry time.
zygisk::Api* g_api = nullptr;
JavaVM* g_jvm = nullptr;
char g_module_dir[256] = {0};

// The entry-point class injected into system_server.
constexpr const char* kBootstrapClass = "org.lsposed.corepatch.zygisk.Bootstrap";
constexpr const char* kRuntimeDex = "classes.dex";

/* Resolve the directory that holds our files (set by the Zygisk runtime). */
bool resolveModuleDir() {
    if (!g_api) return false;
    g_api->setModuleDirByMyself(g_api->impl);
    g_api->getModuleDir(g_api->impl, g_module_dir);
    return g_module_dir[0] != '\0';
}

/* Attach to the current thread's JNIEnv, attaching the thread if needed. */
JNIEnv* getJniEnv() {
    JNIEnv* env = nullptr;
    jint rc = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    } else if (rc != JNI_OK) {
        return nullptr;
    }
    return env;
}

/*
 * Inject the runtime dex into the system classloader and start it.
 *
 * The runtime is loaded with a PathClassLoader whose parent is the system
 * classloader, so `hostClassLoader` inside the module can resolve both
 * framework classes (android.content.pm.*) and system_server classes
 * (com.android.server.pm.PackageManagerService), which is exactly what
 * CorePatch's hooks need.
 */
bool injectRuntime(JNIEnv* env) {
    if (g_module_dir[0] == '\0') resolveModuleDir();

    std::string dexPath = std::string(g_module_dir) + "/" + kRuntimeDex;

    jclass pathClassLoaderCls = env->FindClass("dalvik/system/PathClassLoader");
    if (!pathClassLoaderCls) { env->ExceptionClear(); return false; }
    jclass clClass = env->FindClass("java/lang/ClassLoader");
    if (!clClass) { env->ExceptionClear(); return false; }

    // parent = ClassLoader.getSystemClassLoader()
    jmethodID getSystemCl =
        env->GetStaticMethodID(clClass, "getSystemClassLoader",
                               "()Ljava/lang/ClassLoader;");
    jobject parent = env->CallStaticObjectMethod(clClass, getSystemCl);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }

    // new PathClassLoader(dexPath, parent)
    jmethodID ctor = env->GetMethodID(
        pathClassLoaderCls, "<init>",
        "(Ljava/lang/String;Ljava/lang/ClassLoader;)V");
    jstring jDexPath = env->NewStringUTF(dexPath.c_str());
    jobject loader =
        env->NewObject(pathClassLoaderCls, ctor, jDexPath, parent);
    env->DeleteLocalRef(jDexPath);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }

    // Bootstrap.main(ClassLoader self, String moduleDir)
    jclass bootstrapCls = env->FindClass(kBootstrapClass);
    if (bootstrapCls == nullptr) {
        // The dex wasn't picked up by FindClass through this loader.
        env->ExceptionClear();
        jmethodID loadClass = env->GetMethodID(
            clClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        jobject jCls = env->CallObjectMethod(loader, loadClass,
                                             env->NewStringUTF(kBootstrapClass));
        if (jCls == nullptr) {
            env->ExceptionClear();
            LOGE("Bootstrap class not found via %s", dexPath.c_str());
            return false;
        }
        bootstrapCls = static_cast<jclass>(jCls);
    }

    // Register the JNI natives before Java touches them.
    jclass nativeRuntimeCls =
        env->FindClass("org/lsposed/corepatch/zygisk/NativeRuntime");
    if (nativeRuntimeCls != nullptr) {
        if (corepatch_register_natives(env, nativeRuntimeCls) != JNI_OK) {
            env->ExceptionClear();
            LOGE("RegisterNatives failed");
            return false;
        }
        env->DeleteLocalRef(nativeRuntimeCls);
    } else {
        env->ExceptionClear();
        LOGE("NativeRuntime class not found");
        return false;
    }

    jmethodID main = env->GetStaticMethodID(
        bootstrapCls, "main",
        "(Ljava/lang/ClassLoader;Ljava/lang/String;)V");
    if (main == nullptr) { env->ExceptionClear(); return false; }

    jstring jModuleDir = env->NewStringUTF(g_module_dir);
    env->CallStaticVoidMethod(bootstrapCls, main, loader, jModuleDir);
    env->DeleteLocalRef(jModuleDir);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("Bootstrap.main threw an exception");
        return false;
    }
    LOGI("Runtime bootstrap complete in system_server");
    return true;
}

/*
 * Module base: the object the Zygisk runtime keeps per-process. We only act
 * inside system_server.
 */
class ModuleBase : public zygisk::ZygiskModuleBase {
public:
    void onLoad(zygisk::Api* api, JNIEnv* /*env*/) override {
        resolveModuleDir();
        LOGI("onLoad moduleDir=%s", g_module_dir[0] ? g_module_dir : "(none)");
    }

    void postServerSpecialize(
        const zygisk::ServerSpecializeArgs* /*args*/) override {
        // Only system_server takes this path in the zygote.
        JNIEnv* env = getJniEnv();
        if (!env) { LOGE("no JNIEnv in postServerSpecialize"); return; }
        injectRuntime(env);
    }

    // Some runtimes route system_server through the app path; handle both.
    void postAppSpecialize(const zygisk::AppSpecializeArgs* args) override {
        if (args->uid != 1000) return;  // only system uid
        if (args->nice_name != nullptr) {
            JNIEnv* env = getJniEnv();
            if (env) {
                const char* n = env->GetStringUTFChars(args->nice_name, nullptr);
                bool isSystemServer = n && strcmp(n, "system_server") == 0;
                if (n) env->ReleaseStringUTFChars(args->nice_name, n);
                if (!isSystemServer) return;
            }
        }
        if (g_module_dir[0] == '\0') resolveModuleDir();
        JNIEnv* env = getJniEnv();
        if (env) injectRuntime(env);
    }
};

/*
 * Module entry: the object the Zygisk runtime calls onLoad on.
 */
class ModuleEntry : public zygisk::ZygiskModule {
public:
    zygisk::ZygiskModuleBase* onLoad(JavaVM* vm) override {
        g_jvm = vm;
        return new ModuleBase();
    }
};

ModuleEntry g_module_entry;

}  // namespace

/*
 * The single exported entry point of a Zygisk native module.
 *   api    - pointer to the zygisk::Api vtable
 *   module - out-slot where we must store our zygisk::ZygiskModule*
 */
extern "C" __attribute__((visibility("default"))) int
zygisk_api_entry(void* api, void* module) {
    g_api = static_cast<zygisk::Api*>(api);
    auto entry = static_cast<zygisk::ZygiskModule**>(module);
    *entry = &g_module_entry;
    return 0;
}
