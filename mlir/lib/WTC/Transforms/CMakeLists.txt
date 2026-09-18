add_mlir_library(MLIRWTCTransforms
  LiftCIRToWTC.cpp
  LowerWTCToCIR.cpp
  QueueOwnershipAnalysis.cpp
  Utils.cpp
  LINK_LIBS PUBLIC
  MLIRWTC

  # All four sources use CIR types and attributes. This used to work only
  # because the archive's undefined references happened to be satisfied when
  # wtc-opt linked MLIRCIR itself; a second consumer would not be so lucky.
  MLIRCIR
  MLIRCIRInterfaces

  MLIRIR
  MLIRPass
  MLIRSupport
  MLIRTransformUtils
)