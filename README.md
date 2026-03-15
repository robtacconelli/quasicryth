# Golden Compensation Theorem: Fibonacci Quasicrystals and Lossless Compression

**Quasicryth** *(quasicrystalline tiling hierarchy)* — a lossless text compressor that serves as empirical proof of three novel theorems about Fibonacci quasicrystal hierarchies.

---

## Theoretical Contribution

### Theorem 1 — Golden Compensation

In a Fibonacci quasicrystal tiling of *W* words, the potential word coverage at hierarchy level *m* converges to a **constant independent of depth**:

$$C(m) = P(m) \cdot F_m \;\longrightarrow\; \frac{W\,\varphi}{\sqrt{5}} \approx 0.724\,W$$

The exponential decay in position count (*∝ φ^{-(m-1)}*) is cancelled exactly by the exponential growth in phrase length (*F_m ∝ φ^m*) — a consequence of Binet's formula and the golden-ratio identity *φ² = φ+1*. All structural variation across hierarchy levels is therefore encoded entirely in the codebook hit rate *r(m)*.

### Theorem 2 — Level Activation Threshold

Hierarchy level *m* first contributes useful compression when:

$$W \geq W^*_m = \frac{T_m \cdot \varphi^{m-1}}{r_m}$$

This predicts the corpus scale at which each deep level (89-gram, 144-gram, ...) activates.

### Theorem 3 — Piecewise-Linear Aperiodic Advantage

The advantage of the Fibonacci tiling over any periodic alternative is a **convex, piecewise-linear function** of corpus size, with slope strictly increasing at each activation threshold *W\*_m*. The empirically observed 33× advantage jump from 100 MB to 1 GB is the signature of two new linear terms activating (89-gram, 144-gram), not superlinear growth of existing ones.

---

## Why Fibonacci Hierarchies Never Collapse

All periodic tilings (including Period-5 = LLSLS, the best rational approximant with the same L/S ratio) collapse within *O(log p)* levels: eventually all tiles become S, yielding **zero positions** for deep n-gram lookups.

The Fibonacci quasicrystal never collapses because *φ* is a Pisot-Vijayaraghavan number, the tiling is Sturmian, and Weyl's equidistribution holds at every scale. Period-5 collapses at level 4 — zero 13-gram, 21-gram, ..., 144-gram positions. Fibonacci maintains *O(W/φ^k)* positions at every level forever.

---

## Quasicryth v5.5 — Compressor as Empirical Proof

Quasicryth implements the full Fibonacci substitution hierarchy to depth 9, with phrase lengths {2, 3, 5, 8, 13, 21, 34, 55, 89, 144} words across eleven codebooks. Every deep hit is a position that is **structurally inaccessible to any periodic tiling**.

### Compression Results

| File | Size | Quasicryth | gzip -9 | bzip2 -9 | xz -9 |
|---|---|---|---|---|---|
| alice29.txt | 152 KB | **36.92%** | 35.63% | 28.41% | 31.89% |
| enwik8\_3M | 3 MB | **39.29%** | 36.29% | 28.89% | 28.39% |
| enwik8\_10M | 10 MB | **38.51%** | 36.85% | 29.16% | 27.21% |
| enwik8 | 100 MB | **37.70%** | 36.45% | 29.01% | 24.87% |
| enwik9 | 1 GB | **35.99%** | 32.26% | 25.40% | 21.57% |

### A/B Test: Fibonacci vs Period-5 (identical codebooks)

| File | Period-5 payload | Fibonacci payload | Aperiodic advantage |
|---|---|---|---|
| enwik8\_3M | 802,632 B | 801,260 B | **+1,372 B** |
| enwik8\_10M | 2,659,942 B | 2,654,135 B | **+5,807 B** |
| enwik8 | 26,237,097 B | 26,196,712 B | **+40,385 B** |
| enwik9 | 256,302,106 B | 254,952,735 B | **+1,349,371 B** |

Every byte of advantage comes from deep hierarchy levels (13g–144g) that Period-5 cannot reach.

---

## Charts

### Deep Hierarchy Hits vs Corpus Size
Labels show absolute deep hit counts at each scale.

![Deep hierarchy hits vs corpus size](charts/chart1.png)

