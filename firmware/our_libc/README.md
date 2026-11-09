# our_libc — the "write it ourselves" libc branch

The firmware uses exactly two libc functions: **`memcpy`** and **`memset`**.
Nothing else — no `malloc`, no `printf`, no string functions. (Verify any time
with `grep -rnoE '\b(mem|str|printf|malloc|free|calloc)\w*' app bootloader common`.)

Because the surface is that small, the build offers three ways to provide those
two functions, selected by one macro (see the firmware `Makefile`, `LIBC`):

| `make LIBC=...` | Macro defined | Where memcpy/memset come from |
|---|---|---|
| `new` (default) | `USE_NEW_LIBC` | newlib's `libc.a` from the arm-none-eabi toolchain — the historical behaviour |
| `musl` | `USE_MUSL_LIBC` | musl's own `memcpy.c` / `memset.c`, vendored under `our_libc/musl/` |
| `our` | `USE_OUR_LIBC` | the hand-written ARM assembly in this directory |

This directory holds the `our` branch.

## The assembly

- [`memcpy.S`](memcpy.S) — `void *memcpy(void *dest, const void *src, size_t n)`
- [`memset.S`](memset.S) — `void *memset(void *dest, int c, size_t n)`

Both are ARMv6-M Thumb (Cortex-M0+ is Thumb-only, no Thumb-2). They follow the
AAPCS exactly — `r0=dest, r1=src/c, r2=n`, return `dest` in `r0` — because the
compiler emits `bl memcpy` / `bl memset` for struct copies and array inits and
expects standard-conforming implementations at those symbol names.

Each does a word-at-a-time bulk loop when the pointers are word aligned (which
all our real buffers are — frames, EEPROM records, image half-pages) and falls
back to a byte loop otherwise. They are deliberately plain: on an M0+ the memory
bus, not the instruction stream, is the limit, so unrolling would buy nothing and
cost flash.

## Why not "real" musl for the `musl` branch?

musl is a *hosted* libc — it assumes an OS and syscalls, and there is no
pre-built `arm-none-eabi` bare-metal musl. For two freestanding functions the
only sane thing is to vendor musl's `memcpy.c` and `memset.c` (both pure,
freestanding C, MIT-licensed) into `our_libc/musl/`. That is what `LIBC=musl`
compiles. See `our_libc/musl/README.md` for provenance.

## Correctness

The three implementations are interchangeable and are checked against each other
on the build host by `our_libc/test/test_mem.c` (run `make libc-check` in the
firmware dir). memcpy/memset are the kind of thing that is easy to get subtly
wrong at the alignment boundaries, so the test hammers every offset/length/
overlap-free combination in a small range and compares byte-for-byte against the
host's system memcpy/memset.
