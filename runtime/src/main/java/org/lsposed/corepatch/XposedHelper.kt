package org.lsposed.corepatch

import android.annotation.SuppressLint
import android.graphics.Point
import android.util.Log
import java.lang.reflect.Executable
import java.lang.reflect.Field
import java.lang.reflect.Method
import org.lsposed.corepatch.zygisk.ConfigStore
import org.lsposed.corepatch.zygisk.HookChain
import org.lsposed.corepatch.zygisk.HookHandle
import org.lsposed.corepatch.zygisk.NativeRuntime
import org.lsposed.corepatch.zygisk.Runtime
import org.lsposed.corepatch.zygisk.RuntimeLog

typealias BeforeCallback = (XposedHelper.BeforeHookCallback) -> Unit
typealias AfterCallback = (XposedHelper.AfterHookCallback) -> Unit

/**
 * Adapted XposedHelper: same public surface CorePatch's hooks rely on, but
 * backed by the self-contained zygisk runtime instead of libxposed.
 */
object XposedHelper {
    lateinit var hostClassLoader: ClassLoader
        private set
    lateinit var moduleDir: String
        private set

    /** Config toggles; matches the `SharedPreferences.getBoolean(key, def)` contract. */
    val prefs: ConfigStore by lazy { ConfigStore }

    fun setHostClassLoader(classLoader: ClassLoader) {
        hostClassLoader = classLoader
    }

    fun setModuleDir(dir: String) {
        moduleDir = dir
    }

    class BeforeHookCallback(private val chain: HookChain) {
        val thisObject: Any? get() = chain.thisObject
        val args: Array<Any?> get() = chain.args

        fun returnAndSkip(result: Any?) = chain.returnAndSkip(result)
        fun isSkipped() = chain.isSkipped()
        fun getSkipResult() = chain.getSkipResult()
    }

    class AfterHookCallback(
        private val chain: HookChain,
        var result: Any?,
        var throwable: Throwable?,
    ) {
        val thisObject: Any? get() = chain.thisObject
        val args: Array<Any?> get() = chain.args
    }

    fun hookBefore(
        member: Executable, callback: BeforeCallback
    ): HookHandle {
        return Runtime.hook(member, before = { chain ->
            callback(BeforeHookCallback(chain))
        }, after = null)
    }

    fun hookAfter(
        executable: Executable, callback: AfterCallback
    ): HookHandle {
        return Runtime.hook(executable, before = null, after = { chain ->
            val acb = AfterHookCallback(chain, chain.result, chain.throwable)
            callback(acb)
            chain.result = acb.result
            chain.throwable = acb.throwable
        })
    }

    fun log(message: String, throwable: Throwable? = null) {
        if (throwable != null) {
            RuntimeLog.e("$message\n${Log.getStackTraceString(throwable)}")
        } else {
            RuntimeLog.i(message)
        }
    }

    fun deoptimize(method: Method): Boolean = Runtime.deoptimize(method)

    /** Returns an invoker that calls the ORIGINAL implementation (bypassing any hook). */
    fun getOriginInvoker(method: Method): OriginInvoker? = OriginInvoker(method)

    fun findClassIfExists(name: String): Class<*>? {
        return try {
            hostClassLoader.loadClass(name)
        } catch (_: ClassNotFoundException) {
            null
        }
    }

    fun setStaticBoolean(field: Field, value: Boolean) {
        try {
            field.isAccessible = true
            field.get(null)
        } catch (_: IllegalAccessException) {
        }

        val offset = UnsafeAccess.getInt(field, fieldOffsetValue).toLong()
        UnsafeAccess.putBoolean(field.declaringClass, offset, value)
    }

    @SuppressLint("DiscouragedPrivateApi")
    private object UnsafeAccess {
        private val unsafeClass = Class.forName("sun.misc.Unsafe")
        private val unsafeInstance = unsafeClass.getDeclaredField("theUnsafe").let { field ->
            field.isAccessible = true
            field.get(null)
        }
        private val getIntMethod = unsafeClass.getMethod(
            "getInt", Any::class.java, Long::class.javaPrimitiveType
        )
        private val putIntMethod = unsafeClass.getMethod(
            "putInt", Any::class.java, Long::class.javaPrimitiveType, Int::class.javaPrimitiveType
        )
        private val putBooleanMethod = unsafeClass.getMethod(
            "putBoolean",
            Any::class.java,
            Long::class.javaPrimitiveType,
            Boolean::class.javaPrimitiveType
        )
        private val objectFieldOffsetMethod = unsafeClass.getMethod(
            "objectFieldOffset", Field::class.java
        )

        fun getInt(target: Any, offset: Long) =
            getIntMethod.invoke(unsafeInstance, target, offset) as Int

        fun putInt(target: Any, offset: Long, value: Int) {
            putIntMethod.invoke(unsafeInstance, target, offset, value)
        }

        fun putBoolean(target: Any, offset: Long, value: Boolean) {
            putBooleanMethod.invoke(unsafeInstance, target, offset, value)
        }

        fun objectFieldOffset(field: Field) =
            objectFieldOffsetMethod.invoke(unsafeInstance, field) as Long
    }

    @Suppress("DEPRECATION")
    @SuppressLint("SoonBlockedPrivateApi")
    private fun getFieldOffsetOffset(): Long {
        var noSuchFieldException: NoSuchFieldException? = null
        try {
            val offsetField = Field::class.java.getDeclaredField("offset")
            offsetField.isAccessible = true
            offsetField.getInt(offsetField)
            return UnsafeAccess.objectFieldOffset(offsetField)
        } catch (e: NoSuchFieldException) {
            noSuchFieldException = e
        } catch (_: IllegalAccessException) {
        } catch (_: UnsupportedOperationException) {
        }

        val probeField = Point::class.java.getDeclaredField("x")
        probeField.getInt(Point())
        val fieldOffset = UnsafeAccess.objectFieldOffset(probeField).toInt()
        for (offset in 8 until 256 step 4) {
            val offsetLong = offset.toLong()
            if (UnsafeAccess.getInt(probeField, offsetLong) != fieldOffset) continue

            val modifiedOffset = fieldOffset.inv()
            UnsafeAccess.putInt(probeField, offsetLong, modifiedOffset)
            val currentOffset = UnsafeAccess.objectFieldOffset(probeField).toInt()
            UnsafeAccess.putInt(probeField, offsetLong, fieldOffset)
            if (currentOffset == modifiedOffset) return offsetLong
        }
        throw noSuchFieldException ?: NoSuchFieldException("Field.offset")
    }
}

/**
 * Calls the original implementation of a method. If the method is currently
 * hooked, routes through the native origin invoker (entry restored then
 * re-hooked) to avoid recursion; otherwise a plain reflective invoke.
 */
class OriginInvoker(private val method: Method) {
    fun invoke(instance: Any?, vararg args: Any?): Any? {
        val handle = Runtime.handleOf(method)
        return if (handle != 0L) {
            NativeRuntime.invokeOrigin(handle, instance, args as Array<Any?>)
        } else {
            method.invoke(instance, *args)
        }
    }
}
