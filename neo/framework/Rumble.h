/*
===========================================================================

Rumble.h - gamepad rumble mixer.  Game code posts effects through
idCommon::GetAdditionalFunction(FT_Rumble); the mixer sums one slot per
category and drives Sys_SetRumble once per common frame.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#ifndef __RUMBLE_H__
#define __RUMBLE_H__

// Post an effect.  low/hi are motor amplitudes in [0,1]; it decays linearly to 0 over durMs.
// category is an idCommon::rumbleCategory_t.  Safe to call when rumble is disabled.
void Rumble_Post(int category, float low, float hi, int durMs);

// Main thread, once per common frame: mix active effects and update the motors.
// gameLive = false (menu, console, loading) stops the motors and drops pending effects.
void Rumble_Frame(bool gameLive);

// Menu, console, loading or ImGui up = false.  Defined in Common.cpp, which owns that state.
bool Rumble_GameLive(void);

// Kill all effects and stop the motors (shutdown, map change).
void Rumble_StopAll(void);

#endif /* !__RUMBLE_H__ */
