/*
 * QTC v5.4 - CLI entry point
 * Usage: qtc c|d|bench <file> [outfile]
 */
#define _FILE_OFFSET_BITS 64
#include "qtc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseeko(f, 0, SEEK_END);
    *len = (size_t)ftello(f);
    fseeko(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(*len);
    if (!buf) { fprintf(stderr, "Failed to allocate %zu bytes\n", *len); fclose(f); return NULL; }
    size_t total = 0;
    while (total < *len) {
        size_t chunk = *len - total;
        if (chunk > (1ULL << 30)) chunk = (1ULL << 30);  /* read in 1GB chunks */
        size_t got = fread(buf + total, 1, chunk, f);
        if (got == 0) { perror("fread"); free(buf); fclose(f); return NULL; }
        total += got;
    }
    fclose(f);
    return buf;
}

static int write_file(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    size_t total = 0;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > (1ULL << 30)) chunk = (1ULL << 30);  /* write in 1GB chunks */
        size_t written = fwrite(data + total, 1, chunk, f);
        if (written == 0) { perror("fwrite"); fclose(f); return -1; }
        total += written;
    }
    fclose(f);
    return 0;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void usage(void) {
    fprintf(stderr,
        "QTC-Multi v%s - Multi-Tiling Quasicrystalline Compressor\n"
        "Usage: qtc [options] <command> <file> [outfile]\n"
        "Options:\n"
        "  -f             Fibonacci-only mode (12 golden-ratio tilings)\n"
        "  -n             No-tiling mode (unigrams + escapes only, A/B baseline)\n"
        "  -p5            Period-5 mode (LLSLS periodic tiling, A/B test)\n"
        "                 Default: multi-structure (18 tilings)\n"
        "Commands:\n"
        "  c|compress   <file> [outfile]   Compress file\n"
        "  d|decompress <file> [outfile]   Decompress file\n"
        "  bench        <file>             Compress + decompress + verify\n",
        QTC_VERSION);
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(); return 1; }

    int argi = 1;
    /* Parse options */
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "-f") == 0) {
            extern int qtc_tiling_mode;
            qtc_tiling_mode = QTC_TILING_FIB;
            argi++;
        } else if (strcmp(argv[argi], "-n") == 0) {
            extern int qtc_tiling_mode;
            qtc_tiling_mode = QTC_TILING_NONE;
            argi++;
        } else if (strcmp(argv[argi], "-p5") == 0) {
            extern int qtc_tiling_mode;
            qtc_tiling_mode = QTC_TILING_PERIOD5;
            argi++;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[argi]);
            usage();
            return 1;
        }
    }
    if (argc - argi < 2) { usage(); return 1; }

    const char *cmd = argv[argi];
    const char *infile = argv[argi + 1];
    const char *outfile = (argc - argi > 2) ? argv[argi + 2] : NULL;

    if (strcmp(cmd, "c") == 0 || strcmp(cmd, "compress") == 0) {
        size_t in_len;
        uint8_t *in = read_file(infile, &in_len);
        if (!in) return 1;

        double t0 = now_sec();
        size_t out_len;
        uint8_t *out = qtc_compress(in, in_len, &out_len, true);
        double dt = now_sec() - t0;

        if (!out) { fprintf(stderr, "Compression failed\n"); free(in); return 1; }
        fprintf(stderr, "  Time: %.2fs\n", dt);

        char default_out[4096];
        if (!outfile) {
            snprintf(default_out, sizeof(default_out), "%s.qtc", infile);
            outfile = default_out;
        }
        if (write_file(outfile, out, out_len) < 0) { free(in); free(out); return 1; }
        fprintf(stderr, "  Written: %s\n", outfile);

        free(in); free(out);

    } else if (strcmp(cmd, "d") == 0 || strcmp(cmd, "decompress") == 0) {
        size_t in_len;
        uint8_t *in = read_file(infile, &in_len);
        if (!in) return 1;

        double t0 = now_sec();
        size_t out_len;
        uint8_t *out = qtc_decompress(in, in_len, &out_len, true);
        double dt = now_sec() - t0;

        if (!out) { fprintf(stderr, "Decompression failed\n"); free(in); return 1; }
        fprintf(stderr, "  Time: %.2fs\n", dt);

        char default_out[4096];
        if (!outfile) {
            snprintf(default_out, sizeof(default_out), "%s.out", infile);
            outfile = default_out;
        }
        if (write_file(outfile, out, out_len) < 0) { free(in); free(out); return 1; }
        fprintf(stderr, "  Written: %s (%zu bytes)\n", outfile, out_len);

        free(in); free(out);

    } else if (strcmp(cmd, "bench") == 0) {
        size_t in_len;
        uint8_t *in = read_file(infile, &in_len);
        if (!in) return 1;

        fprintf(stderr, "\n  Benchmarking %s (%zu bytes)\n", infile, in_len);
        fprintf(stderr, "  ──────────────────────────────────\n");

        /* Compress */
        double t0 = now_sec();
        size_t comp_len;
        uint8_t *comp = qtc_compress(in, in_len, &comp_len, false);
        double ct = now_sec() - t0;
        if (!comp) { fprintf(stderr, "Compression failed\n"); free(in); return 1; }

        /* Decompress */
        t0 = now_sec();
        size_t dec_len;
        uint8_t *dec = qtc_decompress(comp, comp_len, &dec_len, false);
        double dt = now_sec() - t0;
        if (!dec) { fprintf(stderr, "Decompression failed\n"); free(in); free(comp); return 1; }

        /* Verify */
        bool ok = (dec_len == in_len && memcmp(dec, in, in_len) == 0);
        fprintf(stderr, "  QTC:  %8zu B (%.1f%%) comp:%.1fs decomp:%.1fs %s\n",
                comp_len, 100.0 * comp_len / in_len, ct, dt,
                ok ? "PASS" : "FAIL");

        free(in); free(comp); free(dec);
        if (!ok) return 1;

    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        usage();
        return 1;
    }

    return 0;
}
