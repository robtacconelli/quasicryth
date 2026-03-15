/*
 * QTC - Fibonacci quasicrystal tiling, hierarchy, deep positions
 */
#ifndef QTC_FIB_H
#define QTC_FIB_H

#include <stdint.h>
#include <stdbool.h>
#include "qtc.h"

/* ── Tiling ────────────────────────────────────────── */

/* Generate Fibonacci QC word tiling.
 * Returns malloc'd array of tiles. Caller must free(). */
qtc_tile_t *qc_word_tiling(uint32_t n_words, double phase, uint32_t *out_n_tiles);

bool verify_no_adjacent_S(const qtc_tile_t *tiles, uint32_t n_tiles);

/* ── Hierarchy ─────────────────────────────────────── */

/* Build substitution hierarchy. Returns number of levels built.
 * Allocates hierarchy->levels and hierarchy->parent_maps arrays. */
int build_hierarchy(const qtc_tile_t *tiles, uint32_t n_tiles,
                    int max_levels, qtc_hierarchy_t *hier);

void free_hierarchy(qtc_hierarchy_t *hier);

/* ── Hierarchy context ─────────────────────────────── */

/* Hash hierarchical context to 3-bit integer (0-7). */
uint8_t get_hier_ctx(uint32_t tile_idx, const qtc_hierarchy_t *hier);

/* ── Deep position detection ───────────────────────── */

/* Detect which tiles can attempt n-gram lookup at each level.
 * Returns malloc'd qtc_deep_t. Caller must call free_deep(). */
qtc_deep_t detect_deep_positions(const qtc_tile_t *tiles, uint32_t n_tiles,
                                  const qtc_hierarchy_t *hier);

void free_deep(qtc_deep_t *dp, uint32_t n_tiles);

#endif /* QTC_FIB_H */
