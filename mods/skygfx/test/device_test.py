#!/usr/bin/env python3
"""SkyGfx on-device end-to-end test.

Builds libskygfx.so, deploys it, launches the game, gets into gameplay on its own (intros, Rockstar sign-in,
the repack's "While in development" dialog, Resume), then drives each scenario by editing SkyGfx.ini live (core
reloads it on the game thread) and asserts on the Log=2 status lines. Screenshots go to --out. The user's ini is
restored at the end, pass or fail.

Needs: adb with root on the device (only to chown pushed files to the app uid), the NDK, a save game to resume.
Usage: device_test.py [--serial S] [--only time,coronas,...] [--no-build] [--keep-running]
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import time


import numpy as np
PKG = "com.rockstargames.gtasa.de"
EXT = f"/sdcard/Android/data/{PKG}"
EXT_ROOT = f"/data/media/0/Android/data/{PKG}"
HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(HERE)
NDK_BUILD = os.path.expanduser("~/Library/Android/sdk/ndk/27.3.13750724/ndk-build")

# Touch targets as fractions of the landscape screen, measured on 2712x1220.
RESUME = (0.207, 0.659)       # main menu "Resume"
SKIP_SIGN_IN = (0.707, 0.669)  # Rockstar sign-in (a Flutter screen: invisible to uiautomator, found by its activity)
STICK = (0.175, 0.849)        # move stick centre
STICK_UP = (0.175, 0.656)     # stick pushed fully forward

SCENARIOS = ["boot", "hud", "time", "coronas", "lights", "sun", "speedfx", "radiosity", "rain", "freecam", "noclip"]
# The minimap in the top-left corner of DE's HUD (it sits there in every gameplay view).
MINIMAP = (0.07, 0.22, 0.03, 0.32)


class Fail(Exception):
    pass


class Device:
    def __init__(self, serial):
        self.serial = serial
        self.w = self.h = 0

    def adb(self, *args, check=True, capture=True, timeout=60):
        r = subprocess.run(["adb", "-s", self.serial, *args], capture_output=capture, timeout=timeout)
        if check and r.returncode:
            raise Fail(f"adb {' '.join(args)}: {r.stderr.decode(errors='replace').strip()}")
        return r.stdout.decode(errors="replace") if capture else ""

    def sh(self, cmd, **kw):
        return self.adb("shell", cmd, **kw)

    def su(self, cmd, **kw):
        return self.sh(f"su -c '{cmd}'", **kw)

    def geometry(self):
        m = re.search(r"(\d+)x(\d+)", self.sh("wm size"))
        a, b = int(m.group(1)), int(m.group(2))
        self.w, self.h = max(a, b), min(a, b)  # the game runs landscape

    def px(self, frac):
        return int(frac[0] * self.w), int(frac[1] * self.h)

    def tap(self, frac):
        x, y = self.px(frac)
        self.sh(f"input tap {x} {y}")

    def hold_stick(self, ms, wait=True):
        """Holds the move stick fully forward for ms (motionevent: full deflection at once; a swipe would ramp it up
        from the centre over the whole duration). wait=False returns the running hold (screenshot during it)."""
        (x0, y0), (x1, y1) = self.px(STICK), self.px(STICK_UP)
        script = (f"input motionevent DOWN {x0} {y0}; input motionevent MOVE {x1} {y1}; sleep {ms / 1000:.2f}; "
                  f"input motionevent UP {x1} {y1}")
        p = subprocess.Popen(["adb", "-s", self.serial, "shell", script], stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        if wait:
            p.wait(timeout=ms / 1000 + 30)
        return p

    def ui_texts(self):
        """Native (Android view) texts on screen with their tap centres. The game's own UI is not visible here."""
        self.sh("rm -f /data/local/tmp/ui.xml; uiautomator dump /data/local/tmp/ui.xml", check=False)
        xml = self.sh("cat /data/local/tmp/ui.xml", check=False)
        out = {}
        for m in re.finditer(r'text="([^"]+)"[^>]*bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', xml):
            out[m.group(1)] = ((int(m.group(2)) + int(m.group(4))) // 2, (int(m.group(3)) + int(m.group(5))) // 2)
        return out

    def screenshot(self, path):
        png = subprocess.run(["adb", "-s", self.serial, "exec-out", "screencap", "-p"], capture_output=True,
                             timeout=60).stdout
        with open(path, "wb") as f:
            f.write(png)
        return load_image(path)

    def push(self, local, remote):
        self.adb("push", local, remote)


# ---------------------------------------------------------------- image metrics

def load_image(png_path):
    """PNG -> HxWx3 float array 0..1, through macOS sips (BMP is trivially read with numpy)."""
    bmp = png_path[:-4] + ".bmp"
    subprocess.run(["sips", "-s", "format", "bmp", png_path, "--out", bmp], check=True, capture_output=True)
    with open(bmp, "rb") as f:
        data = f.read()
    os.remove(bmp)
    off, = struct.unpack_from("<I", data, 10)
    w, h, _, bpp = struct.unpack_from("<iiHH", data, 18)
    if bpp not in (24, 32):
        raise Fail(f"unsupported BMP depth {bpp}")
    stride = (w * bpp // 8 + 3) & ~3
    rows = np.frombuffer(data, np.uint8, stride * abs(h), off).reshape(abs(h), stride)
    img = rows[:, :w * bpp // 8].reshape(abs(h), w, bpp // 8)[:, :, 2::-1]  # BGR(A) -> RGB
    if h > 0:
        img = img[::-1]  # bottom-up rows
    return img.astype(np.float32) / 255.0


def region(img, x0, x1, y0, y1):
    h, w = img.shape[:2]
    return img[int(y0 * h):int(y1 * h), int(x0 * w):int(x1 * w)]


def luminance(img):
    return float((img * np.array([0.2126, 0.7152, 0.0722], np.float32)).sum(axis=2).mean())


def sharpness(img):
    """Mean absolute horizontal + vertical luminance gradient: blur lowers it."""
    y = (img * np.array([0.2126, 0.7152, 0.0722], np.float32)).sum(axis=2)
    return float(np.abs(np.diff(y, axis=1)).mean() + np.abs(np.diff(y, axis=0)).mean())


def seam(img):
    """Step across the vertical screen centre relative to the image's own column-to-column change nearby: ~1 for a
    smooth image, well above for a hard seam (a torn frame: the effect on one half only)."""
    h, w = img.shape[:2]
    c, rows = w // 2, slice(int(h * 0.1), int(h * 0.9))
    at = np.abs(img[rows, c + 2] - img[rows, c - 2]).mean()
    near = np.abs(img[rows, c - 6] - img[rows, c - 10]).mean()
    return float(at / (near + 1e-4))


# The sky above the overpass in the Grove Street spawn view, clear of the HUD (minimap left, clock/money right).
SKY = (0.30, 0.75, 0.02, 0.12)
# Screen sides at mid height, where forward motion streaks most, clear of the touch buttons on the right.
SIDES = ((0.02, 0.28, 0.25, 0.70), (0.62, 0.88, 0.25, 0.70))


# ---------------------------------------------------------------- ini

def render_ini(overrides, base=None):
    """base (default: the repo SkyGfx.ini) with {(section, key): value} applied. Section-aware: Enabled exists in
    several sections."""
    if base is None:
        with open(os.path.join(MOD, "SkyGfx.ini")) as f:
            base = f.read()
    out, section, done = [], "", set()
    for line in base.splitlines():
        s = line.strip()
        if s.startswith("[") and "]" in s:
            section = s[1:s.index("]")]
        elif "=" in s and not s.startswith(";"):
            key = s.split("=", 1)[0].strip()
            if (section, key) in overrides:
                line = f"{key}={overrides[(section, key)]}"
                done.add((section, key))
        out.append(line)
    missing = set(overrides) - done
    if missing:
        raise Fail(f"ini keys not in SkyGfx.ini: {sorted(missing)}")
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------- the test run

class Run:
    def __init__(self, dev, out):
        self.dev, self.out = dev, out
        self.ini = {("General", "Log"): 2}
        self.results = []

    # -- config
    def set(self, **kv):
        """set(Debug__SetHour=23) -> [Debug] SetHour=23, pushed and stamped so the ini watch fires."""
        for k, v in kv.items():
            sec, key = k.split("__")
            self.ini[(sec, key)] = v
        local = os.path.join(self.out, "SkyGfx.ini")
        with open(local, "w") as f:
            f.write(render_ini(self.ini))
        self.dev.push(local, f"{EXT}/configs/SkyGfx.ini")
        self.dev.su(f"chown 10448:ext_data_rw {EXT_ROOT}/configs/SkyGfx.ini; touch {EXT_ROOT}/configs/SkyGfx.ini")

    # -- log
    def log(self):
        return self.dev.adb("logcat", "-d", "-v", "brief", "SkyGfx:I", "DEBUG:*", "libc:F", "*:S")

    def status(self, feature, since=0.0):
        """Latest status of a feature as {key: value} (k=v tokens), or None."""
        lines = [l for l in self.log().splitlines() if f": {feature}: " in l and "=" in l]
        if not lines:
            return None
        body = lines[-1].split(f": {feature}: ", 1)[1]
        return dict(t.split("=", 1) for t in body.split() if "=" in t)

    def wait(self, feature, pred, timeout, what):
        end, last = time.time() + timeout, None
        while time.time() < end:
            last = self.status(feature)
            try:
                if last and pred(last):
                    return last
            except (KeyError, ValueError):
                pass
            time.sleep(1.5)
        raise Fail(f"{what}: timed out after {timeout}s, last {feature} status: {last}")

    def fresh(self, feature, timeout=20):
        """Waits for a status line printed after now, so asserts never read a stale one."""
        self.dev.adb("logcat", "-c")
        return self.wait(feature, lambda s: True, timeout, f"{feature} status")

    def check_no_crash(self):
        log = self.log()
        for bad in ("Fatal signal", "self-check failed", "hook failed", "MISSING "):
            if bad in log:
                line = next(l for l in log.splitlines() if bad in l)
                raise Fail(f"log has '{bad}': {line.strip()}")
        if not self.dev.sh(f"pidof {PKG}", check=False).strip():
            raise Fail("game process is gone")

    # -- game
    def launch_to_gameplay(self, timeout=240):
        d = self.dev
        d.sh(f"am force-stop {PKG}")
        d.sh("input keyevent KEYCODE_WAKEUP")
        d.adb("logcat", "-c")
        d.sh(f"monkey -p {PKG} -c android.intent.category.LAUNCHER 1", check=False)
        start, last_tap = time.time(), 0.0
        while time.time() - start < timeout:
            time.sleep(3)
            if "FlutterScreenHost" in d.sh("dumpsys activity activities | grep topResumedActivity", check=False):
                d.tap(SKIP_SIGN_IN)  # never signs in
                print("  skipped Rockstar sign-in")
                continue
            texts = d.ui_texts()
            if "Normal Action" in texts:  # the repack's "While in development" dialog: the default response
                x, y = texts["Normal Action"]
                d.sh(f"input tap {x} {y}")
                print("  dismissed 'While in development'")
                continue
            s = self.status("tools")
            if s and s.get("player", "0x0") not in ("0x0", "(nil)"):
                print(f"  in gameplay after {time.time() - start:.0f}s")
                time.sleep(4)  # let streaming settle
                return
            if time.time() - last_tap > 8:  # intros, then the main menu's Resume; harmless while loading
                d.tap(RESUME)
                last_tap = time.time()
        raise Fail("did not reach gameplay")

    def shot(self, name):
        path = os.path.join(self.out, name + ".png")
        self.dev.screenshot(path)
        print(f"  screenshot {path}")

    # -- scenarios
    def sc_boot(self):
        self.check_no_crash()
        log = self.log()
        if "coronas: " not in log or "lights for" not in log:
            raise Fail("coronas did not load SALodLights.dat")
        for want in ("vk: loader hooked", "copyable", "vkfx: ready", "vkfx: first frame drawn", "fxstate: ready"):
            if want not in log:
                raise Fail(f"Vulkan post effects not running: no '{want}' in the log")
        s = self.fresh("coronas")
        if int(s["lamps"]) < 500:
            raise Fail(f"only {s['lamps']} lamps caught (expect thousands)")
        v = self.fresh("vkfx")
        print(f"  lamps={s['lamps']}, post effects drawn on {v['drawn']} frames at {v['size']}")

    def sc_hud(self):
        # The effects run between UE's scene pass and its HUD pass; UE's command-buffer state must come back intact
        # (a lost viewport drew the HUD upside down). Reference: the same view with SkyGfx off (no GPU work at all).
        self.set(General__Enabled=0)
        time.sleep(3)
        ref = self.dev.screenshot(os.path.join(self.out, "hud_off.png"))
        self.set(General__Enabled=1)
        time.sleep(3)
        on = self.dev.screenshot(os.path.join(self.out, "hud_on.png"))
        diff = float(np.abs(region(on, *MINIMAP) - region(ref, *MINIMAP)).mean())
        print(f"  minimap region difference effects on vs off: {diff:.3f}")
        if diff > 0.08:
            raise Fail(f"HUD disturbed by the post effects: minimap region differs by {diff:.3f}")

    def sky(self, name):
        img = self.dev.screenshot(os.path.join(self.out, name + ".png"))
        return luminance(region(img, *SKY))

    def sc_time(self):
        # Noon first, so the jump to night is a real one; each value differs from the last, so the one-shot fires.
        # One fixed weather (EXTRASUNNY_LA) for the whole run: a weather change mid-test would change the sky too.
        self.set(Debug__ForceWeather=0, Debug__SetHour=12, Debug__SetMinute=0, Debug__FreezeTime=1)
        self.wait("tools", lambda s: s["clock"] == "12:00", 20, "SetHour 12:00")
        time.sleep(2)
        noon = self.sky("time_noon")
        self.set(Debug__SetHour=23, Debug__SetMinute=30)
        self.wait("tools", lambda s: s["clock"] == "23:30", 20, "SetHour 23:30")
        time.sleep(2)  # the sky must already be night: DE's own slide takes far longer
        night = self.sky("time_night")
        print(f"  sky luminance noon {noon:.3f}, 2 s after the jump to 23:30 {night:.3f}")
        if night > 0.5 * noon:
            raise Fail(f"sky still bright after the jump to night: {night:.3f} vs noon {noon:.3f}")
        time.sleep(6)
        s = self.fresh("tools")
        if s["clock"] != "23:30":
            raise Fail(f"FreezeTime: clock moved to {s['clock']}")
        print("  clock frozen at 23:30")

    def sc_coronas(self):
        if self.ini.get(("Debug", "SetHour")) != 23:
            self.sc_time()
        s = self.wait("coronas", lambda s: int(s["shown"]) > 0, 30, "coronas shown at night")
        print(f"  shown={s['shown']} of {s['lamps']} farclip={s['farclip']} component={s['component']}")
        time.sleep(2)
        self.shot("night_coronas_on")
        self.set(Corona__Enabled=0)
        self.wait("coronas", lambda s: int(s["shown"]) == 0, 20, "coronas removed when disabled")
        time.sleep(2)
        self.shot("night_coronas_off")
        self.set(Corona__Enabled=1)
        self.wait("coronas", lambda s: int(s["shown"]) > 0, 20, "coronas back when re-enabled")

    def sc_lights(self):
        # Street light draw distance (DE's lamp update applies it to every lamp light).
        self.set(StreetLights__DrawDistanceDm=15)
        s = self.wait("lights", lambda s: s["draw"] == "15000" and s["maxdraw"] == "15000", 30,
                      "street light distance on a live lamp")
        print(f"  draw={s['draw']} globe={s['globe']} sample maxdraw={s['maxdraw']}")

    def sc_sun(self):
        # [ModernLook] SunStrength scales DE's base sun intensity: a stronger sun lights the midday frame more.
        self.set(Debug__SetHour=13, Debug__SetMinute=0, Debug__FreezeTime=1, ModernLook__SunStrength=1.0)
        self.wait("tools", lambda s: s["clock"] == "13:00", 20, "SetHour 13:00")
        time.sleep(3)
        base = self.frames("sun_1")
        self.set(Debug__SetHour=13, Debug__SetMinute=0, Debug__FreezeTime=1, ModernLook__SunStrength=3.0)
        time.sleep(3)
        strong = self.frames("sun_3")
        self.set(ModernLook__SunStrength=1.0)
        l1 = sum(luminance(i) for i in base) / len(base)
        l3 = sum(luminance(i) for i in strong) / len(strong)
        print(f"  frame luminance SunStrength 1: {l1:.4f}, 3: {l3:.4f}")
        if l3 < l1 + 0.01:
            raise Fail(f"SunStrength 3 does not brighten the sunlit frame: {l3:.4f} vs {l1:.4f}")

    def frames(self, name, n=2):
        """n screenshots of the same still view (time frozen, no input)."""
        return [self.dev.screenshot(os.path.join(self.out, f"{name}_{i}.png")) for i in range(n)]

    def sc_speedfx(self):
        # PC SpeedFX draws the frame `passes` times with a per-pass zoom: the screen edges smear even with a still
        # camera (TestMode = the full row 6, as the original's m_bSpeedFXTestMode). Daylight, so a broken frame shows.
        self.set(Debug__SetHour=14, Debug__SetMinute=20, Debug__FreezeTime=1)
        self.wait("tools", lambda s: s["clock"] == "14:20", 20, "SetHour 14:20")
        time.sleep(2)
        plain = self.frames("speedfx_off", 3)
        self.set(SpeedFX__TestMode=1)
        self.wait("fx", lambda s: s["speedfx"] == "6", 20, "SpeedFX TestMode publishes row 6")
        time.sleep(1)
        on = self.frames("speedfx_on", 3)
        v = self.fresh("vkfx")
        self.set(SpeedFX__TestMode=0)
        self.wait("fx", lambda s: s["speedfx"] == "-1", 20, "SpeedFX off again")
        a = sum(sharpness(region(i, *r)) for i in plain for r in SIDES)
        b = sum(sharpness(region(i, *r)) for i in on for r in SIDES)
        seams = [round(seam(i), 2) for i in on]
        base = max(seam(i) for i in plain)  # the view's own edge at the centre column (CJ, a lamp post)
        print(f"  screen-edge sharpness: SpeedFX off {a:.4f}, TestMode {b:.4f} ({b / a:.0%}); centre seam {seams} "
              f"(off {base:.2f}); presents {v['presents']} hits {v['hits']} unbegun {v['unbegun']}")
        if b > 0.85 * a:
            raise Fail(f"no SpeedFX zoom blur: edge sharpness {b:.4f} vs {a:.4f} without it")
        if max(seams) > max(1.8, 1.5 * base):
            raise Fail(f"torn frame: hard seam down the screen centre (ratio {max(seams)}, {base:.2f} without SpeedFX)")

    def sc_radiosity(self):
        # The PS2 glow adds thresholded highlights back at alpha Intensity/255: the frame gets brighter.
        self.set(Debug__SetHour=13, Debug__SetMinute=0)
        self.wait("tools", lambda s: s["clock"] == "13:00", 20, "SetHour 13:00")
        time.sleep(2)
        on = self.frames("radiosity_on")
        self.set(Radiosity__Radiosity=0)
        time.sleep(3)
        off = self.frames("radiosity_off")
        self.set(Radiosity__Radiosity=1)
        lon = sum(luminance(i) for i in on) / len(on)
        loff = sum(luminance(i) for i in off) / len(off)
        print(f"  frame luminance radiosity on {lon:.4f}, off {loff:.4f}")
        if lon < loff + 0.005:
            raise Fail(f"radiosity adds no glow: {lon:.4f} vs {loff:.4f}")

    def sc_rain(self):
        # RAINY_COUNTRYSIDE: CWeather::Rain > 0 outside -> PS2 grain ramps up and water drops land on the lens.
        self.set(Debug__ForceWeather=16)
        s = self.wait("fx", lambda s: int(s["grain"]) > 0 and s["noRain"] == "0", 60, "rain grain mask")
        v = self.wait("vkfx", lambda v: int(v["drops"]) > 0, 30, "water drops on the lens")
        self.shot("rain")
        print(f"  grain mask {s['grain']}, rain {s['rain']}, {v['drops']} drops")
        self.set(Debug__ForceWeather=0)

    def positions(self):
        s = self.fresh("tools")
        p = [float(v) for v in s["ppos"].split(",")]
        c = [float(v) for v in s["cpos"].split(",")]
        return p, c

    def sc_freecam(self):
        self.set(Debug__Freecam=1)
        self.wait("tools", lambda s: s["freecam"] == "1", 20, "freecam on")
        p0, c0 = self.positions()
        before = self.dev.screenshot(os.path.join(self.out, "freecam_before.png"))
        self.dev.hold_stick(3000)
        p1, c1 = self.positions()
        after = self.dev.screenshot(os.path.join(self.out, "freecam_after.png"))
        self.set(Debug__Freecam=0)
        self.wait("tools", lambda s: s["freecam"] == "0", 20, "freecam off")
        dc, dp = dist(c0, c1), dist(p0, p1)
        # What the player sees, not just GTA's camera matrix: the centre of the frame (CJ in the chase view) must change.
        view = float(np.abs(region(after, 0.3, 0.7, 0.3, 0.9) - region(before, 0.3, 0.7, 0.3, 0.9)).mean())
        print(f"  camera matrix moved {dc:.1f} m, player {dp:.2f} m, frame centre changed {view:.3f}")
        if dc < 5.0:
            raise Fail(f"freecam: camera moved {dc:.1f} m on a 3 s stick hold")
        if view < 0.05:
            raise Fail(f"freecam: GTA's camera moved {dc:.1f} m but the rendered view did not (centre diff {view:.3f})")
        if dp > 0.5:
            raise Fail(f"freecam: player moved {dp:.1f} m (stick must not walk)")

    def sc_noclip(self):
        self.set(Debug__Noclip=1)
        self.wait("tools", lambda s: s["noclip"] == "1", 20, "noclip on")
        p0, _ = self.positions()
        self.dev.hold_stick(3000)
        p1, _ = self.positions()
        self.set(Debug__Noclip=0)
        self.wait("tools", lambda s: s["noclip"] == "0", 20, "noclip off")
        time.sleep(3)
        p2, _ = self.positions()
        self.check_no_crash()
        if dist(p0, p1) < 3.0:
            raise Fail(f"noclip: player moved {dist(p0, p1):.1f} m on a 3 s stick hold")
        if p2[2] < p1[2] - 5.0:
            raise Fail(f"noclip off: player fell from z={p1[2]:.1f} to {p2[2]:.1f}")
        print(f"  moved {dist(p0, p1):.1f} m, z after release {p2[2]:.1f}")

    def run(self, names):
        for name in names:
            print(f"[{name}]")
            try:
                getattr(self, "sc_" + name)()
                self.check_no_crash()
                self.results.append((name, "PASS", ""))
            except Fail as e:
                self.results.append((name, "FAIL", str(e)))
                print(f"  FAIL: {e}")
                if not self.dev.sh(f"pidof {PKG}", check=False).strip():
                    break


def dist(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", default=os.environ.get("ANDROID_SERIAL", "192.168.1.40:39309"))
    ap.add_argument("--only", default=",".join(SCENARIOS))
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--no-launch", action="store_true", help="the game is already in gameplay with this build")
    ap.add_argument("--out", default="/tmp/skygfx_test")
    args = ap.parse_args()
    names = [n for n in args.only.split(",") if n]
    bad = set(names) - set(SCENARIOS)
    if bad:
        sys.exit(f"unknown scenarios {sorted(bad)}; known {SCENARIOS}")
    os.makedirs(args.out, exist_ok=True)

    if not args.no_build:
        subprocess.run([NDK_BUILD, "NDK_PROJECT_PATH=.", "APP_BUILD_SCRIPT=Android.mk", "APP_PLATFORM=android-23",
                        "APP_ABI=arm64-v8a", "APP_STL=c++_static"], cwd=MOD, check=True, capture_output=True)
    dev = Device(args.serial)
    dev.geometry()
    # The user's ini is backed up once and kept until a restore succeeds: after an aborted run the device holds a test
    # ini, and a fresh pull would back that up instead.
    user_ini = os.path.join(args.out, "user_SkyGfx.ini")
    if not os.path.exists(user_ini):
        dev.adb("pull", f"{EXT}/configs/SkyGfx.ini", user_ini)
    run = Run(dev, args.out)
    dev.sh("svc power stayon true")  # a screen timeout mid-run gets the game killed by the lock-screen cleaner
    try:
        if not args.no_launch:
            dev.push(os.path.join(MOD, "libs/arm64-v8a/libskygfx.so"), f"{EXT}/mods/libskygfx.so")
            for data in ("SALodLights.dat", "timecyc_ps2.dat"):
                dev.push(os.path.join(MOD, data), f"{EXT}/configs/{data}")
            dev.su(f"chown -R 10448:ext_data_rw {EXT_ROOT}/mods {EXT_ROOT}/configs")
            run.set()  # repo ini + Log=2: a known starting state
            print("[launch]")
            run.launch_to_gameplay()
        else:
            run.set()
        run.run(names)
    except Fail as e:
        run.results.append(("launch", "FAIL", str(e)))
    finally:
        # The user's ini, with its one-shot keys cleared: restoring must not jump their clock or move the player.
        with open(user_ini) as f:
            restored = render_ini({("Debug", "SetHour"): -1, ("Debug", "SetMinute"): -1,
                                   ("Debug", "MovePlayerToCamera"): 0}, f.read())
        with open(user_ini, "w") as f:
            f.write(restored)
        dev.push(user_ini, f"{EXT}/configs/SkyGfx.ini")
        dev.su(f"chown 10448:ext_data_rw {EXT_ROOT}/configs/SkyGfx.ini; touch {EXT_ROOT}/configs/SkyGfx.ini")
        os.remove(user_ini)
        with open(os.path.join(args.out, "logcat.txt"), "w") as f:
            f.write(run.log())
        dev.sh(f"am force-stop {PKG}", check=False)  # testing over: the game does not stay open
        dev.sh("svc power stayon false", check=False)
    print("\n" + "\n".join(f"{r:4} {n:8} {m}" for n, r, m in run.results))
    sys.exit(0 if run.results and all(r == "PASS" for _, r, _ in run.results) else 1)


if __name__ == "__main__":
    main()
