# -----------------------------------------------------------------------------
#  Emit, build and run node tests (SPECIFICATION 6.13).
#
#  `sec --test` only WRITES a test program; building and running it is a
#  separate step, by the same rule that keeps `sec --emit` from invoking a
#  compiler (12.4). This script is that step, for a developer's shell.
#
#      powershell -File tools\nodetest.ps1                   every tested stdlib node
#      powershell -File tools\nodetest.ps1 stdlib\se\kin     every tested node under a directory
#      powershell -File tools\nodetest.ps1 stdlib\se\kin\EulerToQuat.se
#      powershell -File tools\nodetest.ps1 -I mylib mylib\pkg\Node.se
#
#  -I names the root(s) the files are resolved against, as for sec; the
#  default is `stdlib`. Output goes under build\test (or -Out), one directory
#  per file. A file is tested if it contains a `tests {` section.
#
#  Exit status: 0 when every program built and passed, 1 otherwise, 2 when
#  sec or the compiler could not be found.
#
#  ASCII only: Windows PowerShell 5.1 reads a BOM-less .ps1 as cp1252.
# -----------------------------------------------------------------------------
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Path = @('stdlib'),
    [string[]]$I = @('stdlib'),
    [string]$Out = 'build\test'
)

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Push-Location $root
try {
    $sec = Join-Path $root 'build\sec.exe'
    if (-not (Test-Path $sec)) {
        Write-Host "nodetest: $sec not found; run build.bat first" -ForegroundColor Red
        exit 2
    }

    # The compiler. Found here, not by sec: `cl` on PATH, or else the latest
    # Visual Studio's environment imported into this session.
    if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path $vswhere) {
            $vsRoot = & $vswhere -latest -products * -property installationPath 2>$null
            $vcvars = if ($vsRoot) { Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat' }
            if ($vcvars -and (Test-Path $vcvars)) {
                & cmd /c "call ""$vcvars"" >nul 2>&1 && set" | ForEach-Object {
                    if ($_ -match '^([^=]+)=(.*)$') {
                        Set-Item -Path ("env:" + $matches[1]) -Value $matches[2] `
                            -ErrorAction SilentlyContinue
                    }
                }
            }
        }
        if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
            Write-Host "nodetest: cl not found (run from a Developer Command Prompt)" -ForegroundColor Red
            exit 2
        }
    }

    # The files: each argument is a .se file or a directory searched for them.
    $files = @()
    foreach ($p in $Path) {
        if (-not (Test-Path $p)) {
            Write-Host "nodetest: no such file or directory: $p" -ForegroundColor Red
            exit 2
        }
        $item = Get-Item $p
        $cands = if ($item.PSIsContainer) {
            Get-ChildItem -Recurse -File -Path $item.FullName -Filter '*.se' | Sort-Object FullName
        } else {
            @($item)
        }
        foreach ($f in $cands) {
            if ((Get-Content -Raw -Encoding UTF8 $f.FullName) -match '(?m)^\s*tests\s*\{') {
                $files += $f.FullName
            }
        }
    }
    if ($files.Count -eq 0) {
        Write-Host "nodetest: no file with a tests section under: $($Path -join ', ')"
        exit 0
    }

    $rootArgs = @()
    foreach ($r in $I) { $rootArgs += @('-I', $r.Replace('\', '/')) }

    # Native tools write to stderr; that is output here, not an exception.
    $ErrorActionPreference = 'Continue'
    $programs = 0
    $failed = 0
    foreach ($full in $files) {
        $rel = if ($full.StartsWith($root)) { $full.Substring($root.Length + 1) } else { $full }
        $rel = $rel.Replace('\', '/')
        $tag = ($rel -replace '\.se$', '') -replace '[/:]', '_'
        $dir = Join-Path $Out $tag
        if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }

        & $sec --quiet --test @rootArgs -o $dir.Replace('\', '/') $rel
        if ($LASTEXITCODE -ne 0) {
            Write-Host "FAIL $rel : sec --test refused it" -ForegroundColor Red
            $failed++
            continue
        }

        # One subdirectory per tested node in the file.
        foreach ($nodeDir in (Get-ChildItem -Directory -Path $dir | Sort-Object Name)) {
            $programs++
            Push-Location $nodeDir.FullName
            # A child PowerShell, so cl's output is captured for the failure report.
            $log = (& powershell -NoProfile -File '.\build.ps1' exe 2>&1 | Out-String)
            $code = $LASTEXITCODE
            $exe = Get-ChildItem -File -Path 'build\release\bin' -Filter '*_test.exe' `
                       -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($code -ne 0 -or -not $exe) {
                Pop-Location
                Write-Host "FAIL $rel : the test program for $($nodeDir.Name) did not build" `
                    -ForegroundColor Red
                Write-Host $log
                $failed++
                continue
            }
            $lines = & $exe.FullName 2>&1
            $code = $LASTEXITCODE
            Pop-Location
            foreach ($line in $lines) {
                $s = "$line"
                if ($s.StartsWith('ok ')) { Write-Host $s -ForegroundColor Green }
                elseif ($s.StartsWith('FAIL')) { Write-Host $s -ForegroundColor Red }
                else { Write-Host $s }
            }
            if ($code -ne 0) { $failed++ }
        }
    }

    Write-Host ""
    if ($failed -eq 0) {
        Write-Host "$programs test program(s), all passed" -ForegroundColor Green
        exit 0
    }
    Write-Host "$programs test program(s), $failed failed" -ForegroundColor Red
    exit 1
} finally {
    Pop-Location
}
