# Steinberg Cubase 15, Nuendo 15 and Dorico 6 on this branch

A step-by-step setup for Steinberg's current applications in a new Wine prefix, from
the Download Assistant to a running project. Every step below was run end to end on
a freshly created prefix on 2026-10-11; steps that turned out to need nothing are
listed as such, so nothing here is done "just in case".

Steinberg does not support Wine. Problems with this setup belong in this fork's
[issue tracker](https://github.com/giang17/wine/issues), not with Steinberg support.

## What was verified

| | Version | Result |
|---|---|---|
| Steinberg Download Assistant | 1.40.1 | installs, signs in through the browser, installs its runtime components |
| Steinberg Install Assistant | 1.4.4 | installs every package below |
| Steinberg Activation Manager | 1.10.0 | signs in, activates and deactivates trial licences |
| Steinberg Library Manager, MediaBay, built-in ASIO Driver | 3.2.81, 1.4.10, 1.0.9 | installed by the Download Assistant |
| Cubase Pro | 15.0.30 | Hub, empty project, instrument track playback through the built-in ASIO Driver, MixConsole with live meters |
| Nuendo | 15.0.30 | templates including Ambisonics Mixing, the demo project "The Crone" with its ProRes video in the video player |
| Dorico | 6.2.30 | Hub, new project, playback |

Host: Ubuntu 24.04.5, KDE Plasma 5.27 on X11, NVIDIA RTX 2000 Ada (driver 580),
Firefox 156 as a snap, branch `d2d1-dcomp-11.0` at `e91e102ef9a`. Licences were
Steinberg's trial versions.

## 1. Build and install the branch

As in the [README](../README.md#quick-start), into a separate prefix such as
`/opt/wine-d2d1`. The commands below use

```bash
export WINE=/opt/wine-d2d1/bin/wine
export WINEPREFIX=~/.wine-steinberg
```

Use this Wine for everything in the prefix. Starting a prefix with a different Wine
build updates it with that build's DLLs.

**The menu entries and URL handlers Wine creates call `wine` without a path**
(`Exec=env "WINEPREFIX=…" wine …`). They run whatever `wine` comes first in `PATH`.
With the distribution's Wine installed next to this build, either put
`/opt/wine-d2d1/bin` first in `PATH` for the desktop session, or change the `Exec=`
lines of the `wine-protocol-net-steinberg-*.desktop` files in
`~/.local/share/applications` (step 5 and 6) to the full path.

## 2. Create the prefix

```bash
"$WINE" wineboot -u
```

`wineboot` installs Wine Mono, which the Install Assistant (a .NET program) needs;
when Wine Mono is not cached, Wine asks to download it. Wine Gecko is not needed by
any of the applications above; the verified prefix was created without it. The script signature provider for signed PowerShell
scripts (`pwrshsip`), which Steinberg's installers check, is registered by this
branch's `wine.inf` during this step, so nothing has to be registered by hand.

## 3. Fonts, including Segoe UI

```bash
scripts/wine-font-setup.sh --prefix "$WINEPREFIX" --wine "$WINE" \
    --segoe-from /path/to/Windows/Fonts
```

**Cubase 15 and Nuendo 15 crash without Segoe UI.** Steinberg's framework looks the
family up in the DirectWrite system collection and does not check the result: the Hub
opens, and a moment later a "serious problem has occurred in … hubservice.dll" dialog
appears (an access violation reading address 0, at the same offset in two separate
measurements). Dorico 6 ran without it.

Segoe UI is proprietary and no distribution ships it. `--segoe-from` copies
`segoeui*.ttf` and `segui*.ttf` from a directory you provide — `C:\Windows\Fonts` of a
Windows installation you are licensed to use, for example on a mounted partition — and
registers them. The rest of the script (MS Core Fonts or free stand-ins, the symbol
fallback, the text rendering switches) is described in the
[font setup guide](wine-font-setup-guide.md).

## 4. PowerShell hosting for the installers

```bash
scripts/mono-sma-shim/install.sh --prefix "$WINEPREFIX" --build
```

Do this **before the first start of the Download Assistant**. Steinberg's `Setup.exe`
runs the PowerShell pre-install scripts of a package in-process, through an assembly
Wine Mono does not ship. Without it a package with such a script is reported as
installed, and nothing is installed. The Download Assistant installs MediaBay at its first start and shows exactly
that in its log (`AppData\Local\Steinberg Download Assistant\logs`):
`Steinberg MediaBay: the installation reported success, but no installed version was
found`. Details: [scripts/mono-sma-shim](../scripts/mono-sma-shim/README.md).

A Wine Mono update (`wineboot -u` with a newer Wine) removes the shim;
`install.sh --status` reports that, and running `install.sh` again restores it.

## 5. Download Assistant

Download the Download Assistant for Windows from steinberg.net and run its installer in
the prefix:

```bash
"$WINE" ~/Downloads/Steinberg_Download_Assistant_*_Installer_win.exe
```

At its first start the Download Assistant installs its runtime components by itself:
Install Assistant, Library Manager, Activation Manager, the built-in ASIO Driver and
MediaBay.

**Sign-in** opens the system browser, and Steinberg's login page hands the result back
through a `net-steinberg-sda://` link. The installer registers that scheme, and Wine
writes the handler for it,
`~/.local/share/applications/wine-protocol-net-steinberg-sda.desktop`, when a Wine
session starts. In the verified run the Download Assistant was started after its
installer had exited, the handler was written at that start, and with a `wine` that
points at this build (step 1) sign-in completed without any further step; the browser
was Firefox installed as a snap. Starting the Download Assistant from the installer's
last page keeps the installer's session running, so run

```bash
"$WINE" winemenubuilder -a -r
```

before signing in if it was started that way. The handler can be tested without signing
in, with the Download Assistant running:

```bash
gio open 'net-steinberg-sda://test'
```

Its log then contains `MessageReceiver : Received: net-steinberg-sda://test`.

## 6. Activation Manager

```bash
"$WINE" winemenubuilder -a -r
```

Run this once after the Download Assistant has installed the Activation Manager, before
signing in to it. Wine writes the handler for a URL scheme when a Wine session starts,
and the Activation Manager registers `net-steinberg-activation-manager://` while the
Download Assistant's session is already running; until the next session start its
sign-in link has nowhere to go. The command writes the handler right away. Closing every
program in the prefix and starting again has the same effect.

The sign-in survives closing the Activation Manager since `94d81487f00` (2026-10-11).
Before it, Wine dropped the attributes of stored credentials, Steinberg's License Engine
discarded its refresh token (`1.1 Format refresh token detecting -- resetting` in
`AppData\Roaming\Steinberg\Activation Manager\Logs\License Engine.txt`), and every start of
a Steinberg product opened the Activation Manager on its login page. A build from before
that commit still behaves that way.

Steinberg counts activations per computer, and a newly created Wine prefix registers as
a separate computer. A copy of a prefix (`cp -a`) kept the identity of its original in
an earlier measurement. Deactivating the licences before deleting a prefix keeps the
activation available for the next one.

## 7. Install the applications

Install Cubase, Nuendo or Dorico from the Download Assistant as on Windows. It hands
each package to the Install Assistant, which runs the package's `Setup.exe --silent`.
In the verified run the application packages, downloaded earlier, went through that
same Install Assistant command line directly; installs started from the Download
Assistant's own list were verified in a long-used prefix.

**Audio:** the Steinberg built-in ASIO Driver that the Download Assistant installs
works under Wine (Studio → Studio Setup → Audio System); playback in Cubase 15 was
verified with it. [WineASIO](https://github.com/giang17/wineasio) is the alternative
for a JACK or PipeWire setup.

## Side effects worth knowing

- `winemenubuilder` also writes the file associations of the installed applications
  (`.cpr`, `.npr`, `.dorico`, `.vstsound`, `.vst3`, …) and their menu entries, all
  pointing at this prefix.
  With a second prefix that has the same applications, the one that ran
  `winemenubuilder` last owns them.
- Wine links the prefix' `Documents` to `~/Documents`, so Steinberg's default project
  folders and crash dumps (`Documents/Steinberg/CrashDumps`) are shared by every prefix.
