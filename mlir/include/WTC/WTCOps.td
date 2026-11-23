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
// A lifted operation is still a tagged call: its contract (ordering,
// linearisability) is stated in decisions/0003, and its progress answer,
// per implementation, by ProgressOpInterface in WTCOps.cpp (decisions/0004).
// The topology attribute is the first fact the dialect carries that CIR
// did not. queue_id, when present, names the one queue the
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

// A run of critical sections on one mutex held as one (shim/mutex.hpp):
// lock_batch_begin takes the mutex, lock_batch_step ends one section and
// every WTC_LOCK_BATCH_K sections lets the mutex go and takes it again,
// lock_batch_end lets it go. `batch` is the holder's own count.
class WTC_MutexBatchOp<string mnemonic> : WTC_ObjectOp<mnemonic> {
    let arguments = (ins WTC_MutexType:$mutex, AnyType:$batch,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_mutex_type,
        OptionalAttr<ArrayAttr>:$source_arg_attrs);
    let results = (outs);
}

def LockBatchBeginOp : WTC_MutexBatchOp<"lock_batch_begin"> {
    let summary = "Takes a mutex for a run of critical sections";
}

def LockBatchStepOp : WTC_MutexBatchOp<"lock_batch_step"> {
    let summary = "Ends one critical section of the run, letting the mutex go now and then";
}

def LockBatchEndOp : WTC_MutexBatchOp<"lock_batch_end"> {
    let summary = "Lets the mutex go and ends the run";
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
        `bounded` marks a push on a queue with a capacity (shim/bounded.hpp,
        the substitute for a ring the program bounded itself), whose
        answer to a full queue is one of three: with `wait` it waits until
        there is room, and blocks (decisions/0004); with a result, the
        `try` form, it appends nothing and answers false; with `overrun`
        it removes the oldest item and appends its own at one step. An
        overrunning push takes an item out of the queue, so its thread is
        a consumer of the queue as well as a producer, for every analysis
        that counts them.
        `batch` marks a run of pushes from a ring of the calling thread's
        (shim/c_queue.h, the substitute for dedup's batched enqueue): it
        waits while the queue is full, like `wait`, then appends at one
        step as many of the ring's elements as fit, up to a limit, and its
        result is how many. `value` is then where the ring keeps its
        elements, and `more` the rest of the ring and the limit, passed
        on as the call took them.
    }];
    let arguments = (ins WTC_QueueType:$queue, AnyType:$value, Variadic<AnyType>:$more,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_queue_type,
        OptionalAttr<ArrayAttr>:$source_arg_attrs,
        OptionalAttr<WTC_TopologyAttr>:$topology,
        OptionalAttr<I64Attr>:$queue_id,
        OptionalAttr<WTC_ImplAttr>:$impl,
        UnitAttr:$bounded,
        UnitAttr:$wait,
        UnitAttr:$overrun,
        UnitAttr:$batch);
    let results = (outs Optional<AnyType>:$pushed);
    let hasVerifier = 1;
}

