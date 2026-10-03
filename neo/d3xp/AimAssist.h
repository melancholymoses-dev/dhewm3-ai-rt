/*
===========================================================================

AimAssist.h - gamepad aim assist for the local player (friction + adhesion).
docs/plans/completed/20260924_controller_gunfeel.md C4.  Mirrored in neo/d3xp/.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#ifndef __GAME_AIMASSIST_H__
#define __GAME_AIMASSIST_H__

class idPlayer;
class idActor;

class idAimAssist
{
  public:
    idAimAssist(void);

    void Clear(void);

    // Called from idPlayer::UpdateViewAngles once the usercmd has been applied.  prevAngles is
    // last frame's view, newAngles this frame's; newAngles is adjusted in place.  Gamepad look,
    // single player and the local player only.
    void Apply(idPlayer *player, const idAngles &prevAngles, idAngles &newAngles);

  private:
    idActor *SelectTarget(idPlayer *player, const idVec3 &eye, const idVec3 &forward, float &weight,
                          idVec3 &aimPoint);

    idEntityPtr<idActor> target;
    idAngles lastTargetAngles; // eye-to-aim-point angles last frame, for adhesion
    bool haveLastTarget;
    int lastLogTime;
};

#endif /* !__GAME_AIMASSIST_H__ */
