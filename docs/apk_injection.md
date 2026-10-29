# APK injection: GTA SA DE 1.87.0 (arm64), loading libgtaml without root

Paths: original decode `apk/dec/` (`apktool d -r --no-assets`), binary manifest dump `apk/manifest_tree.txt` (`aapt dump xmltree`). Every `file:line` below is relative to `dec/` unless it names another file.

## 1. Findings

### Manifest (`manifest_tree.txt`)
- Package `com.rockstargames.gtasa.de`, versionName 1.87.0, versionCode 0x3402fb9, minSdk 30 / targetSdk 36 (l.3-13).
- Application `com.epicgames.ue4.GameApplication` (l.87). Its `onCreate` only registers lifecycle and network callbacks. It never touches `GameActivity` (`smali/com/epicgames/ue4/GameApplication.smali:100-119`).
- **Launcher = `com.an1.unpackdata.MainActivity`** (MAIN+LAUNCHER, l.120-128). The repacker added it. `com.epicgames.ue4.GameActivity` keeps MAIN but has no LAUNCHER category (l.96-119). GameActivity has `android.app.lib_name=UE4` (l.106-107), plus a stray `android:debuggable=false` on the activity element (l.100). That attribute does nothing there.
- `extractNativeLibs`: **absent**, so the default `true` applies. Existing `.so` files are stored Deflated in the APK, which matches. `application android:debuggable` is absent (not debuggable).
- Splits: none required. There is no `split`/`isSplitRequired`/`requiredSplitTypes`. `com.android.dynamic.apk.fused.modules="base,obbassets"` and `com.android.vending.derived.apk.id=2` (l.428-441) show this is a Play *standalone/fused* APK, so one APK is enough.
- Original signature: v3 only, cert `CN=A1, OU=A1, O=A1, L=Lazyland, C=RU` (SHA-256 `1cbd2d05…0c98`). It is already not Rockstar's key.

### Startup flow
1. `an1.MainActivity.onCreate` (`smali/com/an1/unpackdata/MainActivity.smali:3482`) reads prefs `installer_an1_prefs`. If `download_donev3` is set, it calls `launchGame()` right away (`:3597`). Otherwise it calls `startPreparation()` (`:3606`).
2. The preparation thread runs `System.loadLibrary("Notnolib")` (`:3390-3392`, catches UnsatisfiedLinkError → "Failed to initialize. Please reinstall the app." `:3411`). It then calls native `yrjrgpenkjzp(versionCode)` (`:3329`), which returns the data-manifest URL. The game data is downloaded and unzipped into `getFilesDir()/assetpacks` (`:3261-3265`), which is internal storage and is **wiped on uninstall**.
3. `launchGame` → `new Intent(this, GameActivity.class); startActivity; finish()` (`:2977-2986`). `const-class` does not initialise the class, so `GameActivity.<clinit>` first runs when the framework instantiates the activity.
4. **`GameActivity.<clinit>`** (`smali/com/epicgames/ue4/GameActivity.smali:404`, `.locals 33` at `:405`):
   - `:467` `const-string v3, "UE4"` (also used for the Logger tag)
   - `:709` `invoke-static {v4}, La1/b2/c3;->d4(I)…`. The repacker's string decryptor already runs here, so libaaaaaaaa is loaded no later than this point.
   - **`:777` `invoke-static {v3}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V`**. This is the UE4 load. libUE4's own `JNI_OnLoad` (export at 0x4a94ec8) runs inside this call. libUE4 has `DT_NEEDED libc++_shared.so`, so the linker has already mapped c++_shared at this point.
   - `:779-781` `const-string v0, "c++_shared"` + `loadLibrary` (a no-op re-load), then `:783` `return-void`.
5. `GameActivity extends android.app.NativeActivity` (`:2`). In `onCreate` (`:13760`), `invoke-super NativeActivity.onCreate` (`:13781`) calls `ANativeActivity_onCreate` in libUE4 (0x4ac2ea4), which starts the `android_main` engine thread. Later in `onCreate`, `nativeSetGlobalActivity` (`:15880`), GooglePlayLicensing `Init` (`:16339-16347`) and `new GTAPlus(activity)` (`:16555`) run. So `<clinit>` and our load come strictly before any engine init.

### Flutter flow
Flutter is used only for the Rockstar SDK UI. It is not the launcher. `GTAPlus.<init>` calls `Rockstar.setup(...)` (`smali_classes4/com/rockstargames/gtaplus/GTAPlus.smali:56`), which opens `rockstarmobile.ui.FlutterScreenHost` (manifest l.236). In its `onCreate`, `new FlutterEngine(appContext)` (`smali_classes2/rockstarmobile/ui/FlutterScreenHost.smali:517`) triggers `FlutterLoader` → `FlutterJNI.loadLibrary` (`smali_classes3/io/flutter/embedding/engine/loader/FlutterLoader$1.smali:108`) → `ReLinker.loadLibrary(ctx, "flutter")` (`smali_classes4/io/flutter/embedding/engine/FlutterJNI.smali:1618`). libapp.so is loaded by the Flutter engine. All of this happens after GameActivity.onCreate, so it does not affect injection.

