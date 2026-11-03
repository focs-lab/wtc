#ifndef WTC_TRANSFORMS_PASSES_H
#define WTC_TRANSFORMS_PASSES_H

namespace mlir::wtc {
    void registerLiftCIRToWTCPass();
    void registerLiftLockedQueuesPass();
    void registerLiftCQueuesPass();
    void registerLowerWTCToCIRPass();
    void registerQueueOwnershipAnalysisPass();
    void registerWTCSpecializePass();
    void registerWTCBorrowAnalysisPass();
    void registerWTCCheckProgressPass();
    void registerWTCBatchPushesPass();
    void registerWTCBatchLocksPass();
    void registerWTCFalseSharingDiagnosePass();
    void registerWTCAlignByOwnerPass();
    void registerWTCCensusPass();
}

#endif
