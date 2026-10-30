# REQUESTS

- Verify on device: pad walk stick sign (forward = -UpDown) and scale (/128, clamp ±1). Unverified.
- Verify on device: `CPad::GetPedWalkLeftRight/UpDown` bool argument. Code passes `true` (`false` returns 0 at 0x546aef4). Confirm the live path.
- Verify on device: CEntity flags bit `0x01` at `+0x30` is UsesCollision. Evidence is sector-list consumers, not a runtime test.
- Fixed: noclip restores collision only on the player's current ped/vehicle (known alive); a vehicle left mid-noclip keeps collision off.
- Shortcut: `CTimer::ms_fTimeStep / 30` is used as frame delta. Confirm against real-time step if movement speed feels wrong.
- Shortcut: camera up vector rebuilt as `right × fwd` in freecam. Confirm the rendered camera is not flipped.
