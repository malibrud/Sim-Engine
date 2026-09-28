# -----------------------------------------------------------------------------
#  Emit the washout example, and optionally build and run it.
#
#  `sec --emit` writes the generated model and its build.ps1 / build.sh;
#  building and running are separate steps (SPECIFICATION 12.4), which is what
#  -Build and -Run are for.
#
#      powershell -File examples\washout\emit.ps1              emit PulseX
#      powershell -File examples\washout\emit.ps1 -Run         emit, build, run
#      powershell -File examples\washout\emit.ps1 -Run -NoOpen do not open the CSV
#      powershell -File examples\washout\emit.ps1 -Sim Washout.sim
#      powershell -File examples\washout\emit.ps1 -Out build\mine -Run
#
#  -Run opens the recorded CSV in whatever program this machine opens a .csv
#  with, exactly as double-clicking it would; -NoOpen suppresses that.
#
#  PulseX.sim is the default because it is the one that DRIVES the channel: a
#  rectangular pulse into X, recorded at 100 Hz. Washout.sim roots the same
#  node with nothing wired to its inputs, for a host to drive across the
#  boundary, so it emits but has nothing to record.
#
#  Exit status: 0 on success, 1 if sec, the compiler or the model reported a
#  failure, 2 if sec or a compiler could not be found.
#
#  ASCII only: Windows PowerShell 5.1 reads a BOM-less .ps1 as cp1252.
# -----------------------------------------------------------------------------
param(
    [string]$Sim = 'PulseX.sim',
    [string]$Out = 'build\washout',
    [switch]$Build,
    [switch]$Run,
    [switch]$NoOpen
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $here)

Push-Location $root
try {
    $sec = Join-Path $root 'build\sec.exe'
    if (-not (Test-Path $sec)) {
        Write-Host "emit: $sec not found; run build.bat first" -ForegroundColor Red
        exit 2
    }

    # Accept a bare name, a name with the extension, or a path.
    $simPath = $Sim
    if (-not (Test-Path $simPath)) { $simPath = Join-Path $here $Sim }
    if (-not (Test-Path $simPath)) { $simPath = Join-Path $here ($Sim + '.sim') }
    if (-not (Test-Path $simPath)) {
        Write-Host "emit: no such sim file: $Sim" -ForegroundColor Red
        Write-Host ("emit: this example has " +
                    ((Get-ChildItem -Path $here -Filter '*.sim' | ForEach-Object { $_.Name }) -join ', '))
        exit 2
    }
    $simRel = (Resolve-Path $simPath).Path.Substring($root.Length + 1).Replace('\', '/')

    # `examples` and `stdlib` are the roots: the example's own package, and the
    # library it is built out of.
    if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
    & $sec --emit -I examples -I stdlib -o $Out.Replace('\', '/') $simRel
    if ($LASTEXITCODE -ne 0) { exit 1 }
    if (-not ($Build -or $Run)) {
        Write-Host "emit: $simRel -> $Out" -ForegroundColor Green
        exit 0
    }

    # The emitted build.ps1 finds the compiler itself (`cl` on PATH, else the
    # latest Visual Studio). It runs as a child process so its output, cl's
    # included, can be captured and checked for warnings.
    $ErrorActionPreference = 'Continue'
    Push-Location $Out
    try {
        $log = (& powershell -NoProfile -ExecutionPolicy Bypass -File '.\build.ps1' exe 2>&1 |
                Out-String)
        $code = $LASTEXITCODE
        if ($code -ne 0) {
            Write-Host "emit: the generated C++ did not compile" -ForegroundColor Red
            Write-Host $log
            exit $(if ($code -eq 2) { 2 } else { 1 })
        }
        if ($log -match 'warning') {
            Write-Host "emit: the generated C++ compiled with warnings" -ForegroundColor Yellow
            Write-Host $log
        }
        $exe = Get-ChildItem -File -Path 'build\release\bin' -Filter '*.exe' |
               Select-Object -First 1
        Write-Host ("emit: built " + (Join-Path $Out ('build\release\bin\' + $exe.Name))) -ForegroundColor Green
        if (-not $Run) { exit 0 }

        & $exe.FullName
        $code = $LASTEXITCODE
        if ($code -ne 0) {
            Write-Host "emit: the model exited $code" -ForegroundColor Red
            exit 1
        }
    } finally {
        Pop-Location
    }

    # What the run left behind. A sim with no `record:` block writes no CSV,
    # which is worth saying rather than leaving the reader to wonder.
    $csv = Get-ChildItem -File -Path $Out -Filter '*.csv' -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if ($csv) {
        $rows = (Get-Content $csv.FullName | Measure-Object -Line).Lines - 1
        Write-Host ("emit: " + $csv.FullName + " (" + $rows + " rows)") -ForegroundColor Green
        Get-Content $csv.FullName -TotalCount 4 | ForEach-Object { Write-Host "      $_" }
        # Hand it to whatever opens a .csv on this machine, as a double-click
        # would. -NoOpen is for a shell where a window appearing is not wanted.
        if (-not $NoOpen) {
            try {
                Start-Process -FilePath $csv.FullName | Out-Null
            } catch {
                Write-Host ("emit: could not open the CSV: " + $_.Exception.Message) `
                    -ForegroundColor Yellow
            }
        }
    } else {
        # Single-quoted: a backtick starts an escape in a double-quoted string,
        # so "`record:`" would print as "ecord:".
        Write-Host ('emit: the run recorded no CSV (' + $simRel + ' has no `record:` block)')
    }
    exit 0
} finally {
    Pop-Location
}
