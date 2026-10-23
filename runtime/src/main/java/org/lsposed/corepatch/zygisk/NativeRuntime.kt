package org.lsposed.corepatch.zygisk

/**
 * JNI bindings into the native engine (jni_bridge.cpp / art_hooker.cpp).
 * These are `external` and are registered by module.cpp right after the dex is
 * injected into system_server.
 */
object NativeRuntime {
    /** Register the bridge class used by the native trampoline. */
    external fun init(bridgeClass: Class<*>): Boolean

    /** Hook a java.lang.reflect.Method/Constructor; returns an opaque handle (0 = failed). */
    external fun hook(member: Any): Long

    /** Restore the original entry of a hooked method. */
    external fun unhook(handle: Long): Boolean

    /** Ask ART to run the method in the interpreter. */
    external fun deoptimize(member: Any): Boolean

    /** Call the original implementation of a hooked method. */
    external fun invokeOrigin(handle: Long, thiz: Any?, args: Array<Any?>): Any?

    external fun log(level: Int, tag: String, msg: String, t: Throwable?)
}
