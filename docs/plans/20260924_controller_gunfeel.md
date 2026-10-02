# Controller gun-feel

Target: snappy stick aim and physical weapon feedback on a gamepad. Doom 3's
gamepad path is the BFG backport in `neo/framework/UsercmdGen.cpp`; it has a
latching rate limiter on by default, an axial deadzone, and a per-axis response
curve. Rumble is a stub (`assert(0)`), aim assist is `#if 0`.

Every assist is individually switchable off. `joy_aimAssist 0` and
`joy_rumble 0` restore stock behaviour with no recompile.

| # | Stage | Scope | Priority | Status |
|---|---|---|---|---|
| C3 | Rumble: `Sys_SetRumble` + effect mixer + 4 game hooks; C3b monster footsteps | sys + framework + game/d3xp | **next** | built 2026-09-30, untested |
| C1 | Radial look stick, legacy curve on the magnitude (`joy_newLook`) | framework only | low | built 2026-09-30, untested |
| C2 | Ramp latch fix, 333 ms ramp kept (`joy_lookRampFix`) | framework only | low | built 2026-09-30, untested |
| C4 | Aim assist: friction + optional adhesion (`joy_aimAssist`) | framework + game/d3xp, no ABI bump | | built 2026-09-30, untested |
| C5 | D-pad weapon groups, multi-press cycles (`pad_weapGroups`); C5b centre-screen selector (`pad_weapOverlay`) | game/d3xp + pad cfgs + new .gui | | built 2026-10-01, untested |
| C6 | Radial weapon wheel | framework + game/d3xp + new .gui | parked | not planned for now |

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
| `joy_rumble` | 1.0 | Overall strength (0–4); 0 also disables |
| `joy_rumbleFire` | 1.0 | Weapon-fire scale |
| `joy_rumbleMelee` | 1.0 | Melee hit/strike scale |
| `joy_rumbleHit` | 1.0 | Hit-confirm scale |
| `joy_rumbleDamage` | 1.0 | Damage-taken scale |
| `joy_rumbleLength` | 1.5 | Scales every effect's duration |
| `joy_rumbleMinMs` | 90 | Shortest effect, so the motor spins up (after `Length`) |
| `joy_rumbleFloor` | 0.2 | Lowest nonzero motor level, gets past the motor's dead band |
| `joy_rumbleGamma` | 0.6 | Motor curve `floor + (1−floor)·v^gamma` after mixing; <1 lifts weak effects, 1.0 stays 1.0 |
| `joy_rumbleIdleMs` | 5000 | No gamepad button or >25% stick/trigger push for this long → treated as not live (no rumble). 0 = never. Source: `Sys_LastGamepadInputMs` in `events.cpp` |
| `joy_rumbleDebug` | 0 | 1 = log posts, 2 = also log motor updates (with pre-curve values) |

Playtest 2026-09-30: Xbox One pad needed `joy_rumble 2`, which already saturated shotgun/flashlight/chainsaw
at 1.0. Curve + length added instead of a bigger multiplier. Linear original: `Floor 0`, `Gamma 1`,
`Length 1`, `MinMs 0`. Cap raised to 4× on all strength cvars.

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

### C3b — Big-monster footsteps

**Built 2026-10-01, not yet validated in-game.** Walk anims play steps as frame commands
(`sound_body snd_footstep`, hell knight adds `sound_body2 snd_deepfs`, pinky `snd_handstep`).

| Change | Where |
|---|---|
| `RUMBLE_STEP` appended to `rumbleCategory_t`; older engines reject it via the bounds check | `neo/framework/Common.h` |
| `joy_rumbleSteps` scale, `"step"` debug name | `neo/framework/Rumble.cpp`, `Dhewm3SettingsMenu.cpp` |
| `FC_SOUND_BODY`/`BODY2` with a `snd_*` key → `idAI::StepRumble(key)` | `neo/{game,d3xp}/anim/Anim_Blend.cpp` |
| `StepRumble`: step keys only; strength/range from a classname-prefix table, overridden by `rumble_step` / `rumble_step_range` def keys. Local player, single player, on the ground. Low motor, 200 ms, × (1 − dist/range)² | `neo/{game,d3xp}/ai/AI.{h,cpp}` |

