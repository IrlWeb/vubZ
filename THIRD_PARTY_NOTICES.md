# Third-Party Notices

This repository vendors third-party code under `Dependencies/`. The original
supQ code is licensed under the MIT License in `LICENSE`; the components below
remain under their own licenses.

The vendored snapshots did not include the upstream license files. The
canonical license texts are available at the URLs below. Before distributing a
binary, include the full license text and copyright notice for every component
you redistribute.

| Component | Version / snapshot | License | Copyright | Upstream |
|---|---|---|---|---|
| Luau | vendored snapshot | MIT | Roblox Corporation | https://github.com/luau-lang/luau |
| Dear ImGui | 1.88 WIP | MIT | Omar Cornut | https://github.com/ocornut/imgui |
| Crypto++ | 8.9.0 (`CRYPTOPP_VERSION` 890) | Boost Software License 1.0 | Wei Dai and contributors | https://github.com/weidai11/cryptopp |
| cpr | vendored snapshot | MIT | Huu Nguyen and contributors | https://github.com/libcpr/cpr |
| libcurl | 8.6.0 | curl license | Daniel Stenberg and contributors | https://curl.se/ |
| Mbed TLS | 3.6.2 | Apache-2.0 | The Mbed TLS Contributors | https://github.com/Mbed-TLS/mbedtls |
| PSA Crypto headers | 3.6.2 (part of Mbed TLS) | Apache-2.0 OR GPL-2.0-or-later | The Mbed TLS Contributors | https://github.com/Mbed-TLS/mbedtls |
| nlohmann/json | 3.12.0 | MIT | Niels Lohmann | https://github.com/nlohmann/json |
| IXWebSocket | 11.4.6 | BSD-3-Clause | Machine Zone, Inc. | https://github.com/machinezone/IXWebSocket |
| Zstandard | 1.5.6 | BSD-3-Clause OR GPL-2.0 | Meta Platforms, Inc. and affiliates | https://github.com/facebook/zstd |
| cpp-httplib | 0.49.0 | MIT | Yuri K. and contributors | https://github.com/yhirose/cpp-httplib |
| xorstr (`yarafunk.h`, `tikifunk.h`) | 2017-2021 upstream, modified | Apache-2.0 | Justas Masiulis | https://github.com/JustasMasiulis/xorstr |

## Notes

- **Luau**: the vendored headers/source include local changes for Roblox 0.745
  layouts. The upstream MIT notice still applies.
- **xorstr**: `Dependencies/yarafunk.h` is a modified version of the upstream
  header (strengthened key derivation and secure wipe on destruction), as noted
  in the file. `Dependencies/tikifunk.h` is the unmodified upstream header.
- **Zstandard**: this distribution uses the BSD-3-Clause option.
- **Mbed TLS / PSA**: the PSA headers are part of Mbed TLS and carry the
  `Apache-2.0 OR GPL-2.0-or-later` SPDX identifier.
- **libcurl**: the curl license is MIT-like but is not the MIT license; include
  the upstream `COPYING` text with binary distributions.
- **Dear ImGui** includes the `imstb_*` single-header libraries, which are
  public domain / MIT; the ImGui notice covers them.
- **Crypto++** is distributed under the Boost Software License 1.0; retain the
  upstream copyright notice.

If you add or update a dependency, update this file and include its license
text before publishing a binary.
