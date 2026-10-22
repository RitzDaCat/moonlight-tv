# LG TV streaming host setup

Host setup for Windows 10/11 and CachyOS/Arch Linux. Each PC runs its own game, GPU
encoder and Sunshine session. The Moonlight TV client owns layout and per-player
audio controls; games synchronize through their normal LAN/online multiplayer.

## Windows PC

Follow the [Windows setup and pairing guide](windows/README.md). Download
[moonlight-tv-windows-host-setup.zip](https://github.com/RitzDaCat/moonlight-tv/releases/download/v1.6.45/moonlight-tv-windows-host-setup.zip),
extract it, and double-click `install-host.cmd`. It installs official Sunshine
through WinGet when needed and opens this PC's local web UI. Existing Sunshine
settings, game entries, pairing, and service state are retained.

The guide also covers manual MSI installation, controller drivers, pairing,
readiness checks, and removal. The Windows helper does not apply Linux host patches.

## CachyOS or Arch Linux PC

Download the native package from this repository's Releases page. From that PC's
graphical desktop, run:

```sh
sudo pacman -U ./lg-tv-streaming-host-0.1.0-1-any.pkg.tar.zst
lg-tv-host-setup install
lg-tv-host-setup doctor
```

The desktop menu also includes **LG TV streaming host setup**. The package depends
on Sunshine from configured signed pacman repositories. CachyOS provides it;
plain Arch needs a configured Sunshine repository first. See the
[upstream installation instructions](https://docs.lizardbyte.dev/projects/sunshine/master/md_docs_2getting__started.html#archlinux).
Update the distribution normally before installing. The setup does not add
repositories, change GPU drivers, or perform a distribution upgrade.

For installation directly from source:

```sh
bash host/installer/install-host.sh plan
bash host/installer/install-host.sh install
python3 host/installer/install_host.py doctor
```

From the repository root, build the native package without downloads or installation:

```sh
python3 host/installer/build_package.py --output build/codex-release
```

Setup requests sudo for package installation and one-time system permissions.
It gives the package-owned `/usr/bin/sunshine` KMS capture/scheduling capabilities,
loads `uinput` and `uhid`, and refreshes the package-provided input permissions.
A fixed pacman hook restores capabilities after Sunshine upgrades. No root worker
or passwordless sudo policy is installed.

Sunshine's user service and the optional Steam audio/focus helper are enabled.
Use `install --no-start` to stage user files without enabling or starting services.
An already running Sunshine process is retained. Existing Sunshine configuration,
pairing, certificates and game entries are preserved. Only a missing configuration
receives automatic hardware/audio selection, P1/single-pass NVIDIA defaults,
local-only administration policy and UPnP disabled.

Open **https://localhost:47990/** on that PC, create its own credentials, then add
the PC in Moonlight and approve the PIN locally. Across subnets, add the PC's
address manually. Router/firewall rules are retained; streaming needs the relevant
TV-to-host ports: TCP 47984/47989/48010 and UDP 47998-48000. Keep administration
port 47990 local. `doctor` reports input access and services without printing
credentials. If input access is missing, log out/in and rerun `doctor`.

The helper follows active Sunshine capture and routes Steam-owned playback to it.
Xwayland focus handoff uses window-manager requests. Native Wayland focus, games
outside Steam, and target-PC controller/audio behavior require acceptance testing.

## Linux delivery status

The installer uses the distribution's signed Sunshine binary. Experimental host
changes are source patches pinned to upstream commits; installing this package
does not apply them. Two-player TV mode remains under development. Dual 4K120 and
4K120 plus 1440p120 are targets, with no measured support claim.

Per-player TV audio controls are implemented in the client source: independent
mute and -60 to 0 dB gain, saved preferences, half-scale headroom per stream and a
5 ms gain ramp inside existing PCM buffers. Muting preserves codec history and
audio timing. This adds no queued audio. The ARM client and native checks are part of the fork release. A package build
and native tests do not establish TV audio quality or two-PC gameplay.

## Remove the Linux integration

```sh
lg-tv-host-setup uninstall
# Source checkout alternative:
python3 host/installer/install_host.py uninstall
```

Removal touches only unchanged managed user files and a hook created by source
setup. Modified files are retained with an error. Sunshine, its configuration,
credentials, pairing and a live session remain. A package-owned hook is removed
with `sudo pacman -R lg-tv-streaming-host` after user setup is uninstalled.

User data lives in standard XDG config/data/state directories under
`lg-tv-streaming`. Audio/focus state contains local process routing information;
keep it out of Git. `test_installer.py` uses temporary directories and mocked
system commands, so those checks do not validate installation on another PC.
