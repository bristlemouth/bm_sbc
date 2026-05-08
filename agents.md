# Overview

This repo `bm_sbc` is in the `bristlemouth` github organization and is open source.
BM is an abbreviation for Bristlemouth,
an open standard delivering plug-and-play hardware interfaces for marine applications.
SBC stands for "single board computer" and often refers to a Raspberry Pi Zero 2W.

This repo contains `bm_core` as a submodule,
static libraries encapsulating the Bristlemouth and BCMP protocol logic.
The `bm_core` repo is written in strict C17 and aims to be architecture independent.
`bm_core` contains `bm_common_messages` as a submodule.
`bm_common_messages` contains structs and CBOR codecs for common Bristlemouth messages.

The first and more mature repo using `bm_core` is `bm_protocol`;
`bm_protocol` contains embedded firmware for an STM32U575 MCU
on a PCB called a Bristlemouth Mote,
which also contains a 2-port ADIN2111 10BASE-T1L SPE MAC-PHY chip.
`bm_protocol` contains many apps for different sensor modules,
and those devices are mature and have been shipping in production to customers for years.

This `bm_sbc` repo is newer and more experimental.
Its first goal is to enable a Raspberry Pi Zero 2W in the BOREALIS hydrophone
to run processes that speak Bristlemouth over a UART to a mote.

# Build & Test

- Use cmake presets. Example: `cmake --build --preset all`
- The `all` preset builds in the `build/all` directory.
- Run `ctest --test-dir build/all` to run unit tests.
- The `scripts` directory has more elaborate tests that should be run but less often because they take more time.
- `scripts/validate.sh` is a parent script run by CI that runs all tests and can take about a minute to run.
