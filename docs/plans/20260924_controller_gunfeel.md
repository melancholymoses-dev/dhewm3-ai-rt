# Controller gun-feel

Target: snappy stick aim and physical weapon feedback on a gamepad. Doom 3's
gamepad path is the BFG backport in `neo/framework/UsercmdGen.cpp`; it has a
latching rate limiter on by default, an axial deadzone, and a per-axis response
curve. Rumble is a stub (`assert(0)`), aim assist is `#if 0`.

Every assist is individually switchable off. `joy_aimAssist 0` and
`joy_rumble 0` restore stock behaviour with no recompile.

| # | Stage | Scope | Priority | Status |
|---|---|---|---|---|
| C3 | Rumble: `Sys_SetRumble` + effect mixer + 4 game hooks | sys + framework + game/d3xp | **next** | built 2026-09-30, untested |
| C1 | Radial look stick, legacy curve on the magnitude (`joy_newLook`) | framework only | low | built 2026-09-30, untested |
| C2 | Ramp latch fix, 333 ms ramp kept (`joy_lookRampFix`) | framework only | low | built 2026-09-30, untested |
| C4 | Aim assist: friction only, opt-in | game/d3xp + `GAME_API_VERSION` bump | parked | not started |

## Playtest 2026-09-30 (stock defaults)

| Observation | Plan consequence |
|---|---|
| Low sensitivity, "Resident Evil clunk", doesn't feel bad | Feel is a keeper. C1/C2 fix defects only; drop the 240→360 / 130→250 retune |
| Panic with trites at the ankles (never on m/kb) | Slow pitch (130°/s) + 333 ms ramp is creating horror tension. Preserve it; C4 adhesion would erase it |
| Rumble missing; wanted on flashlight melee | C3 first, with a melee hook |

---

## Existing behaviour (the baseline C1/C2 replace)

