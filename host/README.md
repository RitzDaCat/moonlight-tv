# Sunshine host patches and tools

This directory contains the Sunshine work used alongside the Moonlight TV client.
It contains source patches and tools; it does not include a patched Sunshine binary.

- [`installer/`](installer/README.md): Windows setup using official Sunshine, plus
  CachyOS/Arch integration with controller permissions and optional audio/focus setup.
- [`patches/`](patches/): packet-size validation, NVENC presets and split-frame policy,
  FEC matrix reuse, optional video-stage profiling, DMA-BUF import caching,
  experimental KWin capture, CUDA-stream ordering, PipeWire buffer lifetime,
  GFNI parity, and display wake-up changes.
- [`tools/`](tools/README.md): bounded GPU, encoder, capture, and TV diagnostics.
- [`sunshine.conf.example`](sunshine.conf.example): nonsecret configuration template.
- [`latency-test.conf.example`](latency-test.conf.example): experimental NVIDIA settings.

The patch series starts from Sunshine commit
`019b1ba661d829e334706557a3e2bdc3d2374591`. Read the base/context and validation
records with each patch before applying it to another Sunshine revision. Build
and test the chosen series in a separate Sunshine checkout; the host installer
does not apply these patches. Experiments are not a measured TV-latency claim.

Run installer checks without changing your system:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 host/installer/test_installer.py
PYTHONDONTWRITEBYTECODE=1 python3 host/tools/test_session_assist.py
PYTHONDONTWRITEBYTECODE=1 python3 host/tools/test_sunshine_permissions.py
```

For developer-built runtime binaries, `install_sunshine_permissions.py plan`
previews an optional scoped capability helper. Installation requires explicit
sudo and binds the root-owned helper to this checkout's owner and three fixed
runtime paths. It grants `CAP_SYS_ADMIN` and `CAP_SYS_NICE` to those binaries;
review and trust the code first. The normal distribution-package installer does
not create a passwordless sudo policy.

Runtime binaries, credentials, keys, build trees, and generated reports are
ignored by Git. Keep local addresses and account/device settings in local files.
The optional KWin focus script has no built-in games or monitor. Its per-user
`WindowClasses` (comma-separated) and `StreamOutput` settings are opt-in; the
normal host installer does not load this script.
