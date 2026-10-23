# eval-w7  (2026-10-21T22:01:16Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4,5, smt on, build `compiler=tools/wtc-cc, ClangIR 1ebf89d8c5e6;wtc=63c2ba7213ad (dirty);flavour w7_generic_plain_e8=-O2 --no-specialize -DW7_ELEM=E8;flavour w7_generic_batch_e8=-O2 --no-specialize -DW7_ELEM=E8 -DW7_BATCH;flavour w7_generic_hand_e8=-O2 --no-specialize -DW7_ELEM=E8 -DW7_HAND_BATCH;flavour w7_spsc_plain_e8=-O2 --no-specialize -DW7_TAG=1 -DW7_ELEM=E8;flavour w7_spsc_batch_e8=-O2 --no-specialize -DW7_TAG=1 -DW7_ELEM=E8 -DW7_BATCH;flavour w7_spsc_hand_e8=-O2 --no-specialize -DW7_TAG=1 -DW7_ELEM=E8 -DW7_HAND_BATCH;flavour w7_inline_plain_e8=-O2 -DW7_ELEM=E8;flavour w7_inline_batch_e8=-O2 -DW7_ELEM=E8 -DW7_BATCH;flavour w7_inline_hand_e8=-O2 -DW7_ELEM=E8 -DW7_HAND_BATCH;flavour w7_generic_plain_e64=-O2 --no-specialize -DW7_ELEM=E64;flavour w7_generic_batch_e64=-O2 --no-specialize -DW7_ELEM=E64 -DW7_BATCH;flavour w7_generic_hand_e64=-O2 --no-specialize -DW7_ELEM=E64 -DW7_HAND_BATCH;flavour w7_spsc_plain_e64=-O2 --no-specialize -DW7_TAG=1 -DW7_ELEM=E64;flavour w7_spsc_batch_e64=-O2 --no-specialize -DW7_TAG=1 -DW7_ELEM=E64 -DW7_BATCH;flavour w7_spsc_hand_e64=-O2 --no-specialize -DW7_TAG=1 -DW7_ELEM=E64 -DW7_HAND_BATCH;flavour w7_inline_plain_e64=-O2 -DW7_ELEM=E64;flavour w7_inline_batch_e64=-O2 -DW7_ELEM=E64 -DW7_BATCH;flavour w7_inline_hand_e64=-O2 -DW7_ELEM=E64 -DW7_HAND_BATCH;flavour w7_generic_batch_k1_e64=-O2 --no-specialize -DW7_ELEM=E64 -DW7_BATCH -DWTC_BATCH_K=1;flavour w7_generic_hand_k1_e64=-O2 --no-specialize -DW7_ELEM=E64 -DW7_HAND_BATCH -DWTC_BATCH_K=1;flavour w7_spsc_batch_k1_e64=-O2 --no-specialize -DW7_TAG=1 -DW7_ELEM=E64 -DW7_BATCH -DWTC_BATCH_K=1;flavour w7_spsc_hand_k1_e64=-O2 --no-specialize -DW7_TAG=1 -DW7_ELEM=E64 -DW7_HAND_BATCH -DWTC_BATCH_K=1;flavour w7_inline_batch_k1_e64=-O2 -DW7_ELEM=E64 -DW7_BATCH -DWTC_BATCH_K=1;flavour w7_inline_hand_k1_e64=-O2 -DW7_ELEM=E64 -DW7_HAND_BATCH -DWTC_BATCH_K=1;flavour w7_generic_batch_k4_e64=-O2 --no-specialize -DW7_ELEM=E64 -DW7_BATCH -DWTC_BATCH_K=4;flavour w7_generic_hand_k4_e64=-O2 --no-specialize -DW7_ELEM=E64 -DW7_HAND_BATCH -DWTC_BATCH_K=4;flavour w7_spsc_batch_k4_e64=-O2 --no-specialize -DW7_TAG=1 -DW7_ELEM=E64 -DW7_BATCH -DWTC_BATCH_K=4;flavour w7_spsc_hand_k4_e64=-O2 --no-specialize -DW7_TAG=1 -DW7_ELEM=E64 -DW7_HAND_BATCH -DWTC_BATCH_K=4;flavour w7_inline_batch_k4_e64=-O2 -DW7_ELEM=E64 -DW7_BATCH -DWTC_BATCH_K=4;flavour w7_inline_hand_k4_e64=-O2 -DW7_ELEM=E64 -DW7_HAND_BATCH -DWTC_BATCH_K=4;flavour w7_generic_batch_k16_e64=-O2 --no-specialize -DW7_ELEM=E64 -DW7_BATCH -DWTC_BATCH_K=16;flavour w7_generic_hand_k16_e64=-O2 --no-specialize -DW7_ELEM=E64 -DW7_HAND_BATCH -DWTC_BATCH_K=16;flavour w7_spsc_batch_k16_e64=-O2 --no-specialize -DW7_TAG=1 -DW7_ELEM=E64 -DW7_BATCH -DWTC_BATCH_K=16;flavour w7_spsc_hand_k16_e64=-O2 --no-specialize -DW7_TAG=1 -DW7_ELEM=E64 -DW7_HAND_BATCH -DWTC_BATCH_K=16;flavour w7_inline_batch_k16_e64=-O2 -DW7_ELEM=E64 -DW7_BATCH -DWTC_BATCH_K=16;flavour w7_inline_hand_k16_e64=-O2 -DW7_ELEM=E64 -DW7_HAND_BATCH -DWTC_BATCH_K=16;flavour w7_generic_batch_k256_e64=-O2 --no-specialize -DW7_ELEM=E64 -DW7_BATCH -DWTC_BATCH_K=256;flavour w7_generic_hand_k256_e64=-O2 --no-specialize -DW7_ELEM=E64 -DW7_HAND_BATCH -DWTC_BATCH_K=256;flavour w7_spsc_batch_k256_e64=-O2 --no-specialize -DW7_TAG=1 -DW7_ELEM=E64 -DW7_BATCH -DWTC_BATCH_K=256;flavour w7_spsc_hand_k256_e64=-O2 --no-specialize -DW7_TAG=1 -DW7_ELEM=E64 -DW7_HAND_BATCH -DWTC_BATCH_K=256;flavour w7_inline_batch_k256_e64=-O2 -DW7_ELEM=E64 -DW7_BATCH -DWTC_BATCH_K=256;flavour w7_inline_hand_k256_e64=-O2 -DW7_ELEM=E64 -DW7_HAND_BATCH -DWTC_BATCH_K=256;flavour w7_generic_batch_k1024_e64=-O2 --no-specialize -DW7_ELEM=E64 -DW7_BATCH -DWTC_BATCH_K=1024;flavour w7_generic_hand_k1024_e64=-O2 --no-specialize -DW7_ELEM=E64 -DW7_HAND_BATCH -DWTC_BATCH_K=1024;flavour w7_spsc_batch_k1024_e64=-O2 --no-specialize -DW7_TAG=1 -DW7_ELEM=E64 -DW7_BATCH -DWTC_BATCH_K=1024;flavour w7_spsc_hand_k1024_e64=-O2 --no-specialize -DW7_TAG=1 -DW7_ELEM=E64 -DW7_HAND_BATCH -DWTC_BATCH_K=1024;flavour w7_inline_batch_k1024_e64=-O2 -DW7_ELEM=E64 -DW7_BATCH -DWTC_BATCH_K=1024;flavour w7_inline_hand_k1024_e64=-O2 -DW7_ELEM=E64 -DW7_HAND_BATCH -DWTC_BATCH_K=1024;`, git 63c2ba7213ad

