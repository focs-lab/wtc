# Compile cost

Medians over the runs of each source; wall time in seconds. plain is the same clang compiling the source without ClangIR; wtc-cc is the sum of its four steps; the passes are wtc-opt's own timing. Not a reserved measurement: read the ratios, not the third digit.

| source | n | plain | wtc-cc | ratio | CIR | wtc-opt | cir-translate | backend | .text | .text generic |
|---|---|---|---|---|---|---|---|---|---|---|
| batch_by_hand | 10 | 0.810 | 1.698 | 2.10× | 0.616 | 0.530 | 0.164 | 0.390 | 5341 | 5250 |
| batch_loop | 10 | 0.840 | 1.751 | 2.08× | 0.619 | 0.537 | 0.186 | 0.411 | 6717 | 6626 |
| inline_pipeline | 10 | 0.841 | 1.749 | 2.08× | 0.616 | 0.530 | 0.190 | 0.412 | 6829 | 6738 |
| lock_loop | 10 | 0.529 | 1.206 | 2.28× | 0.374 | 0.455 | 0.092 | 0.291 | 1003 | 1003 |
| mutex_basic | 10 | 0.426 | 1.045 | 2.45× | 0.268 | 0.438 | 0.076 | 0.268 | 459 | 459 |
| progress_consumer | 10 | 0.836 | 1.760 | 2.11× | 0.624 | 0.544 | 0.180 | 0.417 | 6749 | - |
| queue_example | 10 | 1.051 | 1.973 | 1.88× | 0.832 | 0.539 | 0.175 | 0.425 | 7338 | 7256 |
| w3_pipeline_wtc | 10 | 2.951 | 4.890 | 1.66× | 2.164 | 1.021 | 0.480 | 1.199 | 45680 | 45505 |
| w7_batch_wtc | 10 | 2.893 | 4.772 | 1.65× | 2.151 | 1.000 | 0.473 | 1.143 | 42049 | 41968 |
| w8_lock_batch_wtc | 10 | 2.900 | 4.819 | 1.66× | 2.094 | 1.010 | 0.485 | 1.218 | 42484 | 42484 |

## wtc-opt by pass, all sources

| pass | median s per source | share of wtc-opt |
|---|---|---|
| Parser | 0.0318 | 5.9% |
| (anonymous namespace)::QueueOwnershipAnalysisPass | 0.0308 | 5.7% |
| Output | 0.0132 | 2.5% |
| (anonymous namespace)::LiftToWTCPass | 0.0105 | 1.9% |
| (anonymous namespace)::WTCSpecializePass | 0.0049 | 0.9% |
| (anonymous namespace)::WTCBatchPushesPass | 0.0035 | 0.6% |
| (anonymous namespace)::WTCBatchLocksPass | 0.0034 | 0.6% |
| (anonymous namespace)::LowerWTCToCIRPass | 0.0028 | 0.5% |
| ReconcileUnrealizedCastsPass | 0.0018 | 0.3% |
| Rest | 0.0013 | 0.2% |
| (anonymous namespace)::WTCBorrowAnalysisPass | 0.0004 | 0.1% |
| (anonymous namespace)::WTCCheckProgressPass | 0.0002 | 0.0% |
