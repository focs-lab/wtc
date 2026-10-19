add_mlir_dialect_library(MLIRWTC
  WTCAttrs.cpp
  WTCDialect.cpp
  WTCInterfaces.cpp
  WTCOps.cpp
  WTCTypes.cpp

  DEPENDS
  MLIRWTCDialectIncGen
  MLIRWTCAttrsIncGen
  MLIRWTCInterfacesIncGen
  MLIRWTCOpsIncGen
  MLIRWTCTypesIncGen

  LINK_LIBS PUBLIC
  MLIRIR
  # The verifiers check fallback_impl against the cir.func it names.
  MLIRCIR
  MLIRSideEffectInterfaces
)

add_subdirectory(Transforms)
