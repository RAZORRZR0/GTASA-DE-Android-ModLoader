"""Builds ProjectPropsDE.dat: Project Props 4 placements for GTA SA: The Definitive Edition on Android.

Same selection as the PC ProjectPropsDE build.py (github.com/RAZORRZR0/ProjectPropsDE), but instead of a pak with
edited IPLs it writes only the added lines, grouped by the DE IPL they go into; libprojectpropsde.so inserts them at
the end of that IPL's inst section when the game opens it.
Why appended to DE's own IPLs: a new IPL file crashes DE (its baked collision cache is indexed per IPL file).
The 12th column is -1: LoadScene gives them the next free slots, and the mod spawns runtime actors for them.
Only placements of models DE already has are kept (DE cannot load new DFF/TXD/COL).

Usage: python3 build_props.py <Project-Props clone> <DE main pak>
  The main pak is assets/main.obb.png inside the game's base.apk (an unencrypted UE pak).
Needs repak (https://github.com/trumank/repak) on PATH or in ~/.cargo/bin.
Writes ProjectPropsDE.dat and manifest.txt next to this script (same inputs -> same output).
"""
import os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPAK = shutil.which("repak") or os.path.expanduser("~/.cargo/bin/repak")
ROOT = "Gameface/Content/OriginalData/GTASA/data"
MODS = os.path.join("modloader", "Project Props 4", "Project Props 4")
LOADERS = ["Stock Props", "Community Maps"]  # the mod's default set, in its loader.txt order
NUM = re.compile(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")
MAX_INST = 4096  # CFileLoader::LoadScene keeps one IPL's instances in a 4096-entry stack array
# Models DE draws wrong when placed at runtime. 646 veg_palmkb14: no DE asset of its own and never placed by DE;
# it shows a big bush hovering ~1.4 m up (classic pivot is the pot's centre). Add ids here as they are reported.
SKIP = {646}


def ide_ids(folder):
    ids = set()
    for d, _, fs in os.walk(folder):
        for f in fs:
            if not f.lower().endswith(".ide"):
                continue
            sec = None
            for line in open(os.path.join(d, f), errors="replace"):
                t = [x.strip() for x in line.split("#")[0].split(",")]
                if len(t) == 1:
                    sec = None if t[0].lower() == "end" else t[0].lower()
                elif sec in ("objs", "tobj", "anim") and t[0].isdigit():
                    ids.add(int(t[0]))
    return ids


def inst_lines(path, stock):
    """The inst entries of a text IPL as 11 columns; LOD is dropped (-1): indices change once merged into a DE IPL."""
    out, dropped, sec = [], 0, None
    for line in open(path, errors="replace"):
        t = line.split("#")[0].replace(",", " ").split()
        if not t:
            continue
        if sec is None:
            sec = t[0].lower()
        elif t[0].lower() == "end":
            sec = None
        elif sec == "inst":
            nums = NUM.findall(" ".join(t[2:]))  # interior, x y z, qx qy qz qw, lod ("-0.92-1" typos split here)
            if not t[0].isdigit() or int(t[0]) not in stock or int(t[0]) in SKIP or len(nums) < 8:
                dropped += 1
                continue
            out.append(f"{t[0]}, {t[1]}, {int(float(nums[0]))}, {', '.join(nums[1:8])}, -1")
    return out, dropped


def main(pp, pak):
    with tempfile.TemporaryDirectory() as stock_dir:
        subprocess.run([REPAK, "unpack", "-q", "-f", "-o", stock_dir, "-i", ROOT, pak], check=True, capture_output=True)
        stock = ide_ids(stock_dir)

        # hosts: exterior map IPLs that gta.dat loads and that already have text instances, so the per-file
        # entity arrays (and their numbering the cache relies on) stay as they are
        data = os.path.join(stock_dir, *ROOT.split("/"))
        listed = {l.split(None, 1)[1].strip().lower().replace("\\", "/")[5:] for l in open(os.path.join(data, "gta.dat"))
                  if l.upper().startswith("IPL ")}
        hosts = {}  # path under data/ -> [next free instance index, added lines]
        for d, _, fs in os.walk(os.path.join(data, "maps")):
            for f in sorted(fs):
                rel = os.path.relpath(os.path.join(d, f), data).replace("\\", "/")
                if rel.lower() in listed and re.fullmatch(r"(?i)maps/(LA|SF|vegas|country)/\w+\.ipl", rel):
                    text = open(os.path.join(d, f), "rb").read().decode("latin-1").splitlines()
                    rows, sec = [], None
                    for t in (l.split("#")[0].replace(",", " ").split() for l in text):
                        if len(t) == 1:
                            sec = None if t[0].lower() == "end" else t[0].lower()
                        elif sec == "inst" and t:
                            rows.append(t)
                    # the mod tells its lines apart by the missing explicit index: every stock line must have one
                    assert all(len(t) == 12 and t[11].isdigit() for t in rows), rel
                    idx = [int(t[11]) for t in rows]
                    if idx:
                        hosts[rel] = [max(idx) + 1, []]

    manifest = []
    for group in LOADERS:
        folder = os.path.join(pp, MODS, group)
        for l in open(os.path.join(folder, "loader.txt"), errors="replace"):
            if not l.upper().startswith("IPL "):
                continue
            name = l.split(None, 1)[1].strip().split("/")[-1]
            insts, dropped = inst_lines(os.path.join(folder, "maps", name), stock)
            host = min(sorted(hosts), key=lambda h: hosts[h][0] + len(hosts[h][1]))
            h = hosts[host]
            h[1] += [f"{s}, -1" for s in insts]
            assert h[0] + len(h[1]) <= MAX_INST, (host, name)
            manifest.append(f"{len(insts):5d} placed  {dropped:4d} dropped  {group}/{name} -> {host}")

    out = ["; ProjectPropsDE placements, generated by build_props.py from Project Props 4 (github.com/user-grinch/Project-Props).",
           "; 'host <IPL>' starts the lines added to the end of that DE IPL's inst section."]
    for rel, (_, added) in sorted(hosts.items()):
        if added:
            out += [f"host {rel}"] + added
    open(os.path.join(HERE, "ProjectPropsDE.dat"), "w", newline="\n").write("\n".join(out) + "\n")
    manifest.append(f"total {sum(int(m.split()[0]) for m in manifest)} placements in {sum(1 for h in hosts.values() if h[1])} DE IPLs")
    open(os.path.join(HERE, "manifest.txt"), "w").write("\n".join(manifest) + "\n")
    print(manifest[-1])


if __name__ == "__main__":
    main(*sys.argv[1:3])
