# HyperLPR3 (vendored C++ core)

This directory contains the unmodified C++ inference core of
[HyperLPR3](https://github.com/szad670401/HyperLPR) (`cpp/src` and `cpp/c_api`),
version **3.0.1** as reported by `cpp/src/version.h`.

* Upstream license: Apache License 2.0 — see `LICENSE` in this directory.
* Vendored on 2026-11-24 from the upstream `cpp/` tree.
* We deliberately do not vendor the upstream CMake project. The UvNVR build
  compiles these sources as the `uvnvr_hyperlpr3` static library and supplies
  MNN 3.6.2 through `UVNVR_MNN_ROOT` instead of upstream's pinned MNN 2.2.0
  download (`LINUX_FETCH_MNN`).

## Modifications

None yet. If the sources must be patched for a newer MNN, record the change and
its reason here and in `docs/ai-recognition.md`.

## Models

The `.mnn` model files are not part of the repository. Use
`scripts/fetch-ai-models.sh` (Linux) or `scripts/fetch-ai-models.ps1`
(Windows) to place them under `<data_dir>/models/hyperlpr3`.
