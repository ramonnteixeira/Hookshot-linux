# Pipeline cache save failures

`pipeline_cache_save` writes a temporary file before replacing the existing cache. Allocation failures in its
background worker must stay in that worker, release its busy flag and leave later saves possible. Starting the
worker can fail too. The synchronous prepare-only save follows the same exception handling.

Run the game-free check with a C++20 compiler and its runtime libraries available:

```sh
python -I -B tests/test_pipeline_cache_save.py --cxx clang++
```

The fixture compiles the actual function with Vulkan API spies, real C++ containers/threads, and authored temporary
file bytes. It injects allocation failures into the vector, temporary path and thread startup; an oversized vector
checks `length_error`. Each fault case checks that the old file survives and a subsequent save completes. Success,
the 32-pipeline threshold, a single forced-waiter scheduling smoke, Vulkan empty/error returns and a failed write
are covered too. The waiter smoke is not a deterministic concurrency proof.

`--baseline-source /path/to/original/vk_draw.cpp` additionally compares the original success path and runs six
original-failure negatives in separate, bounded processes. Uncaught worker exceptions terminate those children;
an independent authored thread control identifies the exact MSVC fail-fast result where its custom terminate
handler is bypassed. Fixed children must always exit successfully.

These checks use no driver or real cache and do not reproduce issue #1022's reported session. They do not establish
its root cause, validate saving a multi-gigabyte driver cache, force an OS thread-resource failure, or change cache
size/retry policy. The existing wait-then-store protocol for concurrent forced callers is unchanged; the current
force call is in prepare-only startup. Driver/native faults and thread-detach failures are outside this change.
