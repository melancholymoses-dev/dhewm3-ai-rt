/*
===========================================================================

AimAssist.cpp - gamepad aim assist for the local player.
docs/plans/completed/20260924_controller_gunfeel.md C4.  Mirrored in neo/d3xp/.

Friction scales this frame's stick-driven turn down while the view is over a target.
Adhesion turns the view by a fraction of the target's angular motion around the eye.
Both run inside idPlayer::UpdateViewAngles, whose UpdateDeltaViewAngles folds the change
into deltaViewAngles, so no game ABI change is needed.  The engine says whether the look
stick (not the mouse) is driving the view through idCommon::FT_GamepadLookActive.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#include "sys/platform.h"
#include "Game_local.h"
#include "ai/AI.h"
#include "Player.h"

#include "AimAssist.h"

idCVar joy_aimAssist("joy_aimAssist", "1", CVAR_GAME | CVAR_ARCHIVE | CVAR_INTEGER,
                     "Gamepad aim assist: 0 = off, 1 = friction (look slows over a target), 2 = friction + "
                     "adhesion (view follows a moving target)",
                     0, 2);
idCVar joy_aimAssistStrength("joy_aimAssistStrength", "0.75", CVAR_GAME | CVAR_ARCHIVE | CVAR_FLOAT,
                             "Aim assist overall strength, scales friction and adhesion", 0.0f, 2.0f);
idCVar joy_aimAssistAngle("joy_aimAssistAngle", "6", CVAR_GAME | CVAR_ARCHIVE | CVAR_FLOAT,
                          "Aim assist cone, degrees from the target's edge", 1.0f, 20.0f);
idCVar joy_aimAssistRange("joy_aimAssistRange", "2000", CVAR_GAME | CVAR_ARCHIVE | CVAR_FLOAT,
                          "Aim assist max target distance", 256.0f, 8192.0f);
idCVar joy_aimAssistFriction("joy_aimAssistFriction", "0.5", CVAR_GAME | CVAR_ARCHIVE | CVAR_FLOAT,
                             "Look-rate reduction over a target (0-1), before strength", 0.0f, 1.0f);
idCVar joy_aimAssistAdhesion("joy_aimAssistAdhesion", "0.5", CVAR_GAME | CVAR_ARCHIVE | CVAR_FLOAT,
                             "Fraction of a target's angular motion the view follows (0-1), before strength", 0.0f,
                             1.0f);
idCVar joy_aimAssistMaxRate("joy_aimAssistMaxRate", "60", CVAR_GAME | CVAR_ARCHIVE | CVAR_FLOAT,
                            "Aim assist adhesion ceiling, degrees per second", 0.0f, 360.0f);
idCVar joy_aimAssistSmallTargets("joy_aimAssistSmallTargets", "0", CVAR_GAME | CVAR_ARCHIVE | CVAR_BOOL,
                                 "Aim assist also on crawler-sized enemies (trites, ticks)");
idCVar joy_aimAssistDebug("joy_aimAssistDebug", "0", CVAR_GAME | CVAR_BOOL,
                          "Draw aim assist candidates (yellow) and target (green), log friction/adhesion");

// Enemies at most this tall (trite/tick bounds are 40) count as small.
static const float AIMASSIST_SMALL_HEIGHT = 48.0f;
// Per-frame view change above this is a snap (teleport, SetViewAngles), not stick input.
static const float AIMASSIST_SNAP_DEG = 30.0f;
// Score bonus that keeps the current target until another is clearly better.
static const float AIMASSIST_HYSTERESIS = 0.25f;

static bool GamepadLookActive(void)
{
    // fetched once per game DLL load; an engine without FT_GamepadLookActive leaves assist off
    static bool (*fn)(void) = NULL;
    static bool fetched = false;
    if (!fetched)
    {
        common->GetAdditionalFunction(idCommon::FT_GamepadLookActive, (idCommon::FunctionPointer *)&fn, NULL);
        fetched = true;
    }
    return fn != NULL && fn();
}

idAimAssist::idAimAssist(void)
{
    Clear();
}

void idAimAssist::Clear(void)
{
    target = NULL;
    lastTargetAngles.Zero();
    haveLastTarget = false;
    lastLogTime = 0;
}

idActor *idAimAssist::SelectTarget(idPlayer *player, const idVec3 &eye, const idVec3 &forward, float &weight,
                                   idVec3 &aimPoint)
{
    const float range = joy_aimAssistRange.GetFloat();
    const float cone = joy_aimAssistAngle.GetFloat();
    const bool debug = joy_aimAssistDebug.GetBool();

    idActor *best = NULL;
    float bestScore = idMath::INFINITY;

    for (idEntity *ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next())
    {
        if (!ent->IsType(idAI::Type))
            continue;
        idActor *actor = static_cast<idActor *>(ent);
        if (actor->health <= 0 || actor->IsHidden() || !actor->fl.takedamage || actor->team == player->team)
            continue;

        const idBounds &b = actor->GetPhysics()->GetAbsBounds();
        const float height = b[1].z - b[0].z;
        if (!joy_aimAssistSmallTargets.GetBool() && height <= AIMASSIST_SMALL_HEIGHT)
            continue;

        // chest height rather than the bounds centre, which sits at the hips
        idVec3 aim = b.GetCenter();
        aim.z = b[0].z + height * 0.6f;
        idVec3 dir = aim - eye;
        const float dist = dir.Normalize();
        if (dist > range || dist < 1.0f)
            continue;

        // angle to the target's edge, so the cone doesn't shrink on close targets
        const float centreDeg = RAD2DEG(idMath::ACos(dir * forward));
        const float radius = 0.5f * Max(b[1].x - b[0].x, b[1].y - b[0].y);
        const float edgeDeg = Max(0.0f, centreDeg - RAD2DEG(idMath::ATan(radius, dist)));
        if (edgeDeg > cone)
            continue;

        float score = edgeDeg / cone;
        if (actor == target.GetEntity())
            score -= AIMASSIST_HYSTERESIS;
        if (score >= bestScore)
            continue;

        trace_t tr;
        gameLocal.clip.TracePoint(tr, eye, aim, MASK_SHOT_RENDERMODEL, player);
        if (tr.fraction < 1.0f && gameLocal.GetTraceEntity(tr) != actor)
            continue;

        if (debug)
            gameRenderWorld->DebugBounds(colorYellow, b);

        best = actor;
        bestScore = score;
        weight = 1.0f - edgeDeg / cone;
        aimPoint = aim;
    }
    return best;
}

void idAimAssist::Apply(idPlayer *player, const idAngles &prevAngles, idAngles &newAngles)
{
    const int mode = joy_aimAssist.GetInteger();
    const float strength = joy_aimAssistStrength.GetFloat();
    if (mode <= 0 || strength <= 0.0f || gameLocal.isMultiplayer || player != gameLocal.GetLocalPlayer() ||
        player->GetInfluenceLevel() != 0 || !GamepadLookActive())
    {
        haveLastTarget = false;
        return;
    }

    const float inPitch = idMath::AngleDelta(newAngles.pitch, prevAngles.pitch);
    const float inYaw = idMath::AngleDelta(newAngles.yaw, prevAngles.yaw);
    if (idMath::Fabs(inPitch) > AIMASSIST_SNAP_DEG || idMath::Fabs(inYaw) > AIMASSIST_SNAP_DEG)
    {
        haveLastTarget = false;
        return;
    }

    const idVec3 eye = player->GetEyePosition();
    float weight = 0.0f;
    idVec3 aimPoint;
    idActor *t = SelectTarget(player, eye, prevAngles.ToForward(), weight, aimPoint);
    if (t == NULL)
    {
        target = NULL;
        haveLastTarget = false;
        return;
    }

    // friction: keep only part of this frame's stick input
    const float friction = idMath::ClampFloat(0.0f, 0.95f, joy_aimAssistFriction.GetFloat() * strength * weight);
    float outPitch = prevAngles.pitch + inPitch * (1.0f - friction);
    float outYaw = prevAngles.yaw + inYaw * (1.0f - friction);

    // adhesion: follow the target's motion around the eye (its movement and the player's strafing)
    const idAngles toTarget = (aimPoint - eye).ToAngles();
    float adhPitch = 0.0f;
    float adhYaw = 0.0f;
    if (mode >= 2 && haveLastTarget && target.GetEntity() == t)
    {
        const float k = idMath::ClampFloat(0.0f, 1.0f, joy_aimAssistAdhesion.GetFloat() * strength) * weight;
        const float maxStep = joy_aimAssistMaxRate.GetFloat() * MS2SEC(gameLocal.msec);
        adhPitch = idMath::ClampFloat(-maxStep, maxStep, idMath::AngleDelta(toTarget.pitch, lastTargetAngles.pitch) * k);
        adhYaw = idMath::ClampFloat(-maxStep, maxStep, idMath::AngleDelta(toTarget.yaw, lastTargetAngles.yaw) * k);
        outPitch += adhPitch;
        outYaw += adhYaw;
    }

    target = t;
    lastTargetAngles = toTarget;
    haveLastTarget = true;

    newAngles.pitch = idMath::AngleNormalize180(outPitch);
    newAngles.yaw = idMath::AngleNormalize180(outYaw);

    if (joy_aimAssistDebug.GetBool())
    {
        gameRenderWorld->DebugBounds(colorGreen, t->GetPhysics()->GetAbsBounds());
        if (gameLocal.time - lastLogTime >= 200)
        {
            lastLogTime = gameLocal.time;
            gameLocal.Printf("AIMASSIST target=%s w=%.2f friction=%.2f in=(%.2f,%.2f) adhesion=(%.2f,%.2f) deg/frame\n",
                             t->GetName(), weight, friction, inYaw, inPitch, adhYaw, adhPitch);
        }
    }
}
