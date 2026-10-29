# GTA_Prop_Fix → Android arm64 port map (`libUE4.so`, GTA SA DE `com.rockstargames.gtasa.de` 1.87.0)

## Counts

| Category | Total | RESOLVED | UNRESOLVED |
|---|---|---|---|
| Signatures (15) | 15 | 15 | 0 |
| Hooks (8) | 8 | 8 | 0 |
| Offset and vtable rows (O1–O44, §3–§5) | 44 | 38 | 6: O14, O19b, O20, O26, O37, O39b |
| UE offsets named in the brief (UObject 0xC/0x10/0x18, SuperStruct 0x40, RootComponent 0x130, PhysicsSettings 0x12C/0x130/0x134) | 8 | 8 | 0 |

Hook caveats that the RESOLVED count hides:
- The CGlass hook has a different argument signature on Android (H5).
- The return types of ProcessCollision, ProcessShift and CGlass were not observed.
- The model-info hook shares one tiny function across several classes (H8).

### Method and trust notes
- **Source of facts.** `history://Arm64Mapper` kept only tool-call stubs and no outputs, so I redid the work. Everything below was read from `/nm_*.txt` and from disassembly I ran, except lines marked `[INFERENCE]`.
- **Scratch files.** I created `/tmp/_d.py` (a capstone disassembler that resolves symbols and relocations) and `/tmp/_rs.pkl` (a relocation and symbol cache). Both are outside the repo and deletable.
- **Addresses.** All addresses are ELF vaddrs in `libUE4.so`. The image base is 0, so runtime address = module base + vaddr.
- **Exports.** The dumps come from `nm -D`, so these names are dlsym-able. Mangled names below are copied from `nm_mangled.txt`.
- **Byte signatures.** None are needed. Every target is exported, or is reachable from an export or a vtable. I did not scan for byte-signature uniqueness because there was nothing to scan for.
- **Argument registers.** AAPCS64 argument registers are x0..x7 for pointers and integers and s0..s7 for floats. FName and FNameEntryId are 8 and 4 bytes, passed by value in x1 and w1.

---

## 1. The 15 signatures

