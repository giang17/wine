```text
+============================================================================+
|                                                                            |
|     ####  ###  #   # ##### ####  ### ####  #   # ##### ### #   #  ####     |
|    #     #   # ##  #   #   #   #  #  #   # #   #   #    #  ##  # #         |
|    #     #   # # # #   #   ####   #  ####  #   #   #    #  # # # #  ##     |
|    #     #   # #  ##   #   #  #   #  #   # #   #   #    #  #  ## #   #     |
|     ####  ###  #   #   #   #   # ### ####   ###    #   ### #   #  ####     |
|                                                                            |
|                    Reports and changes for giang17/wine                    |
|                  Wine with DirectComposition and Direct2D                  |
|                                                                            |
+============================================================================+

+--[ contributing info ]-----------------------------------------------------+
|  issues .......... github.com/giang17/wine/issues                          |
|  pull requests ... against d2d1-dcomp-11.0                                 |
|  vulnerabilities . privately, see SECURITY.md                              |
|  license ......... GNU LGPL 2.1 or later, the same as Wine                 |
+----------------------------------------------------------------------------+

+--[ where it belongs ]------------------------------------------------------+
|  One test routes almost everything: does stock Wine of the same version    |
|  behave the same way?                                                      |
|                                                                            |
|    * Only this fork does it, or a change here broke what worked before:    |
|      an issue here. A note that an application works is welcome too.       |
|    * Stock Wine does it too, or the question is about Wine itself          |
|      (prefixes, winetricks, audio setup, packaging): the Wine project at   |
|      bugs.winehq.org.                                                      |
+----------------------------------------------------------------------------+

+--[ reporting a problem ]---------------------------------------------------+
|    * The output of wine --version, and where Wine came from: a release     |
|      build, a package (which one), or an own build of which branch.        |
|    * The application and its plugin framework (JUCE, VSTGUI, SynthEdit,    |
|      Electron, WebView2, an own engine), with versions where known.        |
|    * For a JUCE 8.0.13+ plugin: whether the host has its HideWineVersion   |
|      entry. Without it the plugin falls back to GDI, the usual cause of a  |
|      black GUI; PATCHES.md describes the entry.                            |
|    * Whether stock Wine of the same version does it too.                   |
|                                                                            |
|  Helpful beyond that: a screenshot, GPU and driver, X11 or Wayland, and    |
|  for rendering a log with WINEDEBUG=+d2d,+dcomp.                           |
+----------------------------------------------------------------------------+

+--[ pull requests ]---------------------------------------------------------+
|    * Based on d2d1-dcomp-11.0; rolling branches are rewritten at each      |
|      devel tag.                                                            |
|    * One change per commit, in Wine's style ("d2d1: Clip the layer to the  |
|      geometry bounds."), without #<number>: in a fork it links into the    |
|      upstream Wine repository.                                             |
|    * Source only, no binaries or build output, and no diagnostic code.     |
|    * Built for both PE architectures (--enable-archs=i386,x86_64) and      |
|      tested: which application, before and after.                          |
|                                                                            |
|  A fix to Wine code outside this fork's own work belongs to WineHQ, where  |
|  it reaches every Wine user.                                               |
+----------------------------------------------------------------------------+

+--[ license and credit ]----------------------------------------------------+
|  Contributions are accepted under the GNU LGPL 2.1 or later. Code from     |
|  another project needs its origin and license stated. Wine's rule against  |
|  contributions from anyone who has studied disassembled Microsoft code or  |
|  leaked Windows source applies here too. Credit goes into the commit       |
|  message, and into the README for a visible part of the work.              |
+----------------------------------------------------------------------------+

+--[ links ]-----------------------------------------------------------------+
|  * issues                                                                  |
|      https://github.com/giang17/wine/issues                                |
|  * patches                                                                 |
|      https://github.com/giang17/wine/blob/d2d1-dcomp-11.0/PATCHES.md       |
|  * wine bugzilla                                                           |
|      https://bugs.winehq.org/                                              |
|  * wine gitlab                                                             |
|      https://gitlab.winehq.org/wine/wine                                   |
+----------------------------------------------------------------------------+

                      -----  giang17/wine  |  GNU LGPL  |  2026  -----
```
