param(
    [Parameter(Mandatory=$true)][string]$ProbeDirectory,
    [Parameter(Mandatory=$true)][string]$LogDirectory
)
$ErrorActionPreference = 'Stop'
$probeRoot = (Resolve-Path -LiteralPath $ProbeDirectory).Path
$logRoot = [System.IO.Path]::GetFullPath($LogDirectory)
if (Test-Path -LiteralPath $logRoot) { throw "LogDirectory must be fresh: $logRoot" }
New-Item -ItemType Directory -Path $logRoot | Out-Null
$exe = Join-Path $probeRoot 'h264-production-flush-probe.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'R4 probe binary missing' }
$env:VIRTIO_WDDM_TAG_LIFE_EXPERIMENT = '1'
$env:VIRTIO_WDDM_MEDIA_TAG_EXPERIMENT = '0'
$env:VIRTIO_WDDM_PIPELINE = '1'
$env:VIRTIO_WDDM_PIPELINE_TRACE = '1'
$env:VIRTIO_WDDM_LOG_FILE = Join-Path $logRoot 'umd.log'
$p = Start-Process -FilePath $exe -WorkingDirectory $probeRoot -ArgumentList '--flush red.h264 blue.h264' -PassThru -RedirectStandardOutput (Join-Path $logRoot 'probe.stdout.log') -RedirectStandardError (Join-Path $logRoot 'probe.stderr.log')
$null = $p.Handle
if (-not $p.WaitForExit(30000)) {
    Stop-Process -Id $p.Id -Force
    throw "R4 probe timed out; no pass. Logs: $logRoot"
}
$p.WaitForExit()
$p.Refresh()
if ($null -eq $p.ExitCode) { throw 'Probe exit code unavailable; fail closed' }
$stdout = [IO.File]::ReadAllText((Join-Path $logRoot 'probe.stdout.log'))
if ($p.ExitCode -ne 0 -or $stdout -match '(?m)^FAIL\b' -or $stdout -notmatch '(?m)^RESULT api_and_pixels=PASS;' -or $stdout -notmatch '(?m)^NORMAL_EXIT cleanup completed') {
    throw "R4 probe failed (exit=$($p.ExitCode)); logs: $logRoot"
}
$umd = [IO.File]::ReadAllText((Join-Path $logRoot 'umd.log'))
$entry = [regex]::Matches($umd, 'LIFE_FLUSH entry[^\r\n]*session=(\d+) token=(\d+) gen=(\d+)')
$commit = [regex]::Matches($umd, 'LIFE_FLUSH commit[^\r\n]*old_session=(\d+) new_session=(\d+) old_token=(\d+) new_token=(\d+) old_gen=(\d+) new_gen=(\d+)')
$resume = [regex]::Matches($umd, 'LIFE_FLUSH resume[^\r\n]*gen=(\d+)')
if ($entry.Count -ne 1 -or $commit.Count -ne 1 -or $resume.Count -ne 1) { throw 'Expected exactly one LIFE_FLUSH entry/commit/resume transaction' }
$c = $commit[0].Groups
if ($c[3].Value -eq $c[4].Value -or [UInt64]$c[6].Value -ne ([UInt64]$c[5].Value + 1)) { throw 'Token/generation did not advance exactly once' }
if ($resume[0].Groups[1].Value -ne $c[6].Value) { throw 'Resume generation does not match committed generation' }
if ($stdout -notmatch '(?m)^READBACK after-blue\.nv12 .*YUVmean=41\.000,240\.000,110\.000 bad=0 written=1') { throw 'Blue NV12 pixel signature missing' }
Write-Output "PASS entry/commit/resume token+generation; probe_exit=$($p.ExitCode); logs=$logRoot"
