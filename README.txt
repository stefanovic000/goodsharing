GoodSharing - portable package
==============================

What this is
------------
A single folder that runs the GoodSharing Windows client. It contains the
application, the Qt runtime and QML modules, the LiveKit SDK libraries, the
third-party notices and a version manifest.

Run it
------
1. Double-click GoodSharing.exe (or run it from this folder).
2. Enter the room address, your access token and the room's 64-digit shared key,
   then press "Join room".
3. Pick a source in "Share your screen", or choose a remote share and press
   "Watch".

Requirements
------------
- Windows 10 22H2 or newer (x64).
- A LiveKit server reachable from this PC. The client does not ship a server.
- A GPU that can capture and present Direct3D 11 content for hardware capture.

Latency and smoothness
----------------------
The "Low latency / Balanced / Smooth" selector next to the quality choice sets
this PC's buffering preference for both watching and sharing:

  Low latency  25 ms lead, 120 ms sender capture budget
  Balanced     30 ms lead, 150 ms sender capture budget  (default)
  Smooth       50 ms lead, 250 ms sender capture budget

The choice is remembered between sessions. For automated runs the environment
variable GOODSHARING_STREAM_PROFILE=low|balanced|smooth overrides it.

Yellow capture indicator
------------------------
Windows draws a one-pixel yellow indicator around a window while it is captured
by Windows Graphics Capture, and around the screen for display capture, on
Windows 10. It is local to the sharer's screen and never appears in the shared
video. Windows 11 22H2 can disable it only for packaged applications that declare
the restricted graphicsCaptureWithoutBorder capability; this build is not
packaged that way.

Portable use
------------
This folder is the product: copy it anywhere and run GoodSharing.exe. Settings
live in GoodSharing.ini next to the executable (stream profile, volume, preview
toggle, share frame rate), so moving or copying the folder keeps them; delete
that file to return to the defaults. The registry is only used when the folder is
not writable.

Install and uninstall (optional)
--------------------------------
To install it for the current
user (no administrator rights), run from the project checkout:

  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install.ps1

That copies this folder to %LOCALAPPDATA%\Programs\GoodSharing, adds a Start Menu
shortcut and registers an uninstaller. To remove it, use Settings > Apps >
GoodSharing > Uninstall, or run uninstall.ps1 from the installation folder.

Troubleshooting
---------------
- "Encrypted room unavailable": the room key is missing or wrong for the room.
- No picture while a window is minimized: that is expected; the sharer's window
  must be restored (viewers see "User minimized app" meanwhile).
- Antivirus quarantine of livekit_ffi.dll: the SDK is unsigned in this build.

More information
----------------
See THIRD-PARTY-NOTICES.txt for the bundled components and their licenses, and
version.json for the exact build and library hashes recorded for this package.
