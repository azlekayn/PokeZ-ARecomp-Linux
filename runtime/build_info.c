// The build's label, as the F1 menu's header and the log's first line show it: "b<N> <commit>" - <N> the AppImage's
// build number (tools/linux/package.sh: ZA_BUILD, else the branch's commit count), <commit> with a "+" when the tree
// had uncommitted changes. Only this file is compiled with it (Makefile BUILD_LABEL: rebuilt every time, nothing
// else is), so a new label doesn't rebuild the runtime - nor change GAMELIB_ID.
#ifndef LARECOMP_BUILD
#define LARECOMP_BUILD "dev"
#endif
const char *rt_build_label(void) { return LARECOMP_BUILD; }
