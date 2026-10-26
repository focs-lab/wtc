# Hand-declared queue topologies: an audit

Read on 23 October 2026. The question: when a program declares a queue's
topology itself, a DPDK ring created with `RING_F_SP_ENQ` or
`RING_F_SC_DEQ`, or a single-producer single-consumer queue from a library,
does the declaration hold, is it more cautious than the code needs, or is it
broken? And what would it take to check it with the compiler rather than by
reading. Every verdict in parts 1 and 2 was reached by reading the code
named; nothing was run. Part 3 (26 October 2026) runs the compiler's own
check over the same code, once the analysis could read these declarations.

Verdicts: **holds** (exactly one thread on each declared side, visible in
the code), **holds by convention** (one thread today, but the single-thread
side rests on something the queue's type does not express: a comment, an API
contract, a lock around a second user, a choice made in another file),
**over-cautious** (a many-threads implementation where one side has one
thread), **broken** (two threads on a side declared single), **undecidable**
(the sides are configured at run time or in code not read).

## Part 1: DPDK's examples

DPDK 26.11.0-rc0 (the corpus's tree), `examples/`. Twelve files create
rings; the ten applications below cover every one that declares a side
single, plus two that compute or omit the flags. Threads start through
`rte_eal_remote_launch` (one lcore function on one lcore) or
`rte_eal_mp_remote_launch` (the same function on every worker lcore); the
main lcore runs what `main` calls after that.

| application | ring (created at) | declared | producers (enqueue at) | consumers (dequeue at) | verdict |
|---|---|---|---|---|---|
| packet_ordering | `rx_to_workers` (`main.c:750`) | SP | the main lcore, `rx_thread_reorder` / `rx_thread_reorder_disabled` (`main.c:465`), called from `main` (`main.c:791-794`) | every worker lcore but the last, `worker_thread` (`main.c:518`), launched in a loop (`main.c:775-778`) | holds; the many-consumer side is left MC, correctly |
| packet_ordering | `workers_to_tx` (`main.c:755`) | SC | the workers (`main.c:530`) | the last lcore, `tx_thread` or `send_thread` (`main.c:650`, `:565`), one of the two launched (`main.c:780-787`) | holds |
| distributor | `rx_dist_ring` (`main.c:871`) | SP, SC | `lcore_rx` on the rx core (`main.c:269`, launched `:1000`) | `lcore_distributor` on the distributor core (`main.c:419`, launched `:985`) | holds; with `enable_lcore_rx_distributor` neither side runs and the ring is idle |
| distributor | `dist_tx_ring` (`main.c:866`) | SP, SC | `lcore_distributor` (`main.c:435`) or `lcore_rx_and_distributor` (`main.c:347`), one of them launched (`main.c:983-1001`) | `lcore_tx` on the tx core (`main.c:490`, launched `:974`) | holds by convention: a comment in `lcore_rx` (`main.c:263-266`) invites swapping its output to `dist_tx_ring`, which would give the SP ring two enqueuing lcores (rx and distributor) with no diagnostic |
| qos_sched | `rx_ring` per flow (`init.c:345`) | SP, SC | `app_rx_thread` on the flow's rx core (`app_thread.c:91`) | `app_worker_thread` or `app_mixed_thread` on its worker core (`app_thread.c:141`, `:176`) | holds; roles are assigned per lcore by `app_main_loop` from the flow configuration (`main.c:53-63`), so one lcore per role per flow |
| qos_sched | `tx_ring` per flow (`init.c:353`) | SP, SC | the worker (`app_thread.c:154`) | `app_tx_thread` on the tx core (`app_thread.c:115`) | holds; same reasoning |
| dma (dmafwd) | `rx_to_tx_ring` per port (`dmafwd.c:833`) | SP, SC | `dma_rx_port` (`dmafwd.c:455`) from `rx_main_loop` or `rxtx_main_loop` | `dma_tx_port` (`dmafwd.c:479`) from `tx_main_loop` or `rxtx_main_loop` | holds: `start_forwarding_cores` launches exactly one rx and one tx lcore, or one doing both (`dmafwd.c:556-575`), whatever the lcore count |
| server_node_efd | `nodes[i].rx_q` (`efd_server/init.c:188`) | SP, SC | the server's main lcore, `flush_rx_queue` (`efd_server/main.c:228`); workers only sleep (`main.c:341`) | node process `i`, `efd_node/node.c:369` | holds, across two processes: the consumer is in another program, found by the ring's name |
| multi_process, client_server_mp | `clients[i].rx_q` (`mp_server/init.c:168`) | SP, SC (the comment says so) | the server's main lcore (`mp_server/main.c:205`) | client process `i` (`mp_client/client.c:249`) | holds, across two processes |
| multi_process, simple_mp | `send_ring`, `recv_ring` (`main.c:92-93`) | none (`flags = 0`, `main.c:78`) | the command line on the main lcore of each process (`mp_commands.c:21`) | `lcore_recv` on every worker lcore of the other process (`main.c:63`, launched in a loop `:114-116`) | correct as MPMC: one producer, but many consumers, so SC would be broken and SP alone would hold |
| bbdev_app | `enc_to_dec_ring` (`main.c:1096`) | SP if one encoder core, SC if one decoder core, computed (`main.c:1092-1094`) | the encoder lcores, `run_encoding` (`main.c:807`) | the decoder lcores, `run_decoding` (`main.c:840`), both launched per lcore by role (`main.c:1164-1172`) | holds: the flags are derived from the same core masks that choose the threads |
| ip_pipeline | `msgq_req`, `msgq_rsp` per data-plane thread (`thread.c:116`, `:128`) and per pipeline (`pipeline.c:89`, `:98`) | SP, SC | requests: the CLI on the main lcore (`thread.c:243`, `:757`); responses: the data-plane thread (`thread.c:450`, `:2221`) | requests: the data-plane thread (`thread.c:435`, `:2206`), one per lcore (`main.c:243`); responses: the CLI (`thread.c:248`, `:762`) | holds: a request-response pair per thread, one side each |
| ip_pipeline | `swq` rings (`swq.c:52`, flags `swq.c:41`) | SP, SC | a pipeline's output port | a pipeline's input port, both bound by CLI script | undecidable from source: which pipelines, on which threads, use a software queue is in the run's script |
| pipeline | rings made by the `ring` CLI command (`cli.c:511`) | SP, SC | pipeline ports bound by script | the same | undecidable from source, as above |

Counts, part 1: 14 rows, one per queue or per set of queues created alike
(ip_pipeline's message queues are four creation sites in one row, simple_mp's
two rings one row), 18 creation sites in all: 10 rows hold, 1 holds by
convention (distributor's commented-out swap), 2 undecidable (script-bound
ports), 1 correct MPMC (simple_mp), 0 over-cautious, 0 broken. DPDK's
example authors declare what the launch code does, and in two cases derive
the flags from the same configuration that chooses the threads (bbdev_app,
qos_sched), which is the right discipline and the one the compiler would
reproduce.

## Part 2: users of SPSC libraries

The corpus has no user of these libraries outside folly itself, so the
projects come from a code search on GitHub (23 Oct 2026); the files named
were read at the commits listed under "Sources read" below. The libraries:
rigtorp's `SPSCQueue`, `boost::lockfree::spsc_queue`, folly's
`ProducerConsumerQueue`, moodycamel's `ReaderWriterQueue`.

| project | queue (declared at) | producer | consumer | verdict |
|---|---|---|---|---|
| mixxx (DJ software) | `MessagePipe`, two `rigtorp::SPSCQueue` (`src/util/messagepipe.h:78-81`) | a controller's script thread | its I/O thread | holds by convention: the header says each end "only be called from a single thread" (`messagepipe.h:62-63`); nothing enforces it |
| mixxx | `HidIoGlobalOutputReportFifo::m_fifoQueue` (`hidioglobaloutputreportfifo.h:40`) | the controller engine | the HID I/O thread (`:24`) | holds by convention, same |
| mixxx | `AnalyzerThread::m_nextTrack` (`analyzerthread.h:113`) | the host thread | the analyzer worker | holds; the comment (`:107-110`) names the two threads and the class owns both ends |
| freeablo (game) | `ThreadManager::mQueue` (`engine/threadmanager.h:55`), a singleton | the game loop thread (`enginemain.cpp:63`), through `playSound`, `playMusic`, `sendRenderState` called from eleven files of world, GUI and renderer code | the main thread, `ThreadManager::run` (`enginemain.cpp:64`, `threadmanager.cpp:26-29`) | holds by convention: every caller found runs in the game loop, but the queue is reachable from anywhere through the singleton, and a sound played from the main thread would be a second producer with no diagnostic |
| freeablo | `InputManager::mQueue` (`components/input/inputmanager.h:81`) | the main thread's event poll | the game loop | holds |
| supercollider, supernova | `read_frames`, `write_frames` (`sndfile_backend.hpp`) | reader thread (`:200`) / audio thread (`:232`) | audio thread (`:149`) / writer thread (`:270`) | holds; both threads are started by the class (`:116-119`) |
| Aleph One (game) | `OpenALManager::audio_players_shared` (`OpenALManager.h:116`, "pipeline main => audio thread") | the game thread, `PlaySound`, `PlayMusic`, `PlayStream` (`OpenALManager.cpp:184`, `:191`, `:199`) | the audio callback, `ProcessAudioQueue` (`:78`), and the game thread again in shutdown (`:230`) | holds by convention: the second consumer runs under `SDL_LockAudio`, which stops the callback; the queue's type says one consumer, the lock makes it true |
| audiogridder (audio plug-in) | `AudioStreamer::m_writeQ`, `m_readQ` (`AudioStreamer.hpp:531`) | the audio thread (`:166`, `:208`) / the streamer thread (`:116`) | the streamer thread (`:82`) / the audio thread (`:288`) | holds |
| Cinder (creative coding) | `audio::dsp::RingBuffer` (`RingBuffer.h`), boost's ring reimplemented | documented "single write thread" (`:104`) | "single read thread" (`:135`) | holds by convention: a documented contract on a library class |
| OSVR-Core (VR) | `TrackerThread::m_imuMessages` (`TrackerThread.h:221`) | the main thread, `submitIMUReport` ("Main thread method!", `TrackerThread.cpp:148-170`), from the device update callback (`org_osvr_unifiedvideoinertial.cpp:190`) | the tracker thread (`:250`, `:320`; started `org_osvr_unifiedvideoinertial.cpp:327`) | holds |
| OSVR-Core | `m_debugDataMessages` (`TrackerThread.h:224`) | the tracker thread (`TrackerThread.cpp:527`) | the main thread (`:175`) | holds |
| EPSILON (autonomous driving, ROS) | `EudmPlannerServer::p_input_smm_buff_` (`eudm_server_ros.cc:19`) | the ROS callback, `PushSemanticMap` (`:34`), dispatched by `ros::spinOnce()` on the main thread (`test_ssc_with_eudm.cc:95`) | the planner's own thread (`:92`, `:113`) | holds by convention: the single producer is the spinner's choice in another file; an `AsyncSpinner` there would make the callbacks concurrent with no diagnostic |
| plugdata (audio) | `Autosave::autoSaveQueue` (`Autosave.h:16`) | the pd thread, in a function queued by the message thread (`:92-98`, run by `Instance::sendMessagesFromQueue`, `Instance.cpp:1230`) | the message thread, `handleAsyncUpdate` (`:135`) | holds |
| plugdata | `Instance::functionQueue`, a `moodycamel::ConcurrentQueue` (`Instance.h:331`) | the message thread only: the autosave timer (`Autosave.h:92`), key handlers (`Instance.cpp:1188`, `:1214`), mouse handlers of Lua objects (`LuaObject.h:324-374`), canvas and object code | the pd thread (`Instance.cpp:1230`); the destructor drains it after that thread is gone (`:271`) | over-cautious: an MPMC queue where every producer site found is on the message thread and the one live consumer is the pd thread |
| Serial-Studio (telemetry) | `LatestFrameTap::m_mirrorRing`, `m_parseLoadRing` (`LatestFrameTap.h:114`, `:117`) | the builder thread (`:57`, `:83`) | the GUI thread | holds by convention: documented in the class comment |
| cactus-rt (robotics) | `ros2::Publisher::queue_` (`publisher.h:36`) | a real-time thread, `Publish` (`:104`) | the ROS thread, `DequeueAndPublishToRos` (`:38-53`) | holds by convention: one real-time thread per publisher is the library's intent, enforced by nothing |
| libremidi (MIDI) | pipewire `midi_out::m_queue`, `m_gcqueue` (`midi_out.hpp:200-201`) | the caller of `send_message` / `schedule_message` (`:176`, `:195`) / the process callback (`:148`) | the process callback (`:128-149`) / the caller (`:173`) | holds by convention: an API contract that one thread sends, stated nowhere in the file |
| ossia score (media) | `LV2::Model::ui_events`, `plugin_events`, `to_process_events` (`EffectModel.hpp:78-81`) | UI, plug-in, score | score, UI, the process | undecidable from the files read: which thread the plug-in writes from is the plug-in's |
| Dn-FamiTracker (tracker) | `CMIDI::m_MidiQueue` (`MIDI.h:103`) | the MIDI input callback | the sound thread | holds (one input device) |

Counts, part 2: 19 rows, one per queue or per pair used as one (25 queues
in all), in 14 projects: 8 rows hold, 9 hold by convention, 1 over-cautious
(plugdata's function queue), 1 undecidable, 0 broken.

### Sources read

DPDK: the corpus's tree of `git://dpdk.org/dpdk` at 6bbb7b3 (26.11.0-rc0).
The projects, each at the head of its default branch on 23 Oct 2026:

| project | repository | commit |
|---|---|---|
| mixxx | https://github.com/mixxxdj/mixxx | 2086fe0e551b |
| freeablo | https://github.com/wheybags/freeablo | 7e0844e08876 |
| supercollider | https://github.com/supercollider/supercollider | b2f6e4422088 |
| Aleph One | https://github.com/Aleph-One-Marathon/alephone | d2d8f6dac0ec |
| audiogridder | https://github.com/apohl79/audiogridder | 76dd9a006a86 |
| Cinder | https://github.com/cinder/Cinder | f90f332d1a4d |
| OSVR-Core | https://github.com/OSVR/OSVR-Core | 1af7438f74f3 |
| EPSILON | https://github.com/HKUST-Aerial-Robotics/EPSILON | 155f1b1c4dae |
| plugdata | https://github.com/plugdata-team/plugdata | f1de4cb4f5aa |
| Serial-Studio | https://github.com/Serial-Studio/Serial-Studio | 6709738c4c0d |
| cactus-rt | https://github.com/cactusdynamics/cactus-rt | 1ac871c818d4 |
| libremidi | https://github.com/celtera/libremidi | 291c7b9154b5 |
| ossia score | https://github.com/ossia/score | 7391d18e82b6 |
| Dn-FamiTracker | https://github.com/Dn-Programming-Core-Management/Dn-FamiTracker | f59ef895edc8 |

### Bugs on record from a wrong declaration

Confirmed from the primary pages (the audit of 22 Oct cites them): a Linux
veth/AF_XDP crash from a ring used by two producers; the DRM `spsc_queue`
misuse; LMAX Disruptor issues 249 and 271 (a single-producer sequencer used
by two); Netty pull requests 9579 and 4865 (an MPSC queue consumed from two
threads). Added here: channels-rs issue 2
(https://github.com/ryntric/channels-rs/issues/2, July 2026, open): the Rust crate's
`Sender` and `Receiver` implement `Clone` unconditionally, so a cloned sender
of an SPSC channel reaches the single-producer sequencer, which advances its
sequence without a compare-and-swap, and two producers claim one slot; the
proposed fix is a cardinality marker type, which is a declaration checked by
the type system, the same thing this compiler checks by analysis.

## Part 3: the compiler's check on the same code

The analysis reads these declarations since 26 October 2026 (`Utils.h`,
`LibraryQueueCall`). A DPDK ring is the object `rte_ring_create` or
`rte_ring_create_elem` hands back, or the one `rte_ring_init` is given;
every `rte_ring_*enqueue*` is a push and every `*dequeue*` a pop, except
inside the ring library's own functions. A side is declared single by an
`rte_ring_sp_*` or `rte_ring_sc_*` call on it, and by the ring's flags
(`RING_F_SP_ENQ`, `RING_F_SC_DEQ`) only when some call on that side follows
the flags: a side reached only by `rte_ring_mp_*` or `rte_ring_mc_*` calls
is many whatever the flags say. Flags computed at run time, given
differently by two creations, or not seen because the ring is created
elsewhere leave a side unknown, and a side unknown with more than one thread
on it is reported as not checkable. The four single-producer single-consumer
classes (five with moodycamel's blocking form) declare one thread on each
side through their type, whether their constructor is in sight or only their
pushes and pops are. A library's bodies are walked like the program's, so
program code that only a library calls, a functor run by boost's
`consume_one`, is counted on the thread that calls it; only the modelled
operations inside them are the library's. `rte_eal_mp_remote_launch` starts
its function on every worker lcore, and on the calling one too unless the
flag is `SKIP_MAIN`.

Any of these queues may be reached from another file, another process, or a
library that takes it whole or inside a structure of parameters, so a
verdict on one holds in this translation unit and says so. It names a reason
when it sees one: a call of `rte_ring_lookup`; a ring, or a structure
holding one in a member or an array member (a per-lcore configuration),
handed to a function with no body here, by its type or cast to a `void *`;
or any other `void *` handed to such a function when a ring is kept in some
structure, which is coarse (the `void *` may hold anything). When it sees
one, no flagless ring is called more cautious than needed. Any use of a
modelled push or pop other than a direct call takes its address (a pointer
set in code or by a file-scope initialiser, a member-function pointer, a
thread's entry) and makes every queue of its type unknown, since calls
through the pointer are not counted; so does a constructor or
`rte_ring_init` the walk cannot place. The tests are in
`test/Transforms/DeclaredTopology/` (`dpdk-*.c`, `spsc-*.cpp`).

**DPDK.** The ten applications of part 1, in the same tree (6bbb7b3,
26.11.0-rc0), by `tools/declared-check/dpdk.sh <work dir> [<dpdk
checkout>]`. It uses a checkout only at that commit, or clones one, and
writes nothing into it; configures it out of tree with meson for its
generated `rte_build_config.h` only (`-Dplatform=generic -Dmax_numa_nodes=1
-Ddisable_drivers='*'`, no apps, tests or examples); makes each application
one translation unit, its `.c` files included into one file (qos_sched's
static `app_numa_mask` in `args.c` renamed for that; simple_mp's command
table generated by `buildtools/dpdk-cmdline-gen.py` as its build does);
compiles it with the pinned ClangIR, `clang -std=gnu11 -O0 -include
rte_config.h -D_GNU_SOURCE -DALLOW_EXPERIMENTAL_API -march=x86-64-v2
-fclangir -emit-cir`, with the tree's `lib/*`,
`lib/eal/{include,x86/include,linux/include}`, `config`, the meson build
directory and the example's own directories on the include path; and runs
`wtc-opt <tu>.cir --lift-cir-to-wtc --queue-ownership-analysis -o
/dev/null`, keeping every remark. Run on 26 Oct 2026 with the analysis as of
the commits that add these models. Each row of part 1, with the check's
answer:

| application, ring | read in part 1 | the check | why it stops there |
|---|---|---|---|
| packet_ordering `rx_to_workers` | holds | **verified in this translation unit**: one producer, many consumers, declared SP | |
| packet_ordering `workers_to_tx` | holds | cannot verify | its two consumers, `tx_thread` and `send_thread`, start on the two arms of one branch; the count does not express "at most one of them" |
| distributor `rx_dist_ring` | holds | cannot verify | the ring reaches each lcore inside a heap `struct lcore_params` written by assigning a compound literal (`*p = (struct lcore_params){...}`), a struct copy the walk does not follow, so every access is unplaced |
| distributor `dist_tx_ring` | holds by convention | cannot verify | the same |
| qos_sched `rx_ring` | holds | cannot verify | each flow's rings live in an element of `qos_conf[]`, indexed at run time |
| qos_sched `tx_ring` | holds | cannot verify | the same |
| dma `rx_to_tx_ring` | holds | cannot verify | one ring per port from one call in a loop, so the verdict is the union over all of them; and the rx, tx and combined loops start on the arms of one branch |
| server_node_efd `rx_q` | holds, across two processes | **verified in this translation unit, producer side, one process**: the server has one producer and no consumer, so only its single-producer flag binds; the node's consumer is another program, which finds the ring by name and is unresolved there | the second process |
| client_server_mp `rx_q` | holds, across two processes | **verified in this translation unit, producer side, one process**, as above; unresolved in the client | the second process |
| simple_mp `send_ring`, `recv_ring` | correct as MPMC | no verdict | each process creates the ring or looks it up by name, depending on its role, so no access is placed |
| bbdev_app `enc_to_dec_ring` | holds, flags computed | no verdict | the ring is read from the lcore's element of a per-lcore configuration array |
| ip_pipeline message queues | holds | cannot verify | the data-plane side reads them from per-thread arrays, and the CLI's handler is reached through a function pointer, so any thread |
| ip_pipeline software queues | undecidable | nothing to check | enqueued and dequeued inside the port library, outside the translation unit |
| pipeline CLI rings | undecidable | nothing to check | the same |

Over the 14 rows: 3 verified in this translation unit, all three read as
holding, two of them on the producer side of one process only; 2 with
nothing to check, both read as undecidable; 9 where the check stops and says
why. It contradicts none of the readings and reports no broken declaration,
true or false. Each of the three verified rings also carries the coarse
reason above: a ring is kept in a structure, and a function with no body
here (`rte_eth_tx_buffer_set_err_callback`, `rte_efd_update`,
`rte_eth_call_rx_callbacks`) takes a `void *`. Where it stops, the cause is
the walk's, and the causes are few (a row may have two): an element of an
array indexed at run time (4 rows), a ring found by name in another process
(3), threads on the arms of one branch (2), a struct copy (2), a ring per
object of a loop (1), a handler reached through a function pointer and so
callable from any thread (1, ip_pipeline's CLI).

Rerun later on 26 Oct, the same translation units, with the analysis
counting an array of queues as one, threads on exclusive arms as one, and
following a record copied whole from a compound literal: 6 verified in this
translation unit, 2 with nothing to check, 6 where the check stops, and
still no broken declaration reported. The three new verified rows, all read
as holding: packet_ordering `workers_to_tx` (its two consumers start on the
two arms of one branch and count as one), distributor `rx_dist_ring` (the
rings in `struct lcore_params` are placed: the copy of a compound literal
is followed, and `rte_free`, with no body here, writes no field), and dma `rx_to_tx_ring` (with
the arms counted as one, the union over every port's ring has one producer
and one consumer, which is then true of each ring). distributor
`dist_tx_ring` now stops for its own reason: its two producers,
`lcore_distributor` and `lcore_rx_and_distributor`, start under two ifs on
one flag with opposite senses, conditions the walk does not compare, so it
warns rather than taking them as two; the reading above is that the ring
holds by convention. The array rows (qos_sched x2, bbdev_app, ip_pipeline)
do not move: there the array holds pointers to rings, or structures that
hold them, not ring objects, and the pointer read from an element indexed
at run time is still unplaced. The by-name rows are out of reach by design.

**Library users.** Of the 14 projects of part 2, one builds as a translation
unit with its own sources and vendored libraries alone: supercollider's
supernova `sndfile_backend` (at b2f6e4422088), driven by the body of its own
test, `testsuite/server/supernova/sndfile_backend_test.cpp`, with Boost.Test
left out (`tools/declared-check/supercollider.sh`, which clones it with the
two submodules it needs, and `supercollider_driver.cpp`; the same `wtc-opt`
invocation). Both `boost::lockfree::spsc_queue` members are recognised, as
members of any `sndfile_backend`, and declared one producer and one consumer
by their type; the check cannot verify either, because the reader and writer
threads start as `std::thread(std::bind(&member, this))`, whose entry the
walk does not resolve, so each is a function callable from any thread. The
other thirteen need their framework to compile at all (Qt, JUCE, ROS, SDL
and OpenAL, MFC, PipeWire), and each starts at least one side from a
callback that framework calls.

## What a compiler check needs

- **Multi-file reach.** Every DPDK verdict crosses files: the ring is
  created in `init.c` and used in `main.c` or `app_thread.c`; the launch is
  in `main`. The analysis is one translation unit today (R3 in
  `docs/mvp.md`).
- **A model of the EAL's launch.** `rte_eal_remote_launch(f, arg, lcore)` is
  one thread running `f`; `rte_eal_mp_remote_launch(f, ...)` is one per
  worker lcore; a loop over `RTE_LCORE_FOREACH_WORKER` around the first is
  one per iteration. The compiler reads both by name (`Utils.cpp`,
  `ThreadInstances.cpp`); the role dispatch inside `app_main_loop`
  (qos_sched) and `processing_loop` (bbdev_app), where one function runs
  on every lcore and branches on the lcore's configured role, needs the
  per-lcore configuration to be read as a constant of the run, which it is
  not.
- **Two processes.** server_node_efd and client_server_mp share rings by
  name across processes; the producer and the consumer are in different
  programs, and the single-consumer side is a property of the deployment.
- **Scripts.** ip_pipeline's and pipeline's software queues are bound to
  threads by a script read at run time; undecidable without the script.
- **What makes "holds by convention" checkable.** Nine of the nineteen
  library uses rest on something outside the queue's type: a comment, a
  singleton reachable from every thread, a lock around a second consumer,
  the spinner chosen in another file, an API contract. Each of these is
  exactly a place where the analysis gives a verdict (two consumer threads,
  a producer reachable from a thread it should not be on) and where a
  declared tag would turn a comment into a build error. That is the
  check's value as a section of the paper: not that the code is wrong
  today, but that nothing but reading keeps it right.

## Verdict on thesis C

The plan's criterion was three real findings, broken or over-cautious with
evidence. Over the 33 rows read (14 of DPDK, 19 of library users) the
audit found one over-cautious queue (plugdata), no broken declaration, two
latent two-producer traps that a change of one line would spring
(distributor's commented swap, EPSILON's spinner), and ten declarations
that hold only by convention (one of DPDK, nine of the library users).
That is fewer than
three; thesis C stays a section, the checking of declared topologies, and
not the paper's claim. The strongest argument the audit gives that section
is the ten conventions: declarations hold because people read, and a
compiler that proves the same two facts the declaration asserts is what
lets them stop.
