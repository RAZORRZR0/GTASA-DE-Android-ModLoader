# GTA_Prop_Fix settings: defaults

Source of truth for the 20 keys in `GTA_Prop_Fix.ini`. The code defaults in
`mods/propfix/propfix.cpp` match the PC `GTA_Prop_Fix/src/dllmain.cpp`
for every key.

| Section | Key | Code default | Shipped PC ini (= repo ini) | Android ini before | Android ini now |
|---|---|---|---|---|---|
| Props | RenderWarePhysicsForDislodgedProps | 1 | 1 | 1 | 1 |
| Props | StreetLightsOffWhenTilted | 1 | 1 | 1 | 1 |
| Props | StreetLightsFallWhole | 1 | 0 | 1 | 0 |
| Props | FallenPolesDisappearSeconds | 6 | 6 | 6 | 6 |
| Props | KnockedPropsDisappearSeconds | 0 | 0 | 0 | 0 |
| Props | MinCollisionCoveragePercent | 60 | 60 | 60 | 60 |
| Props | MinShapeCoveragePercent | 70 | 70 | 70 | 70 |
| Props | GenerateCollisionSpheres | 1 | 1 | 1 | 1 |
| Props | FallenPolesBlockCars | 1 | 1 | 1 | 1 |
| Props | KnockedPropsStayPushable | 1 | 1 | 1 | 1 |
| Debris | OriginalGroundPlane | 1 | 1 | 1 | 1 |
| Debris | Lifetime | 0 (DEBRIS_ORIGINAL) | 0 | 0 | 0 |
| Debris | OriginalPieceMotion | 1 | 1 | 1 | 1 |
| Debris | PiecesFadeSeconds | 2.55 | 2.55 | 2.55 | 2.55 |
| Debris | PiecesDisappearSeconds | 6 | 6 | 6 | 6 |
| Debris | GroundPlaneSizeMeters | 30 | 30 | 30 | 30 |
| Debris | FragmentAngularDamping | 1.5 | 1.5 | 1.5 | 1.5 |
| Debris | ContinuousCollision | 1 | 1 | 1 | 1 |
| PhysX | Substepping | 1 | 1 | 1 | 1 |
| Debug | Log | 1 | 1 | 1 | 1 |

## Notes

- The only key where the shipped ini and the code default differ is
  `StreetLightsFallWhole`: code default 1, shipped ini 0. The PC README says 1.
  Ship the ini: if it is missing, the code default (1) applies.
- The "Android ini before" column is reconstructed from the reported finding
  (the Android ini was identical to the PC ini except `StreetLightsFallWhole=1`).
  The original device file is not in this repo, so this column is not a
  byte-level capture.
- "Android ini now" is the repo `mods/propfix/GTA_Prop_Fix.ini`, byte-identical
  to the PC ini. If your PC ini differs, compare it against this table.