### Other loadLibrary sites (none relevant)
Backtrace (`smali/backtraceio/library/base/BacktraceBase.smali:58`), Sentry (`smali/io/sentry/ndk/SentryNdk.smali:56-64`), TryAlloc (`smali/com/google/android/apps/internal/games/memoryadvice/TryAllocTester.smali:12`), GameSdk (`smali_classes3/com/google/androidgamesdk/GameSdkDeviceInfoJni.smali:17`).

### Repack libs (record only; our tool keeps them untouched and does not depend on them)
`libaaaaaaaa.so` is loaded by `a1.b2.c3.<clinit>` (`smali_classes4/a1/b2/c3.smali:18-20`). Its exports are `Java_a1_b2_c3_kgsuylfb/hrnaxtua`, and it imports only malloc/memcpy/free/pthread_once. It is a pure string-table decryptor behind `c3.d4(int)`, which the repacker substituted for string constants at **1426 call sites in 903 classes** (androidx, gms, sentry, rockstarmobile, com/epicgames/ue4 …). The APK therefore cannot run without it, and it must stay. `libNotnolib.so` (`MainActivity.smali:3390`) exports only `Java_com_an1_unpackdata_MainActivity_yrjrgpenkjzp` and uses AY_OBFUSCATE strings (`ay::obfuscated_data`). Its JNI use is FindClass, GetStaticMethodID, CallStaticObjectMethod, NewStringUTF. Its only "interesting" imports (`syscall`, `__system_property_get`, `getpid`) are all inside the statically linked libc++abi/libunwind: gettid (178) in `__cxa_guard_acquire` and process_vm_readv (270) in the unwinder. Neither lib has signature, package, /proc, or Frida checks. Neither has any `PackageManager`/`signatures` strings. **Neither lib checks the APK signature, so re-signing trips nothing in them.**

### Java signature / license / integrity checks
- `GET_SIGNATURES`/`signingInfo`/`hasSigningCertificate` appear only in library code: androidx `PackageInfoCompat`/`FontProvider`/emoji2, and GMS `GoogleSignatureVerifier`/`GooglePlayServicesUtilLight`, which verify *Google Play services'* signature, not ours. App code calls `getPackageInfo` only with flags 0: `GameActivity.smali:14128` (v12=0, reads targetSdk), `:16063`, and `smali_classes2/rockstarmobile/utilities/ApkInfoUtils.smali:65,101`.
- LVL: `GooglePlayLicensing.Init` runs (`GameActivity.smali:16347`), but `CheckLicense` (`smali/com/epicgames/ue4/GooglePlayLicensing.smali:95`, which calls `LicenseChecker.checkAccess` at `:168`) has **no caller**. No smali references it, and the string `CheckLicense` is absent from libUE4.so, so no JNI caller exists either. It is dead code. The OBB `DownloaderService$LVLRunnable` belongs to `DownloaderActivity`, which the an1 flow does not use.
- No Play Integrity or SafetyNet in app code. The only `integrity` hits are GMS nearby internals. libUE4 strings contain no `getPackageInfo`/`GET_SIGNATURES`/`getInstallerPackageName`.
- What a re-sign does break: the same things the A1 repack already broke, namely Google Sign-In/Play Games (`com.google.android.gms.games.APP_ID`) and Play Billing, which are tied to Rockstar's cert. Our key adds nothing new. **The practical cost** is that the new key differs from the installed A1 key, so the user must uninstall first (`INSTALL_FAILED_UPDATE_INCOMPATIBLE` otherwise). Uninstalling wipes `files/assetpacks`, and the an1 launcher then re-downloads the game data.

## 2. Injection design

**Chosen: one smali `loadLibrary("gtaml")` immediately after `GameActivity.smali:777`.**

```smali
    invoke-static {v3}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V      # UE4 (original :777)
    const-string v33, "gtaml"                                                     # injected
    invoke-static/range {v33 .. v33}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V  # injected
    const-string v0, "c++_shared"                                                 # original :779
```
`.locals 33 → 34`. `<clinit>` is static and takes no params, so `v<old locals>` is always a fresh register that nothing else reads. `/range` is used because 35c `invoke-static {vX}` only encodes v0–v15. The script finds the call by tracking `const-string` per register and matching the single `loadLibrary(reg=="UE4")`, so it uses no hardcoded register numbers or line numbers.

