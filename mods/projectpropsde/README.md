# ProjectPropsDE (Android)

Port of [ProjectPropsDE](https://github.com/RAZORRZR0/ProjectPropsDE) (Project Props 4 for GTA SA: The Definitive
Edition) to the Android game (`com.rockstargames.gtasa.de` 1.87.0, AML). One mod does what the PC version needs
three pieces for:

| PC | Android (`libprojectpropsde.so`) |
|---|---|
| `ProjectPropsDE_P.pak` (DE's IPLs with the props appended) | `ProjectPropsDE.dat`: the added lines per DE IPL, inserted into the in-memory file when the game opens it (`CFileMgr::OpenFile` reads the whole file; the copy is swapped back before `CloseFile` frees it) |
| `IplActorsDE.asi` | `CFileLoader::LoadObjectInstance` hook: instances without an explicit 12th-column index (`-1`) get entity `+0x34` bit 3 cleared, so they get a runtime UE actor and are drawn |
| `DE_LimitAdjuster.asi` + ini | `[Pools]` in `ProjectPropsDE.ini`: `CPools::Initialise` hook (bigger arrays for the empty pools) and `CPlaceable::InitMatrixArray` (Android inlines `CMatrixLinkList::Init(1800)` there) |

Android DE has symbols, so every address comes from its name; the Android code matches the PC notes (LoadScene's
4096-instance array, index packed into the model id's high word, `+0x34` bit 3, pool sizes and layout).

## Build the data

```
python3 build_props.py <Project-Props clone> <pakchunk0-Android_ASTC.pak>
```

The pak is inside the game's `base.apk`: `assets/main.obb.png` is a zip holding
`Gameface/Content/Paks/pakchunk0-Android_ASTC.pak` (not encrypted). Needs
[repak](https://github.com/trumank/repak). Writes `ProjectPropsDE.dat` and `manifest.txt` (both git-ignored: the
placements belong to the Project Props authors). Same selection as the PC `build.py`: 16,161 placements in 20 DE IPLs
(PC: 16,166; five placements use model ids Android's IDEs don't have).

## Install

- `libs/arm64-v8a/libprojectpropsde.so` → `Android/data/com.rockstargames.gtasa.de/mods/`
- `ProjectPropsDE.ini` and `ProjectPropsDE.dat` → `Android/data/com.rockstargames.gtasa.de/configs/`

Logcat tag `ProjectPropsDE`: placements loaded, one line per DE IPL (`stock + props`), pool changes, the
runtime-actor count. Settings apply at the next start (the map loads once).

## Credits

Project Props and all its placements: [user-grinch/Project-Props](https://github.com/user-grinch/Project-Props) and
its contributors (see the PC repo's credits). Please ask the Project Props team before redistributing a built
`ProjectPropsDE.dat`.
