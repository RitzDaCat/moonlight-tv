# Windows PC setup for Moonlight TV

Use this on the Windows PC that runs your games. The LG TV runs the patched
Moonlight `.ipk`; Windows runs official Sunshine. The helper supports Windows
10/11 on Intel/AMD x64 PCs and uses the Windows PowerShell included with Windows.
ARM64 Sunshine is experimental; use its official manual instructions instead.

## Install Sunshine

1. Download [moonlight-tv-windows-host-setup.zip](https://github.com/RitzDaCat/moonlight-tv/releases/download/v1.6.45/moonlight-tv-windows-host-setup.zip)
   and use **Extract All**. Open the extracted folder.
2. Double-click **install-host.cmd**. It checks for an existing Sunshine host.
   If Sunshine is missing, it runs its official interactive installer through
   WinGet (`LizardByte.Sunshine`) and lets the installer request administrator access.
3. Follow the installer options for the Windows service and your trusted private
   network. Keep your GPU driver installed and your display available for capture.
4. The helper opens **https://localhost:47990/** on this PC. Create your own
   Sunshine username/password. Its local self-signed certificate causes a browser
   warning; continue only for this local Sunshine page.

If WinGet is unavailable, the helper opens the official release page. Download
**Sunshine-Windows-AMD64-installer.msi**, install it, then run the helper again.
You can also use the [official MSI directly](https://github.com/LizardByte/Sunshine/releases/latest/download/Sunshine-Windows-AMD64-installer.msi)
without the helper. Existing Sunshine installations are retained; update them
separately using Sunshine's normal upgrade instructions.

## Pair the TV and add your games

1. Install the patched [TV package](https://github.com/RitzDaCat/moonlight-tv#download-and-install)
   using webOS Dev Manager, and put the TV and PC on the same local network.
2. In Sunshine **Applications**, use Desktop/Steam or add your own games and cover
   artwork. No personal game cards or another person's PC configuration are imported.
3. In Moonlight TV, select **Computers** and your PC. If discovery fails, use
   **Add computer** with your PC's local IPv4 address, shown by `ipconfig`.
4. Enter the PIN displayed by the TV in Sunshine's **PIN** page on your PC.
5. Start **Desktop** first and verify picture, sound, and input. Then launch your game.
   Use a wired connection when practical. Keep router port forwarding disabled
   for this local setup and the Sunshine administration page local to your PC.

## Controller setup

A TV-connected controller also needs a supported virtual gamepad driver on the
Windows host. Configure it in Sunshine **Configuration > Input** and follow the
[official Windows input setup](https://docs.lizardbyte.dev/projects/sunshine/latest/md_docs_2getting__started.html#initial-setup).
Current choices include the optional paid Virtual HID Driver and legacy ViGEmBus,
which has reached end of life. Choose the driver that fits your setup and restart
Windows after driver installation when requested. Pairing the PC alone does not
verify controller support.

## Checks and removal

From a terminal inside the extracted folder:

```bat
install-host.cmd plan
install-host.cmd doctor
install-host.cmd webui
```

`plan` previews the setup. `doctor` reports Sunshine detection, service state,
and whether its local web UI port responds. If the service is stopped, start
Sunshine from the Windows Start menu before opening its web UI. For discovery
issues, check the Windows network profile and Sunshine's private-network firewall
rules; do not disable the firewall globally.

The helper keeps no installed files or background service of its own. Delete the
extracted folder to remove it. To remove Sunshine, use Windows **Settings > Apps**.
Review any existing Sunshine configuration before uninstalling or changing installer types.

## Scope and validation

The decoder experiments are in the TV app and work with a Windows Sunshine host.
The optional Linux host patch series and Linux audio/focus helper are separate.
The Windows helper retains existing games, settings, pairing, and service state;
it opens the local web UI for user-controlled setup. It does not bundle credentials.

Automated Windows checks exercise the helper with mocked installation/service
state, native command exit codes, and an owned loopback socket. They do not
install Sunshine or establish Windows GPU capture, TV playback, audio, or controller
acceptance. Verify those on your own PC/TV after installation.