def QueuePopOp : WTC_ObjectOp<"queue_pop"> {
    let summary = "A queue pop; non-blocking unless marked `wait`, and a null result means empty";
    let description = [{
        Removes the oldest item, under the same contract as queue_push
        (decisions/0003); an empty queue gives a null result, which says
        only that no item was available at the pop's linearisation point.

        Two forms, exactly one of which is present. With a result, the popped
        value is the result: the shape hand-written IR uses. With the `out`
        operand (the first of `args`) and no result, the popped value is
        written through `out`: the shape the compiler emits, because the
        shim's try_pop returns a shared_ptr by value and the calling
        convention passes that through a hidden first pointer argument.

        `transfer` marks the third shape, the shim's try_pop(T&): `out` is
        then the program's own object of the element type, into which the
        element is moved, and the result, if there is one, is the call's
        own, whether an element was there. Nothing is allocated for the
        element on either side, and the program keeps nothing of the
        queue's, so such a pop never stands in the way of inline storage.

        `wait` marks a transfer that waits for an item: the shim's
        pop_wait(T&), and with a second operand, `timeout`, its
        pop_wait_for(T&, nanoseconds). It returns an item whenever the
        non-blocking pop would, and while the queue is empty it waits for
        a push instead of answering; it answers "none" only once the queue
        is closed and empty, or, with a timeout, once that has passed with
        the queue empty. Its item obeys the same contract. It blocks, on
        every implementation and every wait policy: it waits for another
        thread by design (decisions/0004). The wait policy (spin, park,
        spin_then_park) is the queue's, set at its construction, and is
        not recorded on the operation.

        `borrowed` says the program only borrows what this pop returns: it
        keeps no copy of the shared_ptr and no pointer into the element past
        the consumer's next pop of the same queue (WTCBorrowAnalysis.cpp).
        `bounded` marks a pop of a queue with a capacity, as on queue_push.
        `batch` marks a waiting transfer of a run of elements into a ring of
        the calling thread's (shim/c_queue.h, the substitute for dedup's
        batched dequeue): `out` is where the ring keeps its elements, and
        the operands after it the rest of the ring and the limit; it takes
        at one step as many as the ring has room for, adjacent in the queue
        whatever the number of consumers.
    }];
    let arguments = (ins WTC_QueueType:$queue,
        Variadic<AnyType>:$args,
        OptionalAttr<FlatSymbolRefAttr>:$fallback_impl,
        OptionalAttr<TypeAttr>:$source_queue_type,
        OptionalAttr<ArrayAttr>:$source_arg_attrs,
        OptionalAttr<WTC_TopologyAttr>:$topology,
        OptionalAttr<I64Attr>:$queue_id,
        OptionalAttr<WTC_ImplAttr>:$impl,
        UnitAttr:$borrowed,
        UnitAttr:$transfer,
        UnitAttr:$wait,
        UnitAttr:$bounded,
        UnitAttr:$batch);
    let results = (outs Optional<AnyType>:$value);
    let extraClassDeclaration = [{
        // The slot or object the item goes to, if there is one.
        ::mlir::Value getOut() {
            auto a = getArgs();
            return a.empty() ? ::mlir::Value() : a.front();
        }
        // A waiting pop's limit, if it has one. A batch's operands after
        // `out` describe the ring and how many to take.
        ::mlir::Value getTimeout() {
            auto a = getArgs();
            return a.size() > 1 && !getBatch() ? a[1] : ::mlir::Value();
        }
    }];
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

// A critical section of one mutex, run by the mutex's delegation server
// (runtime/delegate.hpp) instead of under the mutex by the calling thread
// (--wtc-delegate-sections, docs/delegation.md).
def DelegateOp : WTC_Op<"delegate", [
        DeclareOpInterfaceMethods<SymbolUserOpInterface>,
        DeclareOpInterfaceMethods<WTC_ProgressOpInterface>,
        MemoryEffects<[MemRead, MemWrite]>]> {
    let summary = "Runs a critical section on the delegation server of its mutex";
    let description = [{
        `section` is a critical section of the mutex `mutex` points to,
        outlined into a function of its own: its body is the section as
        the program wrote it, the mutex's own lock at its start and the
        mutex's own unlock at its end included, and it reads the values the
        section needs from `env` and writes there those the code after it
        reads. Its result, and this operation's, is the exit by which the
        section left, numbered from zero.

        The operation means `section(env)` called on this thread, which
        is the program as written. Lowered (--lower-wtc-to-cir), it is a
        call of the runtime's wtc_delegate_run, which runs `section(env)`
        on the server of `mutex`, one thread per mutex, and returns once
        it has returned; a server that reaches the operation runs the
        section on its own thread. The section's writes happen before the
        operation returns, and what the thread did before the operation
        happens before the section, as the original's unlock and lock
        ordered them. The pass proves, before it builds one, that the
        section cannot tell which thread runs it, waits for no other
        thread but by locks it lets go again, and lets no exception out.
        `mutex_name` is the mutex as the option named it, for remarks.
    }];
    let arguments = (ins AnyType:$mutex, AnyType:$env, FlatSymbolRefAttr:$section, StrAttr:$mutex_name);
    let results = (outs AnyType:$exit);
    let assemblyFormat = [{
        $mutex `,` $section `(` $env `)` attr-dict `:` type($mutex) `,` type($env) `->` type($exit)
    }];
}

#endif // WTC_OPS_TD
