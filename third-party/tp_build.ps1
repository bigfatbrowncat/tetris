# Build third-party components (bx bimg bgfx shaderc) via MSBuild.
#
# The whole script runs under a lock, for two reasons:
#   1. CMake's Visual Studio / NMake generators duplicate a file-output
#      custom command into every target that consumes one of its outputs,
#      so a parallel `cmake --build` can launch several copies of this
#      script at once — they must not race on the shared .lib outputs or on
#      the .build/projects tree (concurrent GENie runs would interleave
#      their file writes).
#   2. The GENie regeneration below must be atomic with respect to the
#      MSBuild runs that consume the generated projects.
#
# MSBuild accepts only ONE project file per invocation, so one invocation is
# made per project, in sequence; the remaining dependencies (bx, the
# tint/spirv tree) come in through the generated project references.
#
# Usage:
#   tp_build.ps1 <lock-dir> <msbuild> <genie> <bgfx-root> <sln> <genie-action>
#                <project.vcxproj> [project.vcxproj ...]
param(
    [Parameter(Mandatory=$true)][string]$LockDir,
    [Parameter(Mandatory=$true)][string]$MsBuild,
    [Parameter(Mandatory=$true)][string]$Genie,
    [Parameter(Mandatory=$true)][string]$BgfxRoot,
    [Parameter(Mandatory=$true)][string]$Sln,
    [Parameter(Mandatory=$true)][string]$GenieAction,
    [Parameter(Mandatory=$true, ValueFromRemainingArguments=$true)][string[]]$Projects
)

$pidFile = Join-Path $LockDir "pid"
while ($true) {
    try {
        # An atomic mkdir = lock acquisition.
        New-Item -ItemType Directory -Path $LockDir -ErrorAction Stop | Out-Null
        Set-Content -Path $pidFile -Value ([string]$PID) -NoNewline
        break
    } catch {
        # Steal the lock if its holder is gone (killed build).
        $holder = ""
        if (Test-Path $pidFile) { $holder = (Get-Content $pidFile -Raw).Trim() }
        $alive = $false
        if ($holder -match "^\d+$") {
            try { $alive = [bool](Get-Process -Id ([int]$holder) -ErrorAction Stop) }
            catch { $alive = $false }
        }
        if (-not $alive) {
            Remove-Item -Recurse -Force $LockDir -ErrorAction SilentlyContinue
            continue
        }
        Start-Sleep -Seconds 1
    }
}
try {
    # (Re)generate the project if missing (fresh checkout or after a clean).
    # GENie runs from the bgfx root and reads scripts/genie.lua.
    if (-not (Test-Path $Sln)) {
        Push-Location $BgfxRoot
        & $Genie --with-tools $GenieAction
        $genieExit = $LASTEXITCODE
        Pop-Location
        if ($genieExit -ne 0) { exit $genieExit }
    }

    $exitCode = 0
    foreach ($proj in $Projects) {
        & $MsBuild $proj /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
        if ($LASTEXITCODE -ne 0) { $exitCode = $LASTEXITCODE; break }
    }
    exit $exitCode
} finally {
    Remove-Item -Recurse -Force $LockDir -ErrorAction SilentlyContinue
}
