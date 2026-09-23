---
title: wmm
parent: Libraries
---

# wmm

## Overview

The World Magnetic Model has two parts. The first is a compile-time parser for
NOAA/NCEI `.COF` coefficient files. The second is the field synthesis, ported
from NCEI's `GeomagnetismLibrary.c`. What the estimator will want from it is
the reference field vector at a position and date, which a magnetometer
factor compares with the MTi's reading.

The library is split into two targets. `wmm` is the generic, header-only part:
the parser, `Model<N>`, and `magnetic_field`. `wmm_hr2025` is WMM-HR 2025, a
degree-133 model, behind plain functions. Its one translation unit `#embed`s
the coefficient file and parses it during compilation. As a result, a damaged
file is a build error that names the line, nothing is read at runtime, and a
consumer never sees the parser. The consumer needs only C++20 and does not
need the data file on its include path.

It takes the WGS 84 ellipsoid from [geodesy](geodesy.html), so the tree has
one ellipsoid. The geomagnetic reference radius is the model's own.

{: .note }
As of 2026-09-23 nothing in the estimator uses this library. The magnetometer
factor is deferred until the MTi's hard- and soft-iron calibration exists,
because comparing an uncalibrated magnetometer with this reference would
corrupt heading rather than aid it.

## Public headers

| Header | |
| --- | --- |
| `wmm/wmmhr2025.h` | The compiled model: `wmmhr2025::model()`, `magnetic_field(position, decimal_year)`, an overload taking a `Date`, and `uncertainty()`. The only header a consumer of `wmm_hr2025` includes. |
| `wmm/types.h` | `Coefficient`, `Date`, `decimal_year`, `GeodeticCoord`, and `MagneticElements` (X, Y, Z north/east/down in nT, plus H, F, I, D, grid variation and their rates). Valid C++20. |
| `wmm/model.h` | `Model<N>`: the coefficients of a degree-N model plus its header metadata, with `valid_from()` and `valid_until()`. |
| `wmm/cof.h` | The parser: `parse_cof` at runtime or during constant evaluation, `load_cof<Text>()` for an embedded file, `ParseError`, `CofErrorSite`, `CofParseError`. |
| `wmm/field.h` | `magnetic_field(model, position, year)` for any `Model<N>`. Fully `constexpr`. |
| `wmm/math.h` | The `constexpr` sqrt, sin, atan and related functions the synthesis needs, with the platform libm at runtime. |

## Using it

Link `wmm_hr2025`:

```cpp
#include "wmm/wmmhr2025.h"

const wmm::GeodeticCoord at{.latitude_deg = 33.68, .longitude_deg = -117.86, .height_km = 0.02};
const wmm::MagneticElements b = wmm::wmmhr2025::magnetic_field(at, wmm::Date{2026, 9, 23});
const wmm::MagneticElements sigma = wmm::wmmhr2025::uncertainty(b);
```

Link `wmm` instead only to parse a different `.COF`, or to evaluate a field
inside a `static_assert`. The model's validity window is reported but not
enforced. Grid variation is defined only poleward of ±55° and is NaN
elsewhere, as it is in NCEI's own test values.

## Behaviour worth knowing

A parse error has to reach the compiler's diagnostic with its line number. C++26
would let `static_assert` print a computed message; C++23 does not. So
`load_cof` instantiates `CofParseError<CofErrorSite{...}>`, carrying the
message and the line as a non-type template argument, and the error appears in
the instantiation trace. GCC prints the message text. Clang prints it as
character codes beside the line number.

`read_double` refuses a decimal exponent above 308 or below -400 before it
multiplies anything. Without that check, `1e999` parsed as infinity: a
coefficient that loads cleanly and poisons every field value computed after
it. The malformed-file test found this. The refusal has to come before the
multiply, because GCC does not treat a floating-point overflow as a constant
expression. An exponent in the billions is also refused, since it would
otherwise run the constant evaluator out of steps. Within range, a COF number
parses bit-identical to `strtod`.

`#embed` in C++ is an extension before C++26. Clang warns about it and GCC 15
accepts it silently. The pragma that silences Clang is guarded by `__clang__`.

The coefficients and test values come from NOAA/NCEI. As a work of the US
government they are in the public domain.

## Tests

All carry the labels `wmm unit`.

| Target | What it proves |
| --- | --- |
| `wmm_test_hr2025` | The twelve NCEI test points, every column, each as a `static_assert`, so a wrong coefficient or a wrong recursion fails the build. It also checks that compile-time and runtime evaluation agree. |
| `wmm_test_cof` | The parser: every error it reports and the line it blames. It covers malformed files (truncated records, NaN and infinite coefficients, `1e999`, negative degree or order, an impossible date, missing terms), and a runtime parse of the full file against the compiled model. |
| `wmm_test_api` | The compiled interface, built as C++20 from a TU that cannot reach the parser or the embedded data. |
