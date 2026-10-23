# CorePatch-Zygisk

Self-contained **Magisk Zygisk module** conversion of
[LSPosed/CorePatch](https://github.com/LSPosed/CorePatch) that **does not
require LSPosed** (or any other Xposed framework). It injects CorePatch's
signature/downgrade/shared-user bypass logic directly into `system_server` from
a plain Zygisk module.

> Upstream CorePatch is a thin module over LSPosed's `libxposed` API. To make
> it run without LSPosed, this project embeds the two things LSPosed normally
> provides: a minimal Xposed-compatible **Java runtime** and a native **ART
> hooking engine**. The 18 hook classes themselves are reused from upstream
> almost unchanged.

## What it does

Same behaviour as CorePatch: bypass APK signature verification, downgrade
restrictions, digest errors, resource-arsc restrictions, shared-user id
mismatches, the verification agent, etc., for Android 9–15. See
`runtime/.../hook/` for the exact methods hooked.

## Architecture

```
┌─ system_server (zygote fork)
│
│  Zygisk loads libcorepatchzygisk.so  ── zygisk_api_entry()
│     │
│     └─ module.cpp
│          · detects system_server (uid 1000 / postServerSpecialize)
│          · injects classes.dex via PathClassLoader(system classloader)
│          · registers JNI natives (jni_bridge.cpp)
│          · calls Bootstrap.main(classLoader, moduleDir)
│                  │
│                  ▼
│        Bootstrap.kt ── installs the 18 CorePatch hooks
│                  │
│                  ▼
│        Runtime / XposedHelper ── hookBefore/hookAfter/deoptimize/
│                                  getOriginInvoker/log/prefs
│                  │  (Java, replaces libxposed)
│                  ▼
│        art_hooker.cpp  (native ART hooking engine)
│            · swaps ArtMethod::entry_point_from_quick_compiled_
│            · ARM64 trampoline marshals args → JavaBridge → chain
│            · deoptimize() + origin invoke (entry restore/re-hook)
```

### Key directories

| Path | Role |
|---|---|
| `module/` | Magisk module packaging (`module.prop`, `customize.sh`, `update-binary`) |
| `native/src/main/cpp/module.cpp` | Zygisk entry + dex injection |
| `native/src/main/cpp/art_hooker.cpp` | ART method hooking engine (trampoline + offsets) |
| `native/src/main/cpp/jni_bridge.cpp` | JNI natives exposed to the runtime |
| `native/zygisk.hpp` | Self-contained copy of the classic Zygisk API header |
| `runtime/src/main/java/org/lsposed/corepatch/zygisk/` | The runtime (replaces libxposed) |
| `runtime/src/main/java/org/lsposed/corepatch/hook/` | CorePatch hooks (reused from upstream) |
| `runtime/.../XposedHelper.kt`, `Config.kt` | Adapted to the self-contained runtime |

## Status — read this first

This is a **complete, buildable scaffold** for the conversion. Two layers are
fully implemented and correct-on-paper: the Zygisk module packaging, the
native entry + dex injection, and the entire Java runtime (hook chain,
before/proceed/after semantics, config store, adapted `XposedHelper`/`Config`,
reused upstream hooks). What LSPosed's core did — **hooking ART Java methods
generically** — lives in `art_hooker.cpp`, and that is the bring-up-critical
component:

- **ArtMethod `entry_point_from_quick_compiled_` offset** is auto-calibrated by
  scanning for a pointer into executable memory, with per-version defaults as a
  fallback. It must be validated per Android version/device.
- **The ARM64 trampoline** that marshals the real call arguments (and writes
  back primitive/object results) follows AAPCS64 for the JNI native calling
  convention. Argument marshalling and return-value unboxing are the part that
  needs real-device bring-up (a JNI-style `jvalue` marshaller based on the
  stored `java.lang.reflect.Method` signature).
- **`deoptimize`** currently restores the saved entry point; a full port looks
  up `art_quick_to_interpreter_bridge`.

Until the trampoline marshalling is completed on a device, hooks that inspect
`callback.args` will see an empty argument array (the chain/`proceed`/origin
logic itself is complete). **Plan for on-device bring-up before use.**

### Honest alternative

If your only goal is "run CorePatch on Zygisk", the zero-effort path is the
official **LSPosed Zygisk build** (LSPosed itself ships as a Zygisk module via
ZygiskNext) — that *is* "Zygisk" but still uses LSPosed. This project is for
the case where you explicitly want **no LSPosed at all**.

## Requirements

- Android 9–15 (same as upstream CorePatch)
- Magisk with Zygisk enabled (24+), or ZygiskNext
- Only **arm64** is targeted (the trampoline is ARM64-specific)

## Build → flashable zip

Prerequisites: Android SDK (platform 34, build-tools 34.0.0, NDK 26.1.10909125, CMake 3.22.1), JDK 17, Gradle (wrapper included).

```bash
export ANDROID_HOME=/path/to/android-sdk
./build.sh                 # writes local.properties, runs the build
```

Or directly:

```bash
./gradlew :runtime:packageModule
```

Either way the output is a **flashable Magisk module**:

```
runtime/build/module_out/CorePatch-Zygisk.zip
```

### GitHub Actions (CI)

`.github/workflows/build.yml` builds the module on `ubuntu-latest` (installs SDK + NDK, runs `packageModule`) and:

- uploads `CorePatch-Zygisk.zip` as a **workflow artifact** on every push / PR / manual run;
- on a version tag (`vX.Y.Z`) additionally **publishes the zip as a GitHub Release asset**.

Push the repo to GitHub, then either:

```bash
git tag v1.0.0 && git push origin v1.0.0
```

or run **Actions → Build CorePatch-Zygisk module → Run workflow**, and download the zip from the run's artifact (or the release page). That zip is what you flash.

## Install (flash)

1. Enable **Zygisk** in Magisk (Settings → Zygisk → reboot).
2. Flash `CorePatch-Zygisk.zip` in Magisk (or the Magisk Manager module installer).
3. Reboot. Verify with `adb logcat -s CorePatchZygisk`.

## Configuration

The module has no companion app. Toggles live in
`/data/adb/modules/corepatch_zygisk/config/corepatch.conf`
(seeded by `customize.sh` with sensible defaults, `key=1|0` per line):

```
downgrade=1
bypass_min_sdk=1
bypass_verification=1
bypass_digest=1
use_previous_signatures=1
bypass_shared_user=1
disable_verification_agent=1
bypass_block=1
```

Edit the file and reboot (config is read at bootstrap and cached with a mtime
check, so a reboot is simplest).

## Layout notes / porting

- `module.cpp` looks for `classes.dex` at the module root and passes the module
  dir to `Bootstrap.main`. Both come from the Magisk module directory resolved
  via `Api::setModuleDirByMyself`.
- The `io.github.libxposed` API is **not** used — the runtime in
  `org.lsposed.corepatch.zygisk` is a fresh, minimal implementation of exactly
  the surface CorePatch touches (`hook`/`deoptimize`/`getOriginInvoker`/`log`/
  `prefs` + the chain semantics).

## License

GPL-2.0 (same as upstream CorePatch). Portions of `art_hooker.cpp` follow the
well-known ART entry-point-replacement technique.