Control (shim_generic E64, median of 3 and 3): open 1.11, close 1.10 Mops/s, drift 0.4% — OK

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.000, rep 2 1.000, rep 3 1.000, rep 4 1.000, rep 5 1.000, rep 6 1.000, rep 7 1.000, rep 8 1.000, rep 9 1.000 (n=87) — no trend.  Widest single row: generic_batch_k256_e64 in W7 closed  E64, 42% across repetitions.

## W7 closed  E64

| variant | n | Mops/s | ns/op | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|
| inline_hand_k16_e64 | 10 | 24.97 [24.96–24.98] | 40.04 | 143,783,000 | 236,825,500 | 257,499,000 | 259,514,000 | 259,746,500 |
| inline_hand_k1_e64 | 10 | 24.97 [24.96–24.98] | 40.04 | 136,684,000 | 225,298,500 | 244,915,500 | 246,923,500 | 247,199,500 |
| inline_batch_k16_e64 | 10 | 24.97 [24.95–24.98] | 40.04 | 143,869,500 | 236,919,000 | 257,704,000 | 259,769,500 | 260,025,500 |
| inline_hand_k4_e64 | 10 | 24.97 [24.96–24.97] | 40.05 | 141,950,000 | 233,783,000 | 254,293,500 | 256,394,000 | 256,623,500 |
| inline_hand_e64 | 10 | 24.97 [24.96–24.98] | 40.05 | 143,694,500 | 235,794,500 | 256,474,000 | 258,488,000 | 258,713,000 |
| inline_batch_e64 | 10 | 24.97 [24.96–24.98] | 40.05 | 138,678,000 | 230,678,000 | 251,411,000 | 253,561,000 | 253,786,000 |
| inline_plain_e64 | 10 | 24.97 [24.96–24.98] | 40.05 | 144,495,000 | 238,212,000 | 259,197,000 | 261,372,500 | 261,624,500 |
| inline_batch_k4_e64 | 10 | 24.97 [24.96–24.98] | 40.05 | 142,352,000 | 233,957,500 | 254,440,500 | 256,529,000 | 256,767,000 |
| inline_batch_k1_e64 | 10 | 24.97 [24.95–24.98] | 40.05 | 136,702,500 | 224,991,500 | 244,802,000 | 246,833,000 | 247,102,500 |
| inline_hand_k256_e64 | 10 | 24.97 [24.95–24.98] | 40.05 | 144,031,500 | 236,953,500 | 257,765,000 | 259,828,500 | 260,036,500 |
| inline_batch_k256_e64 | 10 | 24.96 [24.94–24.97] | 40.06 | 144,164,500 | 237,455,500 | 258,269,500 | 260,352,000 | 260,615,000 |
| inline_batch_k1024_e64 | 10 | 24.94 [24.93–24.95] | 40.09 | 144,738,500 | 237,947,500 | 258,634,500 | 260,760,500 | 261,033,000 |
| inline_hand_k1024_e64 | 10 | 24.94 [24.93–24.95] | 40.09 | 144,775,500 | 237,783,000 | 258,559,000 | 260,621,000 | 260,851,000 |
| spsc_batch_k4_e64 | 10 | 3.70 ⚠ [3.45–3.74] | 271 | 1,977 | 2,948 | 6,483 | 12,248 | 51,015 |
| spsc_hand_k16_e64 | 10 | 3.69 ⚠ [3.43–3.76] | 271 | 4,674 | 5,891 | 9,470 | 16,670 | 78,158 |
| spsc_batch_k16_e64 | 10 | 3.61 ⚠ [3.50–3.71] | 277 | 5,063 | 6,286 | 10,248 | 15,897 | 127,761 |
| spsc_hand_k4_e64 | 10 | 3.56 ⚠ [3.42–3.70] | 281 | 2,103 | 3,216 | 7,261 | 15,434 | 78,194 |
| spsc_batch_e64 | 10 | 3.49 ⚠ [3.32–3.71] | 287 | 18,218 | 20,249 | 24,432 | 33,551 | 296,538 |
| spsc_plain_e64 | 10 | 3.48 ⚠ [3.29–3.57] | 287 | 1,738 | 2,839 | 5,745 | 13,415 | 87,174 |
| spsc_batch_k1_e64 | 10 | 3.39 ⚠ [3.30–3.58] | 295 | 1,594 | 2,662 | 5,019 | 11,465 | 70,822 |
| spsc_hand_k1_e64 | 10 | 3.38 ⚠ [3.30–3.48] | 296 | 1,842 | 2,883 | 5,711 | 12,959 | 84,140 |
| spsc_hand_e64 | 10 | 3.33 ⚠ [3.28–3.51] | 300 | 18,740 | 20,873 | 24,967 | 30,674 | 109,075 |
| spsc_hand_k256_e64 | 10 | 3.29 ⚠ [3.21–3.37] | 304 | 74,480 | 80,477 | 88,525 | 105,096 | 147,094 |
| spsc_hand_k1024_e64 | 10 | 3.28 ⚠ [3.23–3.45] | 304 | 277,948 | 307,634 | 320,384 | 329,280 | 363,039 |
| spsc_batch_k256_e64 | 10 | 3.28 ⚠ [3.26–3.44] | 305 | 74,790 | 80,726 | 87,913 | 100,574 | 341,090 |
| spsc_batch_k1024_e64 | 10 | 3.25 ⚠ [3.16–3.45] | 307 | 283,530 | 311,710 | 322,743 | 332,344 | 725,469 |
| generic_hand_e64 | 10 | 1.99 ⚠ [1.89–2.08] | 502 | 32,226 | 34,712 | 38,599 | 42,881 | 94,327 |
| generic_batch_k1024_e64 | 10 | 1.96 ⚠ [1.82–2.17] | 510 | 485,220 | 518,875 | 527,586 | 532,477 | 557,030 |
| generic_hand_k256_e64 | 10 | 1.95 ⚠ [1.78–2.14] | 512 | 126,642 | 132,014 | 136,408 | 141,528 | 192,664 |
| generic_hand_k1024_e64 | 10 | 1.92 ⚠ [1.79–2.07] | 520 | 502,596 | 529,752 | 537,822 | 542,342 | 559,899 |
| generic_batch_e64 | 10 | 1.90 ⚠ [1.88–2.09] | 525 | 33,282 | 35,481 | 40,456 | 44,909 | 97,580 |
| generic_batch_k16_e64 | 10 | 1.84 ⚠ [1.82–1.93] | 542 | 9,114 | 12,489 | 17,640 | 24,215 | 66,576 |
| generic_batch_k256_e64 | 10 | 1.83 ⚠ [1.72–1.95] | 546 | 137,721 | 143,736 | 150,720 | 157,341 | 204,460 |
| generic_hand_k16_e64 | 10 | 1.83 ⚠ [1.80–1.88] | 546 | 9,016 | 11,123 | 16,735 | 23,103 | 72,550 |
| generic_hand_k4_e64 | 10 | 1.65 ⚠ [1.60–1.70] | 607 | 3,994 | 6,895 | 12,368 | 22,406 | 82,192 |
| generic_batch_k4_e64 | 10 | 1.64 ⚠ [1.46–1.74] | 610 | 4,295 | 8,260 | 13,180 | 25,138 | 96,722 |
| generic_batch_k1_e64 | 10 | 1.09 ⚠ [1.04–1.11] | 917 | 2,797 | 5,645 | 11,451 | 18,487 | 103,340 |
| generic_hand_k1_e64 | 10 | 1.07 ⚠ [1.03–1.11] | 932 | 2,760 | 4,995 | 10,480 | 16,204 | 69,950 |
| generic_plain_e64 | 10 | 1.03 ⚠ [1.01–1.06] | 970 | 3,031 | 7,657 | 13,280 | 21,063 | 81,793 |

