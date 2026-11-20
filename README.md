```text
+============================================================================+
|                                                                            |
|                            #   # ### #   # ####                            |
|                            #   #  #  ##  # #                               |
|                            # # #  #  # # # ###                             |
|                            ## ##  #  #  ## #                               |
|                            #   # ### #   # ####                            |
|                                                                            |
|         ####  ####  ####   #      # ####   ####  ###  #   # ####           |
|         #   #     # #   # ##     #  #   # #     #   # ## ## #   #          |
|         #   #  ###  #   #  #    #   #   # #     #   # # # # ####           |
|         #   # #     #   #  #   #    #   # #     #   # #   # #              |
|         ####  ##### ####  ### #     ####   ####  ###  #   # #              |
|                                                                            |
|                  Wine with DirectComposition and Direct2D                  |
|                                                                            |
|       Modern Windows software on Linux: built for DAWs and plugins,        |
|                running .NET and WebView2 applications too.                 |
|                                                                            |
+============================================================================+

+--[ release info ]----------------------------------------------------------+
|  repository ...... github.com/giang17/wine                                 |
|  stable .......... d2d1-dcomp-11.0 on Wine 11.0, default branch            |
|  rolling ......... d2d1-dcomp-11.<N> on the newest WineHQ devel tag        |
|  release builds .. x86_64 Linux, glibc 2.35+, both PE architectures        |
|  license ......... GNU LGPL, the same as Wine                              |
+----------------------------------------------------------------------------+

+--[ about ]-----------------------------------------------------------------+
|  Current Windows audio software draws its windows with DirectComposition,  |
|  Direct2D, DirectWrite and DXGI: JUCE 8 and VSTGUI plugins,                |
|  WebView2-based editors and the DAWs that host them. Stock Wine            |
|  implements little of that stack, so these windows open black, flicker,    |
|  lose their artwork, or the application stops before its first window.     |
|  This fork implements the stack, fixes the Wine code around it from the    |
|  X11 driver to the installers, and is in daily use for music production.   |
|                                                                            |
|  The same work carries applications outside audio: vatSys, an              |
|  air-traffic-control radar client drawn with Direct2D; NinjaTrader, a WPF  |
|  trading platform with a WebView2 chat; and, on the rolling branch, Power  |
|  BI Desktop.                                                               |
|                                                                            |
|  PATCHES.md documents each change with its reason and measurements.        |
+----------------------------------------------------------------------------+

+--[ install ]---------------------------------------------------------------+
|  RELEASE BUILDS                                                            |
|  The Releases page carries a portable build of d2d1-dcomp-11.0 for x86_64  |
|  Linux with both PE architectures. Unpack it into any directory and run    |
|  it from there; the distribution's Wine stays untouched.                   |
|                                                                            |
|    $ sudo tar -C /opt -xf wine-d2d1-dcomp-<version>-x86_64.tar.xz          |
|    $ WINEPREFIX=~/.wine-d2d1 \                                             |
|        /opt/wine-d2d1-dcomp-<version>/bin/wine winecfg                     |
|                                                                            |
|  A build needs glibc 2.35 or newer (Ubuntu 22.04, Debian 12, current       |
|  Fedora and Arch) and the libraries the distribution's own Wine package    |
|  depends on, so installing that package is the simplest way to get them.   |
|  On Arch the audio and media libraries are only optional dependencies of   |
|  wine:                                                                     |
|                                                                            |
|    $ sudo pacman -S --needed wine alsa-lib libpulse \                      |
|        gst-plugins-base-libs opencl-icd-loader libgphoto2 \                |
|        vulkan-icd-loader                                                   |
|                                                                            |
|  Release builds have no FFmpeg support, so media decoding goes through     |
|  GStreamer. They contain Wine only: no applications, plugins or            |
|  installers.                                                               |
|                                                                            |
|  PACKAGES BY OTHERS                                                        |
|    * Fedora 44 and 45: the COPR repository patrickl/wine-staging-dev       |
|      builds the rolling branch with the wine-staging patchsets applied on  |
|      top. A report from this package should say so.                        |
|    * Arch Linux and CachyOS: mklnln/wine-d2d1-dcomp installs to            |
|      /opt/wine-d2d1. Its source package builds the current                 |
|      d2d1-dcomp-11.0, its binary package a fixed release build; compare    |
|      its date before relying on it.                                        |
|  Both are maintained by their authors, independently of this repository.   |
|                                                                            |
|  FROM SOURCE                                                               |
|    $ git clone https://github.com/giang17/wine.git && cd wine              |
|    $ git checkout d2d1-dcomp-11.22                                         |
|    $ ./configure --prefix=/opt/wine-d2d1 --enable-archs=i386,x86_64        |
|    $ make -j$(nproc)                                                       |
|    $ sudo make install                                                     |
|                                                                            |
|  Build both architectures, with the i686-w64-mingw32 and                   |
|  x86_64-w64-mingw32 cross compilers: 32-bit hosts and plugins load d2d1,   |
|  dcomp and dxgi directly.                                                  |
+----------------------------------------------------------------------------+

+--[ before the first run ]--------------------------------------------------+
|    * wine.inf belongs to the install. It carries the fork's registry       |
|      defaults, among them the HideWineVersion entries that keep JUCE       |
|      8.0.13+ plugins on Direct2D instead of their GDI fallback. make       |
|      install and the release builds include it; DLLs copied by hand        |
|      without it fail silently.                                             |
|    * Leave DXVK out of the prefix. It replaces dxgi.dll and d3d11.dll,     |
|      where the fork's composition swapchains live. PATCHES.md describes    |
|      the per-application switch for a machine that needs DXVK elsewhere.   |
|    * On Wayland, keep Wine on Xwayland, the default while DISPLAY is set.  |
|      The fork's display-driver changes are in winex11.drv;                 |
|      winewayland.drv is unmodified.                                        |
+----------------------------------------------------------------------------+

+--[ which branch ]----------------------------------------------------------+
|  d2d1-dcomp-11.0      base wine-11.0                                       |
|                       Most users. Stable, maintained, the default branch   |
|                       and the source of the release builds.                |
|                                                                            |
|  d2d1-dcomp-11.<N>    base newest WineHQ devel tag                         |
|                       Rolling-release setups. Rebased onto each devel tag  |
|                       about every two weeks; always take the highest       |
|                       number.                                              |
+----------------------------------------------------------------------------+

+--[ verify ]----------------------------------------------------------------+
|  A release build: the checksum, and the attestation that the archive was   |
|  built by .github/workflows/release.yml from the commit the release        |
|  names.                                                                    |
|                                                                            |
|    $ sha256sum -c --ignore-missing wine-d2d1-dcomp-<version>.sha256        |
|    $ gh attestation verify \                                               |
|        wine-d2d1-dcomp-<version>-x86_64.tar.xz -R giang17/wine             |
|                                                                            |
|  The source: the branch descends from the upstream release tag, and the    |
|  diff against that tag is the whole fork. Fetching the tag from upstream   |
|  fails if the one in this repository differs.                              |
|                                                                            |
|    $ git fetch https://github.com/wine-mirror/wine.git tag wine-11.0       |
|    $ git merge-base --is-ancestor wine-11.0 d2d1-dcomp-11.0 \              |
|        && echo "descends from wine-11.0"                                   |
|    $ git diff --stat wine-11.0 d2d1-dcomp-11.0                             |
|    $ git diff wine-11.0 d2d1-dcomp-11.0 -- configure.ac .github            |
|                                                                            |
|  The last command shows the files most worth reading in full: the build    |
|  configuration, where a fork could add something that runs before any      |
|  Wine code does, and the workflows that make the release builds. A         |
|  rolling branch is checked the same way against its own base tag.          |
+----------------------------------------------------------------------------+

+--[ what it changes ]-------------------------------------------------------+
|    * Direct2D: geometry, clipping, layers, effects and the bitmap paths    |
|      plugin GUIs use.                                                      |
|    * DirectComposition: devices, visual trees, surfaces and composition    |
|      swapchains, composited inside the process.                            |
|    * DirectWrite: rendering modes, font fallback, glyph caching and        |
|      subpixel text.                                                        |
|    * DXGI, Direct3D 11, WineD3D: composition swapchains, fences, GL        |
|      resource reuse.                                                       |
|    * Windows and input: layered and composited windows, popups and         |
|      window-manager interaction in winex11.drv, win32u and wineserver.     |
|    * Installers and runtime: msi, signed PowerShell scripts, shell links,  |
|      crash dumps and the other pieces applications need to install and     |
|      start.                                                                |
|                                                                            |
|  The applications tested on this fork, with what each one needs:           |
|  PATCHES.md, section Tested Applications.                                  |
+----------------------------------------------------------------------------+

+--[ reporting problems ]----------------------------------------------------+
|  Open an issue with the application and plugin framework, the output of    |
|  wine --version, and whether Wine came from a release build, a package or  |
|  an own build. A plugin GUI that stays black or looks like GDI is most     |
|  often a host without its HideWineVersion entry; PATCHES.md explains the   |
|  switch.                                                                   |
+----------------------------------------------------------------------------+

+--[ upstream ]--------------------------------------------------------------+
|  This is a fork of wine-mirror/wine, not a replacement for it, and a       |
|  private project without affiliation to the Wine project, WineHQ,          |
|  CodeWeavers or any software vendor. Wine's Clean-Room Guidelines do not   |
|  accept LLM-generated code; the fork is developed with LLM assistance, so  |
|  no merge requests go to WineHQ while that rule stands. Several earlier    |
|  fixes from the fork are part of upstream Wine. UPSTREAM.md lists defects  |
|  in upstream Wine that the fork fixes and their state upstream.            |
|  Upstream's own README is kept as README.wine.                             |
+----------------------------------------------------------------------------+

+--[ credits ]---------------------------------------------------------------+
|    * Subpixel text rendering: Cade (@shibco), shibco/ableton-linux pull    |
|      request 155, reviewed there by @ClickSentinel                         |
|    * --enable-archs packaging form for Bottles: @jibeape                   |
+----------------------------------------------------------------------------+

+--[ links ]-----------------------------------------------------------------+
|  * releases                                                                |
|      https://github.com/giang17/wine/releases                              |
|  * patches                                                                 |
|      https://github.com/giang17/wine/blob/d2d1-dcomp-11.0/PATCHES.md       |
|  * upstream status                                                         |
|      https://github.com/giang17/wine/blob/d2d1-dcomp-11.0/UPSTREAM.md      |
|  * issues                                                                  |
|      https://github.com/giang17/wine/issues                                |
|  * fedora copr                                                             |
|      https://copr.fedorainfracloud.org/coprs/patrickl/wine-staging-dev/    |
|  * arch pkgbuilds                                                          |
|      https://github.com/mklnln/wine-d2d1-dcomp                             |
|  * clean room                                                              |
|      https://gitlab.winehq.org/wine/wine/-/wikis/Clean-Room-Guidelines     |
+----------------------------------------------------------------------------+

                      -----  giang17/wine  |  GNU LGPL  |  2026  -----
```
