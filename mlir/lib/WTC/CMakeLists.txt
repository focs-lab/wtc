add_mlir_dialect_library(MLIRWTC
  WTCDialect.cpp
  WTCOps.cpp
  WTCTypes.cpp

  DEPENDS
  MLIRWTCDialectIncGen
  MLIRWTCOpsIncGen
  MLIRWTCTypesIncGen

  LINK_LIBS PUBLIC
  # The verifiers check push and pop against CIR types and fallback_impl
  # against the cir.func it names.
  MLIRCIR
  MLIRIR
  MLIRSideEffectInterfaces
)

add_subdirectory(Transforms)