| # | PC name | Android export (exact mangled) | Addr | Status / how to use |
|---|---|---|---|---|
| S1 | `UObject::ProcessEvent` | `_ZN7UObject12ProcessEventEP9UFunctionPv` | 0x67a758c | RESOLVED. Member function: x0=this, x1=UFunction*, x2=parms. It is also virtual at UObject vtable offset 0x220. Evidence: the tail of `CObject::SetIsStatic` does `ldr x3,[vtbl,#0x220]; br x3` after `UObject::FindFunctionChecked`. Hook the function body and it catches all callers. |
| S2 | `UClass::FindFunctionByName` | `_ZNK6UClass18FindFunctionByNameE5FNameN17EIncludeSuperFlag4TypeE` | 0x6605cb0 | RESOLVED. x0=UClass*, x1=FName (8 bytes: ComparisonIndex low 32, Number high 32), w2=EIncludeSuperFlag. Evidence: it reads `[x0,#0x138]`, `[x0,#0x164]`, `[x0,#0x170]` and `[x0,#0x178]` (FuncMap) and walks `[x,#0x40]` (SuperStruct). Also exported: `_ZNK7UObject12FindFunctionE5FName` (0x67a7344) and `_ZNK7UObject19FindFunctionCheckedE5FName` (0x67a7350). |
| S3 | `CObject::SetIsStatic` | `_ZN7CObject11SetIsStaticEb` | 0x5586f64 | RESOLVED. Also vtable slot 0x30 (§4). Body: `ldr x8,[x0,#0x30]`, bit 2 (0x4) set from the bool argument, `str x8,[x0,#0x30]`. Its `false` path fires the Blueprint event `Dislodged` on `ADynamicIPLMapActor` through ProcessEvent. |
| S4 | `CEntity::SyncActor` | **No function by that name.** The functional match is `_ZN7CEntity14UpdateRwMatrixEv` | 0x5437e68 | RESOLVED, with semantic match `[INFERENCE]`. Evidence: it loads actor `[x0,#0x20]`, RootComponent `[actor,#0x130]` and matrix `[x0,#0x18]`; it multiplies by 100.0f (`mov w9,#0x42c80000`); it builds an FTransform (`FTransform::SetFromMatrix`) and calls `USceneComponent::SetWorldTransform` (0x8c13ea0). It also has a stack cookie and an AIPLMapActor / AGTASMActor IsA check. Other candidates: `_ZN7CEntity9UE4_UpdateEv` (0x543a4f4) is just `if (this+0x20) vtbl[0x90]()`, which is PreRender. |
| S5 | `CPhysical::ProcessCollision` | `_ZN9CPhysical16ProcessCollisionEv` | 0x5479c24 | RESOLVED. Also vtable slot 0x60. |
| S6 | `CPhysical::ProcessShift` | `_ZN9CPhysical12ProcessShiftEv` | 0x547d200 | RESOLVED. Also vtable slot 0x68. |
| S7 | `CGlass::WindowRespondsToCollision` | `_ZN6CGlass25WindowRespondsToCollisionEP7CEntityf7CVectorS2_b` | 0x561b1a4 | RESOLVED. **The signature differs from PC; see H5.** |
| S8 | BreakObject ground probe | `_ZN14BreakManager_c8GetFloorEP7CObjectP7CVectorS3_` | 0x557f764 | RESOLVED. This is `BreakManager_c::GetFloor(CObject*, CVector*, CVector*)`, not a `BreakObject_c` member. The CObject is read as `[x0,#0x3a]` (model index), then `CModelInfo::ms_modelInfoPtrs[idx]`, then a vcall at vtable +0x30 (GetColModel). It is also called from `CObject::ObjectDamage`. That the 2nd and 3rd arguments are outPos and outNormal is `[INFERENCE]`. |
| S9 | `CObject::ObjectDamage` | `_ZN7CObject12ObjectDamageEfP7CVectorS1_P7CEntity11eWeaponType` | 0x55849bc | RESOLVED. Arguments: x0=this, s0=damage, x1, x2, x3=damager, w4=weapon. Same as the PC typedef. |
| S10 | `CPhysical::AddToMovingList` | `_ZN9CPhysical15AddToMovingListEv` | 0x5477848 | RESOLVED. Starts `ldr x8,[x0,#0xF0]; cbz`, then `ldrb w8,[x0,#0x32]; tbnz #2` (flag 0x40000 at +0x30). |
| S11 | `CModelInfo::GetColModel(int)` | `_ZN10CModelInfo11GetColModelEi` | 0x556c7bc | RESOLVED. `g_modelInfos` is `CModelInfo::ms_modelInfoPtrs`, exported as `_ZN10CModelInfo16ms_modelInfoPtrsE` at 0xad272a8. Code reads `ldr x0,[arr, w0, sxtw #3]` and then `vtbl[0x30]`. The symbol is the array itself; no pointer-to-pointer. A null model info returns a static dummy CColModel. |
| S12 | `CEntity::GetColModel` | `_ZN7CEntity11GetColModelEv` | 0x543c620 | RESOLVED. Not virtual. Same logic as PC: type `[x0,#0x6a]&7==2` selects the vehicle path using the signed byte `[x0,#0x6f7]` as an index into a table of 0x38-byte CColModels. Otherwise it uses `ms_modelInfoPtrs[(int16)[x0,#0x3a]]` and vcall `[vtbl,#0x30]`. |
| S13 | GNames ref (`FNamePool`) | **NamePoolData instance is not exported.** Use the exported `_ZN5FName8GetEntryE12FNameEntryId` (0x64f4510) | instance = 0xaf39800 | RESOLVED. See the next section. |
| S14 | GObjects ref | `GUObjectArray` (plain C symbol, type B) | 0xaf558e8 | RESOLVED. See the next section. |
| S15 | `CBaseModelInfo::GetColModel` (PC 0x140D2BE90) | **Not exported.** Reach it through the vtable (see below) | 0x55762c4 | RESOLVED. See the next section. |