| Monster prefix | Strength | Range |
|---|---|---|
| `monster_boss_cyberdemon`, `monster_boss_guardian` | 1.0 | 3000 |
| `monster_demon_mancubus`, `monster_demon_d3xp_bruiser` | 0.6 | 1500 |
| `monster_demon_hellknight` | 0.5 | 1500 |
| `monster_demon_pinky` | 0.175 (halved after playtest 2026-10-01) | 1000 |

No save-game fields: the table is read on each step. Hell knight's double sound on one frame is
two posts into one slot; the mixer keeps the max.

**Check:** `joy_rumbleDebug 1`, `spawn monster_demon_hellknight`: `RUMBLE post step` per footfall,
values falling as it walks away, none while jumping. `joy_rumbleSteps 0`: no step posts.

**Follow-ups:** BFG and plasma use the no-recoil default. If they feel weak, give them
`rumble_*` keys (def-file mod) or derive strength from the projectile's damage.

---

## C4 — Aim assist

**Built 2026-09-30, not yet validated in-game.** Unparked: with C1 the stick moves freely
off-axis, so holding a line on a target is harder than with the old axis-snapped aim.

**No game ABI change.** Everything runs inside `idPlayer::UpdateViewAngles`, which already ends
with `UpdateDeltaViewAngles(viewAngles)`, so any change to `viewAngles` folds into
`deltaViewAngles` and persists. The engine contributes one function through
`GetAdditionalFunction`: `FT_GamepadLookActive`, true while the look stick (not the mouse)
moved the view in the last 100 ms. Mouse aim is never assisted.

| File | Change |
|---|---|
| `neo/framework/UsercmdGen.cpp/.h` | `lastPadLookMs` (look stick past deadzone, both paths), `lastMouseLookMs`; `Usercmd_GamepadLookActive()` |
| `neo/framework/Common.h/.cpp` | `FT_GamepadLookActive` |
| `neo/{game,d3xp}/AimAssist.cpp/.h` (new) | `idAimAssist`: target selection, friction, adhesion, cvars |
| `neo/{game,d3xp}/Player.h/.cpp` | `aimAssist` member; `Apply(this, prevViewAngles, viewAngles)` after the usercmd is applied |
| `neo/CMakeLists.txt` | `AimAssist.cpp` in `src_game` and `src_d3xp` |
| `Dhewm3SettingsMenu.cpp` | "Gamepad Aim Assist": mode combo, strength, crawler toggle |

**Target selection** (every frame while active): `idAI`, alive, visible (`TracePoint` from the eye), takes damage,
other team, within range, and the angle from view to the target's **edge** ≤ the cone. Aim point is
60% of bounds height (chest). Score = edge angle / cone; the current target gets a 0.25 bonus.
Weight = 1 − edge/cone (1 anywhere on the silhouette).

| Mechanism | Per frame |
|---|---|
| Friction | stick input × (1 − friction·strength·weight), capped at 95% reduction |
| Adhesion (mode 2) | + Δ(eye→target angles since last frame) × adhesion·strength·weight, clamped to max rate |

Skipped for: multiplayer, non-local player, any influence level, mouse input, and frames that
change the view by more than 30° (teleport/`SetViewAngles` snaps).