## W7 closed  E8

| variant | n | Mops/s | ns/op | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|
| inline_plain_e8 | 10 | 26.17 [26.16–26.18] | 38.21 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| inline_hand_e8 | 10 | 26.16 [26.14–26.17] | 38.23 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| inline_batch_e8 | 10 | 26.16 [26.14–26.17] | 38.23 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| spsc_batch_e8 | 10 | 3.82 ⚠ [3.66–3.86] | 262 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| spsc_hand_e8 | 10 | 3.69 [3.65–3.79] | 271 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| spsc_plain_e8 | 10 | 3.61 ⚠ [3.37–3.68] | 277 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| generic_hand_e8 | 10 | 1.89 ⚠ [1.69–2.04] | 529 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| generic_batch_e8 | 10 | 1.84 ⚠ [1.80–2.01] | 543 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| generic_plain_e8 | 10 | 1.04 ⚠ [1.01–1.08] | 961 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |

## W7 open  E64  rate=1,000,000/s

| variant | n | Mops/s | ns/op | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|
| inline_batch_k1024_e64 | 10 | 1.00 [1.00–1.00] | 999 | 531,982 | 925,014 | 1,013,525 | 1,022,010 | 1,025,775 |
| inline_hand_k1024_e64 | 10 | 1.00 [1.00–1.00] | 999 | 532,204 | 924,976 | 1,013,470 | 1,022,680 | 1,061,770 |
| spsc_batch_k1024_e64 | 10 | 1.00 [1.00–1.00] | 999 | 604,776 | 942,254 | 1,014,930 | 1,022,675 | 1,074,810 |
| spsc_hand_k1024_e64 | 10 | 1.00 [1.00–1.00] | 999 | 604,042 | 942,248 | 1,014,850 | 1,022,130 | 1,022,975 |
| generic_batch_k1024_e64 | 10 | 1.00 [1.00–1.00] | 999 | 687,796 | 960,303 | 1,017,440 | 1,023,515 | 1,027,580 |
| generic_hand_k1024_e64 | 10 | 1.00 [1.00–1.00] | 999 | 677,962 | 958,646 | 1,017,405 | 1,023,540 | 1,029,120 |
| generic_batch_k256_e64 | 10 | 1.00 [1.00–1.00] | 999 | 175,702 | 240,534 | 255,068 | 256,697 | 265,574 |
| generic_hand_k256_e64 | 10 | 1.00 [1.00–1.00] | 999 | 173,576 | 240,290 | 255,092 | 256,690 | 265,007 |
| inline_batch_k256_e64 | 10 | 1.00 [1.00–1.00] | 999 | 133,182 | 231,128 | 253,206 | 255,172 | 629,134 |
| inline_hand_k256_e64 | 10 | 1.00 [1.00–1.00] | 999 | 133,160 | 231,146 | 253,202 | 255,171 | 625,237 |
| spsc_batch_k256_e64 | 10 | 1.00 [1.00–1.00] | 999 | 153,100 | 235,472 | 253,568 | 255,143 | 360,202 |
| spsc_hand_k256_e64 | 10 | 1.00 [1.00–1.00] | 999 | 153,562 | 235,755 | 253,578 | 255,154 | 261,674 |
| inline_batch_e64 | 10 | 1.00 [1.00–1.00] | 999 | 33,553 | 57,724 | 63,198 | 63,342 | 319,246 |
| inline_hand_e64 | 10 | 1.00 [1.00–1.00] | 999 | 33,593 | 57,760 | 63,240 | 63,316 | 329,601 |
| spsc_batch_e64 | 10 | 1.00 [1.00–1.00] | 999 | 38,558 | 58,507 | 63,188 | 65,332 | 566,192 |
| spsc_hand_e64 | 10 | 1.00 [1.00–1.00] | 999 | 38,441 | 58,527 | 63,228 | 63,330 | 99,663 |
| generic_batch_e64 | 10 | 1.00 [1.00–1.00] | 999 | 44,236 | 60,526 | 64,475 | 66,550 | 89,226 |
| generic_hand_e64 | 10 | 1.00 [1.00–1.00] | 999 | 44,444 | 60,857 | 64,456 | 65,893 | 100,005 |
| inline_batch_k16_e64 | 10 | 1.00 [1.00–1.00] | 999 | 8,658 | 14,274 | 15,281 | 18,670 | 474,064 |
| inline_hand_k16_e64 | 10 | 1.00 [1.00–1.00] | 999 | 8,661 | 14,270 | 15,256 | 18,576 | 262,402 |
| generic_batch_k16_e64 | 10 | 1.00 [1.00–1.00] | 999 | 11,497 | 15,797 | 17,039 | 21,553 | 79,491 |
| generic_batch_k1_e64 | 10 | 1.00 [1.00–1.00] | 999 | 3,621 | 7,947 | 15,595 | 37,667 | 73,886 |
| generic_batch_k4_e64 | 10 | 1.00 [1.00–1.00] | 999 | 3,727 | 5,094 | 7,388 | 13,267 | 67,052 |
| generic_hand_k16_e64 | 10 | 1.00 [1.00–1.00] | 999 | 12,265 | 15,860 | 17,351 | 21,638 | 74,420 |
| generic_hand_k1_e64 | 10 | 1.00 [1.00–1.00] | 999 | 3,385 | 6,545 | 12,806 | 37,988 | 67,613 |
| generic_hand_k4_e64 | 10 | 1.00 [1.00–1.00] | 999 | 3,662 | 4,841 | 7,914 | 12,536 | 68,070 |
| generic_plain_e64 | 10 | 1.00 [1.00–1.00] | 999 | 5,442 | 14,215 | 31,355 | 55,283 | 88,636 |
| inline_batch_k1_e64 | 10 | 1.00 [1.00–1.00] | 999 | 215 | 335 | 362 | 6,101 | 501,796 |
| inline_batch_k4_e64 | 10 | 1.00 [1.00–1.00] | 999 | 2,309 | 3,234 | 3,343 | 8,590 | 501,450 |
| inline_hand_k1_e64 | 10 | 1.00 [1.00–1.00] | 999 | 210 | 333 | 358 | 6,031 | 258,981 |
| inline_hand_k4_e64 | 10 | 1.00 [1.00–1.00] | 999 | 2,294 | 3,256 | 3,316 | 8,212 | 520,988 |
| inline_plain_e64 | 10 | 1.00 [1.00–1.00] | 999 | 239 | 292 | 339 | 5,782 | 485,585 |
| spsc_batch_k16_e64 | 10 | 1.00 [1.00–1.00] | 999 | 9,545 | 14,604 | 15,417 | 20,051 | 528,110 |
| spsc_batch_k1_e64 | 10 | 1.00 [1.00–1.00] | 999 | 411 | 1,332 | 1,517 | 7,354 | 269,742 |
| spsc_batch_k4_e64 | 10 | 1.00 [1.00–1.00] | 999 | 2,469 | 3,387 | 3,594 | 8,361 | 73,816 |
| spsc_hand_k16_e64 | 10 | 1.00 [1.00–1.00] | 999 | 9,559 | 14,610 | 15,400 | 18,148 | 57,296 |
| spsc_hand_k1_e64 | 10 | 1.00 [1.00–1.00] | 999 | 441 | 1,296 | 1,516 | 6,062 | 51,078 |
| spsc_hand_k4_e64 | 10 | 1.00 [1.00–1.00] | 999 | 2,461 | 3,403 | 3,572 | 8,273 | 66,397 |
| spsc_plain_e64 | 10 | 1.00 [1.00–1.00] | 999 | 439 | 1,343 | 1,519 | 6,054 | 57,614 |

Medians across repetitions; each repetition is a fresh process; in brackets the bootstrap 95% CI of the median of the column the table is sorted by. ⚠ marks CoV above 3%. ns/op is 1000 / Mops/s. Latencies in ns; tails are the median across runs of each run's own percentile. "sat" marks a row with a run that did not sustain its offered rate: its latency is a backlog.
