# Security policy

## What this repository distributes

Source code, and nothing else. There are no releases, no binary assets, no installers and
no prebuilt libraries here. Anything produced from this repository is compiled locally by
the person who runs `configure` and `make`. A download link claiming to offer a built
version of this fork did not come from this repository.

The screenshots in [README.md](README.md) are hosted on the `assets/readme-screenshots`
branch, which holds PNG images and a README describing them, and nothing else. No
application software is distributed here.

## What is in scope

This is a fork of [wine-mirror/wine](https://github.com/wine-mirror/wine). It adds a
DirectComposition implementation together with changes to Direct2D, DirectWrite, DXGI,
WineD3D, winex11 and roughly forty other modules. The changed surface is exactly what
`git diff wine-11.0..HEAD` reports on the branch in question; see
[Verifying this fork](README.md#verifying-this-fork).

- A vulnerability in **code this fork adds or changes** belongs here.
- A vulnerability in **upstream Wine** belongs to the Wine project, which has its own
  process. It is reported at <https://bugs.winehq.org/> and developed at
  <https://gitlab.winehq.org/wine/wine>. Reporting it here delays the fix, because
  nothing in this repository reaches upstream users.

If it is not clear which of the two applies, the test that settles it is whether stock
Wine of the same version shows the same behaviour. If it does, it is upstream.

## Supported branches

| Branch | Status |
|---|---|
| `d2d1-dcomp-11.0` | Maintained. Fixes land here first. |
| highest-numbered `d2d1-dcomp-11.<N>` | Maintained, rebased onto each new WineHQ devel tag. |
| lower-numbered `d2d1-dcomp-11.<N>` | Superseded snapshots, kept for reference, not updated. |
| `d2d1-v6` | Deprecated since 2026-05-03, not updated. |

Fixes are not backported to superseded branches. Moving to the maintained branch of the
same line is the upgrade path.

## Reporting

Open a report through GitHub's private vulnerability reporting on the
[Security tab](https://github.com/giang17/wine/security) of this repository. That keeps
the report non-public until a fix exists.

A report is most useful with the branch and commit it was found on, the module, and a way
to reproduce it. Where a proof of concept exists, a minimal one that demonstrates the
condition is preferable to a working exploit.

For anything that is not a vulnerability, the ordinary
[issue tracker](https://github.com/giang17/wine/issues) is the right place; see
[CONTRIBUTING.md](CONTRIBUTING.md).

## Expectations

This is a single-maintainer fork of a large project, maintained alongside other work.
There is no guaranteed response time and no embargo process beyond keeping a report
private until it is fixed. Reports are answered as time allows, and credit is given in the
commit message unless the reporter asks otherwise.

One class of report is out of scope here by construction. Wine runs Windows software with
the privileges of the user who starts it, and a Wine prefix maps the host filesystem into
the guest, so Windows malware executed under Wine can reach the user's files. That follows
from how Wine works rather than from anything this fork changes, and reports of it are
closed as out of scope. Reports that this fork **widens** what stock Wine exposes are in
scope and are wanted.
