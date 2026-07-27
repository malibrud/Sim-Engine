# -----------------------------------------------------------------------------
#  Parser test runner.
#
#  tests/ok/*   must parse with no diagnostics; the AST dump is compared to
#               <name>.expected.
#  tests/err/*  must produce diagnostics; the full diagnostic text is compared
#               to <name>.expected.
#
#      powershell -File tests\run.ps1            run the suite
#      powershell -File tests\run.ps1 -Update    rewrite the .expected files
#
#  Paths are passed with forward slashes so the recorded output is stable.
#  ASCII only: Windows PowerShell 5.1 reads a BOM-less .ps1 as cp1252, where a
#  UTF-8 em-dash decodes to a smart quote and breaks the parse.
# -----------------------------------------------------------------------------
param([switch]$Update)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$sec  = Join-Path $root 'build\sec.exe'

if (-not (Test-Path $sec)) {
    Write-Host "run.ps1: $sec not found; run build.bat first" -ForegroundColor Red
    exit 2
}

Push-Location $root
$script:pass = 0
$script:fail = 0
$script:updated = 0
$exts = @('.se', '.sim', '.settings')

function Run-Case($relPath, $expectDiagnostics) {
    $expected = [IO.Path]::ChangeExtension((Join-Path $root $relPath), '.expected')

    if ($expectDiagnostics) {
        $actual = (& $sec --no-color $relPath 2>&1 | Out-String)
    } else {
        $actual = (& $sec --no-color --dump-ast $relPath 2>&1 | Out-String)
    }
    $code   = $LASTEXITCODE
    $actual = $actual -replace "`r`n", "`n"

    if ($expectDiagnostics -and $code -eq 0) {
        Write-Host "FAIL $relPath : expected diagnostics, got a clean parse" -ForegroundColor Red
        $script:fail++
        return
    }
    if ((-not $expectDiagnostics) -and $code -ne 0) {
        Write-Host "FAIL $relPath : expected a clean parse, got" -ForegroundColor Red
        Write-Host $actual
        $script:fail++
        return
    }

    if ($Update) {
        Set-Content -Path $expected -Value $actual -NoNewline -Encoding utf8
        Write-Host "update $relPath"
        $script:updated++
        return
    }

    if (-not (Test-Path $expected)) {
        Write-Host "FAIL $relPath : no .expected file (run with -Update)" -ForegroundColor Red
        $script:fail++
        return
    }

    $want = (Get-Content -Raw $expected) -replace "`r`n", "`n"
    if ($want -eq $actual) {
        Write-Host "ok   $relPath" -ForegroundColor Green
        $script:pass++
        return
    }

    Write-Host "FAIL $relPath : output differs from .expected" -ForegroundColor Red
    $wantLines = $want -split "`n"
    $gotLines  = $actual -split "`n"
    $n = [Math]::Max($wantLines.Count, $gotLines.Count)
    for ($i = 0; $i -lt $n; $i++) {
        $w = if ($i -lt $wantLines.Count) { $wantLines[$i] } else { '<missing>' }
        $g = if ($i -lt $gotLines.Count)  { $gotLines[$i] }  else { '<missing>' }
        if ($w -ne $g) {
            Write-Host ("  line " + ($i + 1) + ":")
            Write-Host ("    want: " + $w) -ForegroundColor DarkGray
            Write-Host ("    got : " + $g) -ForegroundColor Yellow
        }
    }
    $script:fail++
}

foreach ($f in (Get-ChildItem -Path (Join-Path $root 'tests\ok') -File | Sort-Object Name)) {
    if ($exts -contains $f.Extension) { Run-Case "tests/ok/$($f.Name)" $false }
}
foreach ($f in (Get-ChildItem -Path (Join-Path $root 'tests\err') -File | Sort-Object Name)) {
    if ($exts -contains $f.Extension) { Run-Case "tests/err/$($f.Name)" $true }
}

# The worked example must always parse cleanly.
$exFiles = @('examples/drivetrain/WheelState.se',
             'examples/drivetrain/Wheel.se',
             'examples/drivetrain/TractionController.se',
             'examples/drivetrain/Corner.se',
             'examples/drivetrain/Drivetrain.sim',
             'examples/drivetrain/drivetrain.settings')
foreach ($f in $exFiles) {
    & $sec --no-color $f | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "ok   $f" -ForegroundColor Green
        $script:pass++
    } else {
        Write-Host "FAIL $f : the worked example must parse cleanly" -ForegroundColor Red
        & $sec --no-color $f
        $script:fail++
    }
}

Pop-Location

if ($Update) {
    Write-Host ""
    Write-Host "$script:updated expectation files written"
    exit 0
}
Write-Host ""
if ($script:fail -eq 0) {
    Write-Host "$script:pass passed" -ForegroundColor Green
    exit 0
}
Write-Host "$script:pass passed, $script:fail failed" -ForegroundColor Red
exit 1
