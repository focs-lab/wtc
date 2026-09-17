add_mlir_tool(wtc-opt
  wtc-opt.cpp
)

target_link_libraries(wtc-opt
  PRIVATE
  MLIRWTC
  MLIRWTCTransforms

  /Users/zwezeya/Development/llvm-project/build-cir/lib/libMLIRCIR.a
  /Users/zwezeya/Development/llvm-project/build-cir/lib/libMLIRCIRInterfaces.a

  /Users/zwezeya/Development/llvm-project/build-cir/lib/libclangAST.a
  /Users/zwezeya/Development/llvm-project/build-cir/lib/libclangBasic.a
  /Users/zwezeya/Development/llvm-project/build-cir/lib/libclangLex.a

  MLIRIR
  MLIRParser
  MLIRSupport
  MLIRPass

  MLIRFuncDialect
  MLIRArithDialect
  MLIRSCFDialect
  MLIRMemRefDialect

  MLIRTransforms
  MLIRMlirOptMain
)