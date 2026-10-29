# GTA_Prop_Fix v0.3.2 — porting inventory (PC x64 → Android arm64)

Source: https://github.com/RAZORRZR0/GTA_Prop_Fix cloned to GTA_Prop_Fix. All line refs are `src/dllmain.cpp` unless noted.
RVAs come from `test/offline_test.cpp:936-947` (TestBinary against the real SanAndreas.exe, image base 0x140000000).

## 1. Byte signatures

Scanner: `FindUnique` (L206) — unique match in `.text` only (ambiguous ⇒ fail). `RipTarget(insn, dispOff, len)` (L233) = `insn+len+*(int32*)(insn+dispOff)`.
Table in `Install()` L2140-2155 (14 entries) + 1 optional sig at L2174 = **15 signatures**.

| # | Name / out var | Pattern (`??` = wildcard; all other bytes masked-in) | Resolve | Locates | RVA (test) | Anchors for arm64 re-find |
|---|---|---|---|---|---|---|
| 1 | `ProcessEvent` → `pe` | `40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC F0 00 00 00 48 8D 6C 24 30` | direct (fn start) | `UObject::ProcessEvent(UObject*, UFunction*, void*)` — **hooked** | 0x1C7B6B0 [INFERENCE: order of `hooked[]` = MH_CreateHook order] | UE4.26 source; also UObject vtable slot (ProcessEvent is virtual, index ~0x44/68 in 4.26). Strings: `"Script call stack"`, `"ProcessEvent"` asserts; easiest via Dumper-7/UE4SS AOB for Android or vtable of any UObject. |
| 2 | `UClass::FindFunctionByName` → `ffn` | `44 89 44 24 18 53 55 57 41 55 48 83 EC 48 8B 81 38 01 00 00 45 33 ED 48 8B DA` | direct | `UClass::FindFunctionByName(UClass*, FName, EIncludeSuperFlag)` (reads `cls+0x138` = FuncMap) | 0x1B18550 | Called by `UObject::FindFunction`/`FindFunctionChecked` (string `"Failed to find function %s in %s"`). Offset 0x138 in UClass is engine layout. |
| 3 | `CObject::SetIsStatic` → `sis` | `40 53 48 83 EC 20 83 61 30 FB 48 8B D9 0F B6 C2 C1 E0 02 09 41 30 84 D2` | direct | `CObject::SetIsStatic(CObject*, bool)` — **hooked**; also CObject vtable +0x28 | 0x12139F0 [INFERENCE] | Body: `flags@+0x30 &= ~4; flags |= b<<2` then fires BP event `Dislodged`. Find via xref to FName "Dislodged" / ProcessEvent call, or vtable slot 5 of CObject vtable. |
| 4 | `CEntity::SyncActor` → `sync` | `48 89 5C 24 10 57 48 83 EC 70 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 60 48 8B 79 20 48 8B D9` | direct | DE routine copying RW matrix → linked UE actor (`entity+0x20` = actor) | 0x112E610 | Has stack cookie; reads `+0x20` actor and `+0x18` matrix, converts m→cm & flips Y (×100, ×-100). Calls `K2_SetActorLocationAndRotation`-like native. |
| 5 | `CPhysical::ProcessCollision` → `pc` | `40 55 57 41 54 48 8D AC 24 50 FF FF FF 48 81 EC B0 01 00 00 8B 41 78 45 33 E4` | direct | `CPhysical::ProcessCollision(CPhysical*)` — **hooked** (vtable fn) | 0x115E370 [INFERENCE] | Reads physical flags `+0x78` first; gta-reversed Physical.cpp; CPhysical vtable. |
| 6 | `CPhysical::ProcessShift` → `ps` | `40 55 53 57 48 8D 6C 24 90 48 81 EC 70 01 00 00 48 8B 01 48 8D 54 24 38` | direct | `CPhysical::ProcessShift(CPhysical*)` — **hooked** | 0x115F150 [INFERENCE] | Starts with vcall (`mov rax,[rcx]`) for GetBoundRect; neighbour of ProcessCollision in CPhysical vtable. |
| 7 | `CGlass::WindowRespondsToCollision` → `glass` | `4C 8B DC 55 53 57 41 57 49 8D AB 08 FF FF FF 48 81 EC D8 01 00 00 45 0F 29 8B 78 FF FF FF` | direct | `CGlass::WindowRespondsToCollision(CEntity*, float dmg, CVector* speed, CVector* pos, …)` — **hooked** (plugin types it 4 args) | 0x1274040 [INFERENCE] | Calls BP `SetupBroken` via ProcessEvent; calls ground probe (#8). FName "SetupBroken" xref. |
| 8 | `BreakObject ground probe` → `probe` | `4C 8B DC 49 89 5B 20 55 56 57 48 81 EC 10 01 00 00 41 0F 29 73 D8 41 0F 29 7B C8` | direct | DE helper = BreakObject_c::Init vertical ground probe `(entity, CVector* outPos, CVector* outNormal)` (called, not hooked) | 0x120F290 | Callee of #7 and of ObjectDamage break path; does `CWorld::ProcessVerticalLine`. Derefs `entity+0x18` without null check. |
| 9 | `CObject::ObjectDamage` → `damage` | `4C 8B DC 55 56 57 41 54 41 55 49 8D AB C8 FE FF FF 48 81 EC 10 02 00 00` | direct | `CObject::ObjectDamage(CObject*, float, CVector*, CVector*, CEntity*, eWeaponType)` — **hooked** | 0x12142F0 [INFERENCE] | Big switch on `m_nColDamageEffect (+0x1C0)`; case 0x14 "smash completely"; string `"Unimplemented BREAKABLES"` nearby per header comment L28. |
| 10 | `CPhysical::AddToMovingList` → `moving` | `40 53 48 83 EC 20 48 83 B9 F0 00 00 00 00 48 8B D9 75 ?? F7 41 30 00 00 04 00` | direct | `CPhysical::AddToMovingList(CPhysical*)` (called) | 0x115C0F0 | `cmp [rcx+0xF0],0` (m_pMovingList), tests flag 0x40000 at +0x30. Small fn; arm64: `ldr x8,[x0,#0xF0]; cbnz`. |
| 11 | `CModelInfo::GetColModel` → `colModel` | `48 83 EC 28 48 63 C1 48 8D 0D ?? ?? ?? ?? 48 8B 0C C1 48 85 C9 75 28 65 48 8B 04 25 58 00 00 00` | direct; **plus** `g_modelInfos = RipTarget(colModel+7, 3, 7)` (the `lea rcx,[rip+disp]` at +7) | static `GetColModel(int modelIndex)` → `CColModel*`; second resolve = `CModelInfo::ms_modelInfoPtrs` global | fn 0x120BC80; global 0x5241FE0 | `gs:[0x58]` = TLS (null-case logging path). arm64: `adrp/add` of ms_modelInfoPtrs; global also referenced by every model-info lookup. |
| 12 | `CEntity::GetColModel` → `entColModel` | `40 53 48 83 EC 30 0F B6 41 6A 48 8B D9 24 07 3C 02 75 20 48 0F BE 81 F7 06 00 00` | direct | `CEntity::GetColModel(CEntity*)` — **hooked** (type byte +0x6A &7 ==2 vehicle → special col at +0x6F7) | not checked by test | Reads `+0x6A`, `+0x6F7` (CVehicle m_nSpecialColModel). |
| 13 | `GNames ref` → `gnamesRef` | `48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01 8B 80 70 07 01 00` | rip-relative: `RipTarget(ref,3,7)` (lea rcx) | `FNamePool` (GNames) global | 0x570CDC0 | Standard UE4.26 FNamePool init (`bNamePoolInitialized=1`). On Android: search "None"/"ByteProperty" name entries in pool, or FName::ToString xrefs; UE4SS/Dumper-7 Android patterns. |
| 14 | `GObjects ref` → `gobjRef` | `48 8D 05 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? E8 03 00 00 48 8D 0D ?? ?? ?? ??` | rip-relative: `RipTarget(ref,3,7)` (lea rax) | GUObjectArray's `FChunkedFixedUObjectArray` (plugin reads `Objects**` at +0, NumElements at +0x14) | 0x5086380 | Immediate `0x3E8` (1000) stored next to it; strings `"gc.MaxObjectsNotConsideredByGC"` / `"MaxObjectsInGame"` near GUObjectArray init [INFERENCE]. |
| 15 | `CBaseModelInfo::GetColModel` → `miColModel` (optional, L2174) | `48 8B 41 30 C3 CC CC CC CC` | direct | `CBaseModelInfo::GetColModel()` (vtable +0x28 of model info, shared impl) — **hooked** if found | 0xD2BE90 (header comment 0x140D2BE90) | Trivial `return this->m_pColModel (+0x30)`. arm64 form `ldr x0,[x0,#0x30]; ret` is NOT unique — locate via model-info vtable slot 5 instead. |

Count check: `grep -cE '^\s+\{ "[^"]+", "[0-9A-F?]' src/dllmain.cpp` = 14 table entries + 1 inline `FindUnique("…")` (L2174) = **15 signature definitions; 15 rows above.**
Note: all 15 patterns are x64 machine code — **none can be reused on arm64**; every one must be re-derived from the Android `libUE4.so` (anchors above).

## 2. Hooks (MinHook, L2178-2185)

| Target | Detour | Calls original? | Signature / ABI (x64 MS) |
|---|---|---|---|
| UObject::ProcessEvent | Per call: on game thread drives piece timers + original-piece simulation (`ProcessPieceTimers`, `TickOriginalPieces`). Then by `func` FName: `Dislodged` → swallowed when `t_suppressDislodgedFor==obj`; `SetupBroken` → patch floorTransform (glass), call orig, then `SetupGroundPlane` + `InitOriginalPieces`/`SchedulePieces`; `RemoveFloor` → swallow/keep/hide per Lifetime; `EntityLinked` → clear tracking; `SetLights(true)` → forced false for fallen lights. | Yes, except swallowed Dislodged/RemoveFloor | `void(UObject*, UFunction*, void* parms)` |
| CObject::SetIsStatic | `true`: tracked prop → keep pushable (don't call orig) or untrack+orig. `false`: if dynamic IPL prop & RW col covers it → orig with Dislodged suppressed, set actor bEntityUpdatePosition, record, probe ground, clear lamp-post flag, SetMobility Movable, sync. Else street light → `ForceDEBreak`, else orig. | Mostly yes; skipped for keep-pushable and forced-break | `void(CObject*, bool)` |
| CPhysical::ProcessCollision | Sets TLS `t_simEntity`, orig, then `SyncDislodged`, timed removal (`RemoveKnockedProp`). | Yes | `uintptr_t(CPhysical*)` (plugin assumes return in rax) |
| CPhysical::ProcessShift | Sets `t_simEntity`, orig, `SyncDislodged`. | Yes | `uintptr_t(CPhysical*)` |
| CGlass::WindowRespondsToCollision | Street light RW can simulate → `UprootEntity` and return 0 (no orig). Else ground probe stashed in TLS, orig. | Conditionally | `uintptr_t(CEntity*, float, float* speed, float* pos)` — float in xmm1, ptrs in r8/r9 (MS ABI) |
| CObject::ObjectDamage | Records hit (TLS). Breakable street light → temporarily set colDamageEffect=0, orig, restore, maybe `UprootEntity`. Else orig with `t_breakEntity`. | Yes | `void(CObject*, float, CVector*, CVector*, CEntity*, int)` |
| CEntity::GetColModel | If tracked object with augmented col → return refreshed `AugCol` copy. | Yes (first) | `const CColModel*(CEntity*)` |
| CBaseModelInfo::GetColModel (optional) | If `t_simEntity` tracked & `ms_modelInfoPtrs[model]==this` → augmented col. | Yes (first) | `const CColModel*(CBaseModelInfo*)` |

Calling-convention note: on arm64 AAPCS64 all of these map naturally (x0..x5 ints/ptrs, s0 for float). The MS-ABI quirk to watch: `float` 2nd arg goes to xmm1 on x64 but **s0** on arm64 — C prototypes handle it automatically. Return `uintptr_t` vs real `void`/`bool` must be checked per function in the arm64 disassembly.

Direct (non-hooked) native calls: `FindFunctionByName`, `SyncActor`, `GroundProbe`, `AddToMovingList`, `CModelInfo::GetColModel`, plus vtable calls below.

## 3. Hardcoded offsets

Legend: **UE** = UE4.26 engine layout (generally identical on arm64 clang; both LP64, 8-byte pointers — verify with Android Dumper-7 SDK); **GAME** = DE-specific UE class (Gameface BP/native, likely same but must verify); **RW** = GTA/RenderWare struct compiled by MSVC (field order same, but padding/packing may differ under clang, esp. the unaligned 0x1DC); **ABI** = MSVC vtable index (clang Itanium ABI has different slot layout: no scalar-deleting dtor, 2 dtor entries).

| Struct | Offset | Meaning | Class |
|---|---|---|---|
| UObject | 0x0C | InternalIndex | UE |
| UObject | 0x10 | ClassPrivate | UE |
| UObject | 0x18 | NamePrivate (FName.ComparisonIndex) | UE |
| UStruct | 0x40 | SuperStruct | UE |
| UClass | 0x138 | FuncMap (inside sig #2 bytes) | UE |
| AActor | 0x130 | RootComponent | UE |
| UStaticMeshComponent | 0x480 | StaticMesh | UE (DE build) |
| UPhysicsSettings | 0x12C / 0x130 / 0x134 | bSubstepping (byte) / MaxSubstepDeltaTime (float) / MaxSubsteps (int32) | UE |
| ADynamicIPLMapActor (DE) | 0x2B0 bit 0x04 | bEntityUpdatePosition | GAME |
| FNamePool | 0x08 CurrentBlock, 0x0C CurrentByteCursor, 0x10 Blocks[0x2000] | name pool, 0x20000-byte blocks, entry hdr `len=hdr>>6, wide=hdr&1`, stride 2 | UE |
| FChunkedFixedUObjectArray (`g_objects`) | 0x00 Objects** (chunks of 65536), 0x14 NumElements | object array | UE |
| FUObjectItem | 0x00 Object, 0x08 Flags (bit29 PendingKill, bit28 Unreachable), 0x10 SerialNumber, size 0x18 | | UE |
| UFunction parm blocks | GetActorBounds +0x18 BoxExtent.Z; GetSocketLocation out +0x08; GetSocketQuaternion out +0x10; SetPhysics*Velocity: vec@0, bAdd@0xC, BoneName@0x10; GetClosestPointOnCollision: Point@0, Out@0xC, Bone@0x18, ret@0x20; BreakConstraint Bone@0x18; FindConstraintBoneName idx@0, ret@4; GetBoneName idx@0, ret@4; IsSimulatingPhysics Bone@0, ret@8; SetEnableBodyGravity bool@0, Bone@4; HideBoneByName Bone@0, enum@8; GetPhysicsLinearVelocity Bone@0, ret@8; GetMaterial idx@0, ret@8; K2_SetWorldTransform FTransform@0, bTeleport@0xC0; SetupBrokenParms floorTransform@0x20, ReturnValue@0x50 | UE reflection param layout (UE/GAME) — same on arm64 |
| CEntity | 0x18 | CMatrix* (16-byte rows; m[10]=up.z, m[12..14]=pos) | RW |
| CEntity | 0x20 | linked UE actor | RW (DE addition) |
| CEntity | 0x30 | flags: 0x01 bUsesCollision, 0x04 bIsStatic, 0x80 bIsVisible, 0x40000 (sig #10) | RW |
| CEntity | 0x3A | int16 m_nModelIndex | RW |
| CEntity | 0x6A | type (low 3 bits; 4 = object, 2 = vehicle) | RW |
| CPhysical | 0x78 | physical flags: 0x04 DisableCollisionForce, 0x20 DisableMoveForce, 0x800000 smashed-removed | RW |
| CPhysical | 0x7C / 0x88 | m_vecMoveSpeed / m_vecTurnSpeed (cleared 24 B) | RW |
| CPhysical | 0xF0 | m_pMovingList node | RW |
| CObject | 0x1BC | object flags, 0x100 bIsLampPost | RW |
| CObject | 0x1C0 | uint8 m_nColDamageEffect (200/202 breakable) | RW |
| CObject | 0x1DC | CObjectInfo* (unaligned!) | RW — high risk under clang |
| CVehicle | 0x6F7 | special col model (inside sig #12) | RW |
| CObjectInfo | 0x14 uproot limit, 0x18 col damage mult, 0x3C break velocity, 0x48 break velocity rand | object.dat data | RW |
| CColModel | 0x00 bbox min/max (6 floats), 0x30 m_pColData; copy size 0x38 | | RW |
| CCollisionData | 0x0 u16 nSpheres, 0x2 nBoxes, 0x4 nTris; 0x08 spheres (20 B), 0x10 boxes (28 B), 0x20 vertices (float3), 0x28 tris (16 B), 0x30 triangle planes; copy size 0x60 | | RW (float verts — PC DE uses float not compressed) |
| CBaseModelInfo | 0x30 | m_pColModel (sig #15) | RW |
| CEntity vtable | +0x28 SetIsStatic, +0x40 DeleteRwObject, +0x98 UpdateActorVisibility (DE) | **ABI** — must re-derive on arm64 (Itanium: likely shift by one slot due to the two dtors) |
| CBaseModelInfo vtable | +0x28 GetColModel (comment L788) | **ABI** |
| UE enums used | SetMobility 2=Movable; ECollisionEnabled 0/2; channels 5 PhysicsBody, 7 Destructible; response 0 Ignore, 2 Block; object types 0/2/6/7 | UE |

## 4. UE reflection usage

- FName resolution: own linear scan of FNamePool (`ScanNamePool`, case-insensitive, ANSI only), retried every 1 s for up to 10 min in `InitThread`. Index = `(block<<16)|(offset>>1)`.
- Function lookup: `UClass::FindFunctionByName(obj->Class, FName{idx,0}, IncludeSuper=1)` then `ProcessEvent(obj, fn, parms)` (`Call`, L425; scratch 0x200).
- Class identity: walk `SuperStruct` chain comparing names to `DynamicIPLMapActor` / `StreetLightMapActor` (cached 512 entries).
- GObjects scans: find `Default__PhysicsSettings` (class `PhysicsSettings`) and `Breakable_Floor` (class `StaticMesh`) objects; liveness via FUObjectItem serial/flags.
- Names (46 in `kNames` + `"StaticMesh"` ad hoc):
  - Classes/objects: DynamicIPLMapActor, StreetLightMapActor, PhysicsSettings, Default__PhysicsSettings, Breakable_Floor, StaticMesh.
  - BP events intercepted in ProcessEvent: Dislodged, SetupBroken, RemoveFloor, EntityLinked, SetLights.
  - BP functions called: HideBroken, GetPhysicsFloor, GetPhysicsFloorC, GetBrokenMesh, SetLights, RemoveFloor, SetAlpha (AGTAActor).
  - Engine UFunctions called: SetMobility, K2_DetachFromComponent, K2_SetWorldTransform, SetCollisionObjectType, SetCollisionEnabled, SetCollisionResponseToAllChannels, SetCollisionResponseToChannel, IgnoreComponentWhenMoving, SetAllUseCCD, SetStaticMesh, GetCollisionObjectType, GetActorBounds, SetAngularDamping, SetHiddenInGame, SetSimulatePhysics, GetSocketLocation, GetSocketQuaternion, SetPhysicsLinearVelocity, SetPhysicsAngularVelocityInDegrees, GetNumBones, GetBoneName, IsSimulatingPhysics, SetEnableBodyGravity, PutRigidBodyToSleep, HideBoneByName, BreakConstraint, GetPhysicsLinearVelocity, GetClosestPointOnCollision, GetMaterial, GetNumMaterials, FindConstraintBoneName.
- Properties written directly (no reflection lookup): UPhysicsSettings bSubstepping/MaxSubstepDeltaTime/MaxSubsteps, actor flag 0x2B0, StaticMeshComponent 0x480 (read).
- [INFERENCE] Android port must confirm those BP names exist in the Android cooked content (likely same content, but mobile may strip/cook differently, e.g. PhysX substepping or Breakable_Floor).

## 5. Win32/MSVC dependencies → Android equivalent

| Win32/MSVC | Where | Android (bionic/NDK) |
|---|---|---|
| `DllMain` DLL_PROCESS_ATTACH, `DisableThreadLibraryCalls` | L2196 | `__attribute__((constructor))` in the mod .so / loader-called `init()` |
| `GetModuleHandleA(nullptr)` + PE header walk for `.text` | L191-203 | `dl_iterate_phdr` / parse `/proc/self/maps` for `libUE4.so` r-x segment (ELF PT_LOAD with PF_X) |
| `GetModuleFileNameA(self/exe)` | L72, L2203 | `dladdr(&sym)` for .so path; config/log in a fixed dir, e.g. `/sdcard/Android/data/<pkg>/files/` or `getExternalFilesDir` path |
| `GetPrivateProfileIntA/StringA`, `GetFileAttributesA` | L71-115 | small INI parser (loader API), `access()`/`stat()` |
| `_fsopen(..., _SH_DENYWR)`, `strcpy_s`, `strcat_s`, `_strnicmp` | L137, L386, L1807 | `fopen`, `strlcpy`/`snprintf`, `strncasecmp` |
| `GetEnvironmentVariableA("LOCALAPPDATA")`, `GetTempPathA`, `CreateDirectoryA` | L128-134 | `mkdir`; app files dir; drop fallbacks |
| `OutputDebugStringA` | L143 | `__android_log_print` (logcat) |
| `GetTickCount` | many | `clock_gettime(CLOCK_MONOTONIC)` ms |
| `SRWLOCK`, `AcquireSRWLockExclusive` | L118-159 | `pthread_mutex_t` / `std::mutex` |
| `InterlockedIncrement/Exchange`, `volatile LONG` | L170-180, L401 | `__atomic_*` / `std::atomic<int>` |
| `GetCurrentThreadId` | L1517, L2017 | `gettid()` |
| `CreateThread`, `Sleep` | L2117, L2211 | `pthread_create`/`std::thread`, `usleep` |
| `VirtualAlloc/VirtualFree` | L718, L766 | `mmap`/`munmap` or `calloc` |
| SEH `__try/__except` (≈22 sites) for guarded reads | e.g. L374, L415, L505 | no SEH in clang/Android: use a safe-read helper (`process_vm_readv` on self, or `sigsetjmp`+SIGSEGV handler, or `/proc/self/maps` range check). Only raw reads are guarded; engine calls are not (L503). |
| `thread_local` | L794, L855-862, L1262 | works in NDK (emutls/ELF TLS) |
| MinHook (`MH_Initialize/CreateHook/EnableHook/DisableHook`), uses VirtualProtect internally | L2177-2189, `minhook/` | arm64 inline hook lib: Dobby, ShadowHook (ByteDance), And64InlineHook; `mprotect` + `__builtin___clear_cache` |
| x64 RIP-relative resolve `RipTarget` | L233 | decode arm64 `ADRP+ADD/LDR` pair (page + imm12) and `BL` imm26 |
| `#pragma pack`, `alignas`, `static_assert` | L292-312 | same in clang |
| `MAX_PATH`, `DWORD`, `LONG`, `HMODULE`, `BOOL` | throughout | `PATH_MAX`, `uint32_t`, `int32_t` |

## 6. Size & structure

- LOC: `src/dllmain.cpp` 2220; `test/offline_test.cpp` 962; `minhook/` ≈2,666 (drops out on Android); ini 69. Plugin logic ≈2220 lines total.
- Init flow: `DllMain` → `LoadConfig` (ini next to .asi) → `OpenLog` → `Install()` (`InitTextSection`, 15 sig scans, all-or-nothing; RipTarget GNames/GObjects/ms_modelInfoPtrs; MinHook 7+1 hooks) → `CreateThread(InitThread)`.
- `InitThread`: every 1 s ×600: `ResolveNames` (FName pool scan) + `ConfigureSubstepping` (GObjects scan, writes UPhysicsSettings CDO); then every 30 s `LogStats`.
- Per-frame paths: no dedicated tick hook — `ProcessEvent` hook piggybacks to run `ProcessPieceTimers` and `TickOriginalPieces` (rate-limited to ≥8 ms, game thread only). RW physics hooks (`ProcessCollision/ProcessShift`) run per simulated entity per step → `SyncDislodged` (copy RW matrix to actor via native SyncActor).
- Event paths: SetIsStatic (dislodge), ObjectDamage/Glass (break/uproot), SetupBroken/RemoveFloor/Dislodged/EntityLinked/SetLights (BP events via ProcessEvent), GetColModel ×2 (augmented spheres).
- Sections: config/log (L39-180), scanner (L182-236), layouts (L238-312), FName/reflection (L342-461), collision analysis & sphere generation (L566-851), dislodge/RW sync (L859-1256), breakables/ground plane (L1258-1475), piece timers (L1477-1562), original BreakObject_c piece sim (L1564-2011), ProcessEvent hook (L2013-2071), init (L2073-2220).

## 7. Verdict

- Platform-neutral logic: ≈80-85% of dllmain.cpp (collision measurement, sphere generation, dislodge state machine, ground plane, BreakObject_c piece simulation, ProcessEvent dispatch, reflection calls). It is plain C++ over raw pointers/offsets. [INFERENCE: line estimate by section above; Win32 glue concentrated in L39-236 and L2131-2220 ≈ 330 lines + ~22 `__try` sites.]
- Platform glue: ≈15-20% (config/log/threads/atomics/SEH/PE scan/MinHook) — all replaced 1:1 by the mod-loader API (sigscan, hook, log, ini, safe-read).
- Binary-specific data that must be **re-derived**, not ported: all 15 signatures (x64 bytes are useless), all RW/game struct offsets (verify each in the Android `libUE4.so`; clang padding, especially CObject 0x1DC unaligned CObjectInfo*), and all MSVC vtable offsets (0x28/0x40/0x98 + model-info 0x28). UE offsets likely carry over but must be checked against an Android Dumper-7/UE4SS SDK dump.
- Recommended strategy:
  1. Build the loader with: ELF `.text` sigscan over `libUE4.so`, arm64 ADRP/ADD/BL resolvers, Dobby/ShadowHook inline hooks, logcat+file log, INI, `SafeRead`.
  2. Compile dllmain.cpp mostly unchanged via a thin `win32_compat.h` (DWORD/GetTickCount/Interlocked/SRWLOCK/strcpy_s/_strnicmp) and replace `__try` blocks with `SafeRead` calls; replace `Install()`'s sig table with arm64 patterns.
  3. Re-find each function in the Android binary using the anchors in table 1 (FName strings "Dislodged"/"SetupBroken", UE source-known functions, gta-reversed structure), ideally by diffing against PC IDA DB (BinDiff/Diaphora on structurally similar code), then validate struct offsets by reading the functions' field accesses.
  4. Keep the offline test idea: a host-side check that resolves every arm64 sig against the pulled `libUE4.so` and asserts uniqueness.

Skipped/not checked: no Android binary inspected yet — every arm64 equivalence above is unverified; hooked-RVA-to-function mapping for 5 hooks is inferred from array order.
