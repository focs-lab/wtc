# Reach and precision

Every queue of every program compiled, the analysis's verdict against the topology read
from the source (benchmarks/reach/oracle.json). wtc eb2e7e8dab2e.

| program | queue | oracle | verdict | outcome | cause | the analysis's account |
|---|---|---|---|---|---|---|
| examples/queue_example.cpp | q1 | spsc (1, 1) | spsc (1, 1) | exact |  |  |
| examples/queue_example.cpp | q2 | mpmc (many, many) | mpmc (many, many) | exact |  |  |
| examples/inline_pipeline.cpp | q | spsc (1, 1) | spsc (1, 1) | exact |  |  |
| examples/progress_consumer.cpp | q | spsc (1, 1) | spsc (1, 1) | exact |  |  |
| examples/batch_loop.cpp | q | spsc (1, 1) | spsc (1, 1) | exact |  |  |
| examples/batch_by_hand.cpp | q | spsc (1, 1) | spsc (1, 1) | exact |  |  |
| W3 pipeline (benchmarks/workloads/w3_pipeline_wtc.cpp) | stage1 | spsc (1, 1) | spsc (1, 1) | exact |  |  |
| W3 pipeline (benchmarks/workloads/w3_pipeline_wtc.cpp) | stage2 | spsc (1, 1) | spsc (1, 1) | exact |  |  |
| W7 batched pushes (benchmarks/workloads/w7_batch_wtc.cpp) | q | spsc (1, 1) | spsc (1, 1) | exact |  |  |
| PARSEC ferret, one thread per stage | q_load_seg | spsc (1, 1) | spsc (1, 1) | exact |  |  |
| PARSEC ferret, one thread per stage | q_seg_extract | spsc (1, 1) | spsc (1, 1) | exact |  |  |
| PARSEC ferret, one thread per stage | q_extract_vec | spsc (1, 1) | spsc (1, 1) | exact |  |  |
| PARSEC ferret, one thread per stage | q_vec_rank | spsc (1, 1) | spsc (1, 1) | exact |  |  |
| PARSEC ferret, one thread per stage | q_rank_out | spsc (1, 1) | spsc (1, 1) | exact |  |  |
| PARSEC ferret, threads per middle stage from argv | q_load_seg | spmc (1, many) | spmc (1, many); one per loop iteration on consumers, chosen at run time | exact |  |  |
| PARSEC ferret, threads per middle stage from argv | q_seg_extract | mpmc (many, many) | mpmc (many, many); one per loop iteration on producers and consumers, chosen at run time | exact |  |  |
| PARSEC ferret, threads per middle stage from argv | q_extract_vec | mpmc (many, many) | mpmc (many, many); one per loop iteration on producers and consumers, chosen at run time | exact |  |  |
| PARSEC ferret, threads per middle stage from argv | q_vec_rank | mpmc (many, many) | mpmc (many, many); one per loop iteration on producers and consumers, chosen at run time | exact |  |  |
| PARSEC ferret, threads per middle stage from argv | q_rank_out | mpsc (many, 1) | mpsc (many, 1); one per loop iteration on producers, chosen at run time | exact |  |  |
| PARSEC ferret from its own sources, its queue lifted (--unity of ferret-pthreads.c, queue.c, tpool.c), threads per middle stage from argv | q_load_seg | spmc (1, many) | mpmc (many, many); one per loop iteration on producers and consumers, chosen at run time | conservative | the loader's stage is started by the thread pool's loop over NTHREAD_LOAD, which the program never changes from 1 | producers: started in a loop at tpool.c:83:14 |
| PARSEC ferret from its own sources, its queue lifted (--unity of ferret-pthreads.c, queue.c, tpool.c), threads per middle stage from argv | q_seg_extract | mpmc (many, many) | mpmc (many, many); one per loop iteration on producers and consumers, chosen at run time | exact |  |  |
| PARSEC ferret from its own sources, its queue lifted (--unity of ferret-pthreads.c, queue.c, tpool.c), threads per middle stage from argv | q_extract_vec | mpmc (many, many) | mpmc (many, many); one per loop iteration on producers and consumers, chosen at run time | exact |  |  |
| PARSEC ferret from its own sources, its queue lifted (--unity of ferret-pthreads.c, queue.c, tpool.c), threads per middle stage from argv | q_vec_rank | mpmc (many, many) | mpmc (many, many); one per loop iteration on producers and consumers, chosen at run time | exact |  |  |
| PARSEC ferret from its own sources, its queue lifted (--unity of ferret-pthreads.c, queue.c, tpool.c), threads per middle stage from argv | q_rank_out | mpsc (many, 1) | mpmc (many, many); one per loop iteration on producers and consumers, chosen at run time | conservative | the output stage is started by the thread pool's loop over NTHREAD_OUT, which the program never changes from 1 | consumers: started in a loop at tpool.c:83:14 |
| PARSEC dedup as written, threads per stage from argv | refine_que | spmc (1, many) | mpmc (many, many) | conservative | the threads start in Encode, which only main, in another file, calls | producers: started from Encode, which may run any number of times: nothing in this translation unit calls it |
| PARSEC dedup as written, threads per stage from argv | deduplicate_que | mpmc (many, many) | mpmc (many, many) | exact |  |  |
| PARSEC dedup as written, threads per stage from argv | compress_que | mpmc (many, many) | mpmc (many, many) | exact |  |  |
| PARSEC dedup as written, threads per stage from argv | reorder_que | mpsc (many, 1) | mpmc (many, many) | conservative | the threads start in Encode, which only main, in another file, calls | consumers: started from Encode, which may run any number of times: nothing in this translation unit calls it |
| PARSEC dedup, one thread per stage | refine_que | spsc (1, 1) | mpmc (many, many) | conservative | the threads start in Encode, which only main, in another file, calls | producers: started from Encode, which may run any number of times: nothing in this translation unit calls it; consumers: started from Encode, which may run any number of times: nothing in this translation unit calls it |
| PARSEC dedup, one thread per stage | deduplicate_que | spsc (1, 1) | mpmc (many, many) | conservative | the threads start in Encode, which only main, in another file, calls | producers: started from Encode, which may run any number of times: nothing in this translation unit calls it; consumers: started from Encode, which may run any number of times: nothing in this translation unit calls it |
| PARSEC dedup, one thread per stage | compress_que | spsc (1, 1) | mpmc (many, many) | conservative | the threads start in Encode, which only main, in another file, calls | producers: started from Encode, which may run any number of times: nothing in this translation unit calls it; consumers: started from Encode, which may run any number of times: nothing in this translation unit calls it |
| PARSEC dedup, one thread per stage | reorder_que | mpsc (many, 1) | mpmc (many, many) | conservative | the threads start in Encode, which only main, in another file, calls | consumers: started from Encode, which may run any number of times: nothing in this translation unit calls it |
| PARSEC dedup, one thread per stage, queues as globals | refine_que | spsc (1, 1) | mpmc (many, many) | conservative | the threads start in Encode, which only main, in another file, calls | producers: started from Encode, which may run any number of times: nothing in this translation unit calls it; consumers: started from Encode, which may run any number of times: nothing in this translation unit calls it |
| PARSEC dedup, one thread per stage, queues as globals | deduplicate_que | spsc (1, 1) | mpmc (many, many) | conservative | the threads start in Encode, which only main, in another file, calls | producers: started from Encode, which may run any number of times: nothing in this translation unit calls it; consumers: started from Encode, which may run any number of times: nothing in this translation unit calls it |
| PARSEC dedup, one thread per stage, queues as globals | compress_que | spsc (1, 1) | mpmc (many, many) | conservative | the threads start in Encode, which only main, in another file, calls | producers: started from Encode, which may run any number of times: nothing in this translation unit calls it; consumers: started from Encode, which may run any number of times: nothing in this translation unit calls it |
| PARSEC dedup, one thread per stage, queues as globals | reorder_que | mpsc (many, 1) | mpmc (many, many) | conservative | the threads start in Encode, which only main, in another file, calls | consumers: started from Encode, which may run any number of times: nothing in this translation unit calls it |
| PARSEC dedup, one thread per stage, queues as globals, one translation unit | refine_que | spsc (1, 1) | spsc (1, 1) | exact |  |  |
| PARSEC dedup, one thread per stage, queues as globals, one translation unit | deduplicate_que | spsc (1, 1) | spsc (1, 1) | exact |  |  |
| PARSEC dedup, one thread per stage, queues as globals, one translation unit | compress_que | spsc (1, 1) | spsc (1, 1) | exact |  |  |
| PARSEC dedup, one thread per stage, queues as globals, one translation unit | reorder_que | mpsc (many, 1) | mpsc (many, 1) | exact |  |  |
| PARSEC dedup as written, threads per stage from argv, one translation unit | refine_que | spmc (1, many) | spmc (1, many) | exact |  |  |
| PARSEC dedup as written, threads per stage from argv, one translation unit | deduplicate_que | mpmc (many, many) | mpmc (many, many) | exact |  |  |
| PARSEC dedup as written, threads per stage from argv, one translation unit | compress_que | mpmc (many, many) | mpmc (many, many) | exact |  |  |
| PARSEC dedup as written, threads per stage from argv, one translation unit | reorder_que | mpsc (many, 1) | mpsc (many, 1) | exact |  |  |
| PARSEC dedup, one thread per stage, one translation unit | refine_que | spsc (1, 1) | spsc (1, 1) | exact |  |  |
| PARSEC dedup, one thread per stage, one translation unit | deduplicate_que | spsc (1, 1) | spsc (1, 1) | exact |  |  |
| PARSEC dedup, one thread per stage, one translation unit | compress_que | spsc (1, 1) | spsc (1, 1) | exact |  |  |
| PARSEC dedup, one thread per stage, one translation unit | reorder_que | mpsc (many, 1) | mpsc (many, 1) | exact |  |  |

48 queues: 36 exact (19 of the 25 single-producer single-consumer ones proved so), 12 conservative, 0 refused, 0 wrong. 10 of them have the tag chosen when the queue is built, from the count of the loop that starts a side's threads.
