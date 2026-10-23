package org.lsposed.corepatch.zygisk

import java.lang.reflect.Executable

/** Opaque handle to an installed hook. */
class HookHandle internal constructor(val handle: Long)

/** The mutable chain state a single intercepted call flows through. */
class HookChain internal constructor(val handle: Long, val member: Executable) {
    var thisObject: Any? = null
    var args: Array<Any?> = arrayOf()
    var result: Any? = null
    var throwable: Throwable? = null

    private var skipped = false
    private var skipResult: Any? = null

    fun returnAndSkip(result: Any?) {
        skipped = true
        skipResult = result
    }

    fun isSkipped() = skipped
    fun getSkipResult() = skipResult

    /** Invoke the original implementation with `newArgs` (defaults to current args). */
    fun proceed(newArgs: Array<Any?> = args): Any? {
        args = newArgs
        return try {
            NativeRuntime.invokeOrigin(handle, thisObject, newArgs).also { result = it }
        } catch (t: Throwable) {
            throwable = t
            throw t
        }
    }
}

/** One installed hook: the target member plus the before/after callbacks. */
class HookEntry(
    val handle: Long,
    val member: Executable,
    val before: ((HookChain) -> Unit)?,
    val after: ((HookChain) -> Unit)?,
)
