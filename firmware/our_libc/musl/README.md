# Vendored musl memcpy / memset

For the `LIBC=musl` build branch (`-DUSE_MUSL_LIBC`).

musl has no bare-metal `arm-none-eabi` port — it is a hosted libc that assumes an
OS and syscalls. Since the firmware needs only `memcpy` and `memset`, and both
are pure freestanding C in musl, the honest way to "use musl" here is to vendor
those two source files directly.

## Provenance

| file | source | modified? |
|---|---|---|
| `memcpy.c` | musl `src/string/memcpy.c` | **no** — byte-for-byte upstream |
| `memset.c` | musl `src/string/memset.c` | **no** — byte-for-byte upstream |
| `COPYRIGHT` | musl `COPYRIGHT` | upstream, for the MIT license text |

Fetched from `https://git.musl-libc.org/cgit/musl/plain/src/string/` .

The sources are **not edited**. `memcpy.c` includes `<endian.h>` and uses
`__BYTE_ORDER`, which do not exist in our `-nostdlib` build; rather than patch the
musl source and muddy its provenance, the firmware Makefile puts this directory on
the include path (`-Iour_libc/musl`) so the minimal [`endian.h`](endian.h) here
resolves that include and supplies the little-endian `__BYTE_ORDER` macros from
the compiler's own built-ins. `endian.h` is ours (SPDX MIT to match); the musl
code is unchanged. `size_t` and the fixed-width ints come from the toolchain's
freestanding `<string.h>` / `<stdint.h>`.

## License

musl is MIT-licensed (see `COPYRIGHT`). Attribution is preserved here.

## Updating

To refresh against a newer musl:

```sh
curl -fsSL -o memcpy.c https://git.musl-libc.org/cgit/musl/plain/src/string/memcpy.c
curl -fsSL -o memset.c https://git.musl-libc.org/cgit/musl/plain/src/string/memset.c
```

then re-run `make LIBC=musl libc-check` in the firmware directory to confirm the
new copies still pass the cross-check against the assembly and newlib versions.
