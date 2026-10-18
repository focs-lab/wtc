#include "WTC/WTCDialect.h"
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

  registry.insert<mlir::wtc::WTCDialect>();
  registry.insert<cir::CIRDialect>();

  mlir::wtc::registerLiftCIRToWTCPass();
  mlir::wtc::registerLowerWTCToCIRPass();
  mlir::wtc::registerQueueOwnershipAnalysisPass();
  mlir::wtc::registerSelectQueueImplementationPass();

  return mlir::asMainReturnCode(
      mlir::MlirOptMain(argc, argv, "wtc-opt: the Well-tempered Compiler optimizer driver\n", registry));
}