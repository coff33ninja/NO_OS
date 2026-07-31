param(
    [ValidateSet('build', 'run', 'test', 'clean', 'rebuild')]
    [string]$Action = 'build'
)

$ErrorActionPreference = 'Stop'

$root       = Split-Path -Parent $PSScriptRoot
$buildDir   = Join-Path $root 'build'
$kernelDir  = Join-Path $root 'kernel'
$includeDir = Join-Path $kernelDir 'include'

$zig  = 'zig'
$nasm = 'nasm'

$qemu = 'C:\Program Files\qemu\qemu-system-x86_64.exe'
if (-not (Test-Path $qemu)) {
    $q = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
    if ($q) { $qemu = $q.Source } else { throw 'qemu-system-x86_64 not found' }
}

$kernelElf64 = Join-Path $buildDir 'kernel.elf64'
$kernelElf   = Join-Path $buildDir 'kernel.elf'

$cflags = @(
    '-target', 'x86_64-freestanding'
    '-ffreestanding'
    '-nostdlib'
    '-fno-stack-protector'
    '-fno-builtin'
    '-fno-pie'
    '-fno-pic'
    '-mno-red-zone'
    '-mgeneral-regs-only'
    '-fno-asynchronous-unwind-tables'
    '-g0'
    '-O2'
    '-Wall'
    '-Wextra'
)

# Relative C sources (kernel/ is the base); object name derived from path.
$csrcs = @(
    'kern\kernel.c'
    'drivers\vga.c'
    'drivers\serial.c'
)

function Invoke-Build {
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

    # boot.s -> boot.o
    & $nasm -f elf64 (Join-Path $kernelDir 'arch\x86_64\boot.s') -o (Join-Path $buildDir 'boot.o')
    if ($LASTEXITCODE -ne 0) { throw 'nasm failed' }

    # C sources -> objects
    $objs = @((Join-Path $buildDir 'boot.o'))
    foreach ($s in $csrcs) {
        $obj = Join-Path $buildDir (($s -replace '[\\/]', '_') -replace '\.c$', '.o')
        $objs += $obj
        Write-Host "cc $s"
        & $zig cc @cflags "-I$includeDir" -c (Join-Path $kernelDir $s) -o $obj
        if ($LASTEXITCODE -ne 0) { throw "zig cc failed on $s" }
    }

    # link as 64-bit ELF (correct relocations), then reframe as 32-bit ELF.
    # QEMU's multiboot loader refuses ELFCLASS64 kernels; the 32-bit container
    # keeps the 64-bit code byte-identical. GRUB can use either one.
    $ldPath = (Join-Path $kernelDir 'linker.ld').Replace('\', '/')
    & $zig cc @('-target', 'x86_64-freestanding', '-nostdlib', "-Wl,-T,$ldPath", '-Wl,--gc-sections') @objs -o $kernelElf64
    if ($LASTEXITCODE -ne 0) { throw 'link failed' }

    $oc = Get-Command objcopy -ErrorAction SilentlyContinue
    if (-not $oc) { throw 'objcopy not found (need binutils)' }
    & $oc.Source -O elf32-i386 $kernelElf64 $kernelElf
    if ($LASTEXITCODE -ne 0) { throw 'objcopy reframe failed' }

    Write-Host "built $kernelElf (64-bit: $kernelElf64)"
}

function Invoke-Run {
    & $qemu -kernel $kernelElf -m 64M -serial stdio -no-reboot
}

function Invoke-Test {
    $log = Join-Path $buildDir 'boot-test.log'
    Remove-Item -Force $log -ErrorAction SilentlyContinue

    $p = Start-Process -FilePath $qemu -ArgumentList @(
        '-kernel', $kernelElf, '-m', '64M',
        '-display', 'none', '-serial', 'stdio', '-no-reboot'
    ) -NoNewWindow -RedirectStandardOutput $log -PassThru

    Start-Sleep -Seconds 4
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }

    $content = Get-Content -Raw $log -ErrorAction SilentlyContinue
    Write-Host $content
    if ($content -match 'NO_OS v0\.1 booted\.') {
        Write-Host 'TEST PASS: banner found in serial output'
        exit 0
    }
    Write-Host 'TEST FAIL: banner not found'
    exit 1
}

function Invoke-Clean {
    Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
    Write-Host 'cleaned'
}

switch ($Action) {
    'build'   { Invoke-Build }
    'rebuild' { Invoke-Clean; Invoke-Build }
    'run'     { Invoke-Run }
    'test'    { Invoke-Build; Invoke-Test }
    'clean'   { Invoke-Clean }
}
