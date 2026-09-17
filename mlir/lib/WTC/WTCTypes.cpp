#include "WTC/CosynthDialect.h"
#include "WTC/CosynthTypes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;
using namespace mlir::cosynth;

#define GET_TYPEDEF_CLASSES
#include "WTC/CosynthTypes.cpp.inc"

void CosynthDialect::registerTypes() {
  addTypes<
#define GET_TYPEDEF_LIST
#include "WTC/CosynthTypes.cpp.inc"
      >();
}