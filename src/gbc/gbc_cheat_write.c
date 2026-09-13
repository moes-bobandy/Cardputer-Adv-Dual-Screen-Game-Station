#include "gnuboy/gnuboy.h"
#include "gnuboy/hw.h"

/* C wrapper so C++ cheat code need not include cpu.h macros (L/F/...). */
void gbc_cheat_writeb(unsigned a, unsigned char b)
{
	writeb(a, (byte)b);
}
