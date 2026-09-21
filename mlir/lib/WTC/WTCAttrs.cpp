#include "WTC/WTCAttrs.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "llvm/ADT/TypeSwitch.h"

#include "WTC/WTCEnums.cpp.inc"

#define GET_ATTRDEF_CLASSES
#include "WTC/WTCAttrs.cpp.inc"

namespace mlir::wtc {

void WTCDialect::registerAttributes() {
  addAttributes<
#define GET_ATTRDEF_LIST
#include "WTC/WTCAttrs.cpp.inc"
      >();
}

} // namespace mlir::wtc
