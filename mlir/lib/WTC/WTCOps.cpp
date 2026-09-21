#include "WTC/WTCOps.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/OpImplementation.h"

using namespace mlir;
using namespace mlir::wtc;

#define GET_OP_CLASSES
#include "WTC/WTCOps.cpp.inc"

LogicalResult QueuePopOp::verify() {
  const bool hasOut = static_cast<bool>(getOut());
  const bool hasValue = static_cast<bool>(getValue());
  if (hasOut == hasValue)
    return emitOpError("needs exactly one of an `out` operand or a result");
  return success();
}
