# Installing and uninstalling

Date: 2026-10-30, Europe/Budapest

Status: **per-user install, launch and uninstall verified on this PC**

The portable package could be copied anywhere but had no install path. This adds a
per-user installer (no administrator rights, no machine-wide state) plus the
matching uninstaller.

## Portable by default

The folder is the product: copy it anywhere, run `GoodSharing.exe`, and everything
it needs is inside. Settings are now portable as well - `GoodSharing.ini` is
written **next to the executable** (stream profile, volume, preview toggle, share
frame rate), so a folder that is moved or copied keeps its configuration, and the
registry is untouched on a writable location. A read-only location (for example
Program Files without elevation) falls back to the per-user registry store.

Verified: a copy of the package in a temporary folder started with a minimal PATH
and created `GoodSharing.ini` with the defaults; editing `volume` and `shareFps`
in that file survived the next start, proving the values are read back rather than
overwritten.

## Install (optional)

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package.ps1   # build the package
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install.ps1   # install it
```

`scripts/install.ps1` copies the newest `packaging\dist\GoodSharing-*-win64`
package to `%LOCALAPPDATA%\Programs\GoodSharing`, writes `uninstall.ps1` next to
the executable, creates a Start Menu shortcut and registers an uninstall entry
under `HKCU\...\Uninstall\GoodSharing` (name, version from `version.json`,
publisher, install location, icon, uninstall command). Re-running it replaces the
previous install, which also makes it the upgrade path.

Options: `-Package <dir>` to install a specific package, `-InstallDir <dir>` for a
different location, `-NoShortcut` to skip the Start Menu link.

## Uninstall

Settings > Apps > GoodSharing > Uninstall, or:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\Programs\GoodSharing\uninstall.ps1"
```

It removes the shortcut, the registry entry and the installation directory.

## Verification

| Step | Result |
| --- | --- |
| install | 1,291 files copied, shortcut and uninstall entry created |
| run the installed executable through the 720p A/V case | pass |
| uninstall | directory, shortcut and registry entry all gone |

## Not covered yet

- The executables are **unsigned**, so SmartScreen may warn on first run; code
  signing needs a certificate and key custody (docs/51, Milestone 6).
- No MSIX package, no per-machine installation, no automatic update channel.
- A clean-machine run (another PC or a fresh VM) is still the remaining step for
  "install and run on a clean machine".

## Files

```text
scripts/install.ps1                 per-user installer
packaging/README.txt                install/uninstall notes for the package
```
