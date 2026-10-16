// SPDX-License-Identifier: Apache-2.0
//
// Periodic heap health probe.
//
// Static review is not enough to prove there is no leak or fragmentation, so
// the running firmware reports three numbers on a slow cadence:
//
//   free       total free bytes in the internal 8-bit pool
//   largest    largest single allocatable block ("can I still allocate?")
//   min_free   historical low-water mark ("did usage ever drift up?")
//
// A steady decline in free (and in min_free) indicates a leak; a widening gap
// between free and largest indicates fragmentation. Both are logged, and the
// probe warns when fragmentation or the low-water mark crosses a threshold.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Call from a periodic task; it self-limits to HEAP_WATCH_INTERVAL_MS. */
void heap_watch_poll(void);

#ifdef __cplusplus
}
#endif
