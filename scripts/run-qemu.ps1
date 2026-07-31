param(
    [switch]$Headless
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$kernelElf = Join-Path $root 'build\kernel.elf'

if (-not (Test-Path $kernelElf)) {
    Write-Host 'kernel.elf not found; building first...'
    & (Join-Path $PSScriptRoot 'build.ps1') -Action build
}

$qemu = 'C:\Program Files\qemu\qemu-system-x86_64.exe'
if (-not (Test-Path $qemu)) {
    $q = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
    if ($q) { $qemu = $q.Source } else { throw 'qemu-system-x86_64 not found' }
}

$args = @('-kernel', $kernelElf, '-m', '64M', '-serial', 'stdio', '-no-reboot')
if ($Headless) {
    $args += @('-display', 'none')
}

& $qemu @args
