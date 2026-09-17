#ifndef COSYNTH_TRANSFORMS_PASSES_H
#define COSYNTH_TRANSFORMS_PASSES_H

#include "mlir/IR/BuiltinOps.h"
namespace mlir::cosynth {
    void registerLiftCIRToCosynthPass();
    void registerLowerCosynthToCIRPass();
    void registerQueueOwnershipAnalysisPass();
    void analyseQueues(ModuleOp module);
}

#endif