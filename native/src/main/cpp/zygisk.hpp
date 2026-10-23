/*
 * zygisk.hpp - self-contained copy of the classic Magisk Zygisk API (v1)
 * that Zygisk-compatible runtimes (Magisk 24+, ZygiskNext, ...) expose to
 * native modules through the `zygisk_api_entry` entry point.
 *
 * Reference behaviour (the parts this module actually uses):
 *   - Api::setModuleDirByMyself()   -> fills g_module_dir with our module dir
 *   - ZygiskModule::onLoad(JavaVM*) -> called once; return our module base
 *   - ZygiskModuleBase::postServerSpecialize() -> called in system_server
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <jni.h>
#include <cstddef>

namespace zygisk {

struct Api {
    void* impl;

    bool (*hookJniNativeMethods)(void* impl, JNIEnv* env, const char* clazz,
                                 const JNINativeMethod* methods, int numMethods);
    void* (*getSym)(void* impl, const char* name);
    void (*setOption)(void* impl, int option);
    void (*connectCompanion)(void* impl, void* companion);
    void (*setModuleDir)(void* impl, const char* dir);
    void (*setModuleDirByMyself)(void* impl);
    void (*getModuleDir)(void* impl, char* buf);
};

struct AppSpecializeArgs {
    jint& uid;
    jint& gid;
    jintArray& gids;
    jint& runtime_flags;
    jint& mount_external;
    jstring& se_info;
    jstring& nice_name;
    jstring& class_name;
    jstring& data_dir;
    jstring& instruction_set;
    jstring& app_data_dir;
    jobjectArray& deny_private_data_dir;
};

struct ServerSpecializeArgs {
    jint& uid;
    jint& gid;
    jintArray& gids;
    jint& runtime_flags;
    jlong& permitted_capabilities;
    jlong& effective_capabilities;
};

class ZygiskModuleBase {
public:
    virtual void onLoad(Api* api, JNIEnv* env) {}
    virtual void preAppSpecialize(AppSpecializeArgs* args) {}
    virtual void postAppSpecialize(const AppSpecializeArgs* args) {}
    virtual void preServerSpecialize(ServerSpecializeArgs* args) {}
    virtual void postServerSpecialize(const ServerSpecializeArgs* args) {}
    virtual ~ZygiskModuleBase() = default;
};

class ZygiskModule {
public:
    virtual ZygiskModuleBase* onLoad(JavaVM* vm) = 0;
    virtual ~ZygiskModule() = default;
};

}  // namespace zygisk

/* Every Zygisk native module must export this symbol. */
extern "C" int zygisk_api_entry(void* api, void* module);
