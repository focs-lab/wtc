#ifndef WTC_OPS_TD
#define WTC_OPS_TD

include "mlir/IR/OpBase.td"
include "WTC/WTCDialect.td"
include "WTC/WTCTypes.td"
include "WTC/WTCAttrs.td"

class WTC_Op<string mnemonic, list<Trait> traits = []> :
    Op<WTC_Dialect, mnemonic, traits>;

// Attributes every lifted operation carries so that lowering can rebuild
// the original call exactly: the callee it came from, the CIR type of the
// handle operand, and the call's argument attributes (which is where the
// hidden return-slot marker lives for calls that have one).
//
// A lifted operation is still, for now, a tagged call: nothing here states
// memory ordering or a progress guarantee. That is deliberate scope for
// the gate; the topology attribute is the first fact the dialect carries
// that CIR did not.

def LockOp : WTC_Op<"lock"> {
    let summary = "A mutex lock operation";
    let arguments = (ins WTC_MutexType:$mutex,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_mutex_type,
        OptionalAttr<ArrayAttr>:$source_arg_attrs);
}

def UnlockOp : WTC_Op<"unlock"> {
    let summary = "A mutex unlock operation";
    let arguments = (ins WTC_MutexType:$mutex,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_mutex_type,
        OptionalAttr<ArrayAttr>:$source_arg_attrs);
}

def QueuePushOp : WTC_Op<"queue_push", []> {
    let summary = "A queue push operation";
    let arguments = (ins WTC_QueueType:$queue, AnyType:$value,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_queue_type,
        OptionalAttr<ArrayAttr>:$source_arg_attrs,
        OptionalAttr<WTC_TopologyAttr>:$topology);
    let results = (outs);
}

def QueuePopOp : WTC_Op<"queue_pop", []> {
    let summary = "A non-blocking queue pop; a null result means empty";
    let description = [{
        Two forms, exactly one of which is present. With a result, the popped
        value is the result: the shape hand-written IR uses. With the `out`
        operand and no result, the popped value is written through `out`: the
        shape the compiler emits, because the shim's try_pop returns a
        shared_ptr by value and the calling convention passes that through a
        hidden first pointer argument.
    }];
    let arguments = (ins WTC_QueueType:$queue,
        Optional<AnyType>:$out,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_queue_type,
        OptionalAttr<ArrayAttr>:$source_arg_attrs,
        OptionalAttr<WTC_TopologyAttr>:$topology);
    let results = (outs Optional<AnyType>:$value);
    let hasVerifier = 1;
}

#endif // WTC_OPS_TD
