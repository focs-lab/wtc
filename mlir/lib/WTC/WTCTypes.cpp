#include "WTC/WTCDialect.h"
#include "WTC/WTCTypes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;
using namespace mlir::wtc;

#define GET_TYPEDEF_CLASSES
#include "WTC/WTCTypes.cpp.inc"

void WTCDialect::registerTypes() {
  addTypes<
#define GET_TYPEDEF_LIST
#include "WTC/WTCTypes.cpp.inc"
      >();
}