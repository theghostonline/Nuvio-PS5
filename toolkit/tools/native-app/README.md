<!--
SPDX-License-Identifier: GPL-3.0-or-later
Copyright (C) 2026 BlackBearReloaded  (upstream boilerplate + ProsperoLight)
Copyright (C) 2026 EVO Player contributors  (vendoring + EVO integration)
-->

# `tools/native-app/` — PS5 game-category app-module build tail

This directory is the **link + package tail** for building EVO Player as a
fake-signed, game-category **app module** (`PPSA99039`) instead of an
hbldr/elfldr payload. It is what Phase 1b needs — see
[docs/evo-pro/phase-1b-app-module.md](../../docs/evo-pro/phase-1b-app-module.md).

Compilation still uses EVO's normal toolchain and per-object build; only the
**final link, the LLVM-PIE → PS5-module conversion, the FSELF signing, and the
`dist/` assembly** happen here. Driven by
[`scripts/package-app.sh`](../../scripts/package-app.sh).

## Provenance

Forked verbatim from **`blackbearreloaded/ps5-native-app-boilerplate`**
(`a15ab71`, GPL-3.0-or-later) by way of its downstream **ProsperoLight**
(`github.com/blackbearreloaded/ProsperoLight`, clone commit
`103264dfe2761d35772671792fff4f013a573e70`), which is the tree the Phase 1
hardware gate ran against on 2026-09-01.

The local reference clone lives at `third_party/ProsperoLight/` (git-ignored);
see its `README.EVO.md`. Files were copied from:

| Vendored here | Upstream path (in the clone) |
|---|---|
| `app_crt.cpp`, `app_cpp_runtime.cpp`, `ps5-pie.ld`, `app-symbols.map` | `tooling/native/` |
| `hash.hpp`, `elf_object.{cpp,hpp}`, `self_container.{cpp,hpp}`, `sce_module_writer.{cpp,hpp}` | `tooling/native/` |
| `native_app_builder.cpp`, `libc_builder.cpp` | `tooling/native/` |
| `runtime/api-surface.txt`, `runtime/imports.txt` | `tooling/native/runtime/` |
| `runtime/libc.prx.sha256` | `runtime/` |
| `prospero-clang18` | `tooling/` |
| `stubs/*.c` | `vendor/ps5/sdk/stubs/` |

All upstream SPDX + copyright headers are kept intact. Upstream files are
verbatim (LF-normalised via `dos2unix`) **except**:

