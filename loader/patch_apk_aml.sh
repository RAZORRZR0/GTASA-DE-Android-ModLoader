#!/usr/bin/env bash
# patch_apk_aml.sh <in.apk> <libAML.so> <out.apk>
# Injects System.loadLibrary("AML") right after System.loadLibrary("UE4") in
# com.epicgames.ue4.GameActivity.<clinit>, adds lib/arm64-v8a/libAML.so, re-signs with a debug key.
# Only the one patched classesN.dex and the new .so are replaced in a copy of the input APK;
# every other entry (assets, other dex, libs, resources) is kept byte-identical.
set -euo pipefail

[ $# -eq 3 ] || { echo "usage: $0 <in.apk> <libAML.so> <out.apk>" >&2; exit 2; }
IN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
LIB=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
mkdir -p "$(dirname "$3")"
OUT=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")
[ -f "$IN" ] && [ -f "$LIB" ] || { echo "missing input" >&2; exit 2; }

HERE=$(cd "$(dirname "$0")" && pwd)
KS=${KEYSTORE:-$HERE/debug.keystore}
[ -d "$HOME/tools/jdk-21.0.13+9/Contents/Home" ] && export JAVA_HOME=${JAVA_HOME:-$HOME/tools/jdk-21.0.13+9/Contents/Home}
[ -n "${JAVA_HOME:-}" ] && export PATH="$JAVA_HOME/bin:$PATH"
SDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}
BT=${BUILD_TOOLS:-$SDK/build-tools/$(ls "$SDK/build-tools" | sort -t. -k1,1n -k2,2n -k3,3n | tail -n1)}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/aml_patch.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

echo "[1/6] apktool d (smali only, raw resources, no assets)"
apktool d -f -r --no-assets -o "$WORK/dec" "$IN" >/dev/null

SMALI=$(ls "$WORK"/dec/smali*/com/epicgames/ue4/GameActivity.smali)
[ "$(echo "$SMALI" | wc -l)" -eq 1 ] || { echo "GameActivity.smali not unique: $SMALI" >&2; exit 1; }
SMALIDIR=$(basename "$(dirname "$(dirname "$(dirname "$(dirname "$SMALI")")")")")   # smali or smali_classesN
DEX=classes${SMALIDIR#smali_classes}.dex; [ "$SMALIDIR" = smali ] && DEX=classes.dex

echo "[2/6] inject into $SMALIDIR/com/epicgames/ue4/GameActivity.smali ($DEX)"
python3 - "$SMALI" <<'PY'
import re, sys
path = sys.argv[1]
lines = open(path, encoding="utf-8").read().split("\n")
assert not any('"AML"' in l for l in lines), "already patched"
start = next(i for i, l in enumerate(lines) if l.startswith(".method static constructor <clinit>()V"))
end = next(i for i in range(start, len(lines)) if lines[i].startswith(".end method"))
# <clinit> is static with no params, so v<N> (N = old register count) is always a fresh, unused register.
loc = next(i for i in range(start, end) if re.match(r"\s*\.(locals|registers) \d+\s*$", lines[i]))
kind, n = re.match(r"\s*\.(locals|registers) (\d+)", lines[loc]).groups()
n = int(n)
assert n < 256, "const-string needs an 8-bit register"
# Straight-line scan: remember the last const-string per register, find loadLibrary(<reg holding "UE4">).
val, hits = {}, []
for i in range(loc, end):
    m = re.match(r'\s*const-string(?:/jumbo)? (v\d+), "(.*)"\s*$', lines[i])
    if m:
        val[m.group(1)] = m.group(2); continue
    m = re.match(r"\s*invoke-static(?:/range)? \{(v\d+)(?: \.\. \1)?\}, Ljava/lang/System;->loadLibrary\(Ljava/lang/String;\)V", lines[i])
    if m and val.get(m.group(1)) == "UE4":
        hits.append(i)
assert len(hits) == 1, f"expected one loadLibrary(\"UE4\") in <clinit>, found {len(hits)}"
r = f"v{n}"
lines[loc] = re.sub(r"\d+", str(n + 1), lines[loc], count=1)
lines[hits[0] + 1:hits[0] + 1] = ["", f'    const-string {r}, "AML"', "",
                                   f"    invoke-static/range {{{r} .. {r}}}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V"]
open(path, "w", encoding="utf-8").write("\n".join(lines))
print(f"  .{kind} {n} -> {n + 1}; inserted after line {hits[0] + 1}: loadLibrary(\"AML\") via {r}")
PY

echo "[3/6] apktool b ($SMALIDIR only)"
for d in "$WORK"/dec/smali*; do [ "$(basename "$d")" = "$SMALIDIR" ] || rm -rf "$d"; done
apktool b -o "$WORK/built.apk" "$WORK/dec" >/dev/null
mkdir -p "$WORK/stage/lib/arm64-v8a"
unzip -q -o "$WORK/built.apk" "$DEX" -d "$WORK/stage"
cp "$LIB" "$WORK/stage/lib/arm64-v8a/libAML.so"

echo "[4/6] merge into copy of input APK (drop old v1 signature files)"
cp "$IN" "$WORK/unsigned.apk"
OLDSIG=$(unzip -Z1 "$WORK/unsigned.apk" | grep -E '^META-INF/([^/]+\.(SF|RSA|DSA|EC)|MANIFEST\.MF)$' || true)
[ -z "$OLDSIG" ] || zip -q -d "$WORK/unsigned.apk" $OLDSIG
(cd "$WORK/stage" && zip -q -X -9 "$WORK/unsigned.apk" "$DEX" \
                 && zip -q -X -0 "$WORK/unsigned.apk" lib/arm64-v8a/libAML.so)  # stored: valid for extractNativeLibs true or false

echo "[5/6] zipalign -p 4"
"$BT/zipalign" -f -p 4 "$WORK/unsigned.apk" "$WORK/aligned.apk"

echo "[6/6] apksigner sign ($KS)"
[ -f "$KS" ] || keytool -genkeypair -noprompt -keystore "$KS" -storepass android -keypass android \
    -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --ks-key-alias androiddebugkey \
    --v4-signing-enabled false --out "$OUT" "$WORK/aligned.apk"
"$BT/apksigner" verify "$OUT"
echo "OK: $OUT"
