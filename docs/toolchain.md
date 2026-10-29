# Host toolchain (macOS arm64)

| Tool | Version | Path |
|---|---|---|
| Android NDK | r27d (27.3.13750724) | ~/Library/Android/sdk/ndk/27.3.13750724 (copied from Google darwin .dmg) |
| NDK clang | aarch64-linux-android26-clang (any API 21–35 wrapper) | $ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/ (universal binary, runs native on arm64) |
| JDK | Temurin 21.0.13+9 (tarball, no sudo) | ~/tools/jdk-21.0.13+9/Contents/Home |
| Ghidra | 12.1.4 PUBLIC (20260921) | ~/tools/ghidra_12.1.4_PUBLIC |
| Ghidra natives | mac_arm_64 decompile + sleigh, built locally | ~/tools/ghidra_12.1.4_PUBLIC/Ghidra/Features/Decompiler/build/os/mac_arm_64/ |
| apktool | 3.0.3 (brew) | /opt/homebrew/bin/apktool |
| jadx | 1.5.6 (brew) | /opt/homebrew/bin/jadx |
| frida-tools | 14.11.0 (frida 17.24.3), pip --user | ~/Library/Python/3.12/bin (frida, frida-ps, frida-trace) |

## Env vars
```sh
export JAVA_HOME=~/tools/jdk-21.0.13+9/Contents/Home
export GHIDRA_INSTALL_DIR=~/tools/ghidra_12.1.4_PUBLIC
export ANDROID_NDK_HOME=~/Library/Android/sdk/ndk/27.3.13750724
export PATH=$JAVA_HOME/bin:$HOME/Library/Python/3.12/bin:$PATH
```

## Verification
- `aarch64-linux-android26-clang -shared -fPIC t.c -o libt.so` → `ELF 64-bit LSB shared object, ARM aarch64, version 1 (SYSV), dynamically linked`
- `support/analyzeHeadless` (JAVA_HOME=JDK21) prints "Headless Analyzer Usage".
- Ghidra release ships only linux_x86_64/win_x86_64 natives; mac_arm_64 built via `cd support/gradle && ./gradlew buildNatives` (BUILD SUCCESSFUL, Apple clang 17).

## Failures / notes
- `brew install --cask temurin@21` failed (needs sudo) → used Adoptium tarball instead.
- No Android cmdline-tools/sdkmanager installed (not needed; NDK installed directly). Default `java` on PATH is still JDK 20.
- Ghidra 12.2 exists; 12.1.4 chosen per REA requirement.
