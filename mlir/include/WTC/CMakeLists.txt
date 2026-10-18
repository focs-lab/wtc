set(LLVM_TARGET_DEFINITIONS WTCDialect.td)

mlir_tablegen(WTCDialect.h.inc -gen-dialect-decls -dialect=wtc)
mlir_tablegen(WTCDialect.cpp.inc -gen-dialect-defs -dialect=wtc)

add_public_tablegen_target(MLIRWTCDialectIncGen)

set(LLVM_TARGET_DEFINITIONS WTCOps.td)

mlir_tablegen(WTCOps.h.inc -gen-op-decls)
mlir_tablegen(WTCOps.cpp.inc -gen-op-defs)

add_public_tablegen_target(MLIRWTCOpsIncGen)

set(LLVM_TARGET_DEFINITIONS WTCTypes.td)

mlir_tablegen(WTCTypes.h.inc -gen-typedef-decls -typedefs-dialect=wtc)
mlir_tablegen(WTCTypes.cpp.inc -gen-typedef-defs -typedefs-dialect=wtc)

add_public_tablegen_target(MLIRWTCTypesIncGen)
