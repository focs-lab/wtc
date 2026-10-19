#ifndef WTC_OPS_TD
#define WTC_OPS_TD

include "mlir/IR/OpBase.td"
include "mlir/IR/SymbolInterfaces.td"
include "mlir/Interfaces/SideEffectInterfaces.td"
include "WTC/WTCDialect.td"
include "WTC/WTCTypes.td"
include "WTC/WTCAttrs.td"
include "WTC/WTCInterfaces.td"

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
// that CIR did not. queue_id, when present, names the one queue the
// operation acts on: the number the ownership analysis gave its root.

// Every operation stands for a call it can be lowered back to. When
// fallback_impl is present it is checked against the cir.func it names:
// that function implements the same operation and takes the arguments
// lowering will pass. Each operation reads and writes the object it is
// given; nothing finer is claimed, so other passes treat it as they would
// the call it came from.
class WTC_ObjectOp<string mnemonic, list<Trait> traits = []> :
    WTC_Op<mnemonic, !listconcat(traits, [
        DeclareOpInterfaceMethods<SymbolUserOpInterface>,
        DeclareOpInterfaceMethods<WTC_ProgressOpInterface>,
        MemoryEffects<[MemRead, MemWrite]>])>;

def LockOp : WTC_ObjectOp<"lock"> {
    let summary = "A mutex lock operation";
    let arguments = (ins WTC_MutexType:$mutex,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_mutex_type,
        OptionalAttr<ArrayAttr>:$source_arg_attrs);
}

def UnlockOp : WTC_ObjectOp<"unlock"> {
    let summary = "A mutex unlock operation";
    let arguments = (ins WTC_MutexType:$mutex,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_mutex_type,
        OptionalAttr<ArrayAttr>:$source_arg_attrs);
}

def QueuePushOp : WTC_ObjectOp<"queue_push"> {
    let summary = "A queue push operation";
    let description = [{
        Appends a copy of `value` to the queue. What every implementation
        of it promises (decisions/0003): the push happens before the pop
        that returns its item; operations on one queue are linearisable;
        items from one producer leave in the order that producer pushed
        them. Nothing is promised about the order against operations on
        any other object, nor about progress. A program that relies on
        more is outside the contract and is not a candidate for
        substitution. `impl`, when present, is the implementation the
        queue runs.
    }];
    let arguments = (ins WTC_QueueType:$queue, AnyType:$value,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_queue_type,
        OptionalAttr<ArrayAttr>:$source_arg_attrs,
        OptionalAttr<WTC_TopologyAttr>:$topology,
        OptionalAttr<I64Attr>:$queue_id,
        OptionalAttr<WTC_ImplAttr>:$impl);
    let results = (outs);
    let hasVerifier = 1;
}

def QueuePopOp : WTC_ObjectOp<"queue_pop"> {
    let summary = "A non-blocking queue pop; a null result means empty";
    let description = [{
        Removes the oldest item, under the same contract as queue_push
        (decisions/0003); an empty queue gives a null result, which says
        only that no item was available at the pop's linearisation point.

        Two forms, exactly one of which is present. With a result, the popped
        value is the result: the shape hand-written IR uses. With the `out`
        operand and no result, the popped value is written through `out`: the
        shape the compiler emits, because the shim's try_pop returns a
        shared_ptr by value and the calling convention passes that through a
        hidden first pointer argument.

        `borrowed` says the program only borrows what this pop returns: it
        keeps no copy of the shared_ptr and no pointer into the element past
        the consumer's next pop of the same queue (WTCBorrowAnalysis.cpp).
    }];
    let arguments = (ins WTC_QueueType:$queue,
        Optional<AnyType>:$out,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_queue_type,
        OptionalAttr<ArrayAttr>:$source_arg_attrs,
        OptionalAttr<WTC_TopologyAttr>:$topology,
        OptionalAttr<I64Attr>:$queue_id,
        OptionalAttr<WTC_ImplAttr>:$impl,
        UnitAttr:$borrowed);
    let results = (outs Optional<AnyType>:$value);
    let hasVerifier = 1;
}

// A producer's run of deferred pushes (shim/queue.hpp): batch_begin, a
// push_deferred per element, batch_end. Each element is appended as by
// queue_push, under the same contract, but becomes visible to consumers
// only when the run is published: at batch_end, or every WTC_BATCH_K
// elements. `batch` is the producer's own storage for the run. All three
// are the producer's side of the queue, for every analysis that counts
// producers.
class WTC_BatchOp<string mnemonic, dag extra> : WTC_ObjectOp<mnemonic> {
    let arguments = !con((ins WTC_QueueType:$queue, AnyType:$batch), extra,
        (ins OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_queue_type,
        OptionalAttr<ArrayAttr>:$source_arg_attrs,
        OptionalAttr<WTC_TopologyAttr>:$topology,
        OptionalAttr<I64Attr>:$queue_id,
        OptionalAttr<WTC_ImplAttr>:$impl));
    let results = (outs);
    let hasVerifier = 1;
}

def QueueBatchBeginOp : WTC_BatchOp<"queue_batch_begin", (ins)> {
    let summary = "Starts a producer's run of deferred pushes";
}

def QueuePushDeferredOp : WTC_BatchOp<"queue_push_deferred", (ins AnyType:$value)> {
    let summary = "Appends a copy of `value` to the run, visible when the run is published";
}

def QueueBatchEndOp : WTC_BatchOp<"queue_batch_end", (ins)> {
    let summary = "Publishes what is left of the run and ends it";
}

#endif // WTC_OPS_TD
