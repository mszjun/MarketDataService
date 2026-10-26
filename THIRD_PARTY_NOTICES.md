# Third-party notices

This project is distributed under the MIT License (see [LICENSE](LICENSE)).
The components below keep their own licenses; the MIT license does **not**
replace or override them.

## cJSON

- Files: `third_party/cJSON.h`, `third_party/cJSON.cpp`
- License: MIT
- Copyright: Copyright (c) 2009 Dave Gamble
- Source: https://github.com/DaveGamble/cJSON

## standalone Asio

- Files: `third_party/asio-asio-1-30-2.zip` (extracted into the build tree)
- License: Boost Software License 1.0
- Copyright: Copyright (c) 2003-2025 Christopher M. Kohlhoff
- Source: https://github.com/chriskohlhoff/asio

## CTP market-data SDK

- Files: `third_party/v6.7.10_20250422_traderapi.zip` (extracted into the build tree)
- Owner: 上海期货信息技术有限公司 (Shanghai Futures Information Technology Co., Ltd.)
- License: **proprietary**; not covered by this project's MIT license.
- The archive is included unmodified to keep the build reproducible. It also
  contains the vendor's compliance and privacy documents.
- Redistribution may be restricted by the vendor's terms. If you do not have
  the right to redistribute this SDK, remove the archive from your fork and
  configure the build against a locally installed SDK instead:

  ```bash
  cmake -S . -B build -DCTP_API_DIR=/path/to/mduser_sdk
  ```

- The SDK is required only at build/run time; the project source does not
  contain any CTP code.
