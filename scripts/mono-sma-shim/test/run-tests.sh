#!/usr/bin/env bash
#
# run-tests.sh — regression tests for the PowerShell subset in the shim.
#
# Drives test/smahost.exe under Wine with inline scripts and checks the streams
# it prints.  Each case names the script and the lines expected in the output
# (substring match).  Exit 0 only when every case passes.
#
# Usage:
#   scripts/mono-sma-shim/test/run-tests.sh [--prefix DIR] [--script FILE.ps1]
#
#   --prefix DIR    Wine prefix to run in (default: $WINEPREFIX, else ~/.wine)
#   --script FILE   additionally run a real prerun script (Windows or Unix path)
#                   and require an empty error stream
#
# Environment:  WINE  Wine binary (default: `wine` from PATH); use the prefix' own.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PREFIX=${WINEPREFIX:-$HOME/.wine}
WINE=${WINE:-wine}
EXTRA=()
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$2; shift 2 ;;
        --script) EXTRA+=("$2"); shift 2 ;;
        -h|--help) sed -n '3,16p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
[ -f "$HERE/smahost.exe" ] && [ -f "$HERE/System.Management.Automation.dll" ] || { echo "build first: ../build.sh" >&2; exit 2; }

pass=0; fail=0
run() {   # run <name> <script> <expected...>
    local name=$1 script=$2; shift 2
    local out ok=1 exp
    out=$(cd "$HERE" && WINEPREFIX=$PREFIX WINEDEBUG=-all timeout 120 "$WINE" smahost.exe -c "$script" 2>/dev/null | tr -d '\r')
    for exp in "$@"; do
        if ! printf '%s\n' "$out" | grep -qF -- "$exp"; then ok=0; echo "FAIL  $name: expected »$exp«"; fi
    done
    if [ $ok = 1 ]; then pass=$((pass+1)); echo "ok    $name"; else fail=$((fail+1)); printf '%s\n' "$out" | sed 's/^/      | /'; fi
}

run "variables and interpolation" \
    '$a = "x"; $b = "$a$a-$Env:SystemRoot\z"; Write-Host "b=$b"; Write-Host "lit=$($a.Length)"' \
    'INFO:   b=xx-C:\windows\z' 'INFO:   lit=1' 'EXIT:   0'
run "if/elseif/else with -not and -eq" \
    '$x = 5; IF ($x -eq 4) { Write-Host a } ELSEIF (-Not ($x -eq 5)) { Write-Host b } ELSE { Write-Host c }' \
    'INFO:   c'
run "multi-line if, braces on their own lines" \
    $'IF (-Not (Test-Path -Path "C:\\definitely\\missing"))\n{\n\tWrite-Host "missing"\n\tEXIT 3\n}\nWrite-Host "unreached"' \
    'INFO:   missing' 'EXIT:   3'
run "Test-Path on an existing directory" \
    'IF (Test-Path -Path $Env:SystemRoot) { Write-Host yes } else { Write-Host no }' \
    'INFO:   yes'
run "Start-Process -PassThru -Wait, .ExitCode on the parenthesised call" \
    '$code = (Start-Process -FilePath "C:\windows\system32\cmd.exe" -PassThru -WindowStyle hidden -Wait -ArgumentList "/c exit 3").ExitCode; Write-Host "code=$code"; IF (-Not ($code -eq 0)) { Write-Host nonzero }' \
    'INFO:   code=3' 'INFO:   nonzero'
run "Start-Process into a variable, array ArgumentList" \
    '$p = Start-Process -FilePath cmd.exe -PassThru -Wait -ArgumentList "/c","exit 0"; IF (-Not ($p.ExitCode -eq 0)) { Write-Host bad } else { Write-Host "good $($p.HasExited)" }' \
    'INFO:   good True'
run "Start-Process on a missing file: error record, script continues" \
    'Start-Process -FilePath "C:\no\such.exe" -Wait; Write-Host after' \
    'ERROR:  This command cannot be run due to the error' 'INFO:   after'
run "Write-Error fills the error stream, EXIT 1" \
    'Write-Host "Cancelling installation"; Write-Error "Cancelling installation"; EXIT 1' \
    'ERROR:  Cancelling installation' 'EXIT:   1   HadErrors=True'
run "unknown cmdlet: CommandNotFound, script continues" \
    'Get-CimInstance -ClassName Win32_Process; Write-Host continued' \
    'ERROR:  The term ' 'INFO:   continued'
run "unsupported statement: ParseException" \
    'foreach ($x in 1..3) { $x }' \
    'EXCEPTION: ParseException'
run "native command and LASTEXITCODE" \
    'cmd.exe /c "echo hello & exit 7"; Write-Host "rc=$LASTEXITCODE"' \
    'OUTPUT: hello' 'INFO:   rc=7'
run "Start-Sleep and comparison operators" \
    'Start-Sleep -Milliseconds 50; start-sleep -seconds 0; Write-Host "$((3 -gt 2) -and (2 -le 2)) $(1 -ne 1) $("abc" -like "a*")"' \
    'INFO:   True False True'
run "arithmetic, string concatenation, comments" \
    $'# comment\n<# block\ncomment #>\n$n = 2 * 3 + 1 # trailing\nWrite-Host ("n=" + $n)' \
    'INFO:   n=7'
run "Write-Warning, Write-Output, Out-Null pipeline" \
    'Write-Warning "careful"; "o1"; Write-Output "o2" | Out-Null; echo "o3"' \
    'WARN:   careful' 'OUTPUT: o1' 'OUTPUT: o3'
run "\$args from AddParameters" \
    'Write-Host "count=$($args.Count)"' \
    'INFO:   count=0'
run "try/catch/throw" \
    'try { throw "boom" } catch { Write-Host "caught: $($_.Exception.Message)" }' \
    'INFO:   caught: boom'
run "-ErrorAction SilentlyContinue suppresses the record" \
    'Get-Process -Name no_such_process_zz -ErrorAction SilentlyContinue; Write-Host ok' \
    'INFO:   ok' 'HadErrors=False'

for s in "${EXTRA[@]}"; do
    case $s in
        [A-Za-z]:\\*) wpath=$s ;;
        *) wpath="Z:$(printf '%s' "$(readlink -f "$s")" | sed 's|/|\\|g')" ;;
    esac
    out=$(cd "$HERE" && WINEPREFIX=$PREFIX WINEDEBUG=-all timeout 300 "$WINE" smahost.exe "$wpath" 2>/dev/null | tr -d '\r')
    if printf '%s\n' "$out" | grep -q 'HadErrors=False' && ! printf '%s\n' "$out" | grep -q '^EXCEPTION'; then
        pass=$((pass+1)); echo "ok    script $s"
    else
        fail=$((fail+1)); echo "FAIL  script $s"
    fi
    printf '%s\n' "$out" | sed 's/^/      | /'
done

echo "---- $pass passed, $fail failed"
[ $fail = 0 ]
