#ifndef WTC_TRANSFORMS_PASSES_H
#define WTC_TRANSFORMS_PASSES_H

#include "mlir/IR/BuiltinOps.h"
namespace mlir::wtc {
    void registerLiftCIRToWTCPass();
    void registerLowerWTCToCIRPass();
    void registerQueueOwnershipAnalysisPass();
    void analyseQueues(ModuleOp module);
}

#endif