### S13: FNamePool and the FName exports

**Instance.** `FName::GetEntry(FNameEntryId)` at 0x64f4510 contains:
```
64f4520 adrp x21,#0xaf39000        ; guard byte at 0xaf397c0
64f4524 adrp x20,#0xaf39000
64f452c add  x20,x20,#0x800        ; &NamePoolData = 0xaf39800
64f4540 bl FNamePool::FNamePool()  ; one-time static-local init
64f454c lsr x8,x19,#16             ; block index = id>>16
64f4550 lsl w9,w19,#1 ; and x9,x9,#0x1fffe   ; byte offset = (id&0xffff)*2
64f455c add x8,x20,w8,uxth #3
64f4564 ldr x8,[x8,#0x40]          ; Blocks[block] at pool+0x40
64f4568 add x0,x8,x9
```
This confirms the 0x20000-byte block and stride-2 entry addressing from the PC mod. **Android `FNamePool::Blocks` is at +0x40, not +0x10**, because the lock is larger. The mod's pool-scan code needs that offset.

**Recommended approach.** Call `FName::GetEntry` through dlsym instead of scanning the pool. Entry-header decode (`len = hdr>>6`, `wide = hdr&1`) is stock UE 4.26 `[INFERENCE]`; I did not read it from the binary.

**Derive the address (fallback).** Decode the `adrp x20 …; add x20,x20,#imm` pair at GetEntry+0x14 and +0x1c.

**Other FName exports:**

| Mangled name | Addr |
|---|---|
| `_ZN5FNameC1EPKc9EFindName` (const char*, EFindName; FNAME_Find=0, FNAME_Add=1) | 0x64f4c90 |
| `_ZN5FNameC1EPKDs9EFindName` | 0x64f4a14 |
| `_ZNK5FName8ToStringEv` (FString returned via sret in x8) | 0x64f5f64 |
| `_ZNK5FName8ToStringER7FString` | 0x64f6038 |
| `_ZNK5FName18GetPlainNameStringERA1024_Ds` | 0x64f5c10 |
| `_ZNK5FName18GetPlainNameStringEv` | 0x64f5d5c |
| `_ZNK5FName18GetPlainANSIStringERA1024_c` | 0x64f5dd0 |
| `_ZNK5FName12AppendStringER7FString` | 0x64f676c |
| `_ZN5FName8GetEntryE5EName` | 0x64f4498 |
| `_ZN9FNamePoolC1Ev` | 0x64ef9c4 |
| `_ZNK9FNamePool4FindE15FNameStringView` | 0x64f1c9c |
| `_ZNK9FNamePool10NumEntriesEv` | 0x64f2050 |
| `GNameBlocksDebug` (B) | 0xaea2770 |

`FNamePool::Find` is a member and needs `this`, which is the instance above.

### S14: GObjects

`FUObjectArray::IsValid(const UObjectBase*)` (`_ZNK13FUObjectArray7IsValidEPK11UObjectBase`, 0x67c01ec) reads:
- `[obj,#0xc]` as InternalIndex.
- `[arr,#0x24]` as NumElements.
- `[arr,#0x10]` as `Objects**`.
- Chunk = idx>>16 (`lsr #0xd; and #0x7fff8`).
- Item size 0x18 (`mov w10,#0x18`).

So the `FChunkedFixedUObjectArray` starts at `GUObjectArray+0x10`. Relative to that, `Objects**` is at +0x00 and `NumElements` at +0x14, the same as the PC layout.

### S15: the PC 0x140D2BE90 getter

It is a shared, unnamed 8-byte function at **0x55762c4**:
```
55762c4: ldr x0,[x0,#0x30]
55762c8: ret
```
Bytes: `00 18 40 F9 C0 03 5F D6`.

