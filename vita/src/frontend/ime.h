// The system's on-screen keyboard (IME dialog). See ime.c (ADR-232)
#ifndef RSTV_IME_H
#define RSTV_IME_H

// a line of text from the on-screen keyboard (title, the text it starts with): 1 = entered (out holds it), 0 = cancelled
int rstv_ime_input(const char *title, const char *initial, char *out, int out_size);

#endif
