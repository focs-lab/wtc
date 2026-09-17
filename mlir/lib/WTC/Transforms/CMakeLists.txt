add_mlir_library(MLIRCosynthTransforms
  LiftCIRToWTC.cpp
  LowerWTCToCIR.cpp
  QueueOwnershipAnalysis.cpp
  Utils.cpp
  LINK_LIBS PUBLIC
  MLIRCosynth
  MLIRIR
  MLIRPass
  MLIRSupport
  MLIRTransformUtils
)