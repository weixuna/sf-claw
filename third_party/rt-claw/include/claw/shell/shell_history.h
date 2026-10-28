/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Shell command history — ring buffer for input recall.
 */

#ifndef CLAW_SHELL_HISTORY_H
#define CLAW_SHELL_HISTORY_H

#define SHELL_HISTORY_MAX   16
#define SHELL_HISTORY_LINE  256

/*
 * Add a line to history. Empty lines are ignored.
 * The most recent entry is at index 0.
 */
void shell_history_add(const char *line);

/*
 * Get a history entry by index (0 = most recent).
 * Returns NULL if index is out of range or slot is empty.
 */
const char *shell_history_get(int index);

/* Return number of entries currently stored. */
int shell_history_count(void);

/* Reset navigation position (call before each new prompt). */
void shell_history_reset_nav(void);

/*
 * Navigate: dir = -1 (older / up), +1 (newer / down).
 * Returns the history line to display, or NULL if at boundary.
 * On the first up-press, saves `current` as scratch so it can
 * be restored when the user navigates back down past index 0.
 */
const char *shell_history_navigate(int dir, const char *current);

#endif /* CLAW_SHELL_HISTORY_H */
