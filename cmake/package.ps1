param(
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-f]{40}$')][string]$SourceSha
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Deliberate payload only. Never archive a build tree or the checkout.
$stage = Join-Path $PWD 'dist/payload'
$extract = Join-Path $PWD 'dist/verified'
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item build-dist/Release/audio-repeater.exe $stage
Copy-Item README.md, LICENSE, THIRD_PARTY_NOTICES.md $stage
[IO.File]::WriteAllText((Join-Path $stage 'SOURCE_SHA.txt'), "$SourceSha`n")
$files = @('audio-repeater.exe', 'README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'SOURCE_SHA.txt')
$lines = foreach ($file in $files) {
    "{0}  {1}" -f (Get-FileHash (Join-Path $stage $file) -Algorithm SHA256).Hash.ToLowerInvariant(), $file
}
[IO.File]::WriteAllLines((Join-Path $stage 'SHA256SUMS'), $lines)
$zip = Join-Path $PWD "dist/audio-repeater-windows-x64-$SourceSha.zip"
Compress-Archive -Path "$stage/*" -DestinationPath $zip
Expand-Archive -LiteralPath $zip -DestinationPath $extract
$expected = @($files + 'SHA256SUMS' | Sort-Object)
$actual = @(Get-ChildItem $extract -Recurse | ForEach-Object { $_.Name } | Sort-Object)
if (Compare-Object $expected $actual) { throw 'Unexpected distribution contents' }
foreach ($line in Get-Content (Join-Path $extract 'SHA256SUMS')) {
    $hash, $name = $line -split '  ', 2
    if ($name -notin $files -or (Get-FileHash (Join-Path $extract $name) -Algorithm SHA256).Hash -ne $hash) {
        throw "Checksum mismatch: $name"
    }
}
if ((Get-Content (Join-Path $extract 'SOURCE_SHA.txt') -Raw).Trim() -cne $SourceSha) { throw 'Source SHA mismatch' }
# Inspect both normal and delay-load imports with the compiler's own PE utility.
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
if ($LASTEXITCODE -ne 0) { throw 'vswhere failed' }
$dumpbin = Get-ChildItem "$vs/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe" | Sort-Object FullName -Descending | Select-Object -First 1
$exe = Join-Path $extract 'audio-repeater.exe'
$imports = & $dumpbin.FullName /dependents $exe
if ($LASTEXITCODE -ne 0) { throw 'PE import inspection failed' }
$imports | Write-Output
$allowed = @('KERNEL32.dll', 'USER32.dll', 'GDI32.dll', 'SHELL32.dll', 'ole32.dll', 'OLEAUT32.dll', 'ADVAPI32.dll', 'AVRT.dll')
$dependencies = @($imports | ForEach-Object {
    if ($_ -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') { $Matches[1] }
} | Sort-Object -Unique)
if ($dependencies.Count -eq 0) { throw 'Missing PE dependencies' }
foreach ($dll in $dependencies) {
    if ($dll -notin $allowed -or -not (Test-Path "$env:SystemRoot/System32/$dll")) { throw "Unexpected or missing runtime dependency: $dll" }
}
$binary = [IO.File]::ReadAllBytes($exe)
foreach ($encoding in @([Text.Encoding]::ASCII, [Text.Encoding]::Unicode)) {
    $text = $encoding.GetString($binary)
    if ($text -match '(?i)\.pdb\x00|[a-z]:[\\/](?:a[\\/]|_work[\\/]|Users[\\/])') {
        throw 'Debug or workspace path in distribution executable'
    }
    foreach ($path in @($env:GITHUB_WORKSPACE, $env:RUNNER_TEMP)) {
        if ($path -and $text.Contains($path)) { throw 'Runner path in distribution executable' }
    }
}
# Run from the extraction directory, with no build directory or extra DLL on PATH.
foreach ($mode in @('--self-test', '--gui-self-test')) {
    $stdout = Join-Path $PWD "dist/$($mode.TrimStart('-')).txt"
    $stderr = Join-Path $PWD "dist/$($mode.TrimStart('-'))-error.txt"
    $oldPath = $env:PATH
    try {
        $env:PATH = "$env:SystemRoot/System32;$env:SystemRoot"
        $process = Start-Process -FilePath $exe -WorkingDirectory $extract -ArgumentList $mode -NoNewWindow -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        if (-not $process.WaitForExit(8000)) { $process.Kill(); throw "Extracted $mode timed out" }
        Get-Content $stdout
        Get-Content $stderr
        if ($process.ExitCode -ne 0) { throw "Extracted $mode exit $($process.ExitCode)" }
        $pattern = if ($mode -eq '--self-test') { 'self-test: passed' } else { 'gui self-test: passed' }
        if (-not (Select-String -Path $stdout -SimpleMatch $pattern -Quiet)) { throw "Missing $mode output" }
    } finally { $env:PATH = $oldPath }
}
Write-Output "Source SHA: $SourceSha"
Write-Output 'Verified distribution contents and SHA256:'
Get-Content (Join-Path $extract 'SHA256SUMS')
Get-ChildItem $extract | ForEach-Object { "{0} {1} bytes" -f $_.Name, $_.Length }
Write-Output 'ZIP SHA256:'
Get-FileHash $zip -Algorithm SHA256 | Format-List