| CVar | Default | Meaning |
|---|---|---|
| `joy_aimAssist` | 1 | **Toggle.** 0 off, 1 friction, 2 friction + adhesion |
| `joy_aimAssistStrength` | 0.75 | Overall strength (0–2), scales both |
| `joy_aimAssistSmallTargets` | 0 | Assist on crawlers (bounds ≤ 48 tall: trites, ticks). Off keeps the ankle-panic |
| `joy_aimAssistAngle` | 6 | Cone, degrees from the target's edge |
| `joy_aimAssistRange` | 2000 | |
| `joy_aimAssistFriction` | 0.5 | |
| `joy_aimAssistAdhesion` | 0.5 | |
| `joy_aimAssistMaxRate` | 60 | °/s adhesion ceiling |
| `joy_aimAssistDebug` | 0 | Yellow bounds = candidates that passed the cone + trace, green = target; 5 log lines/s |

**Check:** `joy_aimAssistDebug 1`. Sweep the stick across a zombie: the log shows `friction`
rising toward 0.5 at the centre and `in=` shrinking. Mode 2, strafing imp: nonzero `adhesion=`.
Mouse only: no `AIMASSIST` lines. Trites: never green unless `joy_aimAssistSmallTargets 1`.

**Exit:** tracking a strafing imp at mid range feels assisted, not magnetic; `joy_aimAssist 0`
is inert (returns before touching the angles).

---

## C5 — D-pad weapon groups

**Built 2026-10-01, not yet validated in-game.** Commit sets `idealWeapon` directly (`CommitWeaponSel`), bypassing `SelectWeapon` toggle-back and d3xp weapon toggles. Multiplayer commits immediately. `idCommonLocal::InitGame` (`neo/framework/Common.cpp`) now auto-execs `gamepad.cfg`/`gamepad-d3xp.cfg` (picked by `fs_game`) after `default.cfg` on every launch, so the D-pad binds apply with no manual `exec` step; a saved rebind still overrides it.

Each D-pad direction holds a group of up to 4 weapons. Pressing it selects the group; each further
press steps to the next weapon in it. The stock pad cfgs bind the D-pad to 4 single weapons only.

| Change | Where |
|---|---|
| `IMPULSE_30..33` = weapon group 0..3. Free in both games (d3xp uses 25 and 27) | `neo/framework/UsercmdGen.h` |
| `CycleWeaponGroup(g)` from `PerformImpulse`. Group lists hold classnames, resolved with `SlotForWeapon`; missing classnames are skipped | `neo/{game,d3xp}/Player.{h,cpp}` |
| Step rule: if `idealWeapon` (or the pending pick) is in the group, go to the next owned weapon with ammo (`HasAmmo` or `weaponN_allowempty`), wrapping. Otherwise go to the group's last-used weapon, else its first available one | same |
| Commit delay: hold the pick as `pendingWeapon`, then call `SelectWeapon` once `pad_weapCommitMs` passes with no press. 0 = select on every press | same, ticked in `idPlayer::Think` |
| HUD: `UpdateHudWeapon` highlights `pendingWeapon` when one is set, then fires `weaponChange`. Uses the existing `Weapon0..11` strip in `hud.gui`; no new art | same |
| Binds: `JOY_DPAD_*` → `_impulse30..33` | `base/gamepad.cfg`, `base/gamepad-d3xp.cfg` |
| Overshoot fix: `CycleWeaponGroup(g, dir)` takes a direction; `weapSelActiveGroup` remembers which group a D-pad press opened. While the selector is up (`weapSelPending` set or before `weapSelHideTime`), the prev/next-weapon shoulder buttons (`IMPULSE_14/15`) call `StepActiveWeaponGroup(±1)` to step back/forward within that group instead of the global weapon cycle; they fall back to `NextWeapon`/`PrevWeapon` once it's closed | `neo/{game,d3xp}/Player.{h,cpp}` |

| CVar | Default | Meaning |
|---|---|---|
| `pad_weapGroups` | 1 | **Toggle.** 0 = impulses 30–33 do nothing (bind the D-pad back to stock) |
| `pad_weapGroup0..3` | see below | Space-separated classnames, cycle order |
| `pad_weapCommitMs` | 250 | Commit delay, 0 = immediate |
| `pad_weapGroupShoulderStep` | 1 | **Toggle.** 0 = shoulder buttons always do the global weapon cycle, never step within a group |
| `pad_weapDebug` | 0 | Log group, candidates, skip reason (not owned / no ammo), commit |

