/*
===========================================================================

Rumble.cpp - gamepad rumble mixer (docs/plans/20260924_controller_gunfeel.md C3).

One slot per idCommon::rumbleCategory_t.  A post into a busy slot keeps the stronger
amplitude per motor and restarts the decay, so rapid repeats (chaingun, chainsaw) hold
a steady level instead of stacking.  Slots then sum across categories, so a fire effect
never cancels a simultaneous damage effect.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#include "sys/platform.h"
#include "idlib/Lib.h"
#include "idlib/math/Math.h"
#include "framework/CVarSystem.h"
#include "framework/Common.h"
#include "sys/sys_public.h"

#include "framework/Rumble.h"

idCVar joy_rumbleEnable("joy_rumbleEnable", "1", CVAR_BOOL | CVAR_ARCHIVE, "Enable gamepad rumble");
idCVar joy_rumble("joy_rumble", "1.0", CVAR_FLOAT | CVAR_ARCHIVE,
                  "Gamepad rumble overall strength. 0 also disables rumble", 0.0f, 4.0f);
idCVar joy_rumbleFire("joy_rumbleFire", "1.0", CVAR_FLOAT | CVAR_ARCHIVE, "Rumble scale for weapon fire", 0.0f,
                      4.0f);
idCVar joy_rumbleMelee("joy_rumbleMelee", "1.0", CVAR_FLOAT | CVAR_ARCHIVE, "Rumble scale for melee hits and strikes",
                       0.0f, 4.0f);
idCVar joy_rumbleHit("joy_rumbleHit", "1.0", CVAR_FLOAT | CVAR_ARCHIVE,
                     "Rumble scale for hit confirmation (your shots landing)", 0.0f, 4.0f);
idCVar joy_rumbleDamage("joy_rumbleDamage", "1.0", CVAR_FLOAT | CVAR_ARCHIVE, "Rumble scale for damage taken", 0.0f,
                        4.0f);
idCVar joy_rumbleFloor("joy_rumbleFloor", "0.2", CVAR_FLOAT | CVAR_ARCHIVE,
                       "Lowest nonzero motor level, to get past the motor's dead band. 0 = linear", 0.0f, 0.6f);
idCVar joy_rumbleGamma("joy_rumbleGamma", "0.6", CVAR_FLOAT | CVAR_ARCHIVE,
                       "Motor response curve; below 1 lifts weak effects more than strong ones. 1 = linear", 0.2f,
                       1.0f);
idCVar joy_rumbleLength("joy_rumbleLength", "1.5", CVAR_FLOAT | CVAR_ARCHIVE,
                        "Scales every effect's duration. 1 = original lengths", 0.5f, 4.0f);
idCVar joy_rumbleMinMs("joy_rumbleMinMs", "90", CVAR_INTEGER | CVAR_ARCHIVE,
                       "Shortest effect in ms, so the motor has time to spin up. 0 = no minimum", 0, 300);
idCVar joy_rumbleDebug("joy_rumbleDebug", "0", CVAR_INTEGER,
                       "1 = log each rumble post, 2 = also log every motor update");

// Motors are refreshed at most this often; the sys layer also gives each update a short
// hardware timeout so a stalled frame (level load) cannot leave a motor stuck on.
static const int RUMBLE_MIN_SEND_MS = 16;

struct rumbleSlot_t
{
    float low;
    float hi;
    int startMs;
    int durMs;
};

static rumbleSlot_t s_slots[idCommon::RUMBLE_NUM_CATEGORIES];
static const int RUMBLE_STOP_RESENDS = 4;
static const int RUMBLE_STOP_RESEND_MS = 50;

static bool s_motorsOn = false;
static int s_lastSendMs = 0;
static int s_stopResends = 0;   // extra zero updates still owed after a stop
static bool s_gameLive = false; // last Rumble_Frame verdict; posts outside a live game are dropped

static const char *const s_categoryNames[idCommon::RUMBLE_NUM_CATEGORIES] = {"fire", "melee", "hit", "damage"};

static bool RumbleEnabled(void)
{
    return joy_rumbleEnable.GetBool() && joy_rumble.GetFloat() > 0.0f;
}

static idCVar *CategoryScale(int category)
{
    switch (category)
    {
    case idCommon::RUMBLE_FIRE:
        return &joy_rumbleFire;
    case idCommon::RUMBLE_MELEE:
        return &joy_rumbleMelee;
    case idCommon::RUMBLE_HIT:
        return &joy_rumbleHit;
    default:
        return &joy_rumbleDamage;
    }
}

// Remaining linear-decay fraction of a slot at time now, 0 once expired.
static float SlotEnvelope(const rumbleSlot_t &s, int now)
{
    if (s.durMs <= 0)
        return 0.0f;
    const float t = (float)(now - s.startMs) / (float)s.durMs;
    return t >= 1.0f ? 0.0f : 1.0f - idMath::ClampFloat(0.0f, 1.0f, t);
}

// Maps a mixed 0..1 motor level through the floor + gamma curve; 0 stays 0.
static float MotorCurve(float v)
{
    if (v <= 0.0f)
        return 0.0f;
    const float base = joy_rumbleFloor.GetFloat();
    return base + (1.0f - base) * idMath::Pow(v, joy_rumbleGamma.GetFloat());
}

void Rumble_Post(int category, float low, float hi, int durMs)
{
    if (category < 0 || category >= idCommon::RUMBLE_NUM_CATEGORIES || durMs <= 0)
        return;
    if (!RumbleEnabled() || !s_gameLive)
        return;

    const float scale = CategoryScale(category)->GetFloat();
    low = idMath::ClampFloat(0.0f, 1.0f, low * scale);
    hi = idMath::ClampFloat(0.0f, 1.0f, hi * scale);
    if (low <= 0.0f && hi <= 0.0f)
        return;
    durMs = Max((int)(durMs * joy_rumbleLength.GetFloat()), joy_rumbleMinMs.GetInteger());

    const int now = Sys_Milliseconds();
    rumbleSlot_t &s = s_slots[category];
    const float env = SlotEnvelope(s, now);
    const int remaining = env > 0.0f ? s.startMs + s.durMs - now : 0;

    s.low = Max(low, s.low * env);
    s.hi = Max(hi, s.hi * env);
    s.startMs = now;
    s.durMs = Max(durMs, remaining);

    if (joy_rumbleDebug.GetInteger() >= 1)
    {
        common->Printf("RUMBLE post %-6s in=(%.2f,%.2f,%dms) slot=(%.2f,%.2f,%dms)\n", s_categoryNames[category], low,
                       hi, durMs, s.low, s.hi, s.durMs);
    }
}

// Stops are re-sent a few times so one dropped stop cannot leave a motor running.
static void ResendStops(int now)
{
    if (s_stopResends > 0 && now - s_lastSendMs >= RUMBLE_STOP_RESEND_MS)
    {
        Sys_SetRumble(0, 0, 0);
        s_lastSendMs = now;
        s_stopResends--;
        if (joy_rumbleDebug.GetInteger() >= 2)
            common->Printf("RUMBLE stop resend (%d left)\n", s_stopResends);
    }
}

void Rumble_Frame(bool gameLive)
{
    const int now = Sys_Milliseconds();
    const float master = joy_rumble.GetFloat();
    if (!gameLive || !RumbleEnabled())
    {
        if (gameLive != s_gameLive && joy_rumbleDebug.GetInteger() >= 1)
            common->Printf("RUMBLE game %s\n", gameLive ? "live" : "not live (menu/console/loading)");
        s_gameLive = gameLive;
        // Menu or disabled: stop the motors and drop pending effects, so nothing resumes later.
        Rumble_StopAll();
        ResendStops(now);
        return;
    }
    if (!s_gameLive && joy_rumbleDebug.GetInteger() >= 1)
        common->Printf("RUMBLE game live\n");
    s_gameLive = true;

    float low = 0.0f;
    float hi = 0.0f;
    int active = 0;
    for (int i = 0; i < idCommon::RUMBLE_NUM_CATEGORIES; i++)
    {
        const float env = SlotEnvelope(s_slots[i], now);
        if (env <= 0.0f)
            continue;
        low += s_slots[i].low * env;
        hi += s_slots[i].hi * env;
        active++;
    }
    low = idMath::ClampFloat(0.0f, 1.0f, low * master);
    hi = idMath::ClampFloat(0.0f, 1.0f, hi * master);
    const float rawLow = low;
    const float rawHi = hi;
    low = MotorCurve(low);
    hi = MotorCurve(hi);

    if (active == 0)
    {
        Rumble_StopAll();
        ResendStops(now);
        return;
    }
    if (now - s_lastSendMs < RUMBLE_MIN_SEND_MS)
        return;

    Sys_SetRumble(0, (int)(low * 65535.0f), (int)(hi * 65535.0f));
    s_motorsOn = true;
    s_stopResends = 0;
    s_lastSendMs = now;

    if (joy_rumbleDebug.GetInteger() >= 2)
        common->Printf("RUMBLE motors active=%d low=%.2f hi=%.2f (pre-curve %.2f %.2f)\n", active, low, hi, rawLow,
                       rawHi);
}

void Rumble_StopAll(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    if (s_motorsOn)
    {
        Sys_SetRumble(0, 0, 0);
        s_motorsOn = false;
        s_stopResends = RUMBLE_STOP_RESENDS;
        s_lastSendMs = Sys_Milliseconds();
        if (joy_rumbleDebug.GetInteger() >= 1)
            common->Printf("RUMBLE motors stopped\n");
    }
}
