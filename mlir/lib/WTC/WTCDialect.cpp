#include "WTC/WTCDialect.h"
#include "WTC/WTCOps.h"
#include "WTC/WTCTypes.h"

#include "mlir/IR/DialectImplementation.h"

using namespace mlir;
using namespace mlir::wtc;

#include "WTC/WTCDialect.cpp.inc"

void WTCDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "WTC/WTCOps.cpp.inc"
      >();

  registerTypes();
}