- **`ps5-pie.ld`** — Phase 1b task 4 added `PROVIDE_HIDDEN(__eh_frame[_hdr]_{start,end})`
  in the `.eh_frame[_hdr]` output sections. `libunwind.a` references those
  bounds; a custom `-T` script must define them (the SDK's default script does).
- **`stubs/libc_ext.c`** — NEW, EVO-authored, small. Just `_setjmp`/`_longjmp`
  (asm alias to the plain forms), `dladdr` + `__dl*` no-ops, and `recvmmsg`/
  `sendmmsg` (ENOSYS). `scripts/package-app.sh` compiles it INTO `eboot.bin`.
  The bulk of the FreeBSD libc gap (xlocale `*_l`, `gmtime_r`, `nl_langinfo`,
  `__assert`, `catgets`, `_DefaultRuneLocale`, …) is filled by linking the
  **static SDK `libc.a`** last (in a `--start-group`); its `no-locale.o` etc.
  carry the real C-locale code. `libc_ext.c` only covers what `libc.a` lacks
  (`_setjmp`) or whose object drags an unresolvable rtld cascade (`dladdr` →
  `__dlopen`). An earlier approach — regenerating `libSceLibcInternal.so` with
  `return 0` stubs — SIGSEGV'd on hardware because the console's real
  `libSceLibcInternal` doesn't export the xlocale family and the stub bodies
  are discarded, so the imports bound to NULL.

- **`stubs/evo_dns.c`** — NEW, EVO-authored (#91). `getaddrinfo` /
  `freeaddrinfo` / `getnameinfo` / `gai_strerror`, compiled INTO `eboot.bin`
  because on retail FW 12.70 the console exports them only from
  `libScePosixForWebKit.sprx` as NULL stubs (a call faults at `rip=0`). A
  lookup goes to **the console's own resolver** - `libSceNet`'s
  `sceNetPoolCreate` / `sceNetResolverCreate` / `sceNetResolverStartNtoa` - so it
  uses whatever DNS the user set in the PS5's network settings. That is the call
  sequence the SDK's own `libc.a` (`netdb.o`) uses on payloads; the difference is
  a one-time `sceNetInit()` if the first pool will not create (an app module may
  not have `libSceNet` up the way a payload's process does), and one
  `dns: <host> -> <ip>, <ip>  (console DNS, N addresses, M ms)` line in
  `evo.log` per real lookup. It returns **every** address the resolver finds
  (`sceNetResolverStartNtoaMultipleRecordsEx`, up to ten), chained through
  `ai_next`, so FFmpeg's connect can race past an address that is unreachable
  from the user's network - one of raw.githubusercontent.com's four was, and the
  single-record call returned only that one. That call's result layout is not
  documented in this repo, so it is trusted only as far as it checks out (a
  canary past the buffer, counts in range, plausible addresses); anything else
  falls back to the single-address lookup, which is then used for the rest of
  the run. `/mnt/usb0/evo_dns_single` turns the multi-record call off without a
  rebuild. IPv4 literals, `localhost` and a 64-entry, 5-minute cache never reach
  the resolver. It replaced two hand-rolled UDP DNS clients that hardcoded eight
  servers, the first a developer's laptop. The `libSceNet` import resolves from
  the SDK's link stub like `libSceImeDialog`'s does; no `.syms` file is needed.
  `tools/dns_host.sh` tests it on the host against a mock `libSceNet`.

- **`stubs/malloc_shim.c`** — NEW, EVO-authored. Interposes the whole
  `malloc`/`free`/`calloc`/`realloc`/`posix_memalign`/`aligned_alloc`/…
  family onto an **mmap-backed allocator**: requests > 12 KiB get one
  page-rounded `mmap` each (32-byte-aligned user pointer, `munmap` on free),
  smaller ones come from size-classed slabs carved out of 2 MiB `mmap` arenas
  with per-class free lists. Compiled in first so its definitions win.
  Needed because the clean-room `libc.prx` installs a **bounded** heap mspace
  (via `_sceKernelRtldSetApplicationHeapAPI`) that fills up — on hardware
  `malloc(8 MB)` returned NULL with ~360 MB flexible memory free. This is plan
  risk **R1**.

## Licensing

EVO Player is already GPL-3.0-or-later ([LICENSE](../../LICENSE)), so vendoring
this GPL-3.0-or-later tooling and shipping the generated `libc.prx` adds **no
new obligation**. Compliance rests on shipping `libc.prx` **with** its in-repo
emitter (`libc_builder.cpp` + `runtime/api-surface.txt` + `runtime/imports.txt`)
— there is no Sony implementation in it. The generated `runtime/libc.prx`
binary itself is git-ignored and rebuilt locally.

## What each piece does

| Piece | Role |
|---|---|
| `app_crt.cpp` | Custom `_start` → `_init_env` → `.preinit`/`.init_array` → `main` (asm-aliased) → `exit`. Replaces the SDK `crt1.o`. |
| `app_cpp_runtime.cpp` | `operator new`/`delete`; routes allocations ≥64 KB to `mmap`, smaller to `posix_memalign`. Also `ar`-packed as a fake `libpthread.a` to satisfy libc++'s recorded dependency. |
| `ps5-pie.ld` | Segregated `PT_LOAD` layout (text / ro / relro / data), `.text` at 0, keeps `.dynsym`/`.rela` only for the converter to consume. |
| `app-symbols.map` | `{ local: *; }` — hides every app symbol so the module writer sees only PS5 imports. |
| `prospero-clang18` | Compiler wrapper: `-target x86_64-sie-ps5 -fno-stack-protector -fno-plt -femulated-tls`, SDK sysroot + libc++ v1 includes. |
| `native_app_builder.cpp` (+ `elf_object`, `self_container`, `sce_module_writer`, `hash.hpp`) | Host converter: rewrites the intermediate LLVM PIE → PS5 module `eboot.elf`, stamps module-SDK `0x02000009` / companion `0x08050001`, rewrites imports to SCE stubs, then `self --sign` → `eboot.bin` (FSELF magic `0x1D3D154F`). |
| `libc_builder.cpp` + `runtime/{api-surface,imports}.txt` | Deterministically emits the clean-room `runtime/libc.prx` shim (forwards `malloc`/`free`/`posix_memalign` to `libSceLibcInternal`, stubs/zero-fills the rest). |
| `stubs/*.c` | Linker-only `--shared` import descriptions for SCE modules with no `$SDK/target/lib/libSce*.so`. Never packaged, never executed. |

## Rebuilding `runtime/libc.prx`

`scripts/package-app.sh` builds it automatically when missing. To force it:

```bash
docker compose run --rm ps5-dev ./scripts/package-app.sh --rebuild-libc
```

The generated binary is verified byte-for-byte against `runtime/libc.prx.sha256`
(and the pinned raw/signed digests inside the builder script). EVO will
**re-harvest** `runtime/api-surface.txt` from its own link in Phase 1b task 4
(FFmpeg / OpenSSL / libass add symbols); that intentionally breaks the pinned
digest and EVO re-pins its own.
