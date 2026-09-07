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

# sec.exe writes UTF-8: it is built with /utf-8, and its diagnostics carry
# section signs and em-dashes. PowerShell decodes a native tool's stdout using
# [Console]::OutputEncoding, which in a normal console is the OEM codepage, so
# without this every non-ASCII byte is mangled on the way in -- and -Update
# then writes the mangling back into the .expected files. This has to happen
# before the first capture below; it is restored after the run.
$utf8 = New-Object System.Text.UTF8Encoding $false
$priorConsole = $null
try {
    $priorConsole = [Console]::OutputEncoding
    [Console]::OutputEncoding = $utf8
} catch {
    $priorConsole = $null
    Write-Host "run.ps1: could not switch the console to UTF-8; non-ASCII output may be mangled" -ForegroundColor Yellow
}
$priorPipe = $OutputEncoding
$OutputEncoding = $utf8

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
    $want = (Get-Content -Raw -Encoding UTF8 $expected) -replace "`r`n", "`n"
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

    # `stdlib` is a root like any other (sec. 2.1): the standard library gets no
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

# The topology diagram (SPEC 15.x). Opt-in the same way the manifest is: drop a
# <name>.topology beside the .sim and it is compared.
#
# The lint is not decoration. Mermaid's comment stripper needs at least one
# character after the `%%`, so a BARE `%%` line is not treated as a comment: it
# survives into the parser, concatenates with whatever follows, and the diagram
# fails to render with an error pointing at line 1 that mentions none of this.
# The emitter cannot express one any more, and this is the assertion that it
# stays that way -- a golden-file diff would not catch it being reintroduced
# somewhere new, because the diff would just be accepted on -Update.
function Check-Topology($stem, $outDir) {
    $produced = Join-Path $outDir 'Sim.topology.mmd'
    if (-not (Test-Path $produced)) {
        Write-Host "FAIL tests/emit/$stem : no Sim.topology.mmd was written" -ForegroundColor Red
        $script:fail++
        return
    }
    $lines = Get-Content $produced
    $bare = @($lines | Where-Object { $_ -eq '%%' })
    if ($bare.Count -gt 0) {
        Write-Host ("FAIL tests/emit/$stem.topology : " + $bare.Count +
                    " bare '%%' line(s); Mermaid will not parse the diagram") -ForegroundColor Red
        $script:fail++
    } else {
        Write-Host "ok   tests/emit/$stem.topology : no bare '%%' lines" -ForegroundColor Green
        $script:pass++
    }

    $expected = Join-Path $root ("tests\emit\" + $stem + '.topology')
    if (-not (Test-Path $expected)) { return }
    # -Encoding UTF8 for the manifest's reason: sec writes BOM-less UTF-8 and
    # PowerShell 5.1 would otherwise decode it as the ANSI codepage.
    $actual = (Get-Content -Raw -Encoding UTF8 $produced) -replace "`r`n", "`n"
    Compare-Text "tests/emit/$stem.topology" $expected $actual
}

$emitDirs = @{}
foreach ($f in (Get-ChildItem -Path (Join-Path $root 'tests\emit') -File -Filter '*.sim' |
                Sort-Object Name)) {
    $d = Run-Emit $f.Name
    if ($d) {
        $stem = [IO.Path]::GetFileNameWithoutExtension($f.Name)
        $emitDirs[$stem] = $d
        Check-Manifest $stem $d
        Check-Topology $stem $d
    }
}

# The worked examples must emit cleanly too. `washout` needs the stdlib on the
# path as well: it is the first example built out of library blocks rather than
# out of nodes it declares itself.
$examples = @(
    @{ name = 'drivetrain'; sim = 'examples/drivetrain/Drivetrain.sim'; roots = @('examples') },
    @{ name = 'washout';    sim = 'examples/washout/Washout.sim';       roots = @('examples', 'stdlib') },
    # The same channel as `washout`, built from one vector block instead of
    # Split3 + three scalar blocks + Merge3. It is the acceptance case for
    # record settings (SPEC 6.2c), so it has to compile on every run.
    @{ name = 'washout3';   sim = 'examples/washout/ClassicalFromScratch.sim';
       roots = @('examples', 'stdlib') }
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
#  The stdlib smoke model.
# -----------------------------------------------------------------------------
#  Every `node` under stdlib/ instantiated exactly once, with every input driven,
#  generated from the directory itself so a new block joins without anyone
#  remembering to add it.
#
#  This exists because a stdlib block that nothing instantiates is compiled by
#  nothing: `Merge2` declared `output(x, y, z)` on two inputs, `Vec3ToVec2` did
#  not match its own filename, `SecondOrderSystem2` declared
#  `node SecondOrderSystem3`, and `Integrator3` collided its instances with its
#  own ports AND merged `U` where the axes carried `U*s`. Four blocks, four
#  faults, none of them reachable by any test. Each was found by being USED for
#  the first time, which is the slowest way there is to find them.
#
#  Every unit parameter binds to `m`; nothing here is numerical, so any
#  dimension serves. A block with a setting that has no default has to be listed
#  in $smokeSettings -- if it is not, the emit fails and says which setting is
#  missing, which is the intended way to notice.
$smokeSkip = @(
    # Unfinished drafts that do not compile today, kept out so the check stays
    # green and the debt stays visible. Delete the entry, not the test, when the
    # block is finished.
    'se.sig.WnZeta2Poly',   # pre-dates the "a unit is a type argument" unification
                            # (b989ec4): still spells `wn (rad/s) double;`, gives
                            # `output(x)` an input its empty `inputs {}` does not
                            # declare, and writes Poly3's fields as a2/a1/a0
                            # where the type calls them c2/c1/c0.
    'se.kin.AngRateToQuatRate'
                            # unfinished: `w` is spelled `(rad/sec)`, which is not
                            # a unit in Appendix A -- it is `rad/s` -- and
                            # `output(q)` omits the `w` its body reads, which
                            # SPEC 8.3 makes a C++ compile error rather than a
                            # `sec` diagnostic.
                            #
                            # The bad unit used to CRASH `sec` rather than
                            # diagnose -- a wire between two parametric record
                            # ports carrying the same unknown unit dereferenced
                            # a null record. Fixed now, with `tests/emit/RecNull`
                            # holding the line, so this entry is an ordinary
                            # unfinished-draft skip again: without it the smoke
                            # emit FAILS, where before it died silently.
)
$smokeSettings = @{
    'se.sig.ScaleLimit'             = 'limit = 1.0(m);'
    'se.sig.ScaleLimit3'            = 'limit = { 1.0, 1.0, 1.0 };'
    'se.sig.ct.SecondOrderLowpass'  = 'wn = 1.0(rad/s); z = 1.0;'
    'se.sig.ct.SecondOrderLowpass2' = 'wn = 1.0(rad/s); z = 1.0;'
    'se.sig.dt.Butterworth'         = 'fc = 1.0(Hz);'
    'se.sig.dt.Differentiator'      = 'wc = 1.0(rad/s);'
    'se.sig.dt.LowPass2'            = 'wn = 1.0(rad/s);'
    'se.sig.src.Ramp'               = 'slope = 1.0(m/s);'
}

# Sec. 6.3: an unconnected input is an error, there is no implicit zero, so
# every record-valued port needs a record SOURCE -- and that source has to carry
# the port's unit. `Vec3(rad)` fed from a `Merge3(m)` is an SE0410, not a smoke
# pass, so one merge is made per (width, unit) pair on demand and shared by
# fan-out. Sec. 6.9.2 forbids fan-IN only; fan-out is free.
$smokeSrc = [ordered]@{}

function Get-SmokeVecSource([int]$n, [string]$u) {
    $key = "k${n}_" + ($u -replace '[^A-Za-z0-9]', '_')
    if (-not $smokeSrc.Contains($key)) {
        $ports = if ($n -eq 2) { @('x', 'y') } else { @('x', 'y', 'z') }
        $fan   = ($ports | ForEach-Object { "$key.$_" }) -join ', '
        $smokeSrc[$key] = @(
            "        node ${key}: math.Merge${n}($u) {};",
            "        0.0 --> $fan;")
    }
    return $key
}

# A quaternion source, for the ports whose type is `kin.Quat`. There is no
# `MergeQuat` to reach for and there should not be one -- four loose doubles are
# not a rotation. `QuatExp` of a zero rotation vector is, and it is identity.
function Get-SmokeQuatSource() {
    if (-not $smokeSrc.Contains('kq')) {
        $rv = Get-SmokeVecSource 3 'rad'
        $smokeSrc['kq'] = @(
            "        node kq: se.kin.QuatExp {};",
            "        $rv --> kq.v;")
    }
    return 'kq'
}

$smokeRoot = Join-Path $emitOut 'stdlib-smoke'
if (Test-Path $smokeRoot) { Remove-Item -Recurse -Force $smokeRoot }
$smokePkg = Join-Path $smokeRoot 'smoke'
New-Item -ItemType Directory -Force $smokePkg | Out-Null

$smokeDecls = New-Object Collections.Generic.List[string]
$smokeWires = New-Object Collections.Generic.List[string]
$smokeCount = 0
foreach ($f in (Get-ChildItem (Join-Path $root 'stdlib') -Recurse -Filter *.se | Sort-Object FullName)) {
    # Comments come off first: a `//` line mentioning `node Foo {` or `inputs {`
    # would otherwise read as a declaration.
    $text = ((Get-Content $f.FullName) -replace '//.*$', '') -join "`n"
    $m = [regex]::Match($text, '(?m)^\s*node\s+(\w+)\s*\{')
    if (-not $m.Success) { continue }          # a `type` file, not a node
    $pkg = [regex]::Match($text, '(?m)^\s*package\s+([\w.]+)\s*;').Groups[1].Value
    $fq  = $pkg + '.' + $m.Groups[1].Value
    if ($smokeSkip -contains $fq) { continue }

    $um = [regex]::Match($text, 'units\s*\{([^}]*)\}')
    $na = 0
    if ($um.Success) { $na = @($um.Groups[1].Value -split ';' | Where-Object { $_.Trim() }).Count }
    $uargs = ''
    if ($na -gt 0) { $uargs = ' (' + ((@('m') * $na) -join ', ') + ')' }

    $ident = 'n_' + $fq.Replace('.', '_')
    $smokeDecls.Add("        node ${ident}: ${fq}${uargs} { " + $smokeSettings[$fq] + " };")

    # The unit parameters this node declares. An input annotated with one of
    # them binds to `m` at the instantiation above; anything else is a literal
    # unit and needs a source of its own.
    $uparams = @()
    if ($um.Success) {
        $uparams = @($um.Groups[1].Value -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    }

    # A scalar port takes `0.0` whatever its unit -- a bare literal takes the
    # site's. A record port does not have that luxury; see $smokeSrc above.
    $im = [regex]::Match($text, 'inputs\s*\{([^}]*)\}')
    if ($im.Success) {
        foreach ($d in ($im.Groups[1].Value -split ';')) {
            if (-not $d.Trim()) { continue }
            $parts = $d -split ':', 2
            $port  = $parts[0].Trim()
            $src   = '0.0'
            if ($parts[1] -match 'Vec([23])\s*\(\s*([^)]*?)\s*\)') {
                $n = [int]$Matches[1]
                $u = $Matches[2]
                if ($uparams -contains $u) { $u = 'm' }
                $src = Get-SmokeVecSource $n $u
            } elseif ($parts[1] -match 'Quat') {
                $src = Get-SmokeQuatSource
            }
            $smokeWires.Add("        $src --> ${ident}.${port};")
        }
    }
    $smokeCount++
}

$smokeSe = @"
// GENERATED by tests/run.ps1 -- do not edit, do not commit.
package smoke;

use se.math;
use se.kin;

node Smoke {
    structure {
$(($smokeSrc.Values | ForEach-Object { $_ }) -join "`n")

$($smokeDecls -join "`n")

$($smokeWires -join "`n")
    }
}
"@
$smokeSim = @"
sim Smoke {
    root: smoke.Smoke {};

    step:     10 (ms);
    solver:   rk4;
    duration: 10 (ms);
}
"@
Set-Content (Join-Path $smokePkg 'Smoke.se')  $smokeSe  -Encoding utf8
Set-Content (Join-Path $smokePkg 'Smoke.sim') $smokeSim -Encoding utf8

$smokeOut = Join-Path $smokeRoot 'emit'
$smokeErr = (& $sec --no-color --quiet --max-errors=0 --emit `
                    -I stdlib -I ($smokeRoot -replace '\\', '/') `
                    -o ($smokeOut -replace '\\', '/') `
                    ((Join-Path $smokePkg 'Smoke.sim') -replace '\\', '/') 2>&1 | Out-String)
if ($LASTEXITCODE -eq 0) {
    Write-Host ("ok   stdlib smoke --emit ($smokeCount nodes)") -ForegroundColor Green
    $script:pass++
    $emitDirs['stdlib-smoke'] = $smokeOut
} else {
    Write-Host ("FAIL stdlib smoke --emit ($smokeCount nodes)") -ForegroundColor Red
    Write-Host $smokeErr
    $script:fail++
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

    # The cases in the suite that MUST NOT COMPILE.
    #
    # Section 10.5 put `sim` in scope of every body. That is only safe because
    # the surface it widens is NOT the one section 10.2 confines: a leaf holds
    # a plain se_rt::RunView, and preamble() shadows it with an
    # se_rt::RunControl in init() and on_step() alone. Section 10.6 rests on the
    # same split -- `sim.check_*` observes and is legal everywhere, `sim.require`
    # halts and is not. Nothing else here would notice a refactor that injected
    # RunControl everywhere or moved a halting member onto RunView, so these
    # assert the negative by compiling it and requiring the failure.
    #
    # Each case pins THREE conditions, because "the compile failed" on its own
    # would pass for a typo: it must fail, name the confined member on RunView,
    # and say nothing about the member on the next line -- the positive half of
    # the same claim, which has to still work.
    #
    # Kept outside tests/emit deliberately: the loop above compiles every case
    # it finds there and expects success.
    $cppFail = @(
        @{ name = 'PureStop'; member = 'stop';    legal = 'time';
           note = 'sim.stop in output() is a compile error at the .se line, and sim.time() is not' },
        @{ name = 'PureReq';  member = 'require'; legal = 'check_near';
           note = 'sim.require in output() is a compile error at the .se line, and sim.check_near() is not' }
    )
    foreach ($cf in $cppFail) {
        $cfOut = Join-Path $emitOut $cf.name
        if (Test-Path $cfOut) { Remove-Item -Recurse -Force $cfOut }
        New-Item -ItemType Directory -Force $cfOut | Out-Null
        $cfArg = $cfOut.Replace('\', '/')
        & $sec --no-color --quiet --emit -I 'tests/cppfail/lib' -o $cfArg ("tests/cppfail/" + $cf.name + ".sim")
        if ($LASTEXITCODE -ne 0) {
            Write-Host ("FAIL tests/cppfail/" + $cf.name +
                        ".sim : should emit cleanly, sec refused it") -ForegroundColor Red
            $script:fail++
            continue
        }
        Push-Location $cfOut
        $cfLog  = (& '.\build.bat' 2>&1 | Out-String)
        $cfCode = $LASTEXITCODE
        Pop-Location
        $named  = ($cfLog -match ("'" + $cf.member + "'")) -and ($cfLog -match 'RunView')
        $quiet  = ($cfLog -notmatch ("'" + $cf.legal + "'"))
        $atLine = ($cfLog -match ($cf.name + '\.se'))
        if ($cfCode -ne 0 -and $named -and $quiet -and $atLine) {
            Write-Host ("ok   tests/cppfail/" + $cf.name + " : " + $cf.note) -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host ("FAIL tests/cppfail/" + $cf.name + " : expected C2039 on '" +
                        $cf.member + "' at the .se line") -ForegroundColor Red
            Write-Host ("     exit=" + $cfCode + " named=" + $named + " quiet=" + $quiet +
                        " at_se_line=" + $atLine)
            Write-Host $cfLog
            $script:fail++
        }
    }

    # build.ps1 is the third emitted script, and an emitted script nobody runs
    # is an emitted script that does not work. build.bat has just built every
    # case above, so this proves the PowerShell spelling on ONE of them rather
    # than paying for a second full compile everywhere: the two scripts differ
    # only in how the SAME argument list is quoted and how the exit code is
    # read, so one case exercises the whole difference. build.sh cannot be run
    # here at all, which is exactly why the two that can be, are.
    if ($emitDirs.ContainsKey('Decay')) {
        $psDir = $emitDirs['Decay']
        Push-Location $psDir
        # Removed first, so a build.ps1 that silently does nothing cannot pass
        # on the executable build.bat left behind.
        if (Test-Path 'build') { Remove-Item -Recurse -Force 'build' }
        $out = (& powershell -NoProfile -File '.\build.ps1' 2>&1 | Out-String)
        $code = $LASTEXITCODE
        $made = Test-Path '.\build\Decay.exe'
        Pop-Location
        if ($code -eq 0 -and $made -and $out -notmatch 'warning') {
            Write-Host "ok   build/emit/Decay : build.ps1 compiles clean" -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host "FAIL build/emit/Decay : build.ps1 did not compile clean" -ForegroundColor Red
            Write-Host $out
            $script:fail++
        }
    }

    # Decay is x(t) = exp(-k t) with k = 2, so the recorded column is checkable
    # against the closed form rather than against a golden file.
    if ($emitDirs.ContainsKey('Decay')) {
        $decayDir = $emitDirs['Decay']
        Push-Location $decayDir
        & '.\build\Decay.exe' 2>&1 | Out-Null
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

    # SPECIFICATION 5.2 -- a record wire converts field by field, the same way a
    # scalar port does. RecConv drives 1000/2000/3000 mm through a Vec3(m) port
    # and sums the three, so the recorded column is 6 exactly. Drop the
    # conversion and it reads 6000, so this is a real check and not a smoke test.
    if ($emitDirs.ContainsKey('RecConv')) {
        $rcDir = $emitDirs['RecConv']
        Push-Location $rcDir
        & '.\build\RecConv.exe' 2>&1 | Out-Null
        $ran = ($LASTEXITCODE -eq 0)
        Pop-Location
        $worst = [double]::PositiveInfinity
        $csv = Join-Path $rcDir 'recconv.csv'
        if ($ran -and (Test-Path $csv)) {
            $worst = 0.0
            foreach ($line in (Get-Content $csv | Select-Object -Skip 1)) {
                if (-not $line) { continue }
                $err = [Math]::Abs([double]($line -split ',')[1] - 6.0)
                if ($err -gt $worst) { $worst = $err }
            }
        }
        if ($worst -lt 1e-12) {
            Write-Host ("ok   build/emit/RecConv : Vec3(mm) into Vec3(m) converts " +
                        "per field") -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host ("FAIL build/emit/RecConv : expected 6 m, off by " + $worst) `
                -ForegroundColor Red
            $script:fail++
        }
    }

    # SPECIFICATION 13.5 -- a port is addressable at any level of nesting. Seven
    # columns, one per way a producer chain can end: the root's own output, a
    # composite output, a composite input, two fields of a record leaf input,
    # and leaf inputs ending on a setting and on a literal. Every expected value
    # is distinct, and every wire in the model converts, so dropping the chain
    # walk, the conversion or the flattened field index moves a column by a
    # factor of a thousand or onto a different field of the Vec3.
    if ($emitDirs.ContainsKey('PortPath')) {
        $ppDir = $emitDirs['PortPath']
        Push-Location $ppDir
        & '.\build\PortPath.exe' 2>&1 | Out-Null
        $ran = ($LASTEXITCODE -eq 0)
        Pop-Location
        $want = @(8.0, 7.5, 0.5, 1.0, 3.0, 4.0, 0.5)
        $worst = [double]::PositiveInfinity
        $csv = Join-Path $ppDir 'portpath.csv'
        if ($ran -and (Test-Path $csv)) {
            $worst = 0.0
            foreach ($line in (Get-Content $csv | Select-Object -Skip 1)) {
                if (-not $line) { continue }
                $cols = $line -split ','
                for ($c = 0; $c -lt $want.Count; $c++) {
                    $err = [Math]::Abs([double]$cols[$c + 1] - $want[$c])
                    if ($err -gt $worst) { $worst = $err }
                }
            }
        }
        if ($worst -lt 1e-12) {
            Write-Host ("ok   build/emit/PortPath : ports read through their " +
                        "producers at every level") -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host ("FAIL build/emit/PortPath : a column is off by " + $worst) `
                -ForegroundColor Red
            $script:fail++
        }
    }

    # `se.kin.QuatExp` and `se.kin.QuatLog` against their closed forms and
    # against each other. See tests/emit/lib/qexp/QExp.se for what every column
    # is for; the short version is that qId and qTiny straddle the 1e-8 Taylor
    # crossover on BOTH maps, qGen's three distinct components catch a
    # transposed axis through the round trip, and qWrap is the one case the
    # round trip cannot check itself -- a rotation past pi, which has to come
    # back the short way round or a washout leak unwinds the wrong direction.
    # RELATIVE tolerance, because qTiny.x is 5e-10 and an absolute bound would
    # pass it whatever it held.
    if ($emitDirs.ContainsKey('QExp')) {
        $qeDir = $emitDirs['QExp']
        Push-Location $qeDir
        & '.\build\QExp.exe' 2>&1 | Out-Null
        $ran = ($LASTEXITCODE -eq 0)
        Pop-Location
        # Written out rather than computed, so an expected column is a fact
        # about exp() and log() and not a second evaluation of the same library
        # call the model already made. sqrt(1/2); cos 1; sin(1)/3; 2 sin(1)/3;
        # cos 2; 4 - 2 pi.
        $want = @(
            # exp(v): qId, q90, qGen, qTiny
            1.0, 0.0,
            0.70710678118654752, 0.70710678118654752,
            0.54030230586813977,
            0.28049032826929884, 0.56098065653859767, 0.56098065653859767,
            1.0, 5.0e-10,
            # log(exp(v)) == v, the same four
            0.0,
            1.5707963267948966,
            0.66666666666666667, 1.3333333333333333, 1.3333333333333333,
            1.0e-9,
            # 4 rad about z: w goes negative, and log brings it back the short
            # way as 4 - 2 pi. Without the qw >= 0 flip this column reads +4.
            -0.41614683654714239, -2.2831853071795865)
        $worst = [double]::PositiveInfinity
        $csv = Join-Path $qeDir 'qexp.csv'
        if ($ran -and (Test-Path $csv)) {
            $worst = 0.0
            foreach ($line in (Get-Content $csv | Select-Object -Skip 1)) {
                if (-not $line) { continue }
                $cols = $line -split ','
                for ($c = 0; $c -lt $want.Count; $c++) {
                    $scale = [Math]::Max(1.0, [Math]::Abs($want[$c]))
                    $err = [Math]::Abs([double]$cols[$c + 1] - $want[$c]) / $scale
                    if ($err -gt $worst) { $worst = $err }
                }
            }
        }
        # The CSV carries %.9g, so 1e-9 is the floor a correct column can reach.
        if ($worst -lt 1e-8) {
            Write-Host ("ok   build/emit/QExp : QuatExp/QuatLog match their closed " +
                        "forms on both sides of the Taylor crossover, round-trip, " +
                        "and take the short way past pi, max error " +
                        $worst.ToString('E2')) -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host ("FAIL build/emit/QExp : a column is off by " + $worst) `
                -ForegroundColor Red
            $script:fail++
        }
    }

    # SPECIFICATION 6.9.2 -- a setting as a wire source. ParWire drives all
    # three spellings (a whole record, a scalar fanned out to two destinations,
    # and a leaf of a record) from `(mm/s^2)` settings into `(m/s^2)` ports, so
    # the recorded column is 17 exactly. Both conversion paths are in the sum:
    # lose the per-field walk or the scalar fold and the answer moves by a
    # factor of a thousand, not by a rounding error.
    if ($emitDirs.ContainsKey('ParWire')) {
        $pwDir = $emitDirs['ParWire']
        Push-Location $pwDir
        & '.\build\ParWire.exe' 2>&1 | Out-Null
        $ran = ($LASTEXITCODE -eq 0)
        Pop-Location
        $worst = [double]::PositiveInfinity
        $csv = Join-Path $pwDir 'parwire.csv'
        if ($ran -and (Test-Path $csv)) {
            $worst = 0.0
            foreach ($line in (Get-Content $csv | Select-Object -Skip 1)) {
                if (-not $line) { continue }
                $err = [Math]::Abs([double]($line -split ',')[1] - 17.0)
                if ($err -gt $worst) { $worst = $err }
            }
        }
        if ($worst -lt 1e-12) {
            Write-Host ("ok   build/emit/ParWire : settings on wires convert and " +
                        "fan out") -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host ("FAIL build/emit/ParWire : expected 17 m/s^2, off by " + $worst) `
                -ForegroundColor Red
            $script:fail++
        }
    }

    # SPECIFICATION 6.9.2 -- a literal as a wire source. LitWire drives all five
    # spellings (bare literals taking the site's unit, a suffixed literal, a
    # negative one, one fanned out to two destinations, and one folded down a
    # two-hop chain into a child composite), so the recorded column is 16
    # exactly. The suffixed values are declared in `(mm/s^2)` and read in
    # `(m/s^2)`: lose the fold and the answer moves by a factor of a thousand,
    # not by a rounding error.
    if ($emitDirs.ContainsKey('LitWire')) {
        $lwDir = $emitDirs['LitWire']
        Push-Location $lwDir
        & '.\build\LitWire.exe' 2>&1 | Out-Null
        $ran = ($LASTEXITCODE -eq 0)
        Pop-Location
        $worst = [double]::PositiveInfinity
        $csv = Join-Path $lwDir 'litwire.csv'
        if ($ran -and (Test-Path $csv)) {
            $worst = 0.0
            foreach ($line in (Get-Content $csv | Select-Object -Skip 1)) {
                if (-not $line) { continue }
                $err = [Math]::Abs([double]($line -split ',')[1] - 16.0)
                if ($err -gt $worst) { $worst = $err }
            }
        }
        if ($worst -lt 1e-12) {
            Write-Host ("ok   build/emit/LitWire : literals on wires fold, convert " +
                        "and fan out") -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host ("FAIL build/emit/LitWire : expected 16 m/s^2, off by " + $worst) `
                -ForegroundColor Red
            $script:fail++
        }
    }

    # RecSet is the record-settings model (SPEC 6.2c). Three axes are driven by
    # three DIFFERENT ramps through three DIFFERENT gains into three DIFFERENT
    # limits, so no two columns are interchangeable and each has its own closed
    # form:
    #
    #   src.a = { t, 2t, 3t }              gain = { 1.0, 0.5, 2.0 }
    #   x = min(1.0 * 1t, 1.0) = min( t, 1.0)
    #   y = min(0.5 * 2t, 2.0) = min( t, 2.0)
    #   z = min(2.0 * 3t, 1.5) = min(6t, 1.5)
    #
    # A fourth column asserts NESTED record settings, where the leaf walk has to
    # recurse and a unit argument has to thread down two levels.
    #
    # That shape is the point. A leaf walk that read `gain.x` for all three axes,
    # or `limit.x` for all three, would still produce a well-formed CSV that a
    # golden diff would accept on -Update; these numbers refuse it.
    #
    # The z column carries two more assertions on its own. Its limit is declared
    # `3000.0 (mm/s^2)` against an (m/s^2) site, so a leaf converts on its own
    # like a record wire's field does; and recset.settings then overrides
    # `cue.limit.z` to 1.5, which is the leaf-addressed override channel. Drop
    # the conversion and the default is 3000 (never saturating); drop the
    # override and it is 3. Only both working gives min(6t, 1.5).
    if ($emitDirs.ContainsKey('RecSet')) {
        $rsDir = $emitDirs['RecSet']
        Push-Location $rsDir
        & '.\build\RecSet.exe' 2>&1 | Out-Null
        $ran = ($LASTEXITCODE -eq 0)
        Pop-Location
        $worst = [double]::PositiveInfinity
        $csv = Join-Path $rsDir 'recset.csv'
        if ($ran -and (Test-Path $csv)) {
            $worst = 0.0
            foreach ($line in (Get-Content $csv | Select-Object -Skip 1)) {
                if (-not $line) { continue }
                $c = $line -split ','
                $t = [double]$c[0]
                $errs = @([Math]::Abs([double]$c[1] - [Math]::Min($t, 1.0)),
                          [Math]::Abs([double]$c[2] - [Math]::Min($t, 2.0)),
                          [Math]::Abs([double]$c[3] - [Math]::Min(6.0 * $t, 1.5)),
                          # The nested-record column. Six leaves, each weighted
                          # by its own power of ten, so this ONE number names
                          # every one of them: 1 + 20 + 300 + 4000 + 50000 +
                          # 600000. Two leaves swapped, `lo` and `hi` flattened
                          # into each other, or the nested unit argument not
                          # threaded through Span into Vec3 (which would leave
                          # hi.z at 6000 rather than 6) all move it by at least
                          # 9 -- far outside any rounding.
                          [Math]::Abs([double]$c[4] - 654321.0))
                foreach ($e in $errs) { if ($e -gt $worst) { $worst = $e } }
            }
        }
        # The state is t' = 1, which rk4 integrates exactly, so the only error
        # here is the recorder's %.9g. The nested column is near 654321, where
        # nine significant figures resolve about 1e-3; the ramp columns resolve
        # about 1e-9. The failures this guards against are all O(1) or larger --
        # a wrong leaf, a dropped conversion, a missed override -- so the bound
        # is set by the file's precision and not by how tight it could be.
        if ($worst -lt 1e-3) {
            Write-Host ("ok   build/emit/RecSet : per-axis record settings match their " +
                        "closed forms, max error " + $worst.ToString('E2')) -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host ("FAIL build/emit/RecSet : max error " + $worst) -ForegroundColor Red
            $script:fail++
        }
    }

    # ChainChk is the wire-chain model (SPEC 6.9.2). One statement,
    #
    #     src --> sq --> off --> dbl, self.tap;
    #
    # carries a four-node chain and a fan-out on the last hop, and the three
    # stages do not commute:
    #
    #   src.y = t   sq.y = t^2   off.y = t^2 + 10   dbl.y = 2*off.y
    #
    # Column 1 is dbl.y. Column 2 is mark.y, which is whatever the comma
    # delivered plus 1000 -- read through a node of its own, because recording
    # off.y would have proved nothing: off.y is what it is however the comma
    # was attached. Order the chain wrongly and column 1 reads 2*(t+10)^2, 288
    # rather than 28 at t = 2; attach the comma one arrow earlier and column 2
    # is low by 10; attach it after dbl and column 2 is high by t^2 + 10. None
    # of those is a rounding difference, so this refuses a golden file that a
    # -Update would otherwise have accepted.
    if ($emitDirs.ContainsKey('ChainChk')) {
        $ccDir = $emitDirs['ChainChk']
        Push-Location $ccDir
        & '.\build\ChainChk.exe' 2>&1 | Out-Null
        $ran = ($LASTEXITCODE -eq 0)
        Pop-Location
        $worst = [double]::PositiveInfinity
        $csv = Join-Path $ccDir 'chain.csv'
        if ($ran -and (Test-Path $csv)) {
            $worst = 0.0
            foreach ($line in (Get-Content $csv | Select-Object -Skip 1)) {
                if (-not $line) { continue }
                $c = $line -split ','
                $t = [double]$c[0]
                $off = $t * $t + 10.0
                $errs = @([Math]::Abs([double]$c[1] - 2.0 * $off),
                          [Math]::Abs([double]$c[2] - ($off + 1000.0)))
                foreach ($e in $errs) { if ($e -gt $worst) { $worst = $e } }
            }
        }
        # t' = 1, which rk4 integrates exactly, so the only error is the
        # recorder's %.9g. The mark column sits near 1014, where nine
        # significant figures resolve about 1e-5. Every failure this guards
        # against is 10 or larger -- a misordered chain, a comma on the wrong
        # arrow -- so the bound is set by the file's precision and not by how
        # tight it could be.
        if ($worst -lt 1e-4) {
            Write-Host ("ok   build/emit/ChainChk : the chain and its last-hop fan-out " +
                        "match their closed forms, max error " + $worst.ToString('E2')) `
                -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host ("FAIL build/emit/ChainChk : max error " + $worst) -ForegroundColor Red
            $script:fail++
        }
    }

    # Contract checks (SPECIFICATION 10.6). The model in tests/emit/lib/contract
    # makes six contracts about itself. Three hold for the whole run and three
    # fail on a schedule that is closed form, so every number below is derived
    # rather than recorded:
    #
    #   q is unit-norm      fails at EVERY evaluation. 16 output passes (ticks
    #                       0..15) plus 15 integrations of 4 rk4 stages = 76.
    #   t in [0, 0.0102]    fails on the trial states past the window: 3 from
    #                       tick 10's integration and 4 from each of ticks
    #                       11..14, so 3 + 16 = 19.
    #   reaches tick 15     the halting form, once, and it ends the run there.
    #
    # The three that HOLD are as much of the assertion as the three that fail. A
    # satisfied contract must leave nothing whatever in the log, and that is
    # what the silence check below refuses to let regress; without it the log
    # would fill with contracts that are working.
    #
    # Two properties here are ones no other case in the suite would catch. The
    # THROTTLE: five reports and then a count, because 76 identical lines bury
    # the first one, which is the only one that locates the cause. And the
    # MINOR-STEP instant: the window's upper end sits between two ticks, so the
    # first failure is reported at t = 0.0105 -- proof that a contract in a pure
    # method sees the solver's clock and not the tick's.
    if ($emitDirs.ContainsKey('Contract')) {
        $ctDir = $emitDirs['Contract']
        Push-Location $ctDir
        & '.\build\Contract.exe' 2>&1 | Out-Null
        $ctCode = $LASTEXITCODE
        Pop-Location

        $bad = @()
        # A violated contract must fail the RUN. This is what lets a contract be
        # a test oracle instead of something a harness has to go grepping for.
        if ($ctCode -ne 1) { $bad += "exit code $ctCode, expected 1" }

        $ctLog = Join-Path $ctDir 'contract.log'
        if (-not (Test-Path $ctLog)) {
            $bad += 'no contract.log was written'
        } else {
            # -Encoding UTF8 for the manifest's reason: the file is written
            # BOM-less and 5.1 would otherwise decode it as the ANSI codepage.
            $ctLines = @(Get-Content -Encoding UTF8 $ctLog)

            $rep = @($ctLines | Where-Object {
                $_.Contains('contract failed: q is unit-norm; got 1.05') })
            if ($rep.Count -ne 5) {
                $bad += "$($rep.Count) reports for the always-failing contract, expected 5"
            } elseif (-not ($rep[0].Contains('error w: ') -and
                            $rep[0].Contains('|err| 0.05 > tol 1e-09') -and
                            $rep[0].Contains('(contract.Contract#Watch, tick 0)'))) {
                # The report has to carry the path, the type, the values AND the
                # bound. A report that only says a contract failed is the thing
                # this feature exists to replace.
                $bad += "the report is missing part of its detail: $($rep[0])"
            }
            $sup = @($ctLines | Where-Object {
                $_.Contains('q is unit-norm; further reports suppressed') })
            if ($sup.Count -ne 1) { $bad += "$($sup.Count) suppression notices, expected 1" }

            foreach ($quiet in @('k is finite', 'k is positive', 't does not run backwards')) {
                if (@($ctLines | Where-Object { $_.Contains($quiet) }).Count -ne 0) {
                    $bad += "a satisfied contract was reported: $quiet"
                }
            }

            $want = @(
                '3 contract(s) violated',
                'w: q is unit-norm -- 76 time(s), first at t = 0 s (tick 0)',
                'w: t is within the checked window -- 19 time(s), first at t = 0.0105 s (tick 10)',
                'w: the run reaches tick 15 -- 1 time(s), first at t = 0.015 s (tick 15)',
                'run error: contract failed: the run reaches tick 15')
            foreach ($w in $want) {
                if (@($ctLines | Where-Object { $_.Contains($w) }).Count -ne 1) {
                    $bad += "missing from the summary: $w"
                }
            }
        }

        if ($bad.Count -eq 0) {
            Write-Host ("ok   build/emit/Contract : contracts report path, values and " +
                        "bound, throttle at five, and fail the run") -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host ("FAIL build/emit/Contract : " + ($bad -join '; ')) -ForegroundColor Red
            $script:fail++
        }
    }

    # ArrSt is the array-state model (SPEC 6.4a): three independent decays in
    # ONE continuous array state, a scalar continuous state beside them in the
    # same block, a discrete array state used as a shift register, and a scalar
    # discrete state that stays in `dis`. All four have exact closed forms, so
    # the dynamic lowering of SPEC 15.5a is checked on arithmetic and not on
    # shape:
    #
    #   x[i](t) = x[i](0) * exp(-(i+1) t)     x[1](0) = 2, from an element IC
    #   total   = 0                            a scalar state that never moves
    #   h[i][k] = k - 1 - i                    the shift register, k >= i + 1
    #   tick    = k
    #
    # The x[1] column is the one that matters most: it is the only value in the
    # suite that a per-element initial condition (SPEC 13.4) can move, and it
    # would read 1 rather than 2 if the override were dropped or landed on the
    # wrong slot.
    if ($emitDirs.ContainsKey('ArrSt')) {
        $asDir = $emitDirs['ArrSt']
        Push-Location $asDir
        & '.\build\ArrSt.exe' 2>&1 | Out-Null
        $ran = ($LASTEXITCODE -eq 0)
        Pop-Location
        $worst = 0.0
        $csv = Join-Path $asDir 'arrst.csv'
        if ($ran -and (Test-Path $csv)) {
            $rows = Get-Content $csv | Select-Object -Skip 1
            foreach ($line in $rows) {
                if (-not $line) { continue }
                $c = $line -split ','
                $t = [double]$c[0]
                $k = [Math]::Round($t / 0.001)
                $x0 = [Math]::Exp(-1.0 * $t)
                $x1 = 2.0 * [Math]::Exp(-2.0 * $t)
                $x2 = [Math]::Exp(-3.0 * $t)
                # The shift register holds nothing until it has been fed.
                $h0 = if ($k -ge 1) { $k - 1 } else { 0 }
                $h2 = if ($k -ge 3) { $k - 3 } else { 0 }
                $errs = @([Math]::Abs([double]$c[1] - $x0),
                          [Math]::Abs([double]$c[2] - $x1),
                          [Math]::Abs([double]$c[3] - $x2),
                          [Math]::Abs([double]$c[4]),
                          [Math]::Abs([double]$c[5] - $h0),
                          [Math]::Abs([double]$c[6] - $h2),
                          [Math]::Abs([double]$c[7] - $k),
                          [Math]::Abs([double]$c[8] - ($x0 + $x1 + $x2)),
                          [Math]::Abs([double]$c[9] - $h2))
                foreach ($err in $errs) { if ($err -gt $worst) { $worst = $err } }
            }
        } else {
            $worst = [double]::PositiveInfinity
        }
        # The recorder writes %.9g, so a tick index near 2000 resolves to about
        # 1e-6 in the file. rk4's own error on these decays is nearer 1e-12.
        if ($worst -lt 1e-6) {
            Write-Host ("ok   build/emit/ArrSt : array states match their closed forms, max error " +
                        $worst.ToString('E2')) -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host ("FAIL build/emit/ArrSt : max error " + $worst) -ForegroundColor Red
            $script:fail++
        }
    }

    # BwChk is the array-state feature against the case it was added for: an
    # Nth-order Butterworth as a cascade of second-order sections in ONE leaf
    # (SPEC 6.4a). The assertion is the magnitude response, which the bilinear
    # transform makes exact rather than approximate -- prewarping puts the
    # analog cutoff at wa = K*tan(pi*fc/fs), so the digital gain at f is the
    # analog gain at K*tan(pi*f/fs) and the K cancels:
    #
    #   |H(f)| = 1 / sqrt(1 + (tan(pi f/fs) / tan(pi fc/fs))^(2N))
    #
    # Both orders are checked, and the second is ODD deliberately: an odd order
    # carries a lone real pole, which is the one section that is not a
    # conjugate pair and the one place a cascade goes wrong. A section
    # miscounted, dropped, or placed at the wrong radius moves the answer by a
    # factor of two per order, so the tolerance can be tight.
    #
    # The drive is 20 Hz on a 1 kHz step and the window is the last second, so
    # it spans exactly twenty whole periods and the RMS of the samples is the
    # RMS of the wave -- no windowing error to allow for.
    if ($emitDirs.ContainsKey('BwChk')) {
        $bwDir = $emitDirs['BwChk']
        Push-Location $bwDir
        & '.\build\BwChk.exe' 2>&1 | Out-Null
        $ran = ($LASTEXITCODE -eq 0)
        Pop-Location
        $worst = 0.0
        $csv = Join-Path $bwDir 'bwchk.csv'
        if ($ran -and (Test-Path $csv)) {
            $ratio = [Math]::Tan([Math]::PI * 20.0 / 1000.0) /
                     [Math]::Tan([Math]::PI * 10.0 / 1000.0)
            $sum = @(0.0, 0.0, 0.0)
            $n = 0
            foreach ($line in (Get-Content $csv | Select-Object -Skip 1)) {
                if (-not $line) { continue }
                $c = $line -split ','
                $t = [double]$c[0]
                if ($t -lt 2.0 -or $t -ge 3.0) { continue }
                for ($j = 0; $j -lt 3; $j++) {
                    $v = [double]$c[$j + 1]
                    $sum[$j] += $v * $v
                }
                $n++
            }
            if ($n -eq 0) {
                $worst = [double]::PositiveInfinity
            } else {
                # The drive itself is the first column: an amplitude-1 sine, so
                # its RMS is the reference the other two are gains against.
                # One element per line, deliberately: a line break after the
                # comma inside @( ) ends the element list, and the operator
                # that follows is then applied to the ARRAY.
                $g4 = 1.0 / [Math]::Sqrt(1.0 + [Math]::Pow($ratio, 8))
                $g5 = 1.0 / [Math]::Sqrt(1.0 + [Math]::Pow($ratio, 10))
                $want = @(1.0, $g4, $g5)
                for ($j = 0; $j -lt 3; $j++) {
                    $rms = [Math]::Sqrt($sum[$j] / $n)
                    $err = [Math]::Abs($rms - $want[$j] / [Math]::Sqrt(2.0))
                    if ($err -gt $worst) { $worst = $err }
                }
            }
        } else {
            $worst = [double]::PositiveInfinity
        }
        if ($worst -lt 1e-8) {
            Write-Host ("ok   build/emit/BwChk : Butterworth matches |H(f)| at orders 4 and 5, max error " +
                        $worst.ToString('E2')) -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host ("FAIL build/emit/BwChk : max error " + $worst) -ForegroundColor Red
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
    #
    # Columns 3-5 are ct.Biquad3 on hp's coefficients, driven by the constant
    # vector {1, 2, 3}. By linearity each axis is hp's own response scaled by
    # its component, so they are checked against 1x, 2x and 3x the SAME closed
    # form. That is what catches a cross-wired axis inside the three-axis leaf:
    # six states in one node, where `der.wdy` reading `state.wdx` compiles
    # perfectly and is off by a factor of 2 in the first row.
    if ($emitDirs.ContainsKey('ContBq')) {
        $cbDir = $emitDirs['ContBq']
        Push-Location $cbDir
        & '.\build\ContBq.exe' 2>&1 | Out-Null
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
                $hpv  = $e * ($cs - $q)
                $errs = @([Math]::Abs([double]$c[1] - (1.0 - $e * ($cs + $q))),
                          [Math]::Abs([double]$c[2] - $hpv),
                          [Math]::Abs([double]$c[3] - (1.0 * $hpv)),
                          [Math]::Abs([double]$c[4] - (2.0 * $hpv)),
                          [Math]::Abs([double]$c[5] - (3.0 * $hpv)))
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
            Write-Host ("ok   build/emit/ContBq : rk4 matches both step responses and all three Biquad3 axes, max error " +
                        $worst.ToString('E2')) -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host ("FAIL build/emit/ContBq : max error " + $worst) -ForegroundColor Red
            $script:fail++
        }
    }

    # CtSrc integrates the SAME sine twice, once from se.sig.ct.Sine and once
    # from se.sig.src.Sine, and both columns have the same closed form:
    #
    #     x' = sin(2 pi t), x(0) = 0   =>   x(t) = (1 - cos(2 pi t)) / (2 pi)
    #
    # The point is the gap between them, which is what proves `sim.time()`
    # reaches the solver's minor steps (section 10.5). The ct source is in the
    # minor-step output pass, so rk4 keeps its order; the dt source declares a
    # `rate` and is zero-order held, so its forcing is a staircase and the
    # error is A*h/2 = 5.0e-4 -- and that is a property of the HOLD, not of the
    # solver, so it does not shrink with a better integrator.
    #
    # Both bounds matter and they fail in opposite directions. ct < 1e-7 breaks
    # if the minor-step pass stops running continuous nodes; dt > 1e-4 breaks
    # if a held node starts seeing minor-step time, which is ZOH quietly lost
    # and which a one-sided accuracy check would happily call an improvement.
    if ($emitDirs.ContainsKey('CtSrc')) {
        $csDir = $emitDirs['CtSrc']
        Push-Location $csDir
        & '.\build\CtSrc.exe' 2>&1 | Out-Null
        $ran = ($LASTEXITCODE -eq 0)
        Pop-Location
        $wct = 0.0
        $wdt = 0.0
        $csv = Join-Path $csDir 'ctsrc.csv'
        if ($ran -and (Test-Path $csv)) {
            $rows = Get-Content $csv | Select-Object -Skip 1
            foreach ($line in $rows) {
                if (-not $line) { continue }
                $c   = $line -split ','
                $t   = [double]$c[0]
                $ref = (1.0 - [Math]::Cos(2.0 * [Math]::PI * $t)) / (2.0 * [Math]::PI)
                $a = [Math]::Abs([double]$c[1] - $ref)
                $b = [Math]::Abs([double]$c[2] - $ref)
                if ($a -gt $wct) { $wct = $a }
                if ($b -gt $wdt) { $wdt = $b }
            }
        } else {
            $wct = [double]::PositiveInfinity
        }
        # 1e-7 rather than rk4's own ~1e-12 for ContBq's reason: the recorder
        # writes %.9g, so at this magnitude the FILE resolves about 5e-10.
        if ($wct -lt 1e-7) {
            Write-Host ("ok   build/emit/CtSrc : the continuous source keeps rk4's order, " +
                        "max error " + $wct.ToString('E2')) -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host ("FAIL build/emit/CtSrc : continuous source error " + $wct +
                        " -- sim.time() is not reaching the minor steps") -ForegroundColor Red
            $script:fail++
        }
        if ($wdt -gt 1e-4) {
            Write-Host ("ok   build/emit/CtSrc : the held source stays held, " +
                        "staircase error " + $wdt.ToString('E2')) -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host ("FAIL build/emit/CtSrc : held source error " + $wdt +
                        " is too SMALL -- a rated node is seeing minor-step time, " +
                        "so zero-order hold has been lost") -ForegroundColor Red
            $script:fail++
        }
    }

    # SrcChk drives all twelve se.sig.src blocks for 20 s at 1 kHz and records
    # every sample. Eight of them have an EXACT closed form and are checked
    # against it; the stochastic four are checked on the properties that define
    # them, because a hash-by-hash golden file would freeze an implementation
    # detail rather than the contract.
    #
    # ASCII only in this file, so: the periodic blocks run at 50 Hz on a 1 kHz
    # step, which is exactly 20 samples per period. That makes the normalised
    # phase u = (n * 50 / 1000) mod 1 land on an exact binary fraction, so the
    # closed forms hold at every sample with no phase ambiguity to allow for.
    if ($emitDirs.ContainsKey('SrcChk')) {
        $scDir = $emitDirs['SrcChk']
        Push-Location $scDir
        & '.\build\SrcChk.exe' 2>&1 | Out-Null
        $ran = ($LASTEXITCODE -eq 0)
        Pop-Location
        $csv = Join-Path $scDir 'srcchk.csv'
        if ($ran -and (Test-Path $csv)) {
            $rows = Get-Content $csv | Select-Object -Skip 1
            $twoPi = 2.0 * [Math]::PI
            $worst = 0.0
            $wn = New-Object 'System.Collections.Generic.List[double]'
            $un = New-Object 'System.Collections.Generic.List[double]'
            $pn = New-Object 'System.Collections.Generic.List[double]'
            $pr = New-Object 'System.Collections.Generic.List[double]'
            $n = 0
            foreach ($line in $rows) {
                if (-not $line) { continue }
                $c = $line -split ','
                $t = [double]$c[0]
                # Normalised phase of the 50 Hz group.
                $u = ($n * 50.0 / 1000.0) % 1.0
                $tri = if ($u -lt 0.25) { 4.0 * $u }
                       elseif ($u -lt 0.75) { 2.0 - 4.0 * $u }
                       else { 4.0 * $u - 4.0 }
                # Chirp: phase is the integral of a linear sweep, restarted
                # every `sweep` seconds. kq = (f1 - f0) / (2 * sweep) = 190.
                $tc = $t % 0.5
                $errs = @(
                    [Math]::Abs([double]$c[1] - 2.5),
                    [Math]::Abs([double]$c[2] - $(if ($t -lt 0.25) { -1.0 } else { 3.0 })),
                    [Math]::Abs([double]$c[3] - (1.0 + 4.0 * $t)),
                    [Math]::Abs([double]$c[4] - 2.0 * [Math]::Sin($twoPi * $u)),
                    [Math]::Abs([double]$c[5] - $(if ($u -lt 0.25) { 1.5 } else { -1.5 })),
                    [Math]::Abs([double]$c[6] - 3.0 * $tri),
                    [Math]::Abs([double]$c[7] - (0.5 + 2.0 * $u - 1.0)),
                    [Math]::Abs([double]$c[8] -
                        [Math]::Sin($twoPi * (10.0 * $tc + 190.0 * $tc * $tc))))
                foreach ($e in $errs) { if ($e -gt $worst) { $worst = $e } }
                $wn.Add([double]$c[9]); $un.Add([double]$c[10])
                $pn.Add([double]$c[11]); $pr.Add([double]$c[12])
                $n++
            }
        } else {
            $worst = [double]::PositiveInfinity
        }

        # 1e-7 for ContBq's reason and not the solver's: the recorder writes
        # %.9g, so a value near 3 resolves only to about 5e-9 in the FILE. These
        # are closed-form evaluations with no integration error at all, so
        # anything above file resolution is a real defect.
        if ($worst -lt 1e-7) {
            Write-Host ("ok   build/emit/SrcChk : 8 waveforms match their closed forms, max error " +
                        $worst.ToString('E2')) -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host ("FAIL build/emit/SrcChk : max waveform error " + $worst) -ForegroundColor Red
            $script:fail++
        }

        # The stochastic four. Tolerances are STATISTICAL, not numerical: over
        # 20000 samples a sample standard deviation sits within roughly 1% of
        # the true one, so 3% never flakes while still catching a dropped scale
        # factor, a wrong mean, or a broken Box-Muller -- all of which are O(1)
        # errors, not O(1%) ones.
        if ($worst -lt [double]::PositiveInfinity) {
            function Stat($v) {
                $m = 0.0; foreach ($x in $v) { $m += $x }; $m /= $v.Count
                $s = 0.0; foreach ($x in $v) { $s += ($x - $m) * ($x - $m) }
                return @($m, [Math]::Sqrt($s / $v.Count))
            }
            $bad = @()
            $s1 = Stat $wn      # sigma = 2.0, mean = 0.5
            if ([Math]::Abs($s1[0] - 0.5) -gt 0.06) { $bad += "wn mean $($s1[0])" }
            if ([Math]::Abs($s1[1] - 2.0) -gt 0.06) { $bad += "wn sd $($s1[1])" }
            $s2 = Stat $un      # lo = -3, hi = 1 -> mean -1, sd 4/sqrt(12)
            if ([Math]::Abs($s2[0] + 1.0) -gt 0.05) { $bad += "un mean $($s2[0])" }
            if ([Math]::Abs($s2[1] - (4.0 / [Math]::Sqrt(12.0))) -gt 0.05) {
                $bad += "un sd $($s2[1])"
            }
            # Pink noise: sd only. Its MEAN is deliberately not asserted -- 1/f
            # power means most of the energy is at the lowest resolvable
            # frequencies, so the sample mean over any finite window is itself a
            # random walk and does not converge. That is the definition of pink,
            # not a defect, and asserting on it would produce a flaky test.
            $s3 = Stat $pn
            if ([Math]::Abs($s3[1] - 1.0) -gt 0.10) { $bad += "pn sd $($s3[1])" }

            # PRBS: the two properties that make it an identification signal.
            # Order 9, so one period is 2^9 - 1 = 511 samples, and it must repeat
            # EXACTLY -- a wrong tap set still produces a plausible-looking
            # bitstream, just with a shorter period, which this catches and no
            # statistical check would.
            $lv = ($pr | Sort-Object -Unique)
            if ($lv.Count -ne 2 -or $lv[0] -ne -1.0 -or $lv[1] -ne 1.0) {
                $bad += "pr levels $($lv -join '/')"
            }
            $per = 511
            for ($i = 0; $i -lt ($pr.Count - $per); $i++) {
                if ($pr[$i] -ne $pr[$i + $per]) { $bad += "pr period broken at $i"; break }
            }
            if ($bad.Count -eq 0) {
                Write-Host ("ok   build/emit/SrcChk : noise statistics and PRBS period " +
                            "match their specifications") -ForegroundColor Green
                $script:pass++
            } else {
                Write-Host ("FAIL build/emit/SrcChk : " + ($bad -join '; ')) -ForegroundColor Red
                $script:fail++
            }
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
        $out = (& cl /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /O2 /MD `
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

$OutputEncoding = $priorPipe
if ($null -ne $priorConsole) {
    try { [Console]::OutputEncoding = $priorConsole } catch { }
}

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
