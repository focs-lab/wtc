set(LLVM_TARGET_DEFINITIONS WTCDialect.td)

mlir_tablegen(WTCDialect.h.inc -gen-dialect-decls -dialect=cosynth)
mlir_tablegen(WTCDialect.cpp.inc -gen-dialect-defs -dialect=cosynth)

add_public_tablegen_target(MLIRCosynthDialectIncGen)

set(LLVM_TARGET_DEFINITIONS WTCOps.td)

mlir_tablegen(WTCOps.h.inc -gen-op-decls)
mlir_tablegen(WTCOps.cpp.inc -gen-op-defs)

add_public_tablegen_target(MLIRCosynthOpsIncGen)

set(LLVM_TARGET_DEFINITIONS WTCTypes.td)

mlir_tablegen(WTCTypes.h.inc -gen-typedef-decls -typedefs-dialect=cosynth)
mlir_tablegen(WTCTypes.cpp.inc -gen-typedef-defs -typedefs-dialect=cosynth)

add_public_tablegen_target(MLIRCosynthTypesIncGen)

add_subdirectory(Transforms)