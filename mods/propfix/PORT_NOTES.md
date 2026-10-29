# Android AML port: notes

## SEH replacement
PC `__try/__except` blocks are replaced with `gtade::SafeRead` (or `Rd<T>` wrapper, `Readable` page probe for arrays). A failed read takes the same fallback as the PC `__except` branch.

| Site | Validation | Fallback on fault |
|---|---|---|
| `FindFunc`, `PropKindOf`, `ReadPtr` | `Rd` / `SafeRead` | `nullptr` / `PROP_NONE` (not cached) |
| `ReadPropInfo` | `Rd` for scalars, `SafeRead` for matrix and bbox | returns false |
| `CollectColIntervals` | header via `Rd`; `Readable` on sphere/box/tri arrays; per-vertex `SafeRead` for tri indices | returns 0, `hasData=false` |
| `MeasureSpheres` | `Readable` on the whole sphere array (all-or-nothing) | returns 0 |
| `GatherColPoints` | same as above; per-vertex `SafeRead` | skips vertex |
| `IsObjectEntity` | `Rd` of `Entity_Type` | false |
| `DislodgedActor` | `Rd` of `Entity_Flags`, `Entity_Actor`, `IPLMapActor_Flags` | false |
| `ExactColBottomZ` | header `Rd`; `Readable`; per-vertex `SafeRead` | returns no bottom |
| `StopTracking` | `SafeRead` 1 byte, then write | write skipped |
| `Hooked_SetIsStatic` (IPL flag) | `SafeRead` 1 byte, then write | returns |
| `Hooked_SetIsStatic` (lamp-post) | `SafeRead` 4 bytes of `Entity_ObjectFlags`, then write | write skipped |
| `CanUprootStreetLight` | `Rd` of `Entity_Flags`, `Entity_PhysicalFlags` | false |
| `Hooked_ObjectDamage` | `SafeRead` of `Physical_MoveSpeed`; `Rd` of `Entity_ColDamageEffect`, `Entity_ObjectInfo`, `ColDamageMult` | speed zeros; effect 0; mult 1.0 |
| `ObjectItem` | `Rd` of chunk table, `NumElements`, chunk slot | nullptr |
| `UObjectAlive` | `Rd` of item object, serial, flags | false |
| `SchedulePieces`, `SlotOf` | `Rd` of `Obj_Index`, `ReadPtr`, `Rd` of serial | return |
| `FindBreakableFloorMesh` | `Rd` of chunk table, chunk slot, object, class; `NameOf` | loop continues |
| Break velocity read | `SafeRead` 12 bytes; `Rd` of rand | defaults kept |
| `ConfigureSubstepping` | `Rd` of chunk table, chunk slot, object, class | returns false |
| `NameOf` | `Rd` of `Obj_Name` | 0 (NAME_None) |

## Known residuals
- `StopTracking` and `Hooked_SetIsStatic` do read-check then write. The read and write are not atomic. The object could be freed between them. The PC version had the same window.
- `ConfigureSubstepping` writes three fields on the PhysicsSettings object after `Rd` succeeds on its name. Writes are not guarded; there is no `SafeWrite`.
- Vertex and triangle indices from collision data are not range-checked against the vertex count. Each vertex read is guarded by `SafeRead`, so a bad index faults into the fallback instead of crashing.
- Engine calls (`g_FindFunctionByName`, `g_GetColModel`, `ProcessEvent`, `g_GroundProbe`, `o_SetIsStatic`) are not wrapped, as in the PC version.

## Logging
- Every `Log()` line goes to logcat (tag `GTAPropFix`) first, then to the file if the level allows.
- Log and ini paths use `JoinDir`. AML `GetDataPath()` and `GetConfigPath()` return directories without a trailing `/`.

## Install
- The first missing symbol logs `ABORT: missing <symbol>` and returns false.
- Hooks are all attempted. The summary line is `hooks installed (k/7)`. Any failure returns false.
- Behavior change: hooks after a failed one are now still attempted (the old `&&` chain stopped at the first failure).
