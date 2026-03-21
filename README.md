<img src="quasicryth_banner.png">

**Aperiodic Structures Never Collapse: Fibonacci Hierarchies for Lossless Compression**

QTC is a lossless text compressor built on 36 aperiodic tilings drawn from multiple irrational-number families. A 10-level Fibonacci substitution hierarchy extracts phrases up to 144 words long, which are then encoded through adaptive arithmetic coding with word-level LZ77. Written in C with no external ML dependencies.

---

## How It Works

1. **Tokenisation and case separation.** Input text is split into word tokens; case information (lower/title/upper) is extracted into a separate stream encoded with order-2 adaptive arithmetic coding.

2. **Codebook construction.** Eleven frequency-ranked codebooks are built across the Fibonacci hierarchy: up to 64K unigrams, 32K bigrams, 32K trigrams, down to 500 entries for 144-grams. Codebook sizes scale automatically with input length.

3. **Multi-structure tiling.** 36 aperiodic tilings are generated via the generalised cut-and-project method:

   ```
   tile(k) = L  if  floor((k+1)*alpha + theta) - floor(k*alpha + theta) = 1
   tile(k) = S  otherwise
   ```

   The 36 tilings come from three families:

   | Family | Count | Alpha | Hierarchy depth |
   |---|---|---|---|
   | Golden-ratio (1/phi) | 12 tilings | 0.6180... | Full 10 levels (144-gram) |
   | Original non-golden (sqrt58, noble5, sqrt13) | 6 tilings | 0.6056--0.6158 | Levels 3--5+ |
   | Optimized alphas (iterative greedy search) | 18 tilings | 0.502--0.619 | Varies; redistribute trigrams to deeper levels |

   Key insight: near-golden optimized alphas do not merely add trigram positions -- they *redistribute* coverage, stealing trigrams to upgrade matches to 13-gram, 21-gram, and 34-gram levels.

4. **Deep substitution hierarchy.** Each tiling's L/S sequence is deflated through 10 levels of Fibonacci substitution (L -> LS, S -> L inverse). At each level, super-L tiles span Fibonacci-many words (3, 5, 8, 13, 21, 34, 55, 89, 144), enabling codebook lookup at that phrase length.

5. **Greedy non-overlapping selection.** Deep matches from all 36 tilings are merged; at each word position the deepest match wins. Three-pass greedy: deep levels first, then bigrams, then unigrams/escapes.

6. **Sequential AC with LZ77.** The parsed event stream undergoes word-level LZ77 (2^22-entry hash table, 4-entry chains), then is encoded with variable-alphabet Fenwick-tree adaptive arithmetic coding at 24-bit precision. Order-2 context conditioning on encoding level, 64-entry MTF recency cache per level, two-tier unigram encoding.

7. **LZMA escape stream.** Out-of-vocabulary words are collected into a separate buffer and compressed with LZMA (preset 9 extreme).

8. **Output format.** Magic `QM56`, 45-byte header, no tiling parameters stored. The decompressor needs no tiling information -- the parsed result is encoded directly in the bitstream. MD5 checksum appended for integrity verification.

---

## Results

### Compression Ratios

| File | Size | QTC Multi | QTC Fib | gzip -9 | bzip2 -9 | xz -9 |
|---|---|---|---|---|---|---|
| alice29.txt | 152 KB | **35.60%** | 35.91% | 35.63% | 28.40% | 31.88% |
| enwik8\_3M | 3 MB | **31.85%** | 32.55% | 36.28% | 28.88% | 28.38% |
| enwik8\_10M | 10 MB | **29.83%** | 30.56% | 36.85% | 29.16% | 27.21% |
| enwik8 | 100 MB | **26.25%** | 27.03% | 36.44% | 29.00% | 24.86% |
| enwik9 | 1 GB | **22.59%** | 23.46% | 32.26% | 25.40% | 21.57% |

QTC beats bzip2 on all benchmarks and approaches xz at scale (gap: 1.02pp on enwik9).

![Compression ratio vs file size](charts/chart4.png)

### Compressed File Breakdown (enwik9)

| Stream | Size | Share |
|---|---|---|
| Payload (AC) | 180,760,315 B | 80.0% |
| Escape words (LZMA) | 24,227,220 B | 10.7% |
| Case data (AC) | 20,397,073 B | 9.0% |
| Codebook (LZMA) | 533,680 B | 0.2% |
| **Total** | **225,918,349 B (22.59%)** | |

### Timing

| File | Compress (Multi) | Decompress (Multi) | C/D ratio |
|---|---|---|---|
| alice29.txt | 0.10s | 0.01s | 10x |
| enwik8\_3M | 2.05s | 0.14s | 15x |
| enwik8\_10M | 11.80s | 0.47s | 25x |
| enwik8 | 120.32s | 4.74s | 25x |
| enwik9 | 1,476s | 44.82s | **33x** |

Decompression throughput: ~22 MB/s. Asymmetric by design -- compression performs tiling search across 36 structures; decompression simply reads the encoded event stream sequentially.

![C/D ratio](charts/chart3.png)

---

## Deep Hierarchy Hits

The core advantage of quasicrystalline tiling is access to deep hierarchy levels that periodic tilings cannot reach. These are the greedy-selected deep phrase matches (13-gram and above) across benchmarks:

