#ifndef COSYNTH_DIALECT_TD
#define COSYNTH_DIALECT_TD

include "mlir/IR/DialectBase.td"

def Cosynth_Dialect : Dialect {
    let name = "cosynth";
    let summary = "Concurrency dialect for WP1 prototyping";
    let description = [{A minimal dialect that gives MLIR first class knowledge of concurrency semantics.}];
    let cppNamespace = "::mlir::cosynth";
    let useDefaultTypePrinterParser = 1;
    let extraClassDeclaration = [{
      void registerTypes();
    }];
}

#endif // COSYNTH_DIALECT_TD