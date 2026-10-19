add_mlir_library(MLIRWTCTransforms
  LiftCIRToWTC.cpp
  LowerWTCToCIR.cpp
  QueueOwnershipAnalysis.cpp
  ThreadInstances.cpp
  WTCAlignByOwner.cpp
  WTCBatchPushes.cpp
  WTCBorrowAnalysis.cpp
  WTCCheckProgress.cpp
  WTCFalseSharingDiagnose.cpp
  WTCSpecialize.cpp
  Utils.cpp

  LINK_COMPONENTS
  Demangle

  LINK_LIBS PUBLIC
  MLIRWTC

  # Every source here uses CIR types and attributes. This used to work only
  # because the archive's undefined references happened to be satisfied when
  # wtc-opt linked MLIRCIR itself; a second consumer would not be so lucky.
  MLIRCIR
  MLIRCIRInterfaces

  MLIRIR
  MLIRPass
  MLIRSupport
  MLIRTransformUtils
)