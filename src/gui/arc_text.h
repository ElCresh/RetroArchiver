/* arc_text - the archive's texts about a box, in the window's language:
 * its outcome ("3 clean, 1 marginal"), its media ("3 / 3 floppy") and its
 * state with what to do. In English they are what core/archive.c writes
 * for dskrun; here every word comes from tr(). Portable C (tested with gcc). */
#ifndef RA_ARC_TEXT_H
#define RA_ARC_TEXT_H

#include <stddef.h>
#include <wchar.h>
#include "core/archive.h"

void arc_outcome_w(const arc_box *b, wchar_t *out, size_t n);
void arc_media_w(const arc_box *b, wchar_t *out, size_t n);
void arc_advice_w(const arc_box *b, wchar_t *out, size_t n);

#endif