Evidence that it is the vtable slot: it is the slot at offset 0x30 in `_ZTV14CBaseModelInfo`, `_ZTV15CClumpModelInfo`, `_ZTV16CAtomicModelInfo`, `_ZTV22CDamageAtomicModelInfo`, `_ZTV17CVehicleModelInfo` and `_ZTV13CPedModelInfo`. `symbol+0x10+0x30` is the entry, applied by a RELATIVE relocation at load. Callers confirm the slot:
- `CModelInfo::GetColModel` at 0x556c7d8 does `ldr x1,[x8,#0x30]; br x1`.
- `CEntity::GetColModel` does the same vcall at +0x30.
- `BreakManager_c::GetFloor` does the same vcall at +0x30.

It has no symbol of its own; nearby nm labels such as `CCustomAtomicModelInfo::ResolveModel+0x830` are only the nearest symbol. The byte pattern is not unique and I did not scan it.

**How to locate it at runtime.** `dlsym("_ZTV14CBaseModelInfo")`, then `*(void**)(sym+0x10+0x30)`.

---

## 2. The 8 hooks

| # | Target | Android hook | Notes |
|---|---|---|---|
| H1 | `UObject::ProcessEvent` | S1 | `void(UObject*, UFunction*, void*)`; the argument layout is the same as the PC typedef with `this` first. |
| H2 | `CObject::SetIsStatic` | S3 | `void(CObject*, bool)`. You can instead patch vtable slot 0x30 of `_ZTV7CObject`, which only affects CObject. |
| H3 | `CPhysical::ProcessCollision` | S5 | Declare it as `uintptr_t(CPhysical*)` and pass x0 through. I did not read the return value in the epilogue. |
| H4 | `CPhysical::ProcessShift` | S6 | Same note as H3. |
| H5 | `CGlass::WindowRespondsToCollision` | S7 | **The Android signature is `(CEntity* x0, float damage s0, CVector speed s1..s3, CVector pos s4..s6, bool w1)`.** CVector is passed by value as an HFA of three floats in s-registers. The mangling `7CVectorS2_b` confirms it. Evidence: the prologue stores `d6/d5/d3/d2` and `fmov s8,s4`. The PC prototype `(entity, float, float* speed, float* pos)` does not apply. Return type not observed. |
| H6 | `CObject::ObjectDamage` | S9 | Matches the PC prototype. |
| H7 | `CEntity::GetColModel` | S12 | Non-virtual. An inline hook only catches call sites that actually call this symbol. |
| H8 | `CBaseModelInfo::GetColModel` | S15 | The code is **shared** by every model-info vtable and is probably an ICF-folded `return this->+0x30`. An inline hook on 0x55762c4 would hit every user of it. A vtable patch at slot 0x30 of each model-info class is more precise. Keep the PC guard "`ms_modelInfoPtrs[model]==this` and tracked simulated entity". |

---

## 3. Struct offsets: PC → Android

