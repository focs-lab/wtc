add_mlir_dialect_library(MLIRWTC
  WTCDialect.cpp
  WTCOps.cpp
  WTCTypes.cpp

  DEPENDS
  MLIRWTCDialectIncGen
  MLIRWTCOpsIncGen
  MLIRWTCTypesIncGen

  LINK_LIBS PUBLIC
  MLIRIR
)

add_subdirectory(Transforms)