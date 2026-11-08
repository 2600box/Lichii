# NXFormat reader

Header-only reader for NXFormat (`.nx`) archives, used by the NX loader
(`src/core/loader/nx.cpp`). The Qt frontend's NX converter uses nxformat's prebuilt C library
instead, downloaded by `CMakeModules/NxFormat.cmake`.

- Upstream: https://github.com/fab918/nxformat by fab918
- Version: tag `V1.0.0` at commit `ddf67861b58ec2d9611e80930670601a65c4a9c4`. The tag was later moved
  to `c70daf6f35d3223d83603dee293f92df7a8f815c`, which leaves these headers unchanged.
- License: MIT (`LICENSE`)

| File | Upstream path | SHA-256 |
|---|---|---|
| `nx_reader.hpp` | `dist/nx_reader.hpp` | `db87154f6f1849782f510826db3bdfa16fa92e3f697f89922dfb8e83851c481a` |
| `nx_session.hpp` | `dist/nx_session.hpp` | `fdd9fabec4264fc7b180061fb60122f938b1d4851bbff08b7bc8d0d8c35c9fc8` |
| `nx_disk_source.hpp` | `dist/nx_disk_source.hpp` | `35ce56e0c1316b55353d56910751869befccced0236502ec9743629ff1fa23f2` |
| `LICENSE` | `LICENSE` | `c803c9420a16a056c0ed7459c30c804ebe35ddee989f068ef6db2031ca24bf1a` |

The headers are identical to the `include/` copies in the V1.0.0 release archives. No local
patches.
