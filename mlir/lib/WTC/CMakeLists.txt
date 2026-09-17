add_mlir_dialect_library(MLIRCosynth
  WTCDialect.cpp
  WTCOps.cpp
  WTCTypes.cpp

  DEPENDS
  MLIRCosynthDialectIncGen
  MLIRCosynthOpsIncGen
  MLIRCosynthTypesIncGen

  LINK_LIBS PUBLIC
  MLIRIR
)

add_subdirectory(Transforms)