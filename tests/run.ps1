# -----------------------------------------------------------------------------
#  Test runner.
#
#  Stages 1-2, the parser:
#    tests/ok/*    must parse with no diagnostics; the AST dump is compared to
#                  <name>.expected.
#    tests/err/*   must produce diagnostics; the full diagnostic text is
#                  compared to <name>.expected.
#
#  Stages 3-6, the code generator:
#    tests/emit/*.sim  are compiled with --emit against the root tests/emit/lib.
#                  A case WITH a <name>.expected must fail, and its diagnostic
#                  text is compared. A case WITHOUT one must emit cleanly.
#                  A case may ALSO carry a <name>.manifest, which is compared
#                  against the emitted Sim.units.txt. That file is the
#                  configuration schema a settings source is checked against,
#                  so a silent change to it is a silent change to the model's
#                  external contract.
#
#  Then, if `cl` is on PATH, the generated C++ is actually compiled, and the
#  decay model is run and its CSV checked against the closed form. Emitting
#  code that does not compile is the failure mode that matters most here, so
#  the suite spends a compiler invocation on it.
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

    $want = (Get-Content -Raw -Encoding UTF8 $expected) -replace "`r`n", "`n"
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

# -----------------------------------------------------------------------------
#  Stages 3-6: the code generator.
# -----------------------------------------------------------------------------
$emitOut = Join-Path $root 'build\emit'

function Compare-Or-Update($relPath, $actual) {
    Compare-Text $relPath ([IO.Path]::ChangeExtension((Join-Path $root $relPath), '.expected')) $actual
}

