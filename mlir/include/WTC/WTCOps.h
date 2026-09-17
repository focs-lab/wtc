#ifndef COSYNTH_COSYNTHOPS_H
#define COSYNTH_COSYNTHOPS_H

#include "mlir/Bytecode/BytecodeOpInterface.h"
#include "mlir/IR/OpDefinition.h"

#include "WTC/CosynthDialect.h"
#include "WTC/CosynthTypes.h"

#define GET_OP_CLASSES
#include "WTC/CosynthOps.h.inc"

#endif // COSYNTH_COSYNTHOPS_H