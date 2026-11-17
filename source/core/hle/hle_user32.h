/* Window/message glue shared between the USER32 HLE and the input/video HLE. */
#ifndef HLE_USER32_H
#define HLE_USER32_H

#include <stdint.h>

void hle_post_message(uint32_t msg, uint32_t wp, uint32_t lp);
void hle_pump_platform(void);                              /* polls pal input; posts messages */
const uint8_t *hle_vk_state(void);                        /* 256 bytes, 0x80 = down */
void hle_input_init(void);
void hle_key_name(uint32_t scancode, char *out, uint32_t n); /* GetKeyNameText, US layout */

#endif
