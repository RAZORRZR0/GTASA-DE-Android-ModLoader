# GTA SA Definitive Edition — Android modding research

Scope: web research only (no device access). Date: 2026-10-30.
`[INFERENCE]` = my conclusion, not stated by a source. "Device fact" = facts Main read off the phone and passed to me.

> **Priority (per user):** the target is **non-rooted** users. Root methods (Zygisk, ptrace, wrap.prop) are for development only. Q5 and the final recommendation rank the no-root options first.

---

## 1. The Android release

| Item | Finding | Source |
|---|---|---|
| SKUs | Two separate Android apps: **Netflix** `com.netflix.NGP.GTASanAndreasDefinitiveEdition` (Netflix membership required, released 14 Dec 2023) and **Rockstar** `com.rockstargames.gtasa.de` (paid on Play, $19.99, page shows "Updated Nov 4, 2025"). | [APKMirror](https://www.apkmirror.com/apk/netflix-inc/gta-san-andreas-netflix/gta-san-andreas-netflix-1-72-42919648-release/gta-san-andreas-netflix-1-72-42919648-android-apk-download), [Play (Rockstar)](https://play.google.com/store/apps/details?id=com.rockstargames.gtasa.de&hl=en_US), [Rockstar newswire](https://www.rockstargames.com/newswire/article/8978k883335o83/grand-theft-auto-the-trilogy-the-definitive-edition-now-available-on-n) |
| Device build | `com.rockstargames.gtasa.de` 1.87.0 (vc 54538169), installed via `com.google.android.packageinstaller` (sideloaded). | Device fact |
| Netflix versions | 1.72.42919648 (Dec 2023) → 1.86.44544238 (Apr 2024). | APKMirror |
| ABI / SDK | Netflix build: **arm64-v8a only**, min API 30 (Android 11), target API 33. Rockstar build: arm64 libs only on the device. Not verified whether other Rockstar splits exist. | APKMirror; device fact |
| Packaging | Netflix: base APK (183 MB) plus a Play Asset Delivery split named `obbassets` (983 MB), all signed by "CN=PPD Builder, O=Netflix, Inc.". On the Netflix install `Android/obb/...` is empty and `Android/data/...` holds no .pak files. | APKMirror; [GTAForums thread](https://gtaforums.com/topic/994733-san-andreas-definitive-edition-mobile-modding) |
| Engine | Unreal Engine 4 ("Unreal® Engine… Uses Oodle" in both store listings). Exact **4.26 is NOT verified for Android**. Check with `strings libUE4.so \| grep -m3 "++UE4+Release-"`. | Store listings |
| Main lib | `libUE4.so` 193 MB, plus `libflutter.so` + `libapp.so` (Flutter, i.e. Rockstar's launcher UI), `libsentry*.so`, `libbacktrace-native.so`, `libc++_shared.so`, `libtry-alloc-lib.so`, `libNotnolib.so`, `libaaaaaaaa.so`. | Device fact |
| Data dir | The game uses `/data/data/<pkg>/files/UE4Game/Gameface/...`, which no-root users can't see. fastman92's tool redirects it and auto-creates `.../files/UE4Game/Gameface/Engine/Content/Paks/~mods/`. So the project name is "Gameface", same as PC, and pak mods load from a `~mods` folder. | [fastman92 APK modifier](https://gtaforums.com/topic/979211-fastman92-apk-modifier/) |
| Permissions (Netflix) | `com.android.vending.CHECK_LICENSE` (Play Licensing), `com.netflix.nfgsdk.permission.ngpstore` (Netflix Games SDK), AD_ID, INSTALL_REFERRER. The Rockstar manifest has not been checked. | APKMirror |
| PC ↔ Android code | Same codebase. PC/console TU 1.112 (12 Nov 2024) back-ported the mobile version's fixes and lighting. But they are separate compiles (MSVC x64 vs clang arm64), so **no byte signature carries over**. Only the logic is shared, and even that is `[INFERENCE]`. | [IGN](https://www.ign.com/articles/gta-trilogy-definitive-edition-gets-surprise-update-on-console-and-pc-adds-visual-features-that-were-exclusive-to-mobile-versions), [Eurogamer](https://www.eurogamer.net/three-years-later-gta-trilogy-finally-gets-pc-and-console-fixes-from-improved-mobile-port) |

### Anti-tamper, integrity and DRM

- **Evidence that re-signing works.** fastman92's APK modifier decompiles, rebuilds and re-signs packages. It lists "GTA III/VC/SA DE Rockstar Games" and "GTA III/VC/SA DE Netflix" as tested. Its changelog (14 Jan 2025) says it "detected signature verification in some cases, the recompilation of the package will be stopped". So no blocking signature check was hit on DE as of 2025 `[INFERENCE]`. ([source](https://gtaforums.com/topic/979211-fastman92-apk-modifier/))
- **Play "automatic protection" (PairIP)** can add an installer check ("get this app from Play") and, for select partners, anti-tamper checks that crash the app or stop it from running. It replaces the Play Licensing library. ([Google doc](https://support.google.com/googleplay/android-developer/answer/10183279?hl=en)) Its runtime is `libpairipcore.so` plus `com.pairip.*` classes in the dex ([SReverse](https://sreverse.simpalabs.com/research/pairip-libpairipcore), [APKiD #495](https://github.com/rednaga/APKiD/issues/495)). **`libpairipcore.so` is not in the device's lib list.** Either Play never added it, or the sideloaded build had it stripped. That can't be decided without a genuine Play-installed copy (unknown).
- **libNotnolib.so / libaaaaaaaa.so**: no web hits at all. I searched the exact names plus "apk mod" variants; grep.app was blocked. Both have newer timestamps and the install was sideloaded, so this is probably a cracked repack and these are its injected libs `[INFERENCE]`. Likely jobs: signature-kill (MT/NP-Manager style, which redirects `open()` of base.apk to the original) or license/PairIP bypass ([ApkSignatureKillerEx](https://github.com/L-JINBIN/ApkSignatureKillerEx) is the usual pattern). **Triage once the APK is pulled:** `readelf -d` (NEEDED/SONAME), `strings` (look for `base.apk`, `getPackageInfo`, `signatures`, `pairip`, `openat`), and grep the smali for the `loadLibrary` calls that load them.
- **libtry-alloc-lib.so**: purpose unknown (searched exact name, no hits). Don't replace it blind.
- **Netflix build**: Netflix Games SDK login (`nfgsdk`) needs the Netflix app or account. Whether that SDK checks the signing cert is **unknown** (searched; nothing public). fastman92 says his tool works on the Netflix DE builds.

---

## 2. Existing modding scene for Android DE

- **No public native mod loader targets DE Android.** Searched: GitHub repo search (`gtasa.de`, "San Andreas Definitive" android), the AML GitHub org (only "DebugInfo" matched, which is classic SA), the [AML mod list](https://androidmodloader.github.io/) (no "Definitive/UE4/Unreal/Trilogy"), GTAForums and YouTube. The YouTube "mods/modpack/cheat menu for SA DE Android/Netflix" videos exist but have no transcripts, so I couldn't verify how they work.
- **Pak modding exists**: fastman92 APK modifier creates the `Paks/~mods/` folder and redirects private storage to `/sdcard/AndUnpr/...`, so no root is needed to reach game files (see §1).
- **UE4SS on Android**: no official port. `xAstroBoy/arm64-ue-modloader` was a UE4SS-compatible Lua loader for arm64 UE4 (Quest, Dobby hooks, ADB bridge). It appears in search ([README snapshot](https://raw.githubusercontent.com/xAstroBoy/arm64-ue-modloader/refs/heads/main/README.md)), but the GitHub page and API returned **404** on 2026-10-30.
- **RusJJ AndroidModLoader (AML)**: MIT, actively maintained (last push 2026-09-30), 113★, "mod loader for almost ANY android game". ([repo](https://github.com/RusJJ/AndroidModLoader), [docs](https://raw.githubusercontent.com/RusJJ/AndroidModLoader/main/docs/DOCUMENTATION_EN.MD))
  - **Injection**: a smali `System.loadLibrary("AML")` placed after the game's own loadLibrary. Since 1.4.1, `net/rusjj/amlcore.launchAMLCore("libX.so,...")` can load mods before the game's `JNI_OnLoad`. ([external/README](https://raw.githubusercontent.com/RusJJ/AndroidModLoader/main/external/README.md))
  - **Mods folder**: `/sdcard/Android/data/<pkg>/mods/*.so` (also `/sdcard/AMLMods/<game>/`). It **copies each .so into the app data dir, then `dlopen`s it** (seen in `main.cpp`), because /sdcard is not executable `[INFERENCE]`. It also knows fastman92's AndUnpr paths (`HasFastmanAPKModified`).
  - **API** (`IAML`): `GetLib/GetLibLength/GetSym` (xDL), `PatternScan(pattern, lib)` / `FindAllPatterns` (byte-level `??` wildcards only, no bitmask), `Hook/HookPLT/HookB/HookBL`, `HookInline` (callback gets the full register context, uses Gloss), `RedirectReg`, `Write/Unprot/PlaceNOP/PlaceJMP/PlaceRET`, `HookVtableFunc`. Also `Config* cfg` (ini, `Bind(key, default)` under section "Preferences"), `logger->Info/Error`, `RegisterInterface/GetInterface` for mod-to-mod APIs, `ON_MOD_LOAD/ON_ALL_MODS_LOAD/ON_GAME_CRASH`, and dependency lists.
  - **Hook backend**: ARMPatch (4x11 ARMhook, Cydia Substrate, Rprop And64InlineHook) plus GlossHook. ([ARMPatch](https://github.com/RusJJ/ARMPatch), [GlossHook](https://github.com/XMDS/GlossHook) MIT)
  - **Usable for DE?** The core is game-agnostic (`NEEDGAME(pkg)` is optional, `GetLib("libUE4.so")` works), so as far as the docs show it would work unchanged `[INFERENCE]`. It already provides sig-scan + hook + log + ini. **Ponytail point: AML is the loader you'd otherwise write.** Either adopt it or copy its API shape: `MYMODCFG` macro, `DECL_HOOK`/`HOOK` macros, `aml->PatternScan`, `cfg->Bind`.
- **PC side, for reference only**: [LemonHaze420/GTA-TDE-UE4-SDK](https://github.com/LemonHaze420/GTA-TDE-UE4-SDK) (2021), [TrilogyDevelopment/TrilogySDK](https://github.com/TrilogyDevelopment/TrilogySDK) (SA DE, 2024), UE4SS signature for SA DE 1.112 ([Nexus 897](https://www.nexusmods.com/grandtheftautothetrilogy/mods/897)).

---

## 3. UE4 tooling on Android (finding GUObjectArray / GNames / ProcessEvent)

- **kp7742/UE4Dumper** (MIT, 969★): an external tool that reads game memory. **You pass the offsets yourself** (`-g` GNames, `-u` GUObjectArray, see `jni/kmods.cpp`). v0.21 is "the last and final update". ([repo](https://github.com/kp7742/UE4Dumper)) `kp7742/UE5Dumper` does not exist (API 404).
- **MJx0/AndUE4Dumper** is archived and says to use **[MJx0/MobileDumper-7](https://github.com/MJx0/MobileDumper-7)** (MIT, active Oct 2026). It is a Dumper-7 fork for iOS/Android, runs as an injected `.so` (auto-dumps 60 s after load, logs to logcat) or as a CLI, and outputs a C++ SDK, `.usmap`, IDA/Ghidra mappings and Dumpspace json. How it finds things on arm64 (from source):
  - **GUObjectArray / GNames** (`UEAnalyzerKitty`): decodes arm64 code and records every global access. `ADRP+ADD`/`ADR` counts as "AddressTaken"; `ADRP+LDR` from a relocated slot counts as "IndirectBase", the way several UE builds reach GUObjectArray. Candidates are ranked by nearby string anchors ("Unable to add more objects to disregard for GC pool", "VERIFYGC", "SHOWPENDINGKILLS", "CreateClustersFromPackage"…) and checked against the expected structure layout (pointer alignment, a chunk table at +off, the NumElementsPerChunk constant). (`Strategies/GUObjectArrayStrategy.cpp`, `Analysis/GlobalAccessHarvester.h`)
  - **ProcessEvent**: takes the vtable of a native class's default object (Texture2D, DataTable, GameViewportClient, otherwise any non-Actor/non-BP class) and scans up to 100 slots. A slot is accepted on ≥4 signals, including ≥3 callee-saved `STP` pairs in the prologue and references to the FUObjectArray. (`Engine/OffsetFinder/Offsets.cpp` `InitProcessEvent`)
- **Dumper-7** (Encryqed, 2350★): Windows only; use the MobileDumper-7 fork on Android.
- **Practical** `[INFERENCE]`: run MobileDumper-7 once to get offsets and an SDK. For UFUNCTION-backed behaviour, hook `ProcessEvent` or swap `UFunction::Func`; that needs no byte signatures. Byte sigs are only needed for non-reflected Gameface C++ (the old RW-era game code).

---

## 4. arm64 inline hook libraries

| Lib | License | Activity | Android support | NDK/CMake integration | Notes |
|---|---|---|---|---|---|
| **ShadowHook** ([bytedance/android-inline-hook](https://github.com/bytedance/android-inline-hook)) | MIT | 2430★, pushed 2026-10-22, v2.0.1 | **Android 4.1–17** (API 16–37), arm32/arm64 | Maven Central AAR with Prefab: `find_package(shadowhook REQUIRED CONFIG)` + `target_link_libraries(... shadowhook::shadowhook)`. Without Gradle, extract the AAR's prefab headers and .so, or build from source `[INFERENCE]`. | Used in ByteDance production apps. Hooks by address or by lib+symbol. Auto-hooks newly loaded ELFs. **Callbacks before/after the linker runs a new ELF's `.init`/`.init_array`**, i.e. you can hook libUE4 before its constructors run. Prevents recursion. Bypasses linker namespaces for dynsym/symtab lookup. Ships `libshadowhook.so` + `libshadowhook_nothing.so`. |
| Dobby ([jmpews/Dobby](https://github.com/jmpews/Dobby)) | Apache-2.0 | 4869★, last push 2025-01-26 | multi-platform | CMake source build | Mature and widely used (AML's ancestor, xAstroBoy's loader). Upstream has stalled; build breakage on newer NDKs is commonly reported `[INFERENCE]`. |
| And64InlineHook ([Rprop](https://github.com/Rprop/And64InlineHook)) | MIT | 764★, last push 2022-07 | arm64 only | 2 files, drop-in | Unmaintained. Minimal instruction relocation. Fine for simple prologues. |
| GlossHook ([XMDS](https://github.com/XMDS/GlossHook)) | MIT | 126★, 2026-06 | arm/arm64 | AML ships prebuilt libs | Register-context hooks (what AML's `HookInline` uses). |

**Recommendation: ShadowHook.** MIT, maintained, tested up to Android 17, single CMake target, and the dl-init callback solves "hook before game init" without patching the ELF. If AML is adopted as-is, its own backend (ARMPatch/Gloss) is used instead.

---

## 5. Injection methods

### 5A. No-root (primary). Every option requires re-signing the APK.

What re-signing breaks or keeps:
- **Updates**: a re-signed package can't update over, or be updated by, the Play version (signature mismatch). Users must uninstall first, and **uninstalling deletes `/data/data/<pkg>` including saves**. Save files must be backed up first. Saves probably live under `files/UE4Game/Gameface/Saved` `[INFERENCE]`. Every game update needs a re-patch.
- **Google Play Games / Google Sign-In**: the OAuth client is bound to the signing cert's SHA-1, so sign-in fails with DEVELOPER_ERROR (10). ([GPGS setup](https://developers.google.com/games/services/console/enabling), [client auth](https://developers.google.com/android/guides/client-auth))
- **PairIP installer/anti-tamper**: fails when the app isn't installed by Play or was modified (§1). It's absent from the device lib list, but status unknown on a genuine copy.
- **Play Licensing (LVL)**: the licence is tied to package + Google account. Re-signing alone probably doesn't flip it, but the installer source may matter `[INFERENCE]`.
- **Rockstar account login (Flutter launcher)**: no source found on whether it's cert-bound. Web/OAuth logins normally aren't `[INFERENCE]`. A reddit snippet for *classic* SA reports "black screen after skipping sign in from rockstar launcher" when AML is installed ([reddit](https://www.reddit.com/r/sanandreas/comments/1vf7h75/no_obb_on_my_android_gta_sa), blocked; search snippet only). So test the launcher→game hand-off.
- **Sentry**: no effect expected `[INFERENCE]`.
- **Evidence it works at all**: fastman92 re-signs both R* and Netflix DE builds (§1), and the device itself runs a sideloaded, apparently repacked build.

Split APKs: Play installs base plus config splits (`split_config.arm64_v8a.apk` holds `lib/`) plus asset-pack splits (the Netflix build has `obbassets`). **All splits must be re-signed with the same key** and installed together (`adb install-multiple`, SAI, APKMirror Installer). Alternatively merge them into one APK with [REAndroid/APKEditor](https://github.com/REAndroid/APKEditor) (`merge`, Apache-2.0) or fastman92's tool ("convert split → APK", "integrate assetpacks"). When repacking, keep native libs **uncompressed and 16 KB page-aligned** (`zipalign -P 16 4`), and build our .so with 16 KB LOAD alignment for Android 15+ devices ([page-sizes doc](https://developer.android.com/guide/practices/page-sizes)).

Ranked:

1. **Smali `System.loadLibrary("modloader")` + re-sign.** Simplest and the most proven (AML's official method).
   - Decode only the dex: `apktool d -r` (skips resources, avoiding the classic resource-rebuild failures). Find the smali that loads `"UE4"` and insert our loadLibrary call just before it. Our `JNI_OnLoad`/constructor then uses ShadowHook's dl-init callback to hook libUE4 before its `.init_array`.
   - Or insert it after: UE4's engine init (`android_main` → `FEngineLoop::PreInit`) runs later on the game thread, so hooking right after `loadLibrary("UE4")` returns is still before game init `[INFERENCE]`.
   - Touches the dex in base.apk plus a new lib in the lib split.
2. **Add DT_NEEDED to a small lib that is already loaded** (`patchelf --add-needed libmodloader.so <smalllib>`, GPL tool, or LIEF). No dex edit, so no apktool. The lib must be loaded *before* libUE4. Its load order and purpose are unknown for `libtry-alloc-lib.so`, so check its loader in smali first.
   - Patching DT_NEEDED on **libUE4.so** also works: bionic maps and relocates the whole dependency tree, then runs constructors dependencies-first, so ours runs before libUE4's `[INFERENCE]`. But rewriting a 193 MB ELF is the heavier and riskier edit.
3. **Replace a tiny lib with a proxy** (e.g. `libtry-alloc-lib.so`). The proxy must re-export every symbol and JNI method the original provides. There's no advantage over #2 (it's still a re-sign) and it adds forwarding code. **Not recommended** unless the original is a no-op.
4. **Frida Gadget** (repack, DT_NEEDED or loadLibrary of `libfrida-gadget.so`). Good for **dev exploration** with JS hooks. It's a ~20 MB+ runtime and not a mod API for users `[INFERENCE]`.
5. **LSPatch fork** ([JingMatrix/LSPatch](https://github.com/JingMatrix/LSPatch), GPL-3.0, active; original LSPosed/LSPatch archived). A no-root Xposed embed with built-in signature-bypass levels. Useful only if an in-app signature check shows up. It loads Java modules, which is overkill for us.

### 5B. Root (dev only)

| Method | Re-sign? | Survives updates? | Timing | Notes |
|---|---|---|---|---|
| Zygisk module (Magisk / KernelSU+ZygiskNext) | No | Yes (installed APK untouched) | `postAppSpecialize` runs before any app code, then hook `dlopen`/ShadowHook dl-init for libUE4 | Best dev loop. [zygisk-module-sample](https://github.com/topjohnwu/zygisk-module-sample), [ZygiskNext](https://github.com/Dr-TSNG/ZygiskNext) |
| ptrace inject at runtime (root daemon) | No | Yes | **After** init (process already running). Fine for dumpers, wrong for init hooks | Most fragile |
| `setprop wrap.<pkg>` LD_PRELOAD | No (if ro.debuggable=1) | Yes | Before everything | Only honoured for debuggable apps or debuggable builds ([wrap.sh doc](https://developer.android.com/ndk/guides/wrap-script)). On user builds you'd need a debuggable repack (= re-sign) or a root-patched `ro.debuggable` (e.g. Magisk resetprop) `[INFERENCE]` |

### Mod drop location for no-root users (Android 11–16)
- **`/sdcard/Android/data/<pkg>/files/mods/`**: the game reads its own external files dir with no permission. Users on Android 11+ can't pick files there via SAF (`ACTION_OPEN_DOCUMENT(_TREE)` excludes `Android/data` and `Android/obb`, see [docs](https://developer.android.com/training/data-storage/shared/documents-files), [Android 11 storage](https://developer.android.com/about/versions/11/privacy/storage)). In practice they copy over USB/MTP from a PC, or use a file manager via Shizuku or the SAF loophole on older versions (see [ScopeVerif, NDSS'25](https://www.ndss-symposium.org/wp-content/uploads/2025-340-paper.pdf)) `[INFERENCE for MTP]`. This is AML's default.
- **Alternative**: add `MANAGE_EXTERNAL_STORAGE` to the repacked manifest and read e.g. `/sdcard/GTASADE/mods`. The user grants "All files access" once in Settings. `requestLegacyExternalStorage` is ignored when targeting API 30+, and the Netflix build targets 33.
- **In-app SAF picker** (import a zip from Downloads): needs injected Java/dex code. Skipped as extra scope.
- Either way, **copy each mod .so to `/data/data/<pkg>/files/...` before `dlopen`** (AML does this).

---

## 6. Signature scanning on arm64

Pitfalls:
- Instructions are a fixed 4 bytes, but **immediates sit in bit fields that don't line up with bytes**, so IDA-style `??` byte wildcards are coarse:
  - **ADRP**: `1 immlo(2) 10000 immhi(19) Rd(5)`. The top byte is `0x90/0xB0/0xD0/0xF0` depending on immlo, and bytes 0–2 mix immhi and Rd, so wildcard the whole word or use a bitmask (`mask 0x9F00001F`).
  - **ADD imm12 / LDR [Xn,#imm12]**: wildcard bits [21:10].
  - **BL** `100101 imm26` → byte3 ∈ `0x94–0x97`. **B** → `0x14–0x17`. CBZ/CBNZ/B.cond use imm19.
  - Prefer a **value/mask scanner** (per-bit) over byte wildcards. AML's `PatternScan` is byte-level only.
- **Resolving targets** (to read globals or call targets): `page = (PC & ~0xFFF) + (sxt(immhi:immlo) << 12)`, then add the ADD/LDR `imm12` (scaled for LDR). BL target = `PC + sxt(imm26)*4`.
- Register allocation differs between builds, so mask `Rd/Rn/Rt` when signatures need to survive game updates.
- Functions may start with `PACIASP`(`D503233F`) or `BTI c`(`D503245F`) if built with branch protection. Don't anchor on prologues, and make sure the hook lib relocates them `[INFERENCE]`.
- Scan the mapped image, not the file. Data pointers are only valid after RELATIVE relocations are applied.
- **x64 → arm64 signatures can't be auto-converted byte-wise.** Port by anchors:
  - (1) **check symbols first**: `llvm-nm -D --defined-only libUE4.so | wc -l`. Classic libGTASA.so shipped full dynsym (AML mods call `aml->GetSym(hGTASA,"ThePaths")`, see [GTASA_GPS](https://github.com/AndroidModLoader/GTASA_GPS/blob/main/main.cpp)). **Local evidence**: a peer already produced `/nm_dem.txt` from the pulled build. It has 275,366 lines, and 281 match `CPed::|CWorld::`, so game classes look symbolized. Whether those come from `.dynsym` (resolvable at runtime via dlsym/xDL/ShadowHook) or `.symtab` was not checked by me. If they're available, sig scanning becomes a fallback only.
  - (2) unique string xrefs (ADRP+ADD to the string → containing function).
  - (3) unique immediates (MOVZ/MOVK constants, float literals).
  - (4) call-graph neighbours of already-found functions.
  - (5) UE reflection (UFunction→Func) for anything that is a UFUNCTION.
- **Diffing tools for cross-arch matching**:
  - [google/bindiff](https://github.com/google/bindiff) (Apache-2.0, open source, uses [BinExport](https://github.com/google/binexport) from IDA/Ghidra/Binary Ninja). Graph and call-graph based, so it works across architectures.
  - [Diaphora](https://github.com/joxeankoret/diaphora) (AGPL-3.0, IDA, pseudo-code heuristics).
  - Ghidra Version Tracking.
  - Expect partial matches. Use them to seed, then verify by hand and write arm64 masks.

---

## Ranked recommendation

**No-root (ship this):**
1. **Injection: smali `System.loadLibrary("modloader")` + re-sign all splits with one key** (or merge them with APKEditor), using `apktool d -r`. Fallback if dex rebuild is a problem: `patchelf --add-needed` on a small lib that loads before libUE4.
2. **Hook lib: ShadowHook** (MIT, Android 4.1–17, dl-init callback for pre-init hooks). Alternative: adopt **AML** whole. It already has the sig-scan/hook/log/ini API and the smali injection recipe, but its scanner is byte-level only.
3. **Mods dir**: `/sdcard/Android/data/com.rockstargames.gtasa.de/files/mods/`, copied to internal storage before `dlopen`. Optionally add `MANAGE_EXTERNAL_STORAGE` for a user-visible folder.
4. **Warn users**: uninstall wipes saves (back up first); no Play/GPGS sign-in; re-patch after each update.

**Root (dev only):** Zygisk module + ShadowHook, for iterating without re-signing. Run MobileDumper-7 as an injected .so to get GObjects/GNames/ProcessEvent and the SDK.

Not checked by me: PairIP/LVL classes in the dex, what libNotnolib/libaaaaaaaa/libtry-alloc-lib do, which smali class loads "UE4", the Rockstar manifest's targetSdk. Peers have the APK under `apk` and `device/` (incl. `notno.dis`), so these can be answered locally.
Risk: the installed build looks like a cracked repack. If the genuine Play build has PairIP anti-tamper, a clean repack will crash or prompt until that's handled, and how to handle it legally is the user's call.
