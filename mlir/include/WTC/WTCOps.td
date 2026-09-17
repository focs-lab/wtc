#ifndef COSYNTH_OPS_TD
#define COSYNTH_OPS_TD

include "mlir/IR/OpBase.td"
include "Cosynth/CosynthDialect.td"
include "Cosynth/CosynthTypes.td"

class Cosynth_Op<string mnemonic, list<Trait> traits = []> :
    Op<Cosynth_Dialect, mnemonic, traits>;

def LockOp : Cosynth_Op<"lock"> {
    let summary = "A mutex lock operation";
    let arguments = (ins AnyType:$mutex,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_mutex_type);
}

def UnlockOp : Cosynth_Op<"unlock"> {
    let summary = "A mutex unlock operation";
    let arguments = (ins AnyType:$mutex,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_mutex_type);
}

def QueuePushOp : Cosynth_Op<"queue_push", []> {
    let summary = "A queue push operation";
    let arguments = (ins Cosynth_QueueType:$queue, AnyType:$value,
        OptionalAttr<I64Attr>:$queue_id,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_queue_type);
    let results = (outs);
}

def QueuePopOp : Cosynth_Op<"queue_pop", []> {
    let summary = "A non-blocking queue pop operation, mirroring shared_ptr<T> semantics: a null result means empty";
    let arguments = (ins Cosynth_QueueType:$queue,
        OptionalAttr<I64Attr>:$queue_id,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_queue_type);
    let results = (outs AnyType:$value);
}

#endif // COSYNTH_OPS_TD