#ifndef WTC_OPS_TD
#define WTC_OPS_TD

include "mlir/IR/OpBase.td"
include "mlir/IR/SymbolInterfaces.td"
include "mlir/Interfaces/SideEffectInterfaces.td"
include "WTC/WTCDialect.td"
include "WTC/WTCTypes.td"

class WTC_Op<string mnemonic, list<Trait> traits = []> :
    Op<WTC_Dialect, mnemonic, traits>;

// Every wtc op stands for a call it can be lowered back to, named by
// fallback_impl and checked against it. Each one reads and writes the
// object it is given; nothing finer is claimed yet. What the ops guarantee
// about ordering and progress is an open design question
// (decisions/0003).
class WTC_ObjectOp<string mnemonic, list<Trait> traits = []> :
    WTC_Op<mnemonic, !listconcat(traits, [
        DeclareOpInterfaceMethods<SymbolUserOpInterface>,
        MemoryEffects<[MemRead, MemWrite]>])>;

def LockOp : WTC_ObjectOp<"lock"> {
    let summary = "A mutex lock operation";
    let arguments = (ins WTC_MutexType:$mutex,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_mutex_type);
}

def UnlockOp : WTC_ObjectOp<"unlock"> {
    let summary = "A mutex unlock operation";
    let arguments = (ins WTC_MutexType:$mutex,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_mutex_type);
}

def QueuePushOp : WTC_ObjectOp<"queue_push"> {
    let summary = "A queue push operation";
    let arguments = (ins WTC_QueueType:$queue, AnyType:$value,
        OptionalAttr<I64Attr>:$queue_id,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_queue_type);
    let results = (outs);
    let hasVerifier = 1;
}

def QueuePopOp : WTC_ObjectOp<"queue_pop"> {
    let summary = "A non-blocking queue pop operation, mirroring shared_ptr<T> semantics: a null result means empty";
    let description = [{
      The popped value is either the op's result or, where the target ABI
      returns it indirectly, written through `result_slot`: the hidden
      argument the original call passed it in. Exactly one of the two is
      present, mirroring the call this op was lifted from.
    }];
    let arguments = (ins WTC_QueueType:$queue,
        Optional<AnyType>:$result_slot,
        OptionalAttr<I64Attr>:$queue_id,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_queue_type);
    let results = (outs Optional<AnyType>:$value);
    let hasVerifier = 1;
}

#endif // WTC_OPS_TD