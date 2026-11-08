# NXFormat reader

Header-only reader for NXFormat (`.nx`) archives, used by `FileSys::NxContainer`
(`src/core/file_sys/nx_container.cpp`).

- Upstream: https://github.com/fab918/nxformat by fab918
- Version: tag `V1.0.0`, commit `ddf67861b58ec2d9611e80930670601a65c4a9c4`
- License: MIT (`LICENSE`, copied unchanged)

## Files

| File | Upstream path | SHA-256 |
|---|---|---|
| `nx_reader.hpp` | `dist/nx_reader.hpp` (identical to `reader/nx_reader.hpp`) | `db87154f6f1849782f510826db3bdfa16fa92e3f697f89922dfb8e83851c481a` |
| `LICENSE` | `LICENSE` | `c803c9420a16a056c0ed7459c30c804ebe35ddee989f068ef6db2031ca24bf1a` |

Not vendored: `nx_session.hpp`, `nx_native_sha256.hpp`, `nx_read_trace.hpp`, `nx_disk_source.hpp`
and the Rust packer. Zstandard decoding is supplied by Lichii (`src/core/file_sys/nx_zstd.h`).

## Local patches

None. Update by copying `dist/nx_reader.hpp` from a new tag and recording its commit and hash here.
