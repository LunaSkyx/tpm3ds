/* Guest-facing input state, in DirectInput terms (DIK scan codes, mouse deltas).
 * Fed by the platform (pal_input_next) and by the scenario runner. */
#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

#define INPUT_BUF 64

typedef struct { uint32_t ofs, data, time, seq; } input_event;   /* = DIDEVICEOBJECTDATA */

void input_reset(void);
void input_key(uint32_t dik, int down);
void input_mouse_move(int32_t dx, int32_t dy);
void input_mouse_button(uint32_t button, int down);

const uint8_t *input_keys(void);                    /* 256 bytes, 0x80 = down */
void input_mouse_take(int32_t *dx, int32_t *dy, int32_t *dz, uint8_t buttons[4]);  /* deltas since last take */
/* Buffered reads; dev 0 = keyboard, 1 = mouse. Returns events copied (and removes them unless peek). */
uint32_t input_take(int dev, input_event *out, uint32_t max, int peek);
void input_set_buffer_size(int dev, uint32_t n);

/* Called on every key state change (the Win32 layer turns it into window messages). */
extern void (*g_input_key_hook)(uint32_t dik, int down);

/* DIK code by name ("RETURN", "ESCAPE", "UP", "A", ...); 0 if unknown. */
uint32_t input_dik_by_name(const char *name);

#endif
