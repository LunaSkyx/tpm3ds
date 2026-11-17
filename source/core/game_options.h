#ifndef GAME_OPTIONS_H
#define GAME_OPTIONS_H
/* Modes the original contains but never enables (see game_options.c). Call
 * after the exe is loaded, before it runs. Returns 1 if applied. */
int game_option_apply(const char *name);
#endif
