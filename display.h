#ifndef DISPLAY_H
#define DISPLAY_H

#include "scanner.h"

/* Render the entry tree to stdout.
 * now: current time for highlight expiry calculation.
 * first_run: if non-zero, suppress green highlighting (initial display).
 * name_width: pre-computed max display width for name+prefix column, or 0 to auto-compute. */
void display_tree(Entry *root, time_t now, int first_run, int name_width);

/* Compute the maximum display width (tree prefix + name) across the tree.
 * Call before display_tree() to determine column alignment. */
int compute_name_width(Entry *root);

#endif
