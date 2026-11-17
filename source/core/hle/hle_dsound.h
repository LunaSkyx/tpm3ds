/* DirectSound HLE: the shared IDirectSound object (also handed out by Miles). */
#ifndef HLE_DSOUND_H
#define HLE_DSOUND_H

#include "core/gmem.h"

gptr hle_dsound_object(void);
void hle_dsound_init(void);

#endif
