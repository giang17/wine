# Contributing

This is a fork of [wine-mirror/wine](https://github.com/wine-mirror/wine) with a
DirectComposition implementation and a large set of Direct2D, DirectWrite, DXGI, WineD3D
and winex11 changes. It is not the Wine project, and most Wine questions belong there
rather than here.

## Where does this belong?

The first question is whether stock Wine of the same version behaves the same way. That
one test routes almost everything.

| Situation | Where it goes |
|---|---|
| A GUI renders wrong, black or not at all **on this fork** | [Issues here](https://github.com/giang17/wine/issues) |
| A patch in this fork broke something that worked before | [Issues here](https://github.com/giang17/wine/issues) — a regression report is the most valuable kind |
| An application works on this fork and the report says so | Issues here, welcome. Confirmed platforms are what the compatibility notes are built from |
| Stock Wine shows the same behaviour | <https://bugs.winehq.org/> |
| A question about Wine itself — prefixes, winetricks, audio setup, distribution packaging | The Wine project: <https://bugs.winehq.org/>, <https://gitlab.winehq.org/wine/wine> |
| A suspected vulnerability | [SECURITY.md](SECURITY.md), not the public tracker |

Reporting an upstream Wine bug here delays its fix, because nothing in this repository
reaches upstream users.

## Reporting a rendering problem

Rendering reports are the main traffic here, and four pieces of information decide whether
one can be acted on at all:

1. **The branch and commit.** `git rev-parse --short HEAD` and the branch name. The stable
   and the rolling branches differ, and superseded rolling branches are not maintained.
2. **The application and its plugin framework** — JUCE, VSTGUI, SynthEdit/GMPI, Electron,
   WebView2, or an in-house engine — with the version if it is known. The framework
   decides which code path is involved, far more than the application does.
3. **Whether the `HideWineVersion` entry applies to the host.** With JUCE 8.0.13 and newer
   this single switch decides whether a plugin renders through Direct2D or falls back to
   GDI, and the GDI fallback is what a "the GUI is black" report usually turns out to be.
   [PATCHES.md](PATCHES.md) describes the entry and which hosts `wine.inf` already covers.
4. **Whether stock Wine of the same version does it too.** This separates a fork
   regression from a gap that was always there, and the two need opposite work.

Useful beyond that, in rough order: a screenshot of the wrong output, the GPU and driver,
the session type (X11 or Wayland), and a log. For rendering, `WINEDEBUG=+d2d,+dcomp` on
the failing run is usually enough; a full `+relay` log is rarely worth its size.

## Patches

Small, self-contained fixes to Wine code that is not part of this fork's own work are
better sent to WineHQ directly. They reach every Wine user there, and this fork picks them
up on its next rebase. The Wine project takes patches as merge requests on
<https://gitlab.winehq.org/wine/wine>; its conventions for patch format and sign-off apply
there, not here.

Pull requests here are the right route for changes to what this fork itself adds: the
`dcomp` implementation, the Direct2D geometry and layer work, the DXGI composition
swapchain path, and the winex11 changes that go with them.

For a pull request here:

- **Base it on `d2d1-dcomp-11.0`** unless the change is specific to a rolling branch.
  Rolling branches are rebased onto each new devel tag and their history is rewritten;
  work based on them is lost at the next rebase.
- **One logical change per commit**, with a message in Wine's style: the module as prefix,
  then what the change does, for example `d2d1: Clip the layer to the geometry bounds.`
  Present tense, no issue numbers — this repository is a fork, and `#<number>` in a commit
  message creates a cross-reference in the upstream Wine repository.
- **No binaries.** No compiled DLLs, no `.so` files, no archives, no build output. The
  repository holds source only and that is deliberate; see [SECURITY.md](SECURITY.md).
- **No diagnostic code.** Probes, markers, FIXME censuses and debug overlays are useful
  while finding a bug and do not belong in the committed fix.
- **Say how it was tested.** Which application, which plugin, what the output looked like
  before and after. A rendering change that nobody has looked at is not testable from the
  diff, and a screenshot pair is worth more than a paragraph.
- Both PE architectures are built here (`--enable-archs=i386,x86_64`). A change that only
  compiles in one of them will be found late, so building both before opening the request
  saves a round trip.

## Licence

Wine is licensed under the GNU LGPL version 2.1 or later; see [LICENSE](LICENSE).
Contributions are accepted on those terms. Code taken from another project needs its
origin and licence stated in the pull request, and code that cannot be licensed this way
cannot be merged.

Wine has a longstanding rule against contributions from anyone who has studied
disassembled Microsoft code or leaked Windows source. It applies to this fork as well, and
for the same reason: a fork that cannot be upstreamed is of limited use to anyone.

## Credit

Contributions are credited in the commit message and, where they carry a visible part of
the work, in the README. Reports that lead to a fix are credited in the commit that fixes
them.