# Compares generated text against a recorded file. Split out from
# Compare-Or-Update so the manifest check can name its own expected file
# rather than being forced onto the .expected extension, which already means
# "the diagnostics this case must produce".
function Compare-Text($label, $expected, $actual) {
    if ($Update) {
        Set-Content -Path $expected -Value $actual -NoNewline -Encoding utf8
        Write-Host "update $label"
        $script:updated++
        return
    }
    if (-not (Test-Path $expected)) {
        Write-Host "FAIL $label : no recorded file at $expected" -ForegroundColor Red
        $script:fail++
        return
    }
    $want = (Get-Content -Raw $expected) -replace "`r`n", "`n"
    if ($want -eq $actual) {
        Write-Host "ok   $label" -ForegroundColor Green
        $script:pass++
        return
    }
    Write-Host "FAIL $label : differs from the recorded file" -ForegroundColor Red
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

function Run-Emit($simName) {
    $rel      = "tests/emit/$simName"
    $stem     = [IO.Path]::GetFileNameWithoutExtension($simName)
    $expected = Join-Path $root ("tests\emit\" + $stem + '.expected')
    $outDir   = Join-Path $emitOut $stem
    $wantsError = Test-Path $expected

    if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
    New-Item -ItemType Directory -Force $outDir | Out-Null

    # `stdlib` is a root like any other (§2.1) — the standard library gets no
    # special mechanism, it is simply on the search path.
    $actual = (& $sec --no-color --quiet --max-errors=0 --emit `
                      -I stdlib -I tests/emit/lib -o ($outDir -replace '\\', '/') $rel 2>&1 | Out-String)
    $code   = $LASTEXITCODE
    $actual = $actual -replace "`r`n", "`n"

    if ($wantsError) {
        if ($code -eq 0) {
            Write-Host "FAIL $rel : expected diagnostics, got a clean emit" -ForegroundColor Red
            $script:fail++
            return $null
        }
        Compare-Or-Update $rel $actual
        return $null
    }

    if ($code -ne 0) {
        Write-Host "FAIL $rel : expected a clean emit, got" -ForegroundColor Red
        Write-Host $actual
        $script:fail++
        return $null
    }
    Write-Host "ok   $rel" -ForegroundColor Green
    $script:pass++
    return $outDir
}

# The unit manifest is a real generator output, not documentation (SPEC 15.6):
# it is the configuration schema a settings source is checked against, so a
# silent change to it is a silent change to the model's external contract.
# Opt-in per case: drop a <name>.manifest beside the .sim and it is compared.
function Check-Manifest($stem, $outDir) {
    $expected = Join-Path $root ("tests\emit\" + $stem + '.manifest')
    # Opt in strictly by the file existing, so -Update refreshes the cases
    # that asked for a manifest rather than minting one for every case.
    if (-not (Test-Path $expected)) { return }
    $produced = Join-Path $outDir 'Sim.units.txt'
    if (-not (Test-Path $produced)) {
        Write-Host "FAIL tests/emit/$stem : no Sim.units.txt was written" -ForegroundColor Red
        $script:fail++
        return
    }
    # -Encoding UTF8 is not optional: sec writes this file BOM-less, and
    # Windows PowerShell 5.1 decodes a BOM-less file as the ANSI codepage, so a
    # bare Get-Content turns every em-dash into mojibake. Both sides would be
    # corrupted identically and still compare equal, which is worse than
    # failing -- it is a test that passes while reading garbage.
    $actual = (Get-Content -Raw -Encoding UTF8 $produced) -replace "`r`n", "`n"
    Compare-Text "tests/emit/$stem.manifest" $expected $actual
}

$emitDirs = @{}
foreach ($f in (Get-ChildItem -Path (Join-Path $root 'tests\emit') -File -Filter '*.sim' |
                Sort-Object Name)) {
    $d = Run-Emit $f.Name
    if ($d) {
        $stem = [IO.Path]::GetFileNameWithoutExtension($f.Name)
        $emitDirs[$stem] = $d
        Check-Manifest $stem $d
    }
}

# The worked examples must emit cleanly too. `washout` needs the stdlib on the
# path as well: it is the first example built out of library blocks rather than
# out of nodes it declares itself.
$examples = @(
    @{ name = 'drivetrain'; sim = 'examples/drivetrain/Drivetrain.sim'; roots = @('examples') },
    @{ name = 'washout';    sim = 'examples/washout/Washout.sim';       roots = @('examples', 'stdlib') }
)
foreach ($ex in $examples) {
    $exDir = Join-Path $emitOut $ex.name
    if (Test-Path $exDir) { Remove-Item -Recurse -Force $exDir }
    New-Item -ItemType Directory -Force $exDir | Out-Null
    $rootArgs = @()
    foreach ($r in $ex.roots) { $rootArgs += '-I'; $rootArgs += $r }
    & $sec --no-color --quiet --emit @rootArgs -o ($exDir -replace '\\', '/') $ex.sim
    if ($LASTEXITCODE -eq 0) {
        Write-Host ("ok   " + $ex.sim + " --emit") -ForegroundColor Green
        $script:pass++
        $emitDirs[$ex.name] = $exDir
    } else {
        Write-Host ("FAIL " + $ex.sim + " --emit") -ForegroundColor Red
        $script:fail++
    }
}

# -----------------------------------------------------------------------------
#  Compile the generated C++, and run the one model with a closed form.
# -----------------------------------------------------------------------------
#  Emitting code that does not compile is the failure mode that matters most,
#  so the suite goes looking for a compiler rather than quietly skipping.
$cl = Get-Command cl -ErrorAction SilentlyContinue
if (-not $cl) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vcvars = $null
    if (Test-Path $vswhere) {
        $vsRoot = & $vswhere -latest -products * -property installationPath 2>$null
        if ($vsRoot) {
            $candidate = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path $candidate) { $vcvars = $candidate }
        }
    }
    if ($vcvars) {
        # Import the compiler environment into this session, so the compile
        # steps below work from a plain shell.
        & cmd /c "call ""$vcvars"" >nul 2>&1 && set" | ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)$') {
                Set-Item -Path ("env:" + $matches[1]) -Value $matches[2] -ErrorAction SilentlyContinue
            }
        }
        $cl = Get-Command cl -ErrorAction SilentlyContinue
        if ($cl) { Write-Host "note: loaded the MSVC environment from $vcvars" -ForegroundColor DarkGray }
    }
}
if (-not $cl) {
    Write-Host "skip generated-code compile: cl not found (run from a Developer Command Prompt)" -ForegroundColor Yellow
} elseif (-not $Update) {
    # Native tools write to stderr, which 'Stop' would turn into a terminating
    # error before the exit code could be looked at.
    $ErrorActionPreference = 'Continue'
    foreach ($name in ($emitDirs.Keys | Sort-Object)) {
        $dir = $emitDirs[$name]
        Push-Location $dir
        $out = (& '.\build.bat' 2>&1 | Out-String)
        $code = $LASTEXITCODE
        Pop-Location
        if ($code -eq 0 -and $out -notmatch 'warning') {
            Write-Host "ok   build/emit/$name : generated C++ compiles clean" -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host "FAIL build/emit/$name : generated C++ did not compile clean" -ForegroundColor Red
            Write-Host $out
            $script:fail++
        }
    }

    # Decay is x(t) = exp(-k t) with k = 2, so the recorded column is checkable
    # against the closed form rather than against a golden file.
    if ($emitDirs.ContainsKey('Decay')) {
        $decayDir = $emitDirs['Decay']
        Push-Location $decayDir
        & '.\Decay.exe' 2>&1 | Out-Null
        $ran = ($LASTEXITCODE -eq 0)
        Pop-Location
        $worst = 0.0
        $csv = Join-Path $decayDir 'decay.csv'
        if ($ran -and (Test-Path $csv)) {
            $rows = Get-Content $csv | Select-Object -Skip 1
            foreach ($line in $rows) {
                if (-not $line) { continue }
                $c = $line -split ','
                $t = [double]$c[0]
                $x = [double]$c[1]
                $err = [Math]::Abs($x - [Math]::Exp(-2.0 * $t))
                if ($err -gt $worst) { $worst = $err }
            }
        } else {
            $worst = [double]::PositiveInfinity
        }
        if ($worst -lt 1e-9) {
            Write-Host ("ok   build/emit/Decay : rk4 matches exp(-2t), max error " +
                        $worst.ToString('E2')) -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host ("FAIL build/emit/Decay : max error " + $worst) -ForegroundColor Red
            $script:fail++
        }
    }

    # ContBq drives two continuous biquads from a unit step -- same poles
    # (wn = 2 rad/s, zeta = 0.5), different numerators -- and both step
    # responses have an exact closed form. This is the check that a
    # canonical-form section actually integrates: `der.w = state.wd` is a bare
    # accessor-to-accessor write, and while value_ref got that wrong it
    # rebound the proxy instead of storing, leaving the low-pass column
    # identically zero. Nothing else in the suite writes one state accessor
    # straight into another.
    #
    #   sigma = zeta*wn = 1,  wd = wn*sqrt(1 - zeta^2) = sqrt(3)
    #   lp   4/(s^2 + 2s + 4):  1 - e^-sigma*t (cos wd t + (sigma/wd) sin wd t)
    #   hp s^2/(s^2 + 2s + 4):      e^-sigma*t (cos wd t - (sigma/wd) sin wd t)
    #
    # The second follows from the first by H_hp + H_bp + H_lp = 1, so it is a
    # derivation rather than a second magic constant. hp starts at exactly 1,
    # which is the assertion on the direct term b0.
    if ($emitDirs.ContainsKey('ContBq')) {
        $cbDir = $emitDirs['ContBq']
        Push-Location $cbDir
        & '.\ContBq.exe' 2>&1 | Out-Null
        $ran = ($LASTEXITCODE -eq 0)
        Pop-Location
        $worst = 0.0
        $csv = Join-Path $cbDir 'contbq.csv'
        if ($ran -and (Test-Path $csv)) {
            $sigma = 1.0
            $wd    = [Math]::Sqrt(3.0)
            $rows = Get-Content $csv | Select-Object -Skip 1
            foreach ($line in $rows) {
                if (-not $line) { continue }
                $c  = $line -split ','
                $t  = [double]$c[0]
                $e  = [Math]::Exp(-$sigma * $t)
                $cs = [Math]::Cos($wd * $t)
                $sn = [Math]::Sin($wd * $t)
                $q  = ($sigma / $wd) * $sn
                $errs = @([Math]::Abs([double]$c[1] - (1.0 - $e * ($cs + $q))),
                          [Math]::Abs([double]$c[2] - (      $e * ($cs - $q))))
                foreach ($err in $errs) { if ($err -gt $worst) { $worst = $err } }
            }
        } else {
            $worst = [double]::PositiveInfinity
        }
        # Looser than Decay's 1e-9, for a reason that is not the solver: the
        # recorder writes %.9g and the low-pass overshoots past 1, so at that
        # magnitude the FILE resolves only about 5e-9. rk4's own error here is
        # nearer 1e-12. A wrong coefficient is O(0.1), so 1e-7 still catches it.
        if ($worst -lt 1e-7) {
            Write-Host ("ok   build/emit/ContBq : rk4 matches both step responses, max error " +
                        $worst.ToString('E2')) -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host ("FAIL build/emit/ContBq : max error " + $worst) -ForegroundColor Red
            $script:fail++
        }
    }

    # tests/emit/host.cpp is a hand-written host: it includes the generated
    # HEADER, drives the root boundary that the batch driver cannot, and checks
    # the block offset tables and the 200 Hz decimation. It fails to link if the
    # generator ever goes back to emitting its own main().
    if ($emitDirs.ContainsKey('drivetrain')) {
        $hostDir = $emitDirs['drivetrain']
        Copy-Item (Join-Path $root 'tests\emit\host.cpp') $hostDir -Force
        Push-Location $hostDir
        $out = (& cl /nologo /std:c++17 /EHsc /W4 /permissive- /utf-8 /O2 /MD `
                    /D_CRT_SECURE_NO_WARNINGS /Fe:host.exe host.cpp 2>&1 | Out-String)
        $built = ($LASTEXITCODE -eq 0 -and $out -notmatch 'warning')
        $ran = $false
        if ($built) {
            & '.\host.exe' 2>&1 | Out-Null
            $ran = ($LASTEXITCODE -eq 0)
        }
        Pop-Location
        if ($built -and $ran) {
            Write-Host "ok   tests/emit/host.cpp : embeds the header and drives the boundary" -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host "FAIL tests/emit/host.cpp" -ForegroundColor Red
            if (-not $built) { Write-Host $out }
            $script:fail++
        }
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
