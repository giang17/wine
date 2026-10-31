# mono-sma-shim — System.Management.Automation for Wine-Mono

## The problem

Steinberg's installer bootstrapper (`Setup.exe`, a .NET application) runs the
`<prerun …ps1>` scripts listed in `setup.xml` — the scripts that shut down a running
MediaBay server or media clients before a package is updated. It does not start
`powershell.exe` for that. It hosts PowerShell in-process through the Windows
PowerShell hosting API:

```
System.Management.Automation, Version=3.0.0.0, PublicKeyToken=31bf3856ad364e35
```

Wine-Mono does not ship that assembly. The load fails, Setup.exe catches the
exception silently, logs "Finished Installation", exits with 0 — and installs
**nothing**. There is no error dialog and no hint in the setup log; the only trace is
in Mono's assembly log (`MONO_LOG_LEVEL=info MONO_LOG_MASK=asm`):

```
The following assembly referenced from …\Setup.exe could not be loaded:
     Assembly:   System.Management.Automation    (assemblyref_index=5)
```

Every Steinberg package whose `setup.xml` carries a `.ps1` prerun is affected
(MediaBay, and the products that depend on it). Packages with EXE preruns only
(`silkupdatehelper.exe`, `vc_redist`) are not.

## What the shim is

A `System.Management.Automation.dll` with the identity Setup.exe asks for — version
3.0.0.0, delay-signed with Microsoft's real public key (`ms-pub.snk`; Mono does not
verify strong-name signatures, so the token matches and nothing else is checked) —
that implements the part of the hosting API installers use, and runs the script with
a small interpreter for the PowerShell subset those prerun scripts are written in.

Disassembling `Setup.exe` (Wine-Mono's own `ikdasm.exe` does it) shows exactly what
its `run_powershell` method touches: `PowerShell.Create`, `AddScript`, `AddParameters`,
`Invoke`, `Dispose`, and `Streams.Information` / `.Warning` / `.Error` with `ToString()`
on each record. It never reads the script's exit code. A prerun has *failed* when the
error stream is not empty (the scripts call `Write-Error` before `EXIT 1`) or when
`Invoke()` throws; then Setup.exe reports `installation_failed` and skips the package.
Information and warning records appear as `Prerun: …` lines in the setup log
(`Setup.exe --silent --logv <dir>`).

The interpreter covers: variables, `$Env:NAME`, string interpolation, `IF/ELSEIF/ELSE`,
`while`, `try/catch/finally`, `throw`, `EXIT n`; the operators `-not -eq -ne -gt -ge -lt
-le -and -or -like -match -contains -replace + - * / %`; member access by reflection
(`(Start-Process …).ExitCode`, `$p.HasExited`); the cmdlets `Write-Host`, `Write-Output`,
`Write-Error`, `Write-Warning`, `Write-Verbose`, `Write-Debug`, `Test-Path`,
`Start-Process` (`-Wait -PassThru -WindowStyle -ArgumentList -WorkingDirectory -Verb`),
`Get-Process`, `Stop-Process`, `Wait-Process`, `Start-Sleep`, `Join-Path`, `Split-Path`,
`New-Item`, `Remove-Item`, `Copy-Item`, `Get-Item`, `Get-Content`, `Get-Date`,
`Out-Null`, `Out-Host`, `Out-String`, `Get-/Set-ExecutionPolicy`; and native commands
found on `PATH` (`taskkill /F /IM …`), whose stderr becomes error records as in
Windows PowerShell 5.1.

**Anything else is an error, not a silent success.** `foreach`, `switch`, `[type]`
literals, hash tables, `Get-CimInstance`, registry drives and unknown cmdlets raise a
`ParseException` or a `CommandNotFoundException`. Setup.exe then reports the script
as failed, which is the honest outcome: a half-executed preparation reported as
success would be worse than the original silence.

## Build, install, verify

No host Mono is needed; Wine-Mono ships `mcs.exe`, and the scripts run it through
Wine. A host `mcs` (`apt install mono-devel`) is used when present.

```bash
scripts/mono-sma-shim/build.sh                      # -> System.Management.Automation.dll + test/smahost.exe
scripts/mono-sma-shim/install.sh --prefix ~/.wine   # into the prefix' Wine-Mono GAC, then a probe
scripts/mono-sma-shim/install.sh --prefix ~/.wine --status
scripts/mono-sma-shim/test/run-tests.sh             # 17 regression cases
scripts/mono-sma-shim/test/run-tests.sh --script "…/Additional Content/Installer Data/preinstall.ps1"
```

Set `WINE` to the Wine binary that owns the prefix when it is not the one on `PATH`.

The shim goes to

```
<prefix>/drive_c/windows/mono/mono-2.0/lib/mono/gac/System.Management.Automation/3.0.0.0__31bf3856ad364e35/
```

because Mono probes the application directory, the GAC and `MONO_PATH`, and only the
GAC is a place that exists for every installer. Setup.exe's own `AssemblyResolve`
handler serves embedded resources only.

**The shim lives in the prefix, not in the Wine build.** A `wineboot -u` with a newer
Wine installs a newer Wine-Mono and replaces the `mono-2.0` directory — and the shim
with it, without anything failing. `install.sh --status` (exit 3 when missing) is the
check to run after a prefix update; symptoms are the old ones, a 2-second
"Finished Installation" without an MSI.

Setting `WINE_SMA_TRACE=<file>` in the installer's environment appends one line per
statement, cmdlet call (with bound parameters), process start and error — the way to
see what a prerun did when the setup log is not enough.

## Verified

MediaBay 1.3.100 removed with `msiexec /x`, then `Setup.exe --silent --logv <dir>` with
the shim in the GAC. Setup log:

```
installState: Absent   request: install
Prerun: Is C:\Program Files\Steinberg\MediaBay\SteinbergMediaBayServer.exe installed at all...
Prerun: No! Ok to install.
Prerun: …\preinstall.ps1 executed successfully
```

followed by the MSI installation. The same command ended after two seconds with no
MSI before. With the product already installed Setup.exe reports `request: already`
and never touches the prerun, so a test needs the package absent.

## Files

| File | Contents |
|---|---|
| `Api.cs` | public types: `PowerShell`, `PSDataStreams`, `PSDataCollection<T>`, the record classes, `PSObject`, exceptions |
| `Parser.cs` | AST and scannerless parser (expression and argument mode, interpolation, blocks) |
| `Interpreter.cs` | evaluation, parameter binding, member access by reflection, native commands, streams |
| `Cmdlets.cs` | cmdlet table with parameter specifications and implementations |
| `Values.cs` | PowerShell truthiness, comparison and conversion rules; the trace log |
| `ms-pub.snk` | Microsoft's public key (160-byte blob; the token is `31bf3856ad364e35`) |
| `test/smahost.cs` | test host that drives the API the way Setup.exe does and prints every stream |
| `test/run-tests.sh` | regression tests |
