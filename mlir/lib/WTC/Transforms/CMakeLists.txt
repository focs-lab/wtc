add_mlir_library(MLIRWTCTransforms
  LiftCIRToWTC.cpp
  LowerWTCToCIR.cpp
  QueueOwnershipAnalysis.cpp
  Utils.cpp
  LINK_LIBS PUBLIC
  MLIRWTC
  MLIRIR
  MLIRPass
  MLIRSupport
  MLIRTransformUtils
)