#ifndef COSYNTH_TYPES_TD
#define COSYNTH_TYPES_TD

include "mlir/IR/AttrTypeBase.td"
include "Cosynth/CosynthDialect.td"

class Cosynth_Type<string name, string typeMnemonic, list<Trait> traits = []>
    : TypeDef<Cosynth_Dialect, name, traits> {
  let mnemonic = typeMnemonic;
}

def Cosynth_MutexType : Cosynth_Type<"Mutex", "mutex"> {
  let summary = "An abstract, compiler-visible mutex handle";
  let description = [{
    Represents a mutex whose acquire/release operations are visible to the
    compiler as first-class IR operations.
  }];
}

def Cosynth_QueueType : Cosynth_Type<"Queue", "queue"> {
    let summary = "A CoSynth queue handle";
    let parameters = (ins "Type":$elementType);
    let assemblyFormat = "`<` $elementType `>`";
}

#endif // COSYNTH_TYPES_TD