#ifndef DISPLAY_H
#define DISPLAY_H

#include "scanner.h"
#include "watcher.h"

/* Render the entry tree + deleted entries to stdout.
 * now:        current time for highlight expiry.
 * first_run:  non-zero suppresses highlighting (initial display).
 * name_width: pre-computed max name width, or 0 to auto-compute.
 * deleted:    linked list of recently-deleted entries to show in red. */
void display_tree(Entry *root, time_t now, int first_run,
                  int name_width, DeletedEntry *deleted);

/* Compute the maximum display width (tree prefix + name + deleted names). */
int compute_name_width(Entry *root);

#endif
