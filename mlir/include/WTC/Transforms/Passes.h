#ifndef WTC_TRANSFORMS_PASSES_H
#define WTC_TRANSFORMS_PASSES_H

namespace mlir::wtc {
    void registerLiftCIRToWTCPass();
    void registerLowerWTCToCIRPass();
    void registerQueueOwnershipAnalysisPass();
    void registerWTCSpecializePass();
    void registerWTCBorrowAnalysisPass();
    void registerWTCCheckProgressPass();
    void registerWTCFalseSharingDiagnosePass();
    void registerWTCAlignByOwnerPass();
}

#endif
