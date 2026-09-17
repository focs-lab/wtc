#include "WTC/CosynthDialect.h"
#include "WTC/Transforms/Passes.h"

#include "mlir/IR/DialectRegistry.h"
#include "mlir/InitAllDialects.h"
#include "mlir/InitAllPasses.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"

#include "clang/CIR/Dialect/IR/CIRDialect.h"

int main(int argc, char **argv) {
  mlir::DialectRegistry registry;

  mlir::registerAllDialects(registry);
  mlir::registerAllPasses();

  registry.insert<mlir::cosynth::CosynthDialect>();
  registry.insert<cir::CIRDialect>();

  mlir::cosynth::registerLiftCIRToCosynthPass();
  mlir::cosynth::registerLowerCosynthToCIRPass();
  mlir::cosynth::registerQueueOwnershipAnalysisPass();

  return mlir::asMainReturnCode(
      mlir::MlirOptMain(argc, argv, "CoSynth optimizer driver\n", registry));
}