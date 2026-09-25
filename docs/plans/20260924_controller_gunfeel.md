# Controller gun-feel

Target: snappy stick aim and physical weapon feedback on a gamepad. Doom 3's
gamepad path is the BFG backport in `neo/framework/UsercmdGen.cpp`; it has a
latching rate limiter on by default, an axial deadzone, and a per-axis response
curve. Rumble is a stub (`assert(0)`), aim assist is `#if 0`.

Every assist is individually switchable off. `joy_aimAssist 0` and
`joy_rumble 0` restore stock behaviour with no recompile.

| # | Stage | Scope | Status |
|---|---|---|---|
| C1 | Radial deadzone + magnitude curve + input overlay | framework only | not started |
| C2 | Acceleration model, replaces the latching limiter | framework only | not started |
| C3 | Rumble: `Sys_SetRumble` + effect mixer + 3 game hooks | sys + framework + game/d3xp | not started |
| C4 | Aim assist: friction then adhesion | game/d3xp + `GAME_API_VERSION` bump | not started |

C3 before C4: rumble is self-contained, C4 is the only stage that touches the
game ABI.

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

## C1 — Radial deadzone + magnitude curve

New CVars, legacy names untouched, so `joy_newLook 0` is bit-identical to today
and the menu cursor path in `events.cpp` is unaffected.

| CVar | Default | Meaning |
|---|---|---|
| `joy_newLook` | 1 | 0 = legacy per-axis path (A/B) |
| `joy_lookDeadZone` | 0.12 | Radial inner deadzone |
| `joy_lookOuterDeadZone` | 0.95 | Magnitude that counts as full deflection |
| `joy_lookCurve` | 2.0 | Exponent applied to magnitude |
| `joy_lookCurveBlend` | 0.7 | 0 = linear, 1 = pure exponent |
| `joy_debugInput` | 0 | Overlay |

Changes in `JoystickMove`:

1. `LookStickBound(up, down, left, right)` — returns true only if all four keys
   map to `UB_LOOKUP`/`UB_LOOKDOWN`/`UB_LEFT`/`UB_RIGHT`, recording sign per
   axis. False (custom binds, stick-as-buttons) falls through to the legacy four
   `HandleJoystickAxis` calls.
2. When true and `joy_newLook`, route that stick to a new `JoystickLook(x, y)`
   and **skip `CircleToSquare`** — the circle→square remap exists for
   `forwardmove`/`rightmove` and only inflates diagonal turn rates for look.
3. The move stick keeps `CircleToSquare` and the existing path entirely.

`JoystickLook` math:

```
m = len(v);  if (m <= dzIn) return;
dir = v / m;                                        // direction preserved exactly
m = min(1, (m - dzIn) / (dzOut - dzIn));
s = mix(m, pow(m, joy_lookCurve), joy_lookCurveBlend);
viewangles[YAW]   -= dt * s * dir.x * joy_yawSpeed;
viewangles[PITCH] += dt * s * dir.y * joy_pitchSpeed * invert;
```

Turn-rate retune once the curve is in: `joy_yawSpeed` 240 → 360,
`joy_pitchSpeed` 130 → 250. Both are already `CVAR_ARCHIVE` with a 600 ceiling.

**Check:** `joy_debugInput 1` draws raw stick x/y, post-deadzone magnitude,
shaped magnitude, and final °/s. Roll the stick around the rim at constant
magnitude — °/s must stay flat and the drawn direction must track the stick
1:1. Push at 27° and confirm the output angle is still 27°.

**Exit:** diagonal notch gone, no angular distortion off-cardinal, legacy path
still reachable via `joy_newLook 0`.

---

## C2 — Acceleration model

Replaces `joy_dampenLook`/`joy_deltaPerMSLook` for the `joy_newLook` path. The
old CVars stay for the legacy path.

| CVar | Default | Meaning |
|---|---|---|
| `joy_lookAccelTime` | 100 | ms from centre to full rate |
| `joy_lookAccelBoost` | 1.0 | Rate multiplier once fully ramped (>1 = outer-ring boost) |
| `joy_lookAccelThreshold` | 0.85 | Magnitude above which the ramp engages |

State: one signed ramp scalar per axis on `idUsercmdGenLocal`, replacing
`lastLookValuePitch`/`lastLookValueYaw`.

| Condition | Behaviour |
|---|---|
| `m` above threshold | Ramp rises toward `joy_lookAccelBoost` over `joy_lookAccelTime` |
| `m` below threshold | Ramp decays to 1.0 at the same rate |
| Stick centred | Ramp resets to 1.0 immediately |
| Direction sign flips | Ramp resets to 1.0 |

The last two rows are the latch fix. Reset must also happen in `Clear()`, not
just the constructor.

`joy_lookAccelBoost 1.0` gives no acceleration at all — a pure linear curve,
which is what a player who wants raw output asks for.

Input granularity is `USERCMD_HZ` = 60 ([UsercmdGen.h:43](../../neo/framework/UsercmdGen.h#L43)),
and `usercmd.angles` is short-quantised to 0.0055°. Neither is worth changing;
note them so a later "still not snappy" report gets measured against the right
ceiling.

**Check:** `joy_debugInput 1` gains a ramp-value readout. Flick the stick to a
rim and back repeatedly — the first flick and the tenth must show the same °/s
trace. Reverse direction mid-turn and confirm the ramp drops to 1.0.

**Exit:** no latch, `joy_lookAccelBoost 1.0` measurably linear, flick response
under 120 ms.

---

## C3 — Rumble

`Sys_SetRumble` is a stub that asserts, with zero callers. The
`SDL_GameController*` handle is opened in three places and discarded every time.

| File | Change |
|---|---|
| [events.cpp:886-1022](../../neo/sys/events.cpp#L886) | Retain the opened handle in a static; clear on `SDL_JOYDEVICEREMOVED` |
| [events.cpp:2059](../../neo/sys/events.cpp#L2059) | Implement via `SDL_GameControllerRumble`; add `#define SDL_GameControllerRumble SDL_RumbleGamepad` to the SDL3 shim at [line 95](../../neo/sys/events.cpp#L95) |
| `UsercmdGen.cpp` | Effect mixer, ticked from `idUsercmdGenLocal::GetDirectUsercmd` at 60 Hz |
| `framework/Common.h` + `Common.cpp` | `virtual void Rumble(float low, float hi, int ms)` on `idCommon` so game code can post effects |

Mixer: a small fixed array of active effects, each `{low, hi, startMs, durMs}`
with a linear decay envelope. Per tick, sum and clamp to 1.0, scale by
`joy_rumble`, call `Sys_SetRumble`. Summing rather than replacing is what keeps
a fire effect from cancelling a simultaneous hit effect.

Game hooks — all three already exist and all three must be mirrored into
`neo/d3xp/`:

| Event | Hook | Effect |
|---|---|---|
| Weapon fired | [Player.cpp:3090](../../neo/game/Player.cpp#L3090) `WeaponFireFeedback` | Short, sharp. Reads `rumble_low`/`rumble_hi`/`rumble_ms` from the passed `weaponDef`, falling back to a default |
| Hit confirm | [Player.cpp:7740](../../neo/game/Player.cpp#L7740) `DamageFeedback` | Very short high-freq tick, scaled by `damage` |
| Damage taken | [Player.cpp:7883](../../neo/game/Player.cpp#L7883) `Damage` | Longer low-freq, scaled by `damage` |

`WeaponFireFeedback` already carries the weapon dict for view kick, so
per-weapon strength is def-file data, not a table in code. Shotgun and BFG get
heavy keys; pistol stays light.

| CVar | Default | Meaning |
|---|---|---|
| `joy_rumble` | 1.0 | Master scale. **0 disables all rumble** |
| `joy_rumbleFire` | 1.0 | Weapon-fire scale |
| `joy_rumbleHit` | 1.0 | Hit-confirm scale |
| `joy_rumbleDamage` | 1.0 | Damage-taken scale |

**Check:** `joy_debugInput 1` prints active effect count and summed low/hi.
Fire each weapon and confirm the envelope matches its def keys; `joy_rumble 0`
must leave `Sys_SetRumble` uncalled, not called with zeroes.

**Exit:** all three events felt distinctly, no stuck motor on level load or
weapon switch, master toggle silent.

---

## C4 — Aim assist

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
| Aim response | `joy_newLook`, `joy_lookDeadZone`, `joy_lookOuterDeadZone`, `joy_lookCurve`, `joy_lookCurveBlend`, `joy_yawSpeed`, `joy_pitchSpeed` |
| Acceleration | `joy_lookAccelTime`, `joy_lookAccelBoost`, `joy_lookAccelThreshold` |
| Rumble | `joy_rumble`, `joy_rumbleFire`, `joy_rumbleHit`, `joy_rumbleDamage` |
| Aim assist | `joy_aimAssist` (combo: Off / Friction / Friction + Adhesion), then the five tuning floats |

`joy_gammaLook`, `joy_powerScale`, `joy_deadZone`, `joy_dampenLook` and
`joy_deltaPerMSLook` stay for the legacy path and get a "legacy" label. Retire
them only once `joy_newLook 1` has shipped as default for a while.

---

## Files touched

| Stage | Files |
|---|---|
| C1 | `neo/framework/UsercmdGen.cpp`, `.h`, `Dhewm3SettingsMenu.cpp` |
| C2 | `neo/framework/UsercmdGen.cpp`, `.h`, `Dhewm3SettingsMenu.cpp` |
| C3 | `neo/sys/events.cpp`, `neo/sys/sys_public.h`, `neo/framework/UsercmdGen.cpp`, `Common.h`, `Common.cpp`, `neo/game/Player.cpp`, `neo/d3xp/Player.cpp`, weapon defs, `Dhewm3SettingsMenu.cpp` |
| C4 | `neo/framework/Game.h`, `neo/game/{AimAssist.cpp,.h,Player.cpp,Player.h,Game_local.cpp}`, same five under `neo/d3xp/`, `neo/CMakeLists.txt`, `Dhewm3SettingsMenu.cpp` |

C3 and C4 both need `neo/game/` and `neo/d3xp/` kept in sync. C4 adds new source
files, so `src_game` and `src_d3xp` in `neo/CMakeLists.txt` need updating.
