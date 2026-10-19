The last line of `log.txt` reports 3 distinct checksums. That count was
taken over whole lines of `checksums.txt`, and each line also names the
implementation its run carried (generic, spsc, spsc_inline), which
differs between builds by design. Every run's checksum value is the same;
`campaign.sh` now compares the values alone.

The runs were made on the tree of the commit that adds these results'
documentation, recorded in `meta.json` under its sha from before the
branch was rebased onto `main` (e6581d1); the rebase changed no tree.
`git_dirty` is true because earlier legs' result directories were
untracked when each leg started.