| Dir | Base default | d3xp default |
|---|---|---|
| Left (0) | fists, chainsaw, flashlight | + `weapon_grabber` |
| Up (1) | pistol, shotgun, machinegun | same (+ double-barrel if separate class) |
| Right (2) | chaingun, plasmagun, rocketlauncher | same |
| Down (3) | handgrenade, bfg, soulcube | soulcube → `weapon_bloodstone_passive` |

The d3xp classnames are not verified; d3xp `player.def` is not in the extracted paks. Check with
`pad_weapDebug 1`, which logs unresolved classnames at spawn. Toggle weapons (flashlight, soul cube)
toggle only on a re-press of the same slot; cycling always passes a different slot.

**Check:** `pad_weapDebug 1`, `give all`. Tap Up 3× quickly: one switch to machinegun, and the HUD strip
steps pistol→shotgun→machinegun. Empty the shotgun: Up skips it. `pad_weapCommitMs 0`: every press switches.
Overshoot: tap Up to machinegun, then tap the left-shoulder button — `WEAPSEL group 1 dir -1` in the
log, stepping back to shotgun. Right shoulder steps forward again. Let the selector fade, then shoulder
taps should do the normal global weapon cycle instead.

### C5b — Centre-screen selector (HL2 style)

**Built 2026-10-01, not yet validated in-game.** Layout is generated (64×44 cells, 640×480 space); cell vars `wsel_G_N_{vis,icon,ammo,sel,empty}` + `wsel_alpha`. Unowned weapons are hidden and owned ones pack toward the centre. Icons from a classname table in `Player.cpp`, else the def's `icon` key. `neo/CMakeLists.txt` `deploy_base_overrides` now copies `base/guis`.

Cross layout around the crosshair, one arm per D-pad direction. Each arm shows its group's icons, the
pending pick highlighted, and its ammo count; weapons with no ammo are dimmed red, unowned ones hidden.

