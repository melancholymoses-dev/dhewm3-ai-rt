## Goal

We are refactoring the Doom3/Dhewm3 code base to use Vulkan.
We are implementing raytracing as our goal.

## Pre-existing implementations
There is another Vulkan implementation in ../vkDoom3/neo that we can use to help develop our version
The renderer in neo/renderer/GL is the original version for this repo and works correctly.

When you work, check the existing GL functionality to understand how Doom3 expects things to render.  

## Plans
There are relevant plans in docs/plans/*.md that discuss various refactor efforts and steps.  The main one is rtx_refactor_plan2.md, with the others being steps along the way.

## Procedure
If there are complex options, favour generating logs and debugging over theoretical discussions.  We can figure it out if we can see a log.  This goes for bugs, as well as analyzing graphical errors.

## Assets
The main doom3 assets have been unzipped and are accessible at
../build_rtx/pak_assets/pak000 for inspection.  Most are text files despite the odd file suffixes.  Check this with a head command first.

## Action to Avoid
Do not attempt to configure or build the project.  A separate process will handle that. 