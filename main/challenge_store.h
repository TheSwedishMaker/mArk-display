#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * Challenge store — NVS persistence for series medal tracking.
 * Per-user namespaces u0_chall .. u5_chall.
 * Each series is keyed by FNV-1a hash of its name.
 */

void    challenge_store_init(void);
void    challenge_set_active_user(int user_idx);

/* Call when a challenge task transitions completed/uncompleted (active user).
   challenge_complete() returns true if a new medal was just awarded. */
bool    challenge_complete(const char *series, int16_t target);
void    challenge_uncomplete(const char *series);

/* Current progress for a series (active user). Reads live from NVS. */
void    challenge_get_progress(const char *series, int16_t *extra_out, int16_t *medals_out);

/* Total medals for any user index (reads NVS; active user uses cached value). */
int16_t challenge_total_medals(int user_idx);

/* Erase all medal and series data for a user (parent-triggered reset). */
void challenge_reset_user(int user_idx);

/** Call after user_store_remove(idx) to keep NVS namespaces in sync. */
void challenge_shift_down(int removed_idx, int new_count);
