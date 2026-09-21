add_mlir_dialect_library(MLIRWTC
  WTCAttrs.cpp
  WTCDialect.cpp
  WTCOps.cpp
  WTCTypes.cpp

  DEPENDS
  MLIRWTCDialectIncGen
  MLIRWTCAttrsIncGen
  MLIRWTCOpsIncGen
  MLIRWTCTypesIncGen

  LINK_LIBS PUBLIC
  MLIRIR
)

add_subdirectory(Transforms)
