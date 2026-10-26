import org.apache.tools.ant.taskdefs.condition.Os

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "org.lsposed.corepatch"
    compileSdk = 34

    defaultConfig {
        applicationId = "org.lsposed.corepatch.zygisk"
        minSdk = 28
        targetSdk = 34
        versionCode = 1
        versionName = "1.0.0"
        ndkVersion = "26.1.10909125"

        externalNativeBuild {
            cmake {
                abiFilters += "arm64-v8a"   // <-- 新增
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../native/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }
}


// ---------------------------------------------------------------------------
// Zygisk module packaging: produces build/module_out/CorePatch-Zygisk.zip
//   module.prop / customize.sh / update-binary
//   classes.dex                 (extracted from the built APK)
//   lib/arm64-v8a/libcorepatchzygisk.so
// ---------------------------------------------------------------------------

val moduleSrcDir = rootProject.file("module")

fun candidateNativeLib(abi: String): File? {
    val candidates = listOf(
        "intermediates/stripped_native_libs/release/stripReleaseDebugSymbols/out/lib/$abi/libcorepatchzygisk.so",
        "intermediates/cmake/release/obj/$abi/libcorepatchzygisk.so",
        "intermediates/merged_native_libs/release/mergeReleaseNativeLibs/out/lib/$abi/libcorepatchzygisk.so",
        "intermediates/merged_native_libs/release/mergeReleaseJniLibFolders/out/lib/$abi/libcorepatchzygisk.so",
    )
    return candidates
        .map { layout.buildDirectory.file(it).get().asFile }
        .firstOrNull { it.exists() }
}

val packageModule = tasks.register("packageModule") {
    group = "corepatch"
    description = "Assemble the Magisk Zygisk module zip."
    dependsOn("assembleRelease", "externalNativeBuildRelease")

    val outZip = layout.buildDirectory.file("module_out/CorePatch-Zygisk.zip")
    val stageDir = layout.buildDirectory.dir("module_stage").get().asFile
    val libAbi = "arm64-v8a"

    inputs.dir(moduleSrcDir)
    outputs.file(outZip)

    doLast {
        // 1) stage the module metadata + scripts
        delete(stageDir)
        mkdir(stageDir)
        copy {
            from(moduleSrcDir)
            into(stageDir)
        }

        // 2) classes.dex from the built APK (single dex is expected; the
        //    runtime is small). Multi-dex (classes2.dex...) is out of scope.
        val apk = layout.buildDirectory.file("outputs/apk/release/runtime-release-unsigned.apk").get().asFile
        if (!apk.exists()) throw GradleException("APK not found: $apk")
        copy {
            from(zipTree(apk))
            include("classes.dex")
            into(stageDir)
        }

        // 3) native library for arm64
                val zygiskLib = File(stageDir, "zygisk/arm64-v8a.so")
        zygiskLib.parentFile.mkdirs()
        copy {
            from(so)
            into(zygiskLib.parentFile)
            rename { "arm64-v8a.so" }
        }


        // 4) zip it up
        ant.invokeMethod("mkdir", mapOf("dir" to outZip.get().asFile.parentFile))
        ant.invokeMethod("zip", mapOf(
            "destfile" to outZip.get().asFile,
            "basedir" to stageDir,
        ))
        logger.lifecycle("module packaged -> {}", outZip.get().asFile)
    }
}

tasks.named("assemble").configure { dependsOn(packageModule) }