| ID | Field | PC | Android | Status | Evidence |
|---|---|---|---|---|---|
| O1 | CEntity matrix ptr | 0x18 | 0x18 | RESOLVED | `CPhysical::ApplyMoveSpeed` 0x5478254 `ldr x9,[x0,#0x18]`, then pos at matrix +0x30/+0x34/+0x38; also `UpdateRwMatrix` 0x5437f98 |
| O2 | CEntity linked UE actor | 0x20 | 0x20 | RESOLVED | `CEntity::UE4_Update` 0x543a4f4 `ldr x8,[x0,#0x20]`; `SetIsStatic` 0x5586fe8 |
| O3 | CEntity flags | 0x30 | 0x30 | RESOLVED | `SetIsStatic` 0x5586f78 `ldr x8,[x0,#0x30]`, clear bit 2 and set from bool, `str x8,[x0,#0x30]`; `AddToMovingList` 0x5477854 `ldrb w8,[x0,#0x32]; tbnz #2` (= 0x40000). Bits 0x01 and 0x80 were not directly observed. |
| O4 | CEntity model index (int16) | 0x3A | 0x3A | RESOLVED | `CEntity::GetColModel` 0x543c67c `ldrsh x8,[x19,#0x3a]` |
| O5 | CEntity type byte (low 3 bits) | 0x6A | 0x6A | RESOLVED | `GetColModel` 0x543c644 `ldrb w8,[x0,#0x6a]; and #7; cmp #2` |
| O6 | CPhysical flags | 0x78 | **0x74** | RESOLVED | `ApplyMoveSpeed` 0x5478234 `ldrh w8,[x0,#0x74]; tst #0x20202020` (0x20 DisableMoveForce); `SetIsStatic` 0x5586f8c `ldr w20,[x0,#0x74]`, `tbz w20,#5` (matches the PC comment that 0x20 is tested there) |
| O7 | CPhysical m_vecMoveSpeed | 0x7C | **0x78** | RESOLVED | `ApplyMoveSpeed` 0x5478244 `str xzr,[x0,#0x78]; str wzr,[x0,#0x80]` (xyz at 0x78/0x7c/0x80) |
| O8 | CPhysical m_vecTurnSpeed | 0x88 | **0x84** | RESOLVED | `ApplyTurnSpeed` 0x54782a8 `ldp s6,s1,[x0,#0x88]; ldr s4,[x0,#0x84]` |
| O9 | CPhysical moving-list node | 0xF0 | 0xF0 | RESOLVED | `AddToMovingList` 0x5477848 `ldr x8,[x0,#0xf0]; cbz` |
| O10 | CObject flags (0x100 lamp post) | 0x1BC | 0x1BC | RESOLVED for the field | `CGlass` 0x561b1ec `ldr w8,[x0,#0x1bc]`; `SetObjectData` 0x55836e8. The lamp-post bit was not tested. |
| O11 | CObject m_nColDamageEffect | 0x1C0 | 0x1C0 | RESOLVED | `SetObjectData` 0x558367c `ldrb w13,[x11,#0x1c]!` (the CObjectInfo byte at +0x1C) then `strb w13,[x19,#0x1c0]`; `ObjectDamage` 0x5584a5c, 0x5584ba0 `ldrb w8,[x19,#0x1c0]` |
| O12 | **CObject → CObjectInfo\*** | 0x1DC | **0x1DC** | RESOLVED | `CObjectData::SetObjectData` 0x5583644 `add x11,x19,#0x1dc; str x12,[x11]`, where x12 = `ms_aObjectInfo + idx*0x58` (`mov w11,#0x58; madd`). sizeof(CObjectInfo) = 0x58. The unaligned store is real. |
| O13 | CVehicle special colmodel index | 0x6F7 | 0x6F7 | RESOLVED | `CEntity::GetColModel` 0x543c654 `ldrsb w8,[x19,#0x6f7]` |
| O14 | CObjectInfo uproot limit | 0x14 | 0x14 `[INFERENCE]` | **UNRESOLVED** | Not read in the code I looked at. The fields on both sides of it are confirmed: +0x10 is copied to CObject+0xD4 and +0x18 is read. Search tried: `SetObjectData`, `ObjectDamage`. |
| O15 | CObjectInfo col-damage mult | 0x18 | 0x18 | RESOLVED | `ObjectDamage` 0x5584a64 `ldr s0,[x9,#0x18]`, 0x5584b1c |
| O16 | CObjectInfo m_vecBreakVelocity | 0x3C | 0x3C | RESOLVED | `CObjectData::Initialise` 0x5445ce4..0x5445d3c reads consecutive floats at +0x38, +0x3C, +0x40, +0x44, +0x48, matching smash-mult then vec3 then rand. The loaded value goes into a compare against the parsed value. Treating `x12` as the CObjectInfo is `[INFERENCE]`. |
| O17 | CObjectInfo m_fBreakVelocityRand | 0x48 | 0x48 | RESOLVED | same evidence (0x5445d3c) |
| O18 | CColModel m_pColData and size | 0x30 / 0x38 | 0x30 / 0x38 | RESOLVED | `CColModel::CColModel()` 0x53122a0 `str xzr,[x0,#0x30]`; the vehicle table stride `mov w10,#0x38` in `GetColModel` |
| O19a | CCollisionData tri count (u16) | 0x4 | 0x4 | RESOLVED | `GetNoVectors` 0x5312d5c `ldrh w8,[x0,#4]` |
| O19b | CCollisionData nSpheres @0 / nBoxes @2 | 0x0 / 0x2 | 0x0 / 0x2 `[INFERENCE]` | **UNRESOLVED** | Only the constructor zero-stores (`str wzr,[x0]`) were seen. |
| O20 | CCollisionData spheres ptr (20 B each) | 0x08 | 0x08 `[INFERENCE]` | **UNRESOLVED | Not directly read. The ctor zeroes 0x08..0x57, and `RemoveCollisionVolumes` frees the pointers at 0x10, 0x18, 0x20, 0x28 and 0x48. Sphere stride unverified. |
| O21 | CCollisionData vertices (float3) | 0x20 | 0x20 | RESOLVED | `GetTrianglePoint` 0x5312144 `ldr x10,[x0,#0x20]`, stride 0xC |
| O22 | CCollisionData triangles | 0x28 | 0x28 | RESOLVED | `GetNoVectors` 0x5312d64 `ldr x10,[x0,#0x28]`, stride 0x10. **Triangle vertex indices are read as 32-bit (`ldp w11,w12`, `ldr w13`) at +0,+4,+8**, so check the PC mod's index width. |
| O23 | CCollisionData triangle planes | 0x30 | 0x30 | RESOLVED | `CalculateTrianglePlanes` 0x5311f58 `str x0,[x19,#0x30]` |
| O24 | CBaseModelInfo m_pColModel | 0x30 | 0x30 | RESOLVED | shared getter 0x55762c4 `ldr x0,[x0,#0x30]` |
| O25 | UStaticMeshComponent::StaticMesh | 0x480 | **0x478** | RESOLVED | `OnRep_StaticMesh` 0x8c60450/0x8c60460 `ldr/str x1,[x0,#0x478]`; `SetStaticMesh` 0x8c6339c |
| O26 | ADynamicIPLMapActor bEntityUpdatePosition (0x2B0 bit 0x04) | 0x2B0 | not found | **UNRESOLVED** | Constructor-range code (0x4cc106c..0x4cc2600) shows accesses at 0x2a0, 0x2a8, 0x2b8, 0x2bb and 0x2c8, but nothing at 0x2b0. Candidate bytes 0x2b8/0x2bb are unverified. |

