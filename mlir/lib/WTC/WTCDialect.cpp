#include "WTC/CosynthDialect.h"
#include "WTC/CosynthOps.h"
#include "WTC/CosynthTypes.h"

#include "mlir/IR/DialectImplementation.h"

using namespace mlir;
using namespace mlir::cosynth;

#include "WTC/CosynthDialect.cpp.inc"

void CosynthDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "WTC/CosynthOps.cpp.inc"
      >();

  registerTypes();
}