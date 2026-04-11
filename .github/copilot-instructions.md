Enforce good C++ and Vulkan discipline and best practices.  We are targeting Vulkan 1.3 for now.  We are using Microsoft C++ guidelines.

When reviewing code try to consider the broader architectural implications.  Compare the PR to the plans in docs/plans/*.md.
I care more about conceptual correctness and performance than ironclad perfect guards that are irrelevant for overall performance and correctness. 
That said, keep an eye out for potential sources of bugs (race conditions, uninitialized memory)

Pay attention to any added operation may adversely affect performance (extra CPU/GPU cycles and uploads, extra memory). 

Keep an eye out for potentially dead logging/debugging code that can be trimmed.  