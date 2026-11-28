```text
+============================================================================+
|                                                                            |
|                 #### ####  #### #   # ####  ### ##### #   #                |
|                #     #    #     #   # #   #  #    #    # #                 |
|                 ###  ###  #     #   # ####   #    #     #                  |
|                    # #    #     #   # #  #   #    #     #                  |
|                ####  ####  ####  ###  #   # ###   #     #                  |
|                                                                            |
|                      Security policy of giang17/wine                       |
|                  Wine with DirectComposition and Direct2D                  |
|                                                                            |
+============================================================================+

+--[ policy info ]-----------------------------------------------------------+
|  repository ...... github.com/giang17/wine                                 |
|  report to ....... private vulnerability reporting, Security tab           |
|  maintained ...... d2d1-dcomp-11.0, highest 11.<N>, newest release         |
|  response ........ as time allows, single maintainer                       |
+----------------------------------------------------------------------------+

+--[ what this repository distributes ]--------------------------------------+
|  Source code on its branches, and release builds on the Releases page. A   |
|  release build is made only by .github/workflows/release.yml on GitHub's   |
|  runners, from the commit of d2d1-dcomp-11.0 that the release names, and   |
|  carries a build provenance attestation. Nothing in a release is uploaded  |
|  by hand.                                                                  |
|                                                                            |
|  An archive that fails the checksum or the attestation check in            |
|  README.md, section verify, is not what this repository published. A       |
|  download offered elsewhere as a release of this fork is genuine only if   |
|  it passes the same two checks. Packages built by others (README.md,       |
|  section install) are maintained by their authors.                         |
|                                                                            |
|  The assets/* branches hold PNG images and Markdown descriptions of them,  |
|  and nothing else.                                                         |
+----------------------------------------------------------------------------+

+--[ in scope ]--------------------------------------------------------------+
|  This is a fork of wine-mirror/wine. It adds a DirectComposition           |
|  implementation and changes Direct2D, DirectWrite, DXGI, WineD3D, winex11  |
|  and further modules up to the installers. The changed surface is exactly  |
|  what git diff wine-11.0 d2d1-dcomp-11.0 reports (README.md, section       |
|  verify).                                                                  |
|                                                                            |
|    * A vulnerability in code this fork adds or changes.                    |
|    * A change in this fork that widens what stock Wine exposes. Reports    |
|      of this kind are wanted.                                              |
|    * A release build that does not match the commit or the attestation it  |
|      names, and weaknesses in the workflow that builds the releases.       |
+----------------------------------------------------------------------------+

+--[ out of scope ]----------------------------------------------------------+
|    * Vulnerabilities in upstream Wine. They belong to the Wine project,    |
|      which takes reports at bugs.winehq.org. Reporting one here delays     |
|      the fix, because nothing in this repository reaches upstream users.   |
|      The test that settles it: if stock Wine of the same version behaves   |
|      the same way, it is upstream.                                         |
|    * The system libraries a release build links; each release lists them   |
|      in its notes. They come from the distribution and are fixed there.    |
|    * Windows software reaching the user's files. Wine runs Windows         |
|      programs with the privileges of the user who starts them and maps     |
|      the host filesystem into the prefix, so malware run under Wine can    |
|      reach that user's files. That follows from how Wine works, not from   |
|      this fork, and reports of it are closed as out of scope.              |
|    * Packages built by others, unless the problem is in this fork's code;  |
|      those go to their maintainers.                                        |
+----------------------------------------------------------------------------+

+--[ supported ]-------------------------------------------------------------+
|  d2d1-dcomp-11.0      Maintained. Fixes land here first, and the release   |
|                       builds are made from it.                             |
|                                                                            |
|  d2d1-dcomp-11.<N>    The highest number is maintained and rebased onto    |
|                       each WineHQ devel tag. Lower numbers are superseded  |
|                       snapshots, not updated.                              |
|                                                                            |
|  release builds       The newest one. A fix arrives in a new release;      |
|                       earlier releases are not rebuilt.                    |
|                                                                            |
|  d2d1-v6              Deprecated since 2026-05-03, not updated.            |
|                                                                            |
|  Fixes are not backported to superseded branches or earlier releases. The  |
|  upgrade path is the maintained branch of the same line, or the newest     |
|  release build.                                                            |
+----------------------------------------------------------------------------+

+--[ reporting ]-------------------------------------------------------------+
|  Reports go through GitHub's private vulnerability reporting on the        |
|  Security tab of this repository, which keeps them non-public until a fix  |
|  exists.                                                                   |
|                                                                            |
|  A report is most useful with the branch and commit or the release name,   |
|  the module, and a way to reproduce it. A minimal proof of concept that    |
|  shows the condition is preferable to a working exploit.                   |
|                                                                            |
|  Anything that is not a vulnerability belongs in the issue tracker; see    |
|  CONTRIBUTING.md.                                                          |
+----------------------------------------------------------------------------+

+--[ expectations ]----------------------------------------------------------+
|  This is a single-maintainer fork of a large project, maintained           |
|  alongside other work. There is no guaranteed response time and no         |
|  embargo process beyond keeping a report private until it is fixed.        |
|  Reports are answered as time allows, and credit is given in the commit    |
|  message unless the reporter asks otherwise.                               |
+----------------------------------------------------------------------------+

+--[ links ]-----------------------------------------------------------------+
|  * report a vulnerability                                                  |
|      https://github.com/giang17/wine/security/advisories/new               |
|  * security tab                                                            |
|      https://github.com/giang17/wine/security                              |
|  * releases                                                                |
|      https://github.com/giang17/wine/releases                              |
|  * issues                                                                  |
|      https://github.com/giang17/wine/issues                                |
|  * wine bugzilla                                                           |
|      https://bugs.winehq.org/                                              |
+----------------------------------------------------------------------------+

                      -----  giang17/wine  |  GNU LGPL  |  2026  -----
```
