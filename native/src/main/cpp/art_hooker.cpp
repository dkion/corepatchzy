/*
 * art_hooker.cpp - ART method hooking engine (reference implementation).
 *
 * Strategy
 * --------
 * A Java method's execution entry is `ArtMethod::entry_point_from_quick_compiled_`.
 * Hooking swaps that pointer for a native trampoline.  When the method runs,
 * ART branches to the trampoline, which saves the incoming registers, hands
 * the (this, args) to the Java runtime
 * (org.lsposed.corepatch.zygisk.JavaBridge), then writes the result back.
 *
 * Each hooked method gets a unique 4-instruction ARM64 thunk that embeds its
 * HookHandle in x9 and branches to a shared body, so the trampoline knows
 * which method was hit without touching ART thread internals.
 *
 * BRING-UP / HONEST SCOPE
 * -----------------------
 * - `entry_point_from_quick_compiled_` offset is auto-calibrated at runtime
 *   by scanning ArtMethod for a pointer into executable memory, with known
 *   per-version defaults as a fallback.
 * - The ARM64 trampoline and the marshal logic follow AAPCS64 for the JNI
 *   native calling convention.  Both MUST be validated and tuned on a real
 *   device for each supported Android version (9..15) — this file is the
 *   bring-up-critical core of the port.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "art_hooker.h"

#include <android/log.h>
#include <jni.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#define LOG_TAG "CorePatchZygisk"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace corepatch::art {

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

struct HookedMethod {
    HookHandle handle = 0;
    void* art_method = nullptr;
    void* original_entry = nullptr;  // saved entry_point_from_quick_compiled_
    void* hooked_entry = nullptr;    // the thunk we installed
    jobject java_method = nullptr;   // global ref: java.lang.reflect.Method
    bool is_static = false;
};

// ARM64 trampoline argument capture layout (72 bytes): x0..x7 then saved sp.
struct SavedArgs {
    void* x0;  // JNIEnv*
    void* x1;  // thiz
    void* x2;
    void* x3;
    void* x4;
    void* x5;
    void* x6;
    void* x7;
    void* sp;
};

// The C++ side of the trampoline. Exported (extern "C") so the naked ARM64
// body can call it via the GOT. Defined at the bottom of this file.
extern "C" jobject trampolineImpl(JNIEnv* env, void* thiz, HookHandle handle,
                                  const SavedArgs* saved, void* sp);

namespace {

std::mutex g_lock;
std::vector<HookedMethod> g_hooks;   // small; linear scan is fine
std::vector<void*> g_thunks;         // mmap'ed thunks for unhook/munmap

// Java bridge (set once by Bootstrap).
jclass g_bridge_class = nullptr;
jmethodID g_bridge_on_enter = nullptr;

uint32_t g_entry_point_offset = 0;
bool g_calibrated = false;

// ---------------------------------------------------------------------------
// ArtMethod entry-point offset discovery
// ---------------------------------------------------------------------------

// Defaults observed for the common ART layout (fallback hints only; the scan
// below wins when it succeeds).
constexpr uint32_t kDefaultOffsets[] = {0x40, 0x38, 0x30};

bool isExecutablePointer(void* p) {
    if (p == nullptr) return false;
    FILE* maps = fopen("/proc/self/maps", "re");
    if (!maps) return false;
    char line[512];
    bool found = false;
    const uintptr_t target = reinterpret_cast<uintptr_t>(p);
    while (fgets(line, sizeof(line), maps)) {
        uintptr_t start = 0, end = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) == 3) {
            if (target >= start && target < end) {
                found = (perms[0] == 'r' && perms[2] == 'x');
                break;
            }
        }
    }
    fclose(maps);
    return found;
}

uint32_t calibrateEntryPointOffset(void* art_method) {
    for (uint32_t off : kDefaultOffsets) {
        void* cand = nullptr;
        memcpy(&cand, static_cast<char*>(art_method) + off, sizeof(cand));
        if (isExecutablePointer(cand)) return off;
    }
    for (uint32_t off = 0x20; off < 0x80; off += 8) {
        void* cand = nullptr;
        memcpy(&cand, static_cast<char*>(art_method) + off, sizeof(cand));
        if (isExecutablePointer(cand)) return off;
    }
    return kDefaultOffsets[0];  // fallback (needs device calibration)
}

void* getEntryPoint(void* art_method) {
    void* p = nullptr;
    memcpy(&p, static_cast<char*>(art_method) + g_entry_point_offset, sizeof(p));
    return p;
}

void setEntryPoint(void* art_method, void* entry) {
    memcpy(static_cast<char*>(art_method) + g_entry_point_offset, &entry,
           sizeof(entry));
    // Flush the i-cache / DMB so ART observes the new branch target.
    __builtin___clear_cache(static_cast<char*>(art_method),
                            static_cast<char*>(art_method) + 0x40);
}

// ---------------------------------------------------------------------------
// ARM64 trampoline
// ---------------------------------------------------------------------------

// thunk: movz/movk x9,#handle ; b trampolineBody   (16 bytes, aligned)

// Shared ARM64 body. On entry x9 = handle (loaded by the thunk).
__attribute__((naked)) void trampolineBody() {
    __asm__ __volatile__(
        // frame: SavedArgs(72) + saved handle(8) = 80 -> round to 112 for
        // alignment on the 16-byte AAPCS64 stack.
        "sub sp, sp, #112\n"
        "stp x0, x1, [sp, #0]\n"
        "stp x2, x3, [sp, #16]\n"
        "stp x4, x5, [sp, #32]\n"
        "stp x6, x7, [sp, #48]\n"
        "add x10, sp, #112\n"
        "str x10, [sp, #64]\n"   // saved.sp = caller's sp
        "str x9, [sp, #72]\n"    // saved handle
        // trampolineImpl(env=x0, thiz=x1, handle=x2, &saved=x3, sp=x4)
        "ldr x2, [sp, #72]\n"
        "mov x3, sp\n"
        "add x4, sp, #112\n"
        "adrp x9, trampolineImpl@GOTPAGE\n"
        "ldr x9, [x9, trampolineImpl@GOTPAGEOFF]\n"
        "blr x9\n"
        "add sp, sp, #112\n"
        "ret\n");
}

}  // namespace

// ---- public engine API ----------------------------------------------------

bool init(JNIEnv* env, jclass bridgeClass) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_bridge_class == nullptr) {
        g_bridge_class = static_cast<jclass>(env->NewGlobalRef(bridgeClass));
        g_bridge_on_enter =
            env->GetStaticMethodID(g_bridge_class, "onEnter",
                                   "(JLjava/lang/Object;[Ljava/lang/Object;)"
                                   "[Ljava/lang/Object;");
        env->ExceptionClear();
        return g_bridge_on_enter != nullptr;
    }
    return true;
}

HookHandle hook(JNIEnv* env, jobject javaMember) {
    // javaMember -> java.lang.reflect.Method
    jmethodID mid = env->FromReflectedMethod(javaMember);
    void* art_method = reinterpret_cast<void*>(mid);
    if (art_method == nullptr) return 0;

    if (!g_calibrated) {
        g_entry_point_offset = calibrateEntryPointOffset(art_method);
        g_calibrated = true;
    }

    std::lock_guard<std::mutex> lk(g_lock);

    // idempotent: if already hooked, return existing handle
    for (auto& h : g_hooks) {
        if (h.art_method == art_method) return h.handle;
    }

    HookHandle handle = reinterpret_cast<HookHandle>(art_method);
    HookedMethod rec;
    rec.handle = handle;
    rec.art_method = art_method;
    rec.original_entry = getEntryPoint(art_method);
    rec.java_method = env->NewGlobalRef(javaMember);
    rec.is_static = false;

    void* thunk = nullptr;
    {
        // generate thunk (12 bytes enough for handle in x9) + branch
        static void* g_body = reinterpret_cast<void*>(&trampolineBody);
        size_t page = sysconf(_SC_PAGESIZE);
        static void* page_p = nullptr;
        static size_t cursor = 0;
        if (page_p == nullptr) {
            page_p = mmap(nullptr, page, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (page_p == MAP_FAILED) { env->DeleteGlobalRef(rec.java_method); return 0; }
        }
        if (cursor + 16 > page) { LOGW("thunk page full"); }
        auto* code = static_cast<uint8_t*>(page_p) + cursor;
        cursor += 16;

        auto enc = [&](uint32_t insn, uint64_t imm, int shift, bool movk) {
            uint32_t base = movk ? 0xf2800000u : 0xd2800000u;
            return base | ((imm & 0xffff) << 5) | (9u) | (static_cast<uint32_t>(shift) << 21);
        };
        uint32_t insns[4];
        insns[0] = enc(0, handle & 0xffff, 0, false);
        insns[1] = enc(0, (handle >> 16) & 0xffff, 16, true);
        insns[2] = enc(0, (handle >> 32) & 0xffff, 32, true);
        // unconditional branch to body
        int64_t off = reinterpret_cast<int64_t>(g_body) -
                      (reinterpret_cast<int64_t>(code) + 12);
        insns[3] = 0x14000000u | (static_cast<uint32_t>((off >> 2) & 0x03ffffff));
        memcpy(code, insns, sizeof(insns));
        mprotect(page_p, page, PROT_READ | PROT_EXEC);
        __builtin___clear_cache(reinterpret_cast<char*>(code),
                                reinterpret_cast<char*>(code) + 16);
        thunk = code;
    }

    setEntryPoint(art_method, thunk);
    rec.hooked_entry = thunk;

    g_hooks.push_back(rec);
    g_thunks.push_back(thunk);
    return handle;
}

bool unhook(JNIEnv* env, HookHandle handle) {
    std::lock_guard<std::mutex> lk(g_lock);
    for (auto it = g_hooks.begin(); it != g_hooks.end(); ++it) {
        if (it->handle == handle) {
            setEntryPoint(it->art_method, it->original_entry);
            env->DeleteGlobalRef(it->java_method);
            g_hooks.erase(it);
            return true;
        }
    }
    return false;
}

bool deoptimize(JNIEnv* env, jobject javaMember) {
    // Ask ART to run the method in the interpreter by pointing the quick entry
    // at the "quick to interpreter bridge". We approximate by re-pointing the
    // entry back to the ORIGINAL saved entry (which may already be the bridge
    // if the method was never compiled). A full implementation looks up
    // `art_quick_to_interpreter_bridge` from libart.  This is part of the
    // bring-up surface.
    jmethodID mid = env->FromReflectedMethod(javaMember);
    void* art_method = reinterpret_cast<void*>(mid);
    std::lock_guard<std::mutex> lk(g_lock);
    for (auto& h : g_hooks) {
        if (h.art_method == art_method) {
            setEntryPoint(art_method, h.original_entry);
            return true;
        }
    }
    return false;
}

bool isHooked(HookHandle handle) {
    std::lock_guard<std::mutex> lk(g_lock);
    for (auto& h : g_hooks) if (h.handle == handle) return true;
    return false;
}

// ---- JavaBridge dispatch (called from trampoline / runtime) ---------------

namespace {

// Convert the raw saved registers + stack into a jobjectArray of boxed args
// and invoke JavaBridge.onEnter. Returns the result array.
// (Reference marshalling for object/primitive args; see bring-up notes.)
jobjectArray dispatchToJava(JNIEnv* env, void* thiz, HookHandle handle,
                            const SavedArgs* saved, void* sp) {
    // Reference build: pass (handle, thiz, args) to the bridge.  The real
    // marshal maps saved->x2..x7 + sp into a jobjectArray according to the
    // stored java.lang.reflect.Method signature; that mapping lives behind
    // the BRING-UP marker below and is the on-device calibration target.
    (void)saved;
    (void)sp;

    jclass objCls = env->FindClass("java/lang/Object");
    if (!objCls) { env->ExceptionClear(); return nullptr; }
    jobjectArray args = env->NewObjectArray(0, objCls, nullptr);

    jobjectArray out = static_cast<jobjectArray>(
        env->CallStaticObjectMethod(g_bridge_class, g_bridge_on_enter,
                                    static_cast<jlong>(handle),
                                    reinterpret_cast<jobject>(thiz), args));
    env->DeleteLocalRef(args);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return nullptr;
    }
    return out;
}

}  // namespace

extern "C" jobject trampolineImpl(JNIEnv* env, void* thiz, HookHandle handle,
                                  const SavedArgs* saved, void* sp) {
    return dispatchToJava(env, thiz, handle, saved, sp);
}

jobject invokeOrigin(JNIEnv* env, HookHandle handle, jobject thiz,
                     jobjectArray args) {
    std::lock_guard<std::mutex> lk(g_lock);
    for (auto& h : g_hooks) {
        if (h.handle == handle) {
            // Call the TRUE original: temporarily restore the original entry
            // point, dispatch reflectively, then re-hook.  (During the origin
            // call a re-entrant hit on the SAME method runs the original body
            // instead of re-entering the hook, which is the correct
            // proceed()/getOriginInvoker semantics.)
            setEntryPoint(h.art_method, h.original_entry);
            jobject result = nullptr;
            {
                jclass methodCls = env->GetObjectClass(h.java_method);
                jmethodID invokeMid =
                    env->GetMethodID(methodCls, "invoke",
                                     "(Ljava/lang/Object;[Ljava/lang/Object;)"
                                     "Ljava/lang/Object;");
                result = env->CallObjectMethod(h.java_method, invokeMid, thiz,
                                               args);
                if (env->ExceptionCheck()) {
                    env->ExceptionDescribe();
                    env->ExceptionClear();
                }
            }
            setEntryPoint(h.art_method, h.hooked_entry);
            return result;
        }
    }
    return nullptr;
}

}  // namespace corepatch::art
