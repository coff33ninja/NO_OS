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
    'kern\format.c'
    'kern\string.c'
    'kern\line.c'
    'kern\sched.c'
    'kern\noc_os.c'
    'noc\lexer.c'
    'noc\parser.c'
    'noc\compiler.c'
    'noc\vm.c'
    'noc\repl.c'
    'noc\exec.c'
    'mm\pmm.c'
    'mm\heap.c'
    'mm\vmm.c'
    'arch\x86_64\gdt.c'
    'arch\x86_64\tss.c'
    'arch\x86_64\idt.c'
    'arch\x86_64\isr.c'
    'arch\x86_64\syscall.c'
    'drivers\vga.c'
    'drivers\serial.c'
    'drivers\pic.c'
    'drivers\pit.c'
    'drivers\kbd.c'
)

# Shared NOC sources compiled into the ring-3 runtime (with -DNOOS_USER).
$userCsrcs = @(
    'noc\lexer.c'
    'noc\parser.c'
    'noc\compiler.c'
    'noc\vm.c'
    'noc\exec.c'
    'kern\format.c'
    'kern\string.c'
)

function Invoke-Build {
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

    # .s files -> objects (boot.s, isr.s, coro.s, ...)
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

    # ---- ring-3 user runtime: nocproc.bin, embedded as nocproc_blob.o ----
    Write-Host 'build user runtime'
    & $nasm -f elf64 (Join-Path $root 'user\ucrt0.s') -o (Join-Path $buildDir 'ucrt0.o')
    if ($LASTEXITCODE -ne 0) { throw 'nasm failed on user\ucrt0.s' }

    $userObjs = @(Join-Path $buildDir 'ucrt0.o')
    # The user image links above 4 GiB (PML4[2]); only the large code
    # model emits 64-bit relocations that can reach it.
    $userCFlags = @($cflags) + @('-DNOOS_USER', '-mcmodel=large')
    foreach ($s in $userCsrcs) {
        $obj = Join-Path $buildDir ('user_' + (($s -replace '[\\/]', '_') -replace '\.c$', '.o'))
        $userObjs += $obj
        Write-Host "cc (user) $s"
        & $zig cc @userCFlags "-I$includeDir" -c (Join-Path $kernelDir $s) -o $obj
        if ($LASTEXITCODE -ne 0) { throw "zig cc failed on user $s" }
    }
    foreach ($s in @('user\noc_os.c', 'user\nocproc.c')) {
        $obj = Join-Path $buildDir (($s -replace '[\\/]', '_') -replace '\.c$', '.o')
        $userObjs += $obj
        Write-Host "cc (user) $s"
        & $zig cc @userCFlags "-I$includeDir" -c (Join-Path $root $s) -o $obj
        if ($LASTEXITCODE -ne 0) { throw "zig cc failed on $s" }
    }

    $userLd = (Join-Path $root 'user\user.ld').Replace('\', '/')
    & $zig cc @('-target', 'x86_64-freestanding', '-nostdlib', "-Wl,-T,$userLd", '-Wl,--gc-sections') @userObjs -o (Join-Path $buildDir 'user.elf')
    if ($LASTEXITCODE -ne 0) { throw 'user link failed' }

    $oc = Get-Command objcopy -ErrorAction SilentlyContinue
    if (-not $oc) { throw 'objcopy not found (need binutils)' }

    # Run objcopy from the build dir so the blob symbols are named after the
    # bare file (nocproc.bin -> _binary_nocproc_bin_start/_end).
    Push-Location $buildDir
    try {
        & $oc.Source -O binary user.elf nocproc.bin
        if ($LASTEXITCODE -ne 0) { throw 'user objcopy binary failed' }
        & $oc.Source -I binary -O elf64-x86-64 -B i386 nocproc.bin nocproc_blob.o
        if ($LASTEXITCODE -ne 0) { throw 'user blob objcopy failed' }
    } finally {
        Pop-Location
    }
    $objs += (Join-Path $buildDir 'nocproc_blob.o')

    # link as 64-bit ELF (correct relocations), then reframe as 32-bit ELF.
    # QEMU's multiboot loader refuses ELFCLASS64 kernels; the 32-bit container
    # keeps the 64-bit code byte-identical. GRUB can use either one.
    $ldPath = (Join-Path $kernelDir 'linker.ld').Replace('\', '/')
    & $zig cc @('-target', 'x86_64-freestanding', '-nostdlib', "-Wl,-T,$ldPath", '-Wl,--gc-sections') @objs -o $kernelElf64
    if ($LASTEXITCODE -ne 0) { throw 'link failed' }

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
        Start-Sleep -Milliseconds 300
        $client.Close()
    }

    function Send-Monitor {
        param([string]$command)
        $client = [System.Net.Sockets.TcpClient]::new('127.0.0.1', $monPort)
        $stream = $client.GetStream()
        $sw = [System.IO.StreamWriter]::new($stream)
        $sw.AutoFlush = $true
        $sw.Write($command)
        # Let QEMU consume the command before closing; closing with unread
        # monitor data (the greeting) can send a TCP RST that drops it.
        Start-Sleep -Milliseconds 300
        $client.Close()
    }

    # Send QEMU monitor sendkey commands by key name, spaced out like
    # Send-Keys. Bursting multiple sendkeys on one connection drops keys;
    # spacing them (and settling before close) makes delivery reliable.
    function Send-KeySeq {
        param([string[]]$keys)
        $client = [System.Net.Sockets.TcpClient]::new('127.0.0.1', $monPort)
        $stream = $client.GetStream()
        $sw = [System.IO.StreamWriter]::new($stream)
        $sw.AutoFlush = $true
        foreach ($k in $keys) {
            $sw.Write("sendkey $k`n")
            Start-Sleep -Milliseconds 150
        }
        Start-Sleep -Milliseconds 300
        $client.Close()
    }

    function Get-Log {
        $c = Get-Content -Raw $log -ErrorAction SilentlyContinue
        if ($c) { $c = $c -replace "`r`n", "`n" }
        return $c
    }

    function Wait-LogCount {
        param([string]$pattern, [int]$want, [int]$timeoutSec = 10)
        $deadline = (Get-Date).AddSeconds($timeoutSec)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 200
            if ($p.HasExited) { break }
            $c = Get-Log
            if ($c) {
                $m = [regex]::Matches($c, $pattern)
                if ($m.Count -ge $want) { return $true }
            }
        }
        return $false
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

        # M2.5 shell: admin commands are pure NOC builtins via bare
        # identifiers with an optional trailing ';' -- no legacy fallback.
        Send-Keys "Version`n"
        if (-not (Wait-LogPattern 'kernel version: NO_OS v0.1')) {
            throw 'version output missing'
        }
        Send-Keys "MemInfo`n"
        if (-not (Wait-LogPattern 'mem: \d+ MiB free of \d+ MiB')) {
            throw 'meminfo output missing'
        }
        Send-Keys "Help`n"
        if (-not (Wait-LogPattern 'commands: Help, Version, MemInfo')) {
            throw 'Help output missing'
        }
        Send-Keys "bogus`n"
        if (-not (Wait-LogPattern "undeclared variable 'bogus'")) {
            throw 'unknown-command handling missing'
        }
        Send-Keys "Echo(`"hello`");`n"
        if (-not (Wait-LogPattern "`nhello`n")) {
            throw 'Echo output missing'
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

        # Line editor history: up-arrow recalls and re-runs the last command.
        Send-Keys "77*7;`n"
        if (-not (Wait-LogCount '\n539\n' 1)) {
            throw 'history base command result missing'
        }
        Send-KeySeq @('up', 'ret')
        if (-not (Wait-LogCount '\n539\n' 2)) {
            throw 'history up-arrow recall did not re-run the command'
        }

        # Line editor: Ctrl+C clears the current line back to a prompt.
        Send-Keys "abc"
        Send-KeySeq @('ctrl-c')
        if (-not (Wait-LogPattern '\^C')) {
            throw 'Ctrl+C line clear did not print ^C'
        }
        Send-Keys "11+11;`n"
        if (-not (Wait-LogPattern "`n22`n")) {
            throw 'shell did not recover after Ctrl+C'
        }

        # Interruptible VM: Esc aborts a runaway NOC program.
        Send-Keys "while(true){}`n"
        Start-Sleep -Milliseconds 500
        Send-KeySeq @('esc')
        if (-not (Wait-LogPattern 'NOC: interrupted')) {
            throw 'Esc did not interrupt the running NOC program'
        }
        Send-Keys "9*9;`n"
        if (-not (Wait-LogPattern "`n81`n")) {
            throw 'shell did not recover after interrupt'
        }

        # No legacy fallback noise anywhere in the session.
        $noise = Get-Log
        if ($noise -match "expected ';'" -or $noise -match 'unknown command') {
            throw 'legacy fallback noise still present in shell output'
        }

        # ---- M3: user mode & multitasking ----
        # Demo spawns two looping ring-3 processes that print A and B every
        # 500 ms; preemption must interleave them on the serial line.
        Send-Keys "Demo;`n"
        Start-Sleep -Seconds 5
        $c = Get-Log
        $idx = $c.IndexOf('Demo;')
        $tail = if ($idx -ge 0) { $c.Substring($idx) } else { $c }
        $aCount = ([regex]::Matches($tail, 'A')).Count
        $bCount = ([regex]::Matches($tail, 'B')).Count
        if ($aCount -lt 3 -or $bCount -lt 3) {
            throw "demo interleaving failed: A=$aCount B=$bCount"
        }
        if ($tail -notmatch 'A.*B') {
            throw 'demo: no A-then-B interleaving observed'
        }
        if ($tail -notmatch 'B.*A') {
            throw 'demo: no B-then-A interleaving observed'
        }

        # REPL stays responsive while the user processes run.
        Send-Keys "9*9;`n"
        if (-not (Wait-LogPattern "`n81`n")) {
            throw 'REPL did not stay responsive after spawning processes'
        }

        # Ps lists 3+ tasks (REPL + the two demo processes).
        Send-Keys "Ps;`n"
        Start-Sleep -Milliseconds 800
        $c = Get-Log
        $tasks = [regex]::Matches($c,
            'pid\s+\d+\s+(ready|run|blocked|zombie)\s+(user|kern)')
        if ($tasks.Count -lt 3) {
            throw "Ps: expected 3+ tasks, saw $($tasks.Count)"
        }

        # Deliberate fault as the final check: #UD must trap, not triple-fault.
        Send-Keys "FaultTest`n"
        if (-not (Wait-LogPattern 'EXCEPTION: Invalid Opcode')) {
            throw 'deliberate fault was not trapped'
        }

        $content = Get-Content -Raw $log -ErrorAction SilentlyContinue
        Write-Host $content
        Write-Host 'TEST PASS: boot self-test, NOC shell (bare commands, history, ctrl-c, interrupt), and fault trapping all verified'
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
