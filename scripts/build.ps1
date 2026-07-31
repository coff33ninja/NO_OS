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
    '-mno-sse'
    '-mno-sse2'
    '-mno-mmx'
    '-fno-asynchronous-unwind-tables'
    '-g0'
    '-O2'
    '-Wall'
    '-Wextra'
)

# Relative C sources (kernel/ is the base); object name derived from path.
$csrcs = @(
    'kern\kernel.c'
    'kern\printk.c'
    'kern\string.c'
    'kern\prompt.c'
    'kern\line.c'
    'noc\lexer.c'
    'noc\parser.c'
    'noc\compiler.c'
    'noc\vm.c'
    'noc\repl.c'
    'mm\pmm.c'
    'mm\heap.c'
    'arch\x86_64\gdt.c'
    'arch\x86_64\idt.c'
    'arch\x86_64\isr.c'
    'drivers\vga.c'
    'drivers\serial.c'
    'drivers\pic.c'
    'drivers\pit.c'
    'drivers\kbd.c'
)

function Invoke-Build {
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

    # .s files -> objects (boot.s, isr.s, ...)
    $objs = @()
    Get-ChildItem (Join-Path $kernelDir 'arch\x86_64') -Filter '*.s' | Sort-Object Name | ForEach-Object {
        $o = Join-Path $buildDir ($_.BaseName + '.o')
        $objs += $o
        Write-Host "asm $($_.Name)"
        & $nasm -f elf64 $_.FullName -o $o
        if ($LASTEXITCODE -ne 0) { throw "nasm failed on $($_.Name)" }
    }

    # C sources -> objects
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

    $monPort = 4444
    $p = Start-Process -FilePath $qemu -ArgumentList @(
        '-kernel', $kernelElf, '-m', '64M',
        '-display', 'none',
        '-serial', "file:$log",
        '-monitor', "tcp:127.0.0.1:$monPort,server,nowait",
        '-no-reboot'
    ) -PassThru

    function Send-Keys {
        param([string]$text)
        $client = [System.Net.Sockets.TcpClient]::new('127.0.0.1', $monPort)
        $stream = $client.GetStream()
        $sw = [System.IO.StreamWriter]::new($stream)
        $sw.AutoFlush = $true
        foreach ($ch in $text.ToCharArray()) {
            $name = switch -Exact ($ch.ToString()) {
                ' ' { 'spc' }
                "`n" { 'ret' }
                '+' { 'shift-equal' }
                '-' { 'minus' }
                ';' { 'semicolon' }
                ',' { 'comma' }
                '.' { 'period' }
                '/' { 'slash' }
                '(' { 'shift-9' }
                ')' { 'shift-0' }
                '*' { 'shift-8' }
                '"' { 'shift-apostrophe' }
                '%' { 'shift-5' }
                '=' { 'equal' }
                '{' { 'shift-bracket_left' }
                '}' { 'shift-bracket_right' }
                '[' { 'bracket_left' }
                ']' { 'bracket_right' }
                '<' { 'shift-comma' }
                '>' { 'shift-period' }
                '!' { 'shift-1' }
                '?' { 'shift-slash' }
                '_' { 'shift-minus' }
                ':' { 'shift-semicolon' }
                '&' { 'shift-7' }
                '|' { 'shift-backslash' }
                '^' { 'shift-6' }
                '~' { 'shift-grave_accent' }
                '#' { 'shift-3' }
                '@' { 'shift-2' }
                '$' { 'shift-4' }
                "'" { 'apostrophe' }
                default {
                    $c = $ch.ToString()
                    if ($c -cmatch '[A-Z]') { "shift-$($c.ToLower())" }
                    else { $c }
                }
            }
            $sw.Write("sendkey $name`n")
            Start-Sleep -Milliseconds 150
        }
        $client.Close()
    }

    function Wait-LogPattern {
        param([string]$pattern, [int]$timeoutSec = 10)
        $deadline = (Get-Date).AddSeconds($timeoutSec)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 200
            if ($p.HasExited) { break }
            $c = Get-Content -Raw $log -ErrorAction SilentlyContinue
            if ($c) {
                $c = $c -replace "`r`n", "`n"
                if ($c -match $pattern) { return $true }
            }
        }
        return $false
    }

    try {
        # Wait for the keyboard echo prompt and drive the boot self-test.
        if (-not (Wait-LogPattern 'keyboard echo test' 20)) {
            $content = Get-Content -Raw $log -ErrorAction SilentlyContinue
            Write-Host $content
            throw 'timeout waiting for keyboard echo prompt'
        }
        Send-Keys "ok`n`n"
        if (-not (Wait-LogPattern 'boot-test-ok')) {
            $content = Get-Content -Raw $log -ErrorAction SilentlyContinue
            Write-Host $content
            throw 'keyboard echo did not yield boot-test-ok'
        }

        # Drive the interactive prompt.
        if (-not (Wait-LogPattern 'no/os> ' 10)) {
            $content = Get-Content -Raw $log -ErrorAction SilentlyContinue
            Write-Host $content
            throw 'prompt did not appear'
        }
        Send-Keys "version`n"
        if (-not (Wait-LogPattern 'kernel version: NO_OS v0.1')) {
            throw 'version output missing'
        }
        Send-Keys "meminfo`n"
        if (-not (Wait-LogPattern 'mem: \d+ MiB free of \d+ MiB')) {
            throw 'meminfo output missing'
        }
        Send-Keys "help`n"
        if (-not (Wait-LogPattern 'commands: help, version, meminfo')) {
            throw 'help output missing'
        }
        Send-Keys "bogus`n"
        if (-not (Wait-LogPattern "unknown command 'bogus'")) {
            throw 'unknown-command handling missing'
        }
        Send-Keys "echo hello`n"
        if (-not (Wait-LogPattern "`nhello`n")) {
            throw 'echo output missing'
        }

        # NOC boot self-test (compiler + VM exercised at boot).
        if (-not (Wait-LogPattern 'noc-self-test-done')) {
            throw 'NOC self-test did not finish'
        }
        if (-not (Wait-LogPattern 'NOC hello')) {
            throw 'NOC self-test Print output missing'
        }
        if (-not (Wait-LogPattern "`n42`n")) {
            throw 'NOC self-test arithmetic/function result missing'
        }
        # Mul2 default arg, loop, if/else, while via the boot self-test.
        if (-not (Wait-LogPattern "`n12`n")) {
            throw 'NOC self-test Mul2 default-arg result missing'
        }
        if (-not (Wait-LogPattern "`n45`n")) {
            throw 'NOC self-test for-loop result missing'
        }
        if (-not (Wait-LogPattern "`n7`n")) {
            throw 'NOC self-test precedence result missing'
        }
        if (-not (Wait-LogPattern "`n100`n")) {
            throw 'NOC self-test if/else result missing'
        }
        if (-not (Wait-LogPattern "`n3`n")) {
            throw 'NOC self-test while-loop result missing'
        }

        # NOC via the real REPL: last-expression result printing.
        Send-Keys "5678+1;`n"
        if (-not (Wait-LogPattern "`n5679`n")) {
            throw 'NOC REPL did not print the last expression result'
        }

        # NOC via the real REPL: builtin call with computed argument.
        Send-Keys "Print(`"%d`", 33*11);`n"
        if (-not (Wait-LogPattern "`n363")) {
            throw 'NOC REPL Print builtin did not run'
        }

        # Deliberate fault as the final check: #UD must trap, not triple-fault.
        Send-Keys "fault`n"
        if (-not (Wait-LogPattern 'EXCEPTION: Invalid Opcode')) {
            throw 'deliberate fault was not trapped'
        }

        $content = Get-Content -Raw $log -ErrorAction SilentlyContinue
        Write-Host $content
        Write-Host 'TEST PASS: boot self-test, prompt, and fault trapping all verified'
        exit 0
    } finally {
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    }
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
