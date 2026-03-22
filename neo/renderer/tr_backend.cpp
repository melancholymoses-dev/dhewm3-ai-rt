/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of
these additional terms immediately following the terms and conditions of the GNU General Public License which
accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software
LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/
#include "sys/platform.h"
#include "sys/sys_imgui.h"

#include "renderer/tr_local.h"
#include "renderer/RendererBackend.h"

// GL state functions (RB_SetDefaultGLState, GL_State, GL_Cull, RB_SwapBuffers, etc.)
// live in GL/gl_backend.cpp. Declarations remain in tr_local.h.

frameData_t *frameData;
backEndState_t backEnd;

/*
====================
RB_ExecuteBackEndCommands

This function will be called syncronously if running without
smp extensions, or asyncronously by another thread.
====================
*/
int backEndStartTime, backEndFinishTime;
void RB_ExecuteBackEndCommands(const emptyCommand_t *cmds)
{
    // r_debugRenderToTexture
    int c_draw3d = 0, c_draw2d = 0, c_setBuffers = 0, c_swapBuffers = 0, c_copyRenders = 0;

    if (cmds->commandId == RC_NOP && !cmds->next)
    {
        return;
    }

    backEndStartTime = Sys_Milliseconds();

    activeBackend->BeginCommandBatch();

    // upload any image loads that have completed
    globalImages->CompleteBackgroundImageLoads();

    for (; cmds; cmds = (const emptyCommand_t *)cmds->next)
    {
        switch (cmds->commandId)
        {
        case RC_NOP:
            break;
        case RC_DRAW_VIEW:
            activeBackend->DrawView((const drawSurfsCommand_t *)cmds);
            if (((const drawSurfsCommand_t *)cmds)->viewDef->viewEntitys)
            {
                c_draw3d++;
            }
            else
            {
                c_draw2d++;
            }
            break;
        case RC_SET_BUFFER:
            activeBackend->SetBuffer(cmds);
            c_setBuffers++;
            break;
        case RC_SWAP_BUFFERS:
            activeBackend->SwapBuffers(cmds);
            c_swapBuffers++;
            break;
        case RC_COPY_RENDER:
            activeBackend->CopyRender(*(const copyRenderCommand_t *)cmds);
            c_copyRenders++;
            break;
        default:
            common->Error("RB_ExecuteBackEndCommands: bad commandId");
            break;
        }
    }

    activeBackend->EndCommandBatch();

    // stop rendering on this thread
    backEndFinishTime = Sys_Milliseconds();
    backEnd.pc.msec = backEndFinishTime - backEndStartTime;

    if (r_debugRenderToTexture.GetInteger() == 1)
    {
        common->Printf("3d: %i, 2d: %i, SetBuf: %i, SwpBuf: %i, CpyRenders: %i, CpyFrameBuf: %i\n", c_draw3d, c_draw2d,
                       c_setBuffers, c_swapBuffers, c_copyRenders, backEnd.c_copyFrameBuffer);
        backEnd.c_copyFrameBuffer = 0;
    }
}
