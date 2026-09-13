#pragma once

#include <cstdio>

#ifdef REMOVE_PRINTF
  #define EMU_LOG(...) ((int)0)
#else
  #define EMU_LOG(...) ::printf(__VA_ARGS__)
#endif
