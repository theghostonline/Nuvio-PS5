<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Clean-room runtime shim

`libc.prx` is generated locally from the independently authored source in
`../libc_builder.cpp` plus the two manifests in this directory
(`api-surface.txt`, `imports.txt`). It contains **no** Sony runtime
implementation, proprietary SDK binary, or game file — it is a loader-satisfying
shim that forwards `malloc`/`free`/`posix_memalign` to the system
`libSceLibcInternal` and stubs/zero-fills the rest.

Distributed under GPL-3.0-or-later, the same as the rest of EVO Player.

- `api-surface.txt` / `imports.txt` — harvested from **ProsperoLight's** link.
  EVO re-harvests these from its own intermediate PIE in Phase 1b task 4
  (FFmpeg / OpenSSL / libass reference symbols ProsperoLight never did), which
  breaks `libc.prx.sha256` below — expected; EVO re-pins its own digest then.
- `libc.prx.sha256` — the upstream ProsperoLight release digest. Valid until
  the re-harvest above.
- `libc.prx` — the generated binary. **Git-ignored**; rebuilt by
  `scripts/package-app.sh` (or `--rebuild-libc`).

## Verify

```sh
cd tools/native-app/runtime && sha256sum -c libc.prx.sha256
```