| Change | Where |
|---|---|
| New `guis/weapsel.gui`: 4 arms × 4 slot windows, `background "gui::wsel_G_N_icon"` (same pattern as `hud.gui`'s `gui::itemicon`), HUD fonts for ammo, `onNamedEvent` show/fade | `base/guis/weapsel.gui` (loose) |
| Icons: `guis/assets/hud/icons/*w.tga`. Widths vary (pistol 64×64, BFG 128×64): fixed-height boxes, width from a `wsel_G_N_wide` flag | — |
| Per slot: icon, owned, ammo text, empty, highlight; plus `wsel_alpha`. Ammo per weapon from `idInventory` (`AmmoIndexForWeaponClass`, clip) | `neo/{game,d3xp}/Player.cpp` |
| Load with `uiManager->FindGui` next to the HUD; draw from `DrawHUD` after `_hud->Redraw` | same |
| Visibility: open on a group press, hold while `pendingWeapon` is set, fade over `pad_weapOverlayFadeMs` after commit. Hidden with the HUD (`g_showHud 0`), in cinematics, PDA, dead | same |

| CVar | Default | Meaning |
|---|---|---|
| `pad_weapOverlay` | 1 | **Toggle.** 0 = the HUD strip only |
| `pad_weapOverlayFadeMs` | 1000 | Hold + fade after commit |

d3xp: grabber and artifact icons not located yet; until found, those slots fall back to the
weapon def's `icon` key.

**Check:** `give all`, tap Up 3×: the overlay appears, the highlight steps along the Up arm, then fades ~1 s
after the switch. Empty the shotgun: its icon goes red and the cycle skips it. `g_showHud 0`: no overlay.

---

## C6 — Radial weapon wheel (parked)

Parked: C5 covers the need. About 2–3× the work, mostly input routing.

| Piece | Work |
|---|---|
| Input | While the wheel button is held, `UsercmdGen` stops look from the right stick and passes the stick direction in `usercmd.mx/my` (already the in-world GUI cursor path). Clear the look ramp on close to avoid a snap |
| GUI | New `guis/weaponwheel.gui`: 12 fixed-angle `windowDef`s, icons from `guis/assets/hud/icons/*w.tga`. Shipped loose or in our pk4 |
| Game | Fill state vars for each slot (owned, ammo from `idInventory`, highlight); draw from `DrawHUD`; select on release with C5's ownership/ammo rules |
| Options | Slow time while open (single player); off in multiplayer |

---

## Settings menu

All new CVars go in the Controls section of
[Dhewm3SettingsMenu.cpp](../../neo/framework/Dhewm3SettingsMenu.cpp), extending
the existing `joy_*` block at [line 1824](../../neo/framework/Dhewm3SettingsMenu.cpp#L1824).

| Group | Entries |
|---|---|
| Look stick | `joy_newLook`, `joy_lookDeadZone`, `joy_lookOuterDeadZone`, `joy_dampenLook`, `joy_deltaPerMSLook`, `joy_lookRampFix` (new "Gamepad Look Stick" heading) |
| Rumble | `joy_rumbleEnable`, `joy_rumble`, `joy_rumbleFire`, `joy_rumbleMelee`, `joy_rumbleHit`, `joy_rumbleDamage`, `joy_rumbleSteps`, `joy_rumbleLength`, `joy_rumbleFloor`, `joy_rumbleGamma`, `joy_rumbleMinMs` |
| Aim assist | `joy_aimAssist` (combo: Off / Friction / Friction + Adhesion), `joy_aimAssistStrength`, `joy_aimAssistSmallTargets`. Tuning floats are console-only |
| Weapon groups | `pad_weapGroups`, `pad_weapCommitMs`, `pad_weapOverlay`, `pad_weapGroupShoulderStep`. Group lists and fade time are console-only |

`joy_gammaLook`, `joy_powerScale`, `joy_deadZone`, `joy_dampenLook` and
`joy_deltaPerMSLook` stay for the legacy path and get a "legacy" label. Retire
them only once `joy_newLook 1` has shipped as default for a while.

---

## Files touched

| Stage | Files |
|---|---|
| C1, C2 | `neo/framework/UsercmdGen.cpp`, `Dhewm3SettingsMenu.cpp` |
| C3 | `neo/sys/events.cpp`, `neo/framework/{Rumble.cpp,Rumble.h,Common.h,Common.cpp,Session.cpp,Dhewm3SettingsMenu.cpp}`, `neo/{game,d3xp}/{Game_local.h,Game_local.cpp,Player.cpp,Weapon.cpp}`, `neo/CMakeLists.txt`; C3b adds `neo/{game,d3xp}/{ai/AI.h,ai/AI.cpp,anim/Anim_Blend.cpp}` |
| C4 | `neo/framework/{UsercmdGen.cpp,UsercmdGen.h,Common.h,Common.cpp,Dhewm3SettingsMenu.cpp}`, `neo/{game,d3xp}/{AimAssist.cpp,AimAssist.h,Player.h,Player.cpp}`, `neo/CMakeLists.txt` |
| C5 | `neo/framework/{UsercmdGen.h,Dhewm3SettingsMenu.cpp}`, `neo/{game,d3xp}/{Player.h,Player.cpp}`, `base/{gamepad.cfg,gamepad-d3xp.cfg}`; C5b adds `base/guis/weapsel.gui`, `neo/CMakeLists.txt` (deploy `base/guis`) |

C3 and C4 both need `neo/game/` and `neo/d3xp/` kept in sync. C4 adds new source
files, so `src_game` and `src_d3xp` in `neo/CMakeLists.txt` need updating.