Why this spot is right:
- libUE4 is fully linked and its JNI_OnLoad has run, so `dlopen("libUE4.so", RTLD_NOLOAD)` + `dlsym` work.
- The engine has not started yet: `ANativeActivity_onCreate`/`android_main` only run later, from `onCreate`.
- ART calls our `JNI_OnLoad(JavaVM*)` for free, in the app's classloader namespace.

**Rejected: adding DT_NEEDED `libgtaml.so` to a lib.**
- It still changes the APK, so it needs the same re-sign. It saves nothing.
- Libraries pulled in through DT_NEEDED get no `JNI_OnLoad`, so no JavaVM. You would need a constructor plus `JNI_GetCreatedJavaVMs`.
- If added to libUE4, our constructor runs *before* libUE4's own constructors/JNI_OnLoad. libUE4 is not yet usable at that point (`RTLD_NOLOAD` lookup during its own load is unreliable [INFERENCE]).
- It needs ELF rewriting of a 190 MB file, and no patchelf/LIEF is installed. It would also have to be redone on every game update.

The other option, swapping the `lib_name` meta-data to a forwarding NativeActivity lib, needs binary manifest edits and symbol forwarding. That is more code for no gain.

## 3. Test lib: `apk/testlib/`
- `gtaml.c`: `JNI_OnLoad` does `dlopen("libUE4.so", RTLD_NOW|RTLD_NOLOAD)` → `dlsym("GUObjectArray")` (exported at 0xaf558e8 in libUE4's .bss) → `dladdr` → `__android_log_print(INFO, "GTAML", "loaded, libUE4 base=%p …")`. If any step fails it logs `dlerror()`.
- `build.sh`: NDK r27d `aarch64-linux-android30-clang -shared -fPIC -O2 -Wl,-z,max-page-size=16384 -llog -ldl` → `libgtaml.so` (6.3 KB; NEEDED liblog, libdl, libc only; exports `JNI_OnLoad`).
- Expected logcat on device (not run here, per instructions): `I GTAML: loaded, libUE4 base=0x7… (GUObjectArray=0x7…, /data/app/…/lib/arm64/libUE4.so)`.

## 4. Patch script: `apk/patch_apk.sh <in.apk> <libgtaml.so> <out.apk>`
Steps:
1. `apktool d -r --no-assets` (smali only, about 7 s).
2. An embedded Python injector edits `GameActivity.smali`. It asserts `<clinit>` exists, finds exactly one `loadLibrary("UE4")`, checks the register count is under 256, and refuses an already-patched APK.
3. `apktool b` with only the smali dir that holds GameActivity (others are deleted, so unrelated dex files are never re-assembled).
4. It copies the **input APK**, removes the old v1 `META-INF/*.SF|RSA|DSA|EC|MANIFEST.MF`, and replaces only `classesN.dex` plus adds `lib/arm64-v8a/libgtaml.so`. The `.so` is *stored*, which is valid whether extractNativeLibs is true or false. Assets, the other dex files, the other libs and `resources.arsc` stay byte-identical. This also avoids apktool re-compressing about 1 GB of assets.
5. `zipalign -f -p 4`.
6. `apksigner sign` with `apk/debug.keystore`. The keystore is generated on first run (alias `androiddebugkey`, pass `android`, RSA 2048, CN=Android Debug). Then `apksigner verify`.

It uses the newest SDK build-tools and JAVA_HOME=JDK21 when present; set `BUILD_TOOLS`, `KEYSTORE`, or `ANDROID_HOME` to override. Runtime is about 37 s for the 900 MB APK.

### Verification run (on `device/base.apk`)
- Output `apk/out/gtasa_gtaml_test.apk` (860.6 MB). `apksigner verify -v` → `Verifies`, v3 true, signer `CN=Android Debug, O=Android, C=US`, SHA-256 `84168b3e…8f40`. `zipalign -c -p 4` passes.
- CRC diff of every entry, original vs patched: the only changes are `classes.dex` (new CRC), `+lib/arm64-v8a/libgtaml.so` (Stored), and the swapped signature files (`CERT.*` → `ANDROIDD.*`, plus a new `MANIFEST.MF`). The other 898 entries are identical.
- Re-decoding the output, `smali/com/epicgames/ue4/GameActivity.smali` shows `:405 .locals 34`, `:777` UE4 load, `:779 const-string v33, "gtaml"`, `:781 invoke-static/range {v33 .. v33}, …loadLibrary`, `:783` the c++_shared load. `lib/arm64-v8a/libgtaml.so` is present next to the original 10 libs.
- Running the script again on the patched APK aborts with `AssertionError: already patched`.

Not verified: the on-device run and the logcat line, because adb/installs were out of scope.
