package org.lsposed.corepatch.zygisk

import java.lang.reflect.Executable
import java.util.concurrent.ConcurrentHashMap

/**
 * The runtime that replaces what LSPosed provided. It owns the host class
 * loader, the hook registry, and the config store.
 *
 * The native trampoline (art_hooker.cpp) routes intercepted calls into
 * [JavaBridge.onEnter], which resolves the [HookChain] and runs the CorePatch
 * before/proceed/after callbacks.
 */
object Runtime {

    lateinit var hostClassLoader: ClassLoader
        private set
    lateinit var moduleDir: String
        private set

    private val hooks = ConcurrentHashMap<Long, HookEntry>()
    private val methodHandles = ConcurrentHashMap<java.lang.reflect.Method, Long>()

    private val initialized = java.util.concurrent.atomic.AtomicBoolean(false)

    /** Called by Bootstrap once the dex is loaded in system_server. */
    fun start(classLoader: ClassLoader, moduleDir: String): Boolean {
        if (!initialized.compareAndSet(false, true)) return true
        hostClassLoader = classLoader
        this.moduleDir = moduleDir
        return NativeRuntime.init(JavaBridge::class.java)
    }

    fun hook(member: Executable, before: ((HookChain) -> Unit)?, after: ((HookChain) -> Unit)?): HookHandle {
        val handle = NativeRuntime.hook(member)
        if (handle != 0L) {
            hooks[handle] = HookEntry(handle, member, before, after)
            if (member is java.lang.reflect.Method) methodHandles[member] = handle
        } else {
            RuntimeLog.e("hook failed: $member")
        }
        return HookHandle(handle)
    }

    fun unhook(handle: Long) {
        NativeRuntime.unhook(handle)
        val entry = hooks.remove(handle)
        if (entry?.member is java.lang.reflect.Method) {
            methodHandles.remove(entry.member as java.lang.reflect.Method)
        }
    }

    /** Handle for a method that is currently hooked, else 0. */
    fun handleOf(method: java.lang.reflect.Method): Long = methodHandles[method] ?: 0L

    fun deoptimize(member: java.lang.reflect.Method): Boolean = NativeRuntime.deoptimize(member)

    fun isHooked(handle: Long) = hooks.containsKey(handle)

    /** Entry point for the native trampoline. Returns arrayOf(result[, throwable]). */
    fun dispatch(handle: Long, thiz: Any?, args: Array<Any?>): Array<Any?> {
        val entry = hooks[handle] ?: return arrayOf(null)

        val chain = HookChain(handle, entry.member).apply {
            this.thisObject = thiz
            this.args = args
        }

        var result: Any? = null
        var throwable: Throwable? = null
        var skipped = false

        entry.before?.let { before ->
            before(chain)
            if (chain.isSkipped()) {
                result = chain.getSkipResult()
                skipped = true
            }
        }

        if (!skipped) {
            try {
                result = chain.proceed(chain.args)
            } catch (t: Throwable) {
                throwable = t
            }
        }

        entry.after?.let { after ->
            after(chain)
            result = chain.result
            throwable = chain.throwable
        }

        if (throwable != null) {
            return arrayOf(null, throwable)
        }
        return arrayOf(result)
    }
}

/** The class the native trampoline calls back into (see art_hooker.cpp). */
object JavaBridge {
    @JvmStatic
    fun onEnter(handle: Long, thiz: Any?, args: Array<Any?>): Array<Any?> {
        return Runtime.dispatch(handle, thiz, args)
    }
}

/** Small logger that routes through the native log bridge. */
object RuntimeLog {
    @JvmStatic
    fun e(msg: String) = NativeRuntime.log(android.util.Log.ERROR, "CorePatch", msg, null)

    @JvmStatic
    fun w(msg: String) = NativeRuntime.log(android.util.Log.WARN, "CorePatch", msg, null)

    @JvmStatic
    fun i(msg: String) = NativeRuntime.log(android.util.Log.INFO, "CorePatch", msg, null)

    @JvmStatic
    fun d(msg: String) = NativeRuntime.log(android.util.Log.DEBUG, "CorePatch", msg, null)
}