### Golden Compensation — Word-Weighted Contribution by Level
Each hit weighted by its phrase length (*hits × n*). The 144-gram contributes 1.1% of covered words at 1 GB despite only 945 hits, because each hit encodes 144 words.

![Word-weighted contribution by n-gram level](charts/chart2.png)

### Asymmetric Compression Profile (C/D Ratio)
Compression is expensive (phase search + n-gram counting); decompression regenerates the tiling deterministically from 2 bytes and is a single sequential pass.

![Compression to decompression ratio](charts/chart3.png)

### QC Tiling Contribution over All-Unigram Baseline
Payload savings attributable to the quasicrystal tiling alone (same codebooks, same escape stream).

![QC savings percentage vs file size](charts/chart4.png)

### Piecewise-Linear Aperiodic Advantage
The dashed line shows linear ∝ file size. The 33× jump at 1 GB is the 89-gram and 144-gram levels crossing their activation threshold — two new linear terms, not superlinear growth.

![Aperiodic advantage Fibonacci vs Period-5](charts/chart5.png)

---

## Deep Hierarchy Hits

| File | Words | 13g | 21g | 34g | 55g | 89g | 144g | Total |
|---|---|---|---|---|---|---|---|---|
| alice29.txt | 36K | 2 | — | — | — | — | — | 2 |
| enwik8\_3M | 821K | 971 | 195 | 77 | — | — | — | 1,243 |
| enwik8\_10M | 2.8M | 2,715 | 383 | 140 | 47 | — | — | 3,285 |
| enwik8 | 27.7M | 24,910 | 3,256 | 1,369 | 551 | — | — | 30,086 |
| enwik9 | 298.3M | 652,124 | 109,492 | 40,475 | 7,222 | 2,396 | 945 | 812,654 |

All deep hits are positions structurally unavailable to Period-5 (collapsed at level 4). 89-gram and 144-gram activate at enwik9 scale, adding new O(W) terms to the aperiodic advantage.

---

## Algorithm Overview

```
Input text
    │
    ├─ Case separation (3-symbol flag stream, AC-coded separately)
    ├─ Word tokenisation
    ├─ Multi-level codebook construction (uni/bi/tri/5g/8g/13g/21g/34g/55g/89g/144g)
    │
    ├─ Phase search (32 candidates, exponential scoring for deep hits)
    │       tile(k) = L  iff  ⌊(k+1+φ)/φ⌋ - ⌊(k+φ)/φ⌋ = 1
    │
    ├─ Deep substitution hierarchy (10 levels, σ⁻¹: LS→super-L, L→super-S)
    │
    ├─ Multi-level AC coding (deepest level first, fallback to bigram/unigram)
    │
    └─ bz2 escape stream (OOV words, ~2.6 bpb vs ~3.5 bpb inline)

Output: [magic|size|phase(2B)|payload(AC)|case|codebook(zlib)|escapes(bz2)|MD5]
```

The **entire tiling structure** — all tile boundaries, hierarchy levels, deep n-gram positions, and model assignments — is implicit in the 2-byte phase value.

---

## Timing (C native implementation, single core)

| File | Compress | Decompress | C/D ratio |
|---|---|---|---|
| alice29.txt (152 KB) | 0.1s | <0.1s | ~5× |
| enwik8\_3M (3 MB) | 1.9s | 0.3s | 6.3× |
| enwik8\_10M (10 MB) | 11.5s | 1.0s | 11.5× |
| enwik8 (100 MB) | 142.0s | 10.0s | 14.2× |
| enwik9 (1 GB) | 1539.7s | 97.0s | 15.9× |

Decompression throughput: ~10 MB/s. Compression: ~0.65 MB/s at 1 GB scale.

---

## Usage

```bash
# Compress
./quasicryth -c input.txt output.qtc

# Decompress
./quasicryth -d output.qtc restored.txt
```

---

## Paper

The full theoretical analysis (Golden Compensation theorem, Activation Threshold theorem, Piecewise-Linear Advantage theorem, proofs via Perron-Frobenius, Pisot-Vijayaraghavan, Sturmian sequences, and Weyl's equidistribution) is in [`quasicryth_paper.pdf`](quasicryth_paper.pdf).

**Author:** Roberto Tacconelli (tacconelli.rob@gmail.com), Independent Researcher
