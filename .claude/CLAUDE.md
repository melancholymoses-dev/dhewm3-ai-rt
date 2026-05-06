## Goal

We are refactoring the Doom3/Dhewm3 code base to use Vulkan.
We are implementing raytracing as our goal.

## Pre-existing implementations
There is another Vulkan implementation in ../vkDoom3/neo that we can use to help develop our version
The renderer in neo/renderer/GL is the original version for this repo and works correctly.

When you work, check the existing GL functionality to understand how Doom3 expects things to render.  

## Plans
There are relevant plans in docs/plans/*.md that discuss various refactor efforts and steps.  The main one is rt_refactor_plan5.md, with the others being steps along the way.

## Procedure
If there are complex options, favour generating logs and debugging over theoretical discussions.  We can figure it out if we can see a log.  This goes for bugs, as well as analyzing graphical errors.  For graphical/GPU debugging suggest a way of outputting color coding for the relevant objects to allow visual inspection.

When adding new shaders update CMakelists.  For glsl files add them to the GLSL_INCLUDES.

## Assets
The main doom3 assets have been unzipped and are accessible at
build_rt/pak_assets/pak000 for inspection.  Most are text files despite the odd file suffixes.  Check this with a head command first.

neo/game/ and neo/d3xp/ are near-identical; changes must be mirrored across both.  
Game is for the base game, d3xp is the expansion pack.

## Action to Avoid
Do not attempt to configure or build the project.  A separate process will handle that. 

## Copyright Notices

When adding a new file include a comment at the top describing the file.
Then add this block to demarcate new additions so it is clear what our changes are.
```
This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
```