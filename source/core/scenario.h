/* Scenario runner: scripted input, screenshots and exit for tests (host and 3DS).
 * File format, one command per line ('#' comments):
 *   quit_after_frames N                 stop the guest after N presented frames
 *   at N key NAME tap|down|up           DIK name from core/input.c (tap = down, up 2 frames later)
 *   at N screenshot FILE.bmp            save the last presented frame
 *   at N log TEXT                       write a marker into the log
 *   expect_frames_min N                 checked at exit: fail if fewer frames were presented
 * Frames are counted by video_frame_count() (one per Flip). */
#ifndef SCENARIO_H
#define SCENARIO_H

#include <stdint.h>

int  scenario_load(const char *path, const char *out_dir);   /* 0 if missing (not an error) */
void scenario_tick(void);                                    /* called from the platform pump */
int  scenario_quit_requested(void);
int  scenario_check(void);                                   /* 1 = all expectations met */

#endif
