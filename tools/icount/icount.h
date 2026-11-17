/* Instruction counters for the host icount build (tools/icount). */
#include <stdint.h>
extern uint32_t *g_icnt;
#define ICNT(a) ((void)(g_icnt[(a) - 0x400000u]++))