---

## 4. Vtable slots (Itanium: vtable symbol + 0x10 = slot 0)

Offsets are from the object's vptr. The PC mod's offsets all shift by +8, because the destructor takes two slots (D1 at 0x0, D0 at 0x8).

| ID | Method | PC | Android | Status | Evidence |
|---|---|---|---|---|---|
| O40 | `SetIsStatic` (CObject) | 0x28 | **0x30** | RESOLVED | `_ZTV7CObject` (0xa2dcfc0): slot 0x30 → `CObject::SetIsStatic` 0x5586f64. `CEntity`/`CPhysical` vtables have `__cxa_pure_virtual` (0x530ef60) there. |
| O41 | `DeleteRwObject` | 0x40 | **0x48** | RESOLVED | slot 0x48 → `CEntity::DeleteRwObject` 0x5438894 (CObject, CEntity and CPhysical). `ObjectDamage` calls it as `ldr x8,[x8,#0x48]` at 0x5584dd4 and 0x5584e40. |
| O42 | `UpdateActorVisibility` | 0x98 | **0xA0** | RESOLVED | slot 0xA0 → `CEntity::UpdateActorVisibility` 0x543a50c |
| O43 | Model-info `GetColModel` | 0x28 | **0x30** | RESOLVED | see S15 |
| O44 | `UObject::ProcessEvent` | n/a | 0x220 | RESOLVED | `SetIsStatic` tail 0x5587104 `ldr x3,[x8,#0x220]; br x3` |

