# Vendored baseline queues

Header-only, copied verbatim at the commits (or the archive) below. Do not
edit; re-vendor to update and record the new commit here. The third column
is what meta.json records as the version, so it has no spaces.

| directory | upstream | commit | licence | fetched |
|---|---|---|---|---|
| readerwriterqueue/ | https://github.com/cameron314/readerwriterqueue | 6867b56452352acf077fccd5f6cc7e3a8cfde0fb | BSD-2-Clause (LICENSE.md) | 2026-09-18 |
| concurrentqueue/ | https://github.com/cameron314/concurrentqueue | 683b9e31ea15eb69f1b81cc1defc7850d5f20b71 | BSD-2-Clause or BSL-1.0 (LICENSE.md) | 2026-09-18 |
| rigtorp/ | https://github.com/rigtorp/SPSCQueue | 1053918dbd251fbff69b24ef27fa5d51c29ec2af | MIT (LICENSE) | 2026-09-18 |
| lprq/ | https://doi.org/10.5281/zenodo.7337237 (the LPRQ paper's artifact, cpp-ring-queues-research.zip, md5 49882199f9a1e65eff96c04332ef10b4) | zenodo-7337237 | MIT (LICENSE) for the artifact's own files; HazardPointers.hpp, FAAArrayQueue.hpp and LCRQueue.hpp carry the Concurrency Freaks BSD-3-Clause notice they came with; lprq/scqueue/ is the SCQ code it carries, dual 2-Clause BSD and MIT (scqueue/README.md) | 2026-10-23 |

lprq/ holds the queues of the LPRQ paper's evaluation (PPoPP 2023: LCRQ,
LPRQ, LSCQ, FAAArrayQueue) with the helpers they need and nothing else of
the artifact; LSCQ sits on the SCQ ring in C under lprq/scqueue/, which is
built as a small static library. These headers need C++20.

boost::lockfree comes from the system package (boost 1.83) and is not vendored.
