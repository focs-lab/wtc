`work` is 64 rounds of a linear congruential step, which the compiler
folds eight at a time into one multiply and one add, so it is about
15 ns of a thread's own arithmetic between sections, not 64 rounds'
worth. Alone, the plain build hides it behind the mutex's own latency
(33.4 against 35.2 million sections a second without it); the batched
build, which no longer waits for the mutex, shows it.

The finish spread is the time from the first thread's end to the last
thread's, as a share of the run. Holding the mutex across 64 sections
did not make it large here: at most 3.6% of the run with work, 2.4%
without, against 1.2% and 1.7% for the plain build. Which thread gets
the mutex when it is let go is the mutex's choice; std::mutex makes no
promise of fairness, and nothing here is one.

The checksum of every run with the same thread count agrees across the
two builds. The runs were made on the tree of commit e363247; `git_dirty`
as in the w2mvp leg of the same session.