![Deep hits scaling](charts/chart1.png)

| File | Words | 13g | 21g | 34g | 55g | 89g | 144g | Total |
|---|---|---|---|---|---|---|---|---|
| alice29.txt | 36K | 9 | -- | -- | -- | -- | -- | 9 |
| enwik8\_3M | 821K | 2,469 | 489 | 98 | 65 | -- | -- | 3,121 |
| enwik8\_10M | 2.8M | 9,400 | 1,553 | 346 | 131 | 8 | 5 | 11,443 |
| enwik8 | 27.7M | 87,344 | 15,736 | 4,118 | 1,464 | 224 | 85 | 108,971 |
| enwik9 | 298.3M | 1,890,784 | 424,084 | 153,713 | 36,776 | 5,544 | 2,026 | 2,512,927 |

At enwik9 scale, over 2.5 million phrase matches span 13 words or more, including 2,026 matches spanning 144 consecutive words each.

![Word-weighted contribution](charts/chart2.png)

---

## Multi-Tiling Contribution

Per-family breakdown of tiling positions on enwik9:

| Family | Tilings | Base positions | Deep (13g+) coverage |
|---|---|---|---|
| Golden (1/phi) | 12 | 132M | Full 10-level hierarchy |
| Original non-golden | 6 | +25M | Levels 3--5+ |
| Optimized alphas | 18 | +4M | Varies by alpha |
| **Total** | **36** | **161M** | **+22% over golden-only** |

Notable per-alpha behaviour:

- **opt-0.502**: adds 2.7M trigram positions but zero 13g+ hits. Pure low-level coverage.
- **opt-0.619**: near-golden alpha that finds matches up to 89-gram depth.
- **Near-golden alphas redistribute**: they steal trigram positions to upgrade coverage to 13g/21g/34g levels.

### A/B Tests

Controlled experiments with identical codebooks, varying only the tiling strategy:

| Comparison | alice29.txt | enwik9 |
|---|---|---|
| Fibonacci vs Period-5 advantage | 999 B | 11,089,469 B |
| Multi vs Fibonacci advantage | 458 B | 8,642,288 B |

The aperiodic advantage grows superlinearly with input size. Period-5 (LLSLS) hierarchy collapses at level 4 -- all tiles become S, yielding zero positions at 13-gram and above. The Fibonacci hierarchy never collapses.

![Aperiodic advantage](charts/chart5.png)

---

## Theoretical Background

The Fibonacci substitution L -> LS, S -> L has substitution matrix with dominant eigenvalue phi (the golden ratio), a Pisot-Vijayaraghavan number. This guarantees:

- **Hierarchy never collapses**: L/S ratio stays exactly phi:1 at every deflation level.
- **Periodic tilings always collapse**: any periodic tiling has rational L-frequency; the second eigenvector component grows as phi^k, driving one tile count to zero within O(log(period)) levels.
- **Equidistribution** (Weyl 1910): for irrational alpha, the sequence {k*alpha mod 1} is equidistributed on [0,1), ensuring uniform n-gram coverage at every scale.
- **Sturmian structure**: the Fibonacci word is balanced and aperiodic with minimal factor complexity (n+1 distinct factors of length n), maximising codebook efficiency.

A detailed formal analysis, including eigenvalue proofs, the three-distance theorem, and information-theoretic bounds on the aperiodic advantage, is available in the accompanying paper: [arXiv:2603.14999](https://arxiv.org/abs/2603.14999).

---

## Build

```bash
cd qtc_c_multi_combined
make
```

Requirements: `gcc`, `zlib` (`-lz`), `bzip2` (`-lbz2`), `liblzma` (`-llzma`).

On Debian/Ubuntu:

```bash
sudo apt install gcc zlib1g-dev libbz2-dev liblzma-dev
```

## Usage

```bash
# Compress (multi-structure, 36 tilings -- default)
./qtc c input.txt output.qtc

# Compress (Fibonacci-only, 12 golden-ratio tilings)
./qtc -f c input.txt output.qtc

# Compress (no-tiling, A/B baseline)
./qtc -n c input.txt output.qtc

# Compress (Period-5, A/B periodic baseline)
./qtc -p5 c input.txt output.qtc

# Decompress
./qtc d output.qtc restored.txt

# Benchmark (compress + decompress + verify round-trip)
./qtc bench input.txt
```

---

## Optimization History

| Change | enwik8 ratio | Delta |
|---|---|---|
| v5.1 baseline | 30.28% | -- |
| Recency cache, context-conditioned indices, order-2 level model, adaptive case encoding | 28.59% | -1.69pp |
| LZMA escapes + adaptive case | 28.09% | -0.50pp |
| LZMA codebook | 28.00% | -0.09pp |
| Word-level LZ77 | 27.06% | -0.94pp |
| Hash-chain (4-entry) + log-scale offset + two-tier unigram | 26.67% | -0.39pp |
| Larger LZ hash (2^22) | 26.53% | -0.14pp |
| Multi-tiling (36 structures) | 26.25% | -0.28pp |
| **Total improvement** | | **-4.03pp** |

---

## Author

Roberto Tacconelli (tacconelli.rob@gmail.com), Independent Researcher

---

## License

See repository root for license information.
