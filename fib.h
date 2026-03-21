/*
 * QTC - Fibonacci quasicrystal tiling, hierarchy, deep positions
 * Extended for multi-tiling: 64 aperiodic structures
 */
#ifndef QTC_FIB_H
#define QTC_FIB_H

#include <stdint.h>
#include <stdbool.h>
#include "qtc.h"

/* ── Tiling ────────────────────────────────────────── */

/* Generate Fibonacci QC word tiling (alpha = 1/phi).
 * Returns malloc'd array of tiles. Caller must free(). */
qtc_tile_t *qc_word_tiling(uint32_t n_words, double phase, uint32_t *out_n_tiles);

/* General cut-and-project tiling with arbitrary alpha in (0,1).
 * Returns malloc'd array of tiles. Caller must free(). */
qtc_tile_t *qc_word_tiling_alpha(uint32_t n_words, double alpha, double phase,
                                  uint32_t *out_n_tiles);

/* Substitution-rule tilings */
qtc_tile_t *gen_thue_morse_tiles(uint32_t n_words, uint32_t *out_n_tiles);
qtc_tile_t *gen_rudin_shapiro_tiles(uint32_t n_words, uint32_t *out_n_tiles);
qtc_tile_t *gen_period_doubling_tiles(uint32_t n_words, uint32_t *out_n_tiles);

/* Period-5 tiling (LLSLS repeated) for A/B testing */
qtc_tile_t *gen_period5_tiles(uint32_t n_words, uint32_t *out_n_tiles);

bool verify_no_adjacent_S(const qtc_tile_t *tiles, uint32_t n_tiles);

/* ── Multi-tiling ─────────────────────────────────── */

/* Tiling descriptor */
typedef struct {
    double alpha;       /* irrational alpha for cut-and-project */
    double phase;       /* phase shift */
    const char *name;   /* human-readable label */
} qtm_tiling_desc_t;

/* Get descriptors for all 64 tilings */
void qtm_get_tiling_descs(qtm_tiling_desc_t descs[QTC_N_TILINGS]);

/* Generate tiling from descriptor (cut-and-project with alpha, phase).
 * Returns malloc'd tiles. Caller must free(). */
qtc_tile_t *qtm_gen_tiling(const qtm_tiling_desc_t *desc, uint32_t n_words,
                            uint32_t *out_n_tiles);

/* ── Hierarchy ─────────────────────────────────────── */

int build_hierarchy(const qtc_tile_t *tiles, uint32_t n_tiles,
                    int max_levels, qtc_hierarchy_t *hier);
void free_hierarchy(qtc_hierarchy_t *hier);

/* ── Hierarchy context ─────────────────────────────── */
uint8_t get_hier_ctx(uint32_t tile_idx, const qtc_hierarchy_t *hier);

/* ── Deep position detection ───────────────────────── */
qtc_deep_t detect_deep_positions(const qtc_tile_t *tiles, uint32_t n_tiles,
                                  const qtc_hierarchy_t *hier);
void free_deep(qtc_deep_t *dp, uint32_t n_tiles);

#endif /* QTC_FIB_H */