| Where | What it does now |
|---|---|
| [UsercmdGen.cpp:1042](../../neo/framework/UsercmdGen.cpp#L1042) `JoystickMove` | Calls `CircleToSquare` on **both** sticks, then dispatches each stick as 4 independent scalars |
| [UsercmdGen.cpp:889](../../neo/framework/UsercmdGen.cpp#L889) `HandleJoystickAxis` | Per-axis deadzone, per-axis `Pow` curve, integrates into `viewangles[]` |
| [UsercmdGen.cpp:963](../../neo/framework/UsercmdGen.cpp#L963) `joy_dampenLook` | Rate-limits `lookValue` to +0.003/ms → 333 ms to full |
| [events.cpp:1747](../../neo/sys/events.cpp#L1747) | Duplicate deadzone for `joyAxis[]`, **menu cursor only** |
| [events.cpp:1707](../../neo/sys/events.cpp#L1707) | Hardcoded 50% digital threshold for stick-as-button |

Three defects, all confirmed by reading:

- **Axial deadzone.** `joy_deadZone` 0.25 is tested per axis, so the dead region
  is a square — 0.35 radius on the diagonal.
- **Per-axis curve.** `pow(x,k)/pow(y,k) = (x/y)^k` rotates the output toward the
  nearest cardinal axis. At `joy_powerScale 2`, a 27° push aims at 14°.
- **The limiter latches.** `lastLookValueYaw` is written only inside the pressed
  branch and initialised only in the constructor ([line 520](../../neo/framework/UsercmdGen.cpp#L520)).
  It never decays on release, and left/right share one variable, so reversing
  direction inherits the opposite direction's ramp.

Each stick direction is a separate virtual key (`K_JOY_STICK2_UP` …) bound
through `idKeyInput::GetUsercmdAction`. Nothing in the current code sees both
axes of one stick as a vector.

---

## C1 — Radial look stick

**Built 2026-09-30, not yet validated in-game.** All in `neo/framework/UsercmdGen.cpp`.

| CVar | Default | Meaning |
|---|---|---|
| `joy_newLook` | 1 | **Toggle.** 0 = legacy per-axis path, unchanged |
| `joy_lookDeadZone` | 0.25 | Radial inner deadzone (= legacy `joy_deadZone`, so cardinal speeds match) |
| `joy_lookOuterDeadZone` | 0.95 | Magnitude that counts as full deflection |
| `joy_debugInput` | 0 | Logs ~10 lines/s while the right stick is off-centre |

| Change | Where |
|---|---|
| `LookStickSigns`: a stick takes the new path only if its 4 directions are bound to exactly `_lookUp/_lookDown/_left/_right` (either way round). Anything else stays legacy | `JoystickMove` |
| `JoystickLook`: radial deadzone → rescale → **the existing curve** (`joy_gammaLook`/`joy_powerScale`) on the magnitude → split by the unit direction. No `CircleToSquare` | new |
| Legacy curve moved into `JoystickShapeLook`, shared by both paths | `HandleJoystickAxis` |
| GUI fake mouse still gets the `CircleToSquare` values | `JoystickMove` |

No new curve cvars: reusing the legacy curve on the magnitude reproduces today's cardinal
speeds exactly (`joy_gammaLook 1` = 0.01·100^v, so half deflection is 10% speed). Diagonals
are no longer inflated: the legacy path gave ~1.4× speed at full diagonal.

**Check:** `joy_debugInput 1`. Roll the stick around the rim: `inDeg` and `outDeg` should
match and `m` should stay flat. A/B `joy_newLook 0/1` on a cardinal push: same `yaw=`/`pitch=`.

---

## C2 — Look ramp latch fix

**Built 2026-09-30, not yet validated in-game.** The 333 ms ramp (`joy_dampenLook 1`,
`joy_deltaPerMSLook 0.003`) is kept; only the latch is fixed. The boost/acceleration model
was dropped.

| CVar | Default | Meaning |
|---|---|---|
| `joy_lookRampFix` | 1 | **Toggle.** 0 = original latch, bit-identical |

| Path | Ramp state | Reset when (fix on) |
|---|---|---|
| Legacy | `rampPitch`, `rampYaw` (replace `lastLookValuePitch/Yaw`) | axis not pushed this poll; direction sign flips; `Clear()` |
| `joy_newLook` | `rampLook` on the magnitude, plus last direction | stick inside deadzone; direction turns > 90°; `Clear()` |

Behaviour change: the latch made every flick after the first hit full speed instantly. With
the fix, every flick ramps over 333 ms. If that reads slower than the playtest feel, raise
`joy_deltaPerMSLook` or set `joy_lookRampFix 0`.

`USERCMD_HZ` = 60 and short-quantised `usercmd.angles` (0.0055°) are the input ceiling; not
changed.

**Check:** `joy_debugInput 1`. Flick to the rim repeatedly: the `ramp=` trace climbs from 0
the same way every time. Reverse mid-turn: `ramp=` drops to 0.

---

## C3 — Rumble

**Built 2026-09-30, not yet validated in-game.**

| File | Change |
|---|---|
| `neo/sys/events.cpp` | `rumbleGamepad` = last pad opened (set in `setGamepadType`, which every open site calls); closed on `SDL_JOYDEVICEREMOVED` if detached. `Sys_SetRumble` → `SDL_GameControllerRumble` with a 150 ms hardware timeout; no-op if `in_useGamepad 0`. SDL3 shim gains `Close`/`GetAttached`/`Rumble` |
| `neo/framework/Rumble.cpp/.h` (new) | Mixer + `joy_rumble*` cvars. Ticked by `Rumble_Frame()` on the main thread after `session->Frame()` |
| `neo/framework/Common.h/.cpp` | `idCommon::FT_Rumble` + `rumbleCategory_t`, served through `GetAdditionalFunction`. **No `GAME_API_VERSION` bump**; an older engine returns false and the game stays silent |
| `neo/framework/Session.cpp`, `Common.cpp` | `Rumble_StopAll()` on map load and shutdown |
| `neo/{game,d3xp}/Game_local.*` | `Game_Rumble(category, low, hi, ms)` wrapper; fn pointer fetched in `Init` |
| `Dhewm3SettingsMenu.cpp` | "Gamepad Rumble" block under Gamepad Settings |
| `neo/CMakeLists.txt` | `framework/Rumble.cpp` |

Mixer: one slot per category. A post into a busy slot keeps the stronger amplitude per motor
and restarts the linear decay, so chaingun and chainsaw repeats hold a level instead of stacking.
Slots sum across categories, then clamp and scale by `joy_rumble`. Motors are refreshed at most
every 16 ms; the 150 ms timeout stops them if frames stall.

| Event | Hook | Effect (low, hi, ms) |
|---|---|---|
| Weapon fired | `idPlayer::WeaponFireFeedback`; local player, `isNewFrame` | `rumble_low/hi/ms` keys, else low = `recoilTime/360` (pistol .35, shotgun/rocket .9), hi .35, ms = ⅔·recoilTime. No recoil (plasma, BFG, grenade): .2/.35/70 |
| Melee, creature | `idWeapon::Event_Melee`, before the hit sound | `rumble_hit_*` keys on the meleeDef, else low = damage·berserk/50 (fists .4, flashlight .8, chainsaw 1), hi .3, 180 ms |
| Melee, other surface | same; only when a strike sound plays (`nextStrikeFx` throttle) | `rumble_strike_*`, else .15/.45/60 |
| Hit confirm | `idPlayer::DamageFeedback`; skipped when `inflictor == this` (melee) | 0 / .1+dmg/150 / 50 |
| Damage taken | `idPlayer::Damage`, beside `DamageImpulse` | sized by damage + armorSave: .2+d/40, d/60, 100+8d ms (≤450) |

| CVar | Default | Meaning |
|---|---|---|
| `joy_rumbleEnable` | 1 | Master on/off. **0 disables all rumble** |
| `joy_rumble` | 1.0 | Overall strength (0–2); 0 also disables |
| `joy_rumbleFire` | 1.0 | Weapon-fire scale |
| `joy_rumbleMelee` | 1.0 | Melee hit/strike scale |
| `joy_rumbleHit` | 1.0 | Hit-confirm scale |
| `joy_rumbleDamage` | 1.0 | Damage-taken scale |
| `joy_rumbleDebug` | 0 | 1 = log posts, 2 = also log motor updates |

**Check:** `joy_rumbleDebug 2`. Fire each weapon and read the posted values; `joy_rumble 0`
must produce no `RUMBLE` lines at all. Flashlight-melee a zombie, then a wall, then the air:
thump, tap, nothing. Chainsaw a zombie: steady `slot=` values, not climbing.

**Exit:** all events felt distinctly, no stuck motor on level load or weapon switch, master
toggle silent.

**Playtest bug 2026-09-30:** a motor started in the menu and ran until exit. Root cause not
yet identified. Changes:

| Change | Where |
|---|---|
| `Rumble_Frame(gameLive)`: not live = `sessLocal.guiActive`, loading, console, or an ImGui menu. Not live → stop, drop pending effects, ignore posts | `Common.cpp`, `Rumble.cpp` |
| Every stop is re-sent 4× at 50 ms | `Rumble.cpp` |
| `joy_rumbleDebug 1` logs live/not-live transitions, stops and any SDL failure (`SDL_GetError`); `2` logs every SDL call | `Rumble.cpp`, `events.cpp` |

If it recurs, capture `joy_rumbleDebug 2`. A run of `RUMBLE sdl ... ok` lines with nonzero values
means something keeps posting; `FAILED` or `skipped` lines mean the stop never reached the pad.

**Follow-ups:** BFG and plasma use the no-recoil default. If they feel weak, give them
`rumble_*` keys (def-file mod) or derive strength from the projectile's damage.

---

## C4 — Aim assist

**Parked 2026-09-30.** The stock pad's scramble against close, fast, low enemies is
part of the horror now. If revived: friction only (`joy_aimAssist 1`), default 0,
and exclude targets below the eye-pitch cone so trites stay a panic. Adhesion is
not planned.

Two mechanisms, staged, both off by default until tuned:

- **Friction** — scale look rate down while the reticle is near a valid target.
  This is what the dead `GetAimAssistSensitivity` hook at
  [UsercmdGen.cpp:936](../../neo/framework/UsercmdGen.cpp#L936) was for.
- **Adhesion** — actively steer the reticle toward a moving target, proportional
  to the target's screen-space velocity, only while the stick is being moved.

Adhesion is the one that reads as "Destiny". It is also the one that feels like
cheating if overdone, so it ships second and separately gated.

### ABI

`idGame` gains two virtuals; `GAME_API_VERSION` goes 9 → 10
([Game.h:391](../../neo/framework/Game.h#L391)). Both `neo/game/Game_local.cpp`
and `neo/d3xp/Game_local.cpp` must implement them — the game is a real DLL
boundary unless `HARDLINK_GAME` is set ([CMakeLists.txt:1719](../../neo/CMakeLists.txt#L1719)).

```
virtual float GetAimAssistSensitivity() = 0;       // friction, 1.0 = none
virtual void  GetAimAssistAngle( idAngles &out ) = 0;  // adhesion, zero = none
```

### Game side

New `game/AimAssist.cpp/.h` (mirrored to `d3xp/`), owned by `idPlayer`, updated
once per think before `UpdateViewAngles`:

1. Candidate set: entities in the PVS within `joy_aimAssistRange`, inside
   `joy_aimAssistAngle` of the view axis, `health > 0`, and reachable by a trace
   from the eye. Reuse the existing `idAI::GetAimDir` visibility pattern
   ([AI.cpp:4592](../../neo/game/ai/AI.cpp#L4592)).
2. Score by angular distance, prefer the previous frame's target with hysteresis
   so the assist does not flick between two adjacent enemies.
3. Friction: scale by angular distance, full effect on-target, none at the cone
   edge.
4. Adhesion: project target velocity to screen space, emit the angular delta
   needed to track it, clamped to `joy_aimAssistMaxRate`.

Adhesion must be applied through `deltaViewAngles` via `UpdateDeltaViewAngles`,
not by writing `viewAngles` — the player derives `viewAngles` from the absolute
`usercmd.angles` every frame ([Player.cpp:5684](../../neo/game/Player.cpp#L5684)),
so a direct write is overwritten on the next tick.

### Gating

| CVar | Default | Meaning |
|---|---|---|
| `joy_aimAssist` | 0 | **0 = off entirely.** 1 = friction only, 2 = friction + adhesion |
| `joy_aimAssistRange` | 2000 | Max target distance |
| `joy_aimAssistAngle` | 8 | Cone half-angle, degrees |
| `joy_aimAssistFriction` | 0.5 | Look-rate scale on target |
| `joy_aimAssistAdhesion` | 0.4 | Adhesion strength |
| `joy_aimAssistMaxRate` | 60 | °/s ceiling on adhesion |
| `joy_aimAssistDebug` | 0 | Overlay |

Hard gates beyond the CVar: no assist when `gameLocal.isMultiplayer` (it is
client-side and unverifiable), no assist for mouse input, none while
`influenceActive` or in a cinematic. Default 0 means a fresh install has stock
aim until the player opts in.

**Check:** `joy_aimAssistDebug 1` draws the cone, every candidate, the selected
target with its score, and the friction scale + adhesion °/s as text. Tune from
that overlay, not by feel. `joy_aimAssist 0` must produce a bit-identical
`usercmd` stream to a build without C4 — verify by recording a demo with the
feature compiled in but disabled.

**Exit:** tracking a strafing Imp at mid range feels assisted but not
magnetic; `joy_aimAssist 0` provably inert; both `game/` and `d3xp/` build.

---

## Settings menu

All new CVars go in the Controls section of
[Dhewm3SettingsMenu.cpp](../../neo/framework/Dhewm3SettingsMenu.cpp), extending
the existing `joy_*` block at [line 1824](../../neo/framework/Dhewm3SettingsMenu.cpp#L1824).

| Group | Entries |
|---|---|
| Look stick | `joy_newLook`, `joy_lookDeadZone`, `joy_lookOuterDeadZone`, `joy_dampenLook`, `joy_deltaPerMSLook`, `joy_lookRampFix` (new "Gamepad Look Stick" heading) |
| Rumble | `joy_rumbleEnable`, `joy_rumble`, `joy_rumbleFire`, `joy_rumbleMelee`, `joy_rumbleHit`, `joy_rumbleDamage` |
| Aim assist | `joy_aimAssist` (combo: Off / Friction / Friction + Adhesion), then the five tuning floats |

`joy_gammaLook`, `joy_powerScale`, `joy_deadZone`, `joy_dampenLook` and
`joy_deltaPerMSLook` stay for the legacy path and get a "legacy" label. Retire
them only once `joy_newLook 1` has shipped as default for a while.

---

## Files touched

| Stage | Files |
|---|---|
| C1, C2 | `neo/framework/UsercmdGen.cpp`, `Dhewm3SettingsMenu.cpp` |
| C3 | `neo/sys/events.cpp`, `neo/framework/{Rumble.cpp,Rumble.h,Common.h,Common.cpp,Session.cpp,Dhewm3SettingsMenu.cpp}`, `neo/{game,d3xp}/{Game_local.h,Game_local.cpp,Player.cpp,Weapon.cpp}`, `neo/CMakeLists.txt` |
| C4 | `neo/framework/Game.h`, `neo/game/{AimAssist.cpp,.h,Player.cpp,Player.h,Game_local.cpp}`, same five under `neo/d3xp/`, `neo/CMakeLists.txt`, `Dhewm3SettingsMenu.cpp` |

C3 and C4 both need `neo/game/` and `neo/d3xp/` kept in sync. C4 adds new source
files, so `src_game` and `src_d3xp` in `neo/CMakeLists.txt` need updating.
