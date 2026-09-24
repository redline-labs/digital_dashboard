# models/

Geophysical models the nodes load at run time. Each is kept **exactly as its
publisher distributes it** and stored in Git LFS (`.gitattributes`), so a
checkout needs `git lfs install && git lfs pull` to have them. They install
under the prefix beside `bin/` (`/opt/redline/models/...`) and are found by
`core::paths::resource("models/...")`, in an install and in a checkout alike.
See `CMakeLists.txt` here for the guard against installing LFS pointer files.

| Path | What | Source | SHA-256 |
|---|---|---|---|
| `deflec2022/SDEFLEC2022.NA.eta.beta_v0a.bin` | DEFLEC2022 static deflection of the vertical, east-west (eta), North America, 1', arcseconds | NGS NAPGD2022 beta, <https://beta.ngs.noaa.gov/NAPGD2022/download.html> | `5b1ffadf045b2549749f042281f51d4b1bf05081a2dd95c210f6bbbe879e98fb` |
| `deflec2022/SDEFLEC2022.NA.xi.beta_v0a.bin` | the same, north-south (xi) | as above | `6c2a0d335016e410756c58dabda3493bbbc79b6eeaa1afa747afda9574181f53` |

Downloaded 2026-09-24. `beta_v0a` is a pre-release; a new release replaces
both files and the names in `libs/deflec/include/deflec/model.h`.
