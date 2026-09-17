#ifndef WTC_OPS_TD
#define WTC_OPS_TD

include "mlir/IR/OpBase.td"
include "WTC/WTCDialect.td"
include "WTC/WTCTypes.td"

class WTC_Op<string mnemonic, list<Trait> traits = []> :
    Op<WTC_Dialect, mnemonic, traits>;

def LockOp : WTC_Op<"lock"> {
    let summary = "A mutex lock operation";
    let arguments = (ins AnyType:$mutex,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_mutex_type);
}

def UnlockOp : WTC_Op<"unlock"> {
    let summary = "A mutex unlock operation";
    let arguments = (ins AnyType:$mutex,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_mutex_type);
}

def QueuePushOp : WTC_Op<"queue_push", []> {
    let summary = "A queue push operation";
    let arguments = (ins WTC_QueueType:$queue, AnyType:$value,
        OptionalAttr<I64Attr>:$queue_id,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_queue_type);
    let results = (outs);
}

def QueuePopOp : WTC_Op<"queue_pop", []> {
    let summary = "A non-blocking queue pop operation, mirroring shared_ptr<T> semantics: a null result means empty";
    let arguments = (ins WTC_QueueType:$queue,
        OptionalAttr<I64Attr>:$queue_id,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_queue_type);
    let results = (outs AnyType:$value);
}

#endif // WTC_OPS_TD