#ifndef WTC_WTCOPS_H
#define WTC_WTCOPS_H

#include "mlir/Bytecode/BytecodeOpInterface.h"
#include "mlir/IR/OpDefinition.h"

#include "WTC/WTCDialect.h"
#include "WTC/WTCTypes.h"

#define GET_OP_CLASSES
#include "WTC/WTCOps.h.inc"

#endif // WTC_WTCOPS_H