Other CObject slots I read: 0x10 CreateActor, 0x18 and 0x20 Add, 0x28 Remove, 0x38 SetModelIndex, 0x40 SetModelIndexNoCreate, 0x50 GetBoundRect, 0x58 ProcessControl, 0x60 ProcessCollision, 0x68 ProcessShift, 0x70 TestCollision, 0x78 Teleport, 0x90 PreRender, 0x98 Render.

---

## 5. UE offsets

| ID | Field | PC | Android | Status | Evidence |
|---|---|---|---|---|---|
| O27 | UObject InternalIndex | 0xC | 0xC | RESOLVED | `FUObjectArray::IsValid` 0x67c01ec `ldr w9,[x1,#0xc]`; `UObjectBase` ctor 0x67c048c `stp w1,w9,[x0,#8]` (flags at 8, index at 0xC) |
| O28 | UObject ClassPrivate | 0x10 | 0x10 | RESOLVED | `UObjectBase` ctor 0x67c04b4 `stp x1,xzr,[x0,#0x10]`; `SetIsStatic` 0x5587084 `ldr x10,[x21,#0x10]` |
| O29 | UObject NamePrivate | 0x18 | 0x18 | RESOLVED | ctor zeroes 0x18 (`stp xzr,xzr,[x0,#0x18]`), with Outer at 0x20 (`str x4,[x0,#0x20]`), then branches to `AddObject(FName,…)`; it is the slot after Class. The 8-byte FName store itself is inside `AddObject`, which I did not read. |
| O30 | UStruct SuperStruct | 0x40 | 0x40 | RESOLVED | `FindFunctionByName` 0x6605d68 `ldr x22,[x21,#0x40]` |
| O31 | UClass FuncMap | 0x138 | 0x138 | RESOLVED | `FindFunctionByName` 0x6605cc8 `ldr w8,[x0,#0x138]` |
| O32 | AActor RootComponent | 0x130 | 0x130 | RESOLVED | `UpdateRwMatrix` 0x5437f64 `ldr x8,[x20,#0x130]`, where x20 = the actor from `[entity+0x20]`; it then makes the SetWorldTransform call on that component |
| O33 | UPhysicsSettings bSubstepping | 0x12C | 0x12C | RESOLVED | ctor 0x924d7bc `strb wzr,[x19,#0x12c]` |
| O34 | UPhysicsSettings MaxSubstepDeltaTime | 0x130 | 0x130 | RESOLVED | ctor 0x924d7d4 `str x8,[x19,#0x130]` (8-byte store covering both) |
| O35 | UPhysicsSettings MaxSubsteps | 0x134 | 0x134 | RESOLVED | same store, upper 32 bits |
| O36 | FNamePool Blocks[] | 0x10 | **0x40** | RESOLVED | see S13 (`ldr x8,[x8,#0x40]` in `FName::GetEntry`) |
| O37 | FNamePool CurrentBlock / CurrentByteCursor | 0x08 / 0x0C | 0x38 / 0x3C `[INFERENCE]` | **UNRESOLVED** | Inferred only from Blocks at 0x40 and an 8-byte slot before it; the lock occupies 0x38. I did not read `FNamePool::Store`. |
| O38 | GUObjectArray chunk table / NumElements | +0 / +0x14 | `GUObjectArray+0x10` / `+0x24` | RESOLVED | `FUObjectArray::IsValid`, see S14 |
| O39a | FUObjectItem size and Object ptr | 0x18, +0 | 0x18, +0 | RESOLVED | `IsValid` 0x67c0218 `mov w10,#0x18`, `ldr x8,[x8,x9]` (Object at +0) |
| O39b | FUObjectItem flags +8 (bit29 pending-kill, bit28 unreachable), serial +0x10 | | not read | **UNRESOLVED** | `IsValid` does not touch flags or serial. |
| O44b | `UObject::ProcessEvent` signature | | `void(UObject*, UFunction*, void*)` | RESOLVED | see S1 |

---

## 6. Other findings that change the port

1. **BreakObject piece simulation is a stub on Android.**
   - `BreakObject_c::Init` (0x557fa9c) is `mov w0,wzr; ret`.
   - `BreakManager_c::Init` returns 1 and `Exit` just returns; `Add` (0x5580898) and `Render` are 4-byte stubs (not disassembled).
   - `BreakManager_c::Update` still loops 64 slots of 0x28 bytes.
   - So the mod's `BreakObject_c` piece-simulation block (dllmain.cpp L1564–2011) has no native counterpart. The Android breakables run through Blueprint: `ADynamicIPLMapActor::SetupBroken`, `AddBreakableToGTA`, `RemoveBreakableFromGTA` and `GetPhysicsFloorC` are all exported.
2. **UFunction natives are exported, so ProcessEvent/FindFunction is not the only route.** Verified exports:
   - `UPrimitiveComponent::SetSimulatePhysics(bool)` `_ZN19UPrimitiveComponent18SetSimulatePhysicsEb` (0x92b659c).
   - `SetCollisionEnabled` `_ZN19UPrimitiveComponent19SetCollisionEnabledEN17ECollisionEnabled4TypeE` (0x92b90a8).
   - `SetCollisionObjectType` `_ZN19UPrimitiveComponent22SetCollisionObjectTypeE17ECollisionChannel` (0x92b8fd4).
   - `SetAllUseCCD` `_ZN19UPrimitiveComponent12SetAllUseCCDEb` (0x92b7a48).
   - `SetPhysicsLinearVelocity` `_ZN19UPrimitiveComponent24SetPhysicsLinearVelocityE7FVectorb5FName` (0x92b6c48).
   - `USceneComponent::SetMobility` `_ZN15USceneComponent11SetMobilityEN18EComponentMobility4TypeE` (0x8c16988).
   - `USceneComponent::K2_SetWorldTransform` `_ZN15USceneComponent20K2_SetWorldTransformERK10FTransformbR10FHitResultb` (0x8c1c140).
   - `UStaticMeshComponent::SetStaticMesh` `_ZN20UStaticMeshComponent13SetStaticMeshEP11UStaticMesh` (0x8c63374).
   - `AActor::K2_SetActorTransform` (0x893b278).
   - `AGTAActor::SetAlpha(float)` `_ZN9AGTAActor8SetAlphaEf` (0x4b6b0f4).
   - The matching `exec*` thunks (e.g. `_ZN19UPrimitiveComponent22execSetSimulatePhysicsEP7UObjectR6FFramePv`) are also exported. I did not check every function name the mod calls; `GetSocketLocation`, `GetBoneName` and the others were not looked up.
3. **Android behaviour of the tail of `SetIsStatic(false)`.** It calls the `Dislodged` Blueprint event on `ADynamicIPLMapActor` through `FindFunctionChecked` plus ProcessEvent. This is the same event the mod suppresses in its ProcessEvent hook.
4. **`CObject::ObjectDamage` and `CGlass` set a "damaged" flag the same way.** `CGlass` ORs 0x10 into the CObject flags at `[x0,#0x1bc]` early (0x561b1f8).

### Not checked
- Whether the 8-byte `ldr x0,[x0,#0x30]; ret` pattern is unique in `.text`.
- The return type of `ProcessCollision`, `ProcessShift` and `CGlass` on Android.
- Bit meanings inside the physical flags at +0x74 (only 0x20 was confirmed) and the CEntity flag bits 0x01 and 0x80.
- The `CObject::ObjectDamage` internal switch on `m_nColDamageEffect`.
- Blueprint names such as `Breakable_Floor` and `SetupBroken` in the cooked content. Only the native `ADynamicIPLMapActor` stubs were confirmed.

Cleanup is yours: `/tmp/_d.py` and `/tmp/_rs.pkl` can be deleted if you want the scratch removed.