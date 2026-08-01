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
$diskImg     = Join-Path $buildDir 'disk.img'

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
    'noc\predict.c'
    'noc\interact.c'
    'noc\train.c'
    'mm\pmm.c'
    'mm\heap.c'
    'mm\vmm.c'
    'mm\pgpred.c'
    'fs\noosfs.c'
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
    'drivers\ide.c'
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

# Raw IDE disk image for M4. 32 MiB, zero-filled. Recreated by -Action rebuild
# or when missing so a fresh FS can be formatted at boot.
function Ensure-Disk {
    param([int]$MiB = 32)
    if (Test-Path $diskImg) { return }
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
    $fs = [System.IO.File]::Open($diskImg, [System.IO.FileMode]::CreateNew)
    try {
        $fs.SetLength([int64]$MiB * 1MB)
    } finally {
        $fs.Dispose()
    }
    Write-Host "created $diskImg ($MiB MiB)"
}

function Invoke-Build {
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null    # .s files -> objects (boot.s, isr.s, coro.s, ...)
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
    Ensure-Disk
    & $qemu -kernel $kernelElf -m 64M -serial stdio -no-reboot `
        -drive file=$diskImg,format=raw,if=ide
}

function Invoke-Test {
    $log = Join-Path $buildDir 'boot-test.log'
    Remove-Item -Force $log -ErrorAction SilentlyContinue

    Ensure-Disk
    $monPort = 4444
    $p = Start-Process -FilePath $qemu -ArgumentList @(
        '-kernel', $kernelElf, '-m', '64M',
        '-display', 'none',
        '-drive', "file=$diskImg,format=raw,if=ide",
        '-serial', "file:$log",
        '-monitor', "tcp:127.0.0.1:$monPort,server,nowait"
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
                '.' { 'dot' }
                '/' { 'slash' }
                '\' { 'backslash' }
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
                '>' { 'shift-dot' }
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
        $sw.Write($command + "`n")
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

    # Wait for a pattern strictly in the portion of the log appended after a
    # captured baseline. Used after a system_reset to tell the second boot
    # apart from the first on the same serial log.
    function Wait-Appended {
        param([string]$baseline, [string]$pattern, [int]$timeoutSec = 10)
        $deadline = (Get-Date).AddSeconds($timeoutSec)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 200
            if ($p.HasExited) { break }
            $c = Get-Log
            if ($c -and $c.Length -gt $baseline.Length) {
                $seg = $c.Substring($baseline.Length)
                if ($seg -match $pattern) { return $true }
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
        # M4: IDE drive detected and LBA0 read back.
        if (-not (Wait-LogPattern 'disk test: ok, LBA0 sig=0x')) {
            $content = Get-Content -Raw $log -ErrorAction SilentlyContinue
            Write-Host $content
            throw 'IDE disk self-test did not pass'
        }
        # M4: filesystem formatted/mounted and a cluster round-trips.
        if (-not (Wait-LogPattern 'fs test: ok \(cluster \d+ alloc/write/read/free\)')) {
            $content = Get-Content -Raw $log -ErrorAction SilentlyContinue
            Write-Host $content
            throw 'filesystem cluster round-trip did not pass'
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

        # ---- M4: filesystem builtins (save/read/stat/list/delete) ----
        # Format first so a persistent disk image never carries stale files
        # between runs, keeping this section deterministic.
        Send-Keys "FormatDisk;`n"
        if (-not (Wait-LogPattern 'fs: formatted and remounted')) {
            throw 'FormatDisk did not report success'
        }
        Send-Keys "SaveFile(`"hello.txt`", `"Hello FS`");`n"
        if (-not (Wait-LogPattern 'fs: saved hello\.txt \(8 bytes\)')) {
            throw 'SaveFile did not report the saved size'
        }
        Send-Keys "StatFile(`"hello.txt`");`n"
        if (-not (Wait-LogPattern 'fs: hello\.txt inode=\d+ size=8 mode=')) {
            throw 'StatFile did not report the file metadata'
        }
        Send-Keys "ListDir;`n"
        if (-not (Wait-LogPattern 'hello\.txt\s+8 bytes')) {
            throw 'ListDir did not show hello.txt'
        }
        Send-Keys "Print(`"%s`", ReadFile(`"hello.txt`"));`n"
        if (-not (Wait-LogPattern "`nHello FS")) {
            throw 'ReadFile did not return the file content'
        }
        Send-Keys "DeleteFile(`"hello.txt`");`n"
        if (-not (Wait-LogPattern 'fs: deleted')) {
            throw 'DeleteFile did not report success'
        }
        Send-Keys "ListDir;`n"
        if (-not (Wait-LogPattern '\(0 entries\)')) {
            throw 'ListDir did not show an empty directory after delete'
        }

        # ---- M4: Run builtin executes a saved .noc script ----
        # Save a three-line script (a function definition, a call that uses it,
        # and a done marker) as a single file, then Run it. Function definitions
        # must persist across the per-line execution, so sq=81 proves lines
        # share the global function table.
        Send-Keys "SaveFile(`"prog.noc`", `"I64 Sq(I64 x) { return x*x; }\nPrintLn(\`"sq=%d\`", Sq(9));\nPrintLn(\`"SCRIPT-DONE\`");`");`n"
        if (-not (Wait-LogPattern 'fs: saved prog\.noc \(\d+ bytes\)')) {
            throw 'SaveFile did not report prog.noc size'
        }
        Send-Keys "Run(`"prog.noc`");`n"
        if (-not (Wait-LogPattern 'fs: run prog\.noc')) {
            throw 'Run did not report reading the script'
        }
        if (-not (Wait-LogPattern 'sq=81')) {
            throw 'Run: script function across lines did not evaluate'
        }
        if (-not (Wait-LogPattern 'SCRIPT-DONE')) {
            throw 'Run did not execute the saved script to its end'
        }
        Send-Keys "DeleteFile(`"prog.noc`");`n"
        if (-not (Wait-LogPattern 'fs: deleted')) {
            throw 'Run cleanup DeleteFile did not report success'
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
        if ($tail -notmatch '(?s)A.*B') {
            throw 'demo: no A-then-B interleaving observed'
        }
        if ($tail -notmatch '(?s)B.*A') {
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
            '(?m)^\d+\s+(ready|run|blocked|zombie)\s+(user|kern)')
        if ($tasks.Count -lt 3) {
            throw "Ps: expected 3+ tasks, saw $($tasks.Count)"
        }

        # ---- M4: filesystem persistence across reboot ----
        # Save a file, reset the machine with QEMU's system_reset, then verify
        # the second boot mounts the existing filesystem (no re-format) and the
        # file survives. Keeping the reboot in-process means the serial log is
        # appended, so phase-B assertions use the post-reset baseline.
        Send-Keys "SaveFile(`"hello.txt`", `"Hello FS`");`n"
        if (-not (Wait-LogCount 'fs: saved hello\.txt \(8 bytes\)' 2)) {
            throw 'persistence SaveFile did not report the saved size'
        }
        $base = Get-Log
        Send-Monitor 'system_reset'
        if (-not (Wait-Appended $base 'keyboard echo test' 30)) {
            throw 'guest did not reboot after system_reset'
        }
        Send-Keys "ok`n`n"
        if (-not (Wait-Appended $base 'boot-test-ok')) {
            throw 'phase-B keyboard echo did not yield boot-test-ok'
        }
        if (-not (Wait-Appended $base 'no/os> ')) {
            throw 'phase-B shell prompt did not appear'
        }
        if (-not (Wait-Appended $base 'fs: mounted')) {
            throw 'phase-B filesystem did not mount'
        }
        $after = Get-Log
        if ($after.Length -gt $base.Length) {
            $seg = $after.Substring($base.Length)
            if ($seg -match 'no filesystem found|format FAILED') {
                throw 'filesystem was re-formatted on reboot (data loss)'
            }
        }
        Send-Keys "ListDir;`n"
        if (-not (Wait-Appended $base 'hello\.txt\s+8 bytes')) {
            throw 'phase-B ListDir did not show the persisted file'
        }
        Send-Keys "Print(`"%s`", ReadFile(`"hello.txt`"));`n"
        if (-not (Wait-Appended $base "`nHello FS")) {
            throw 'phase-B ReadFile did not return persisted content'
        }
        # Reset the disk so future runs start from a known-empty filesystem.
        Send-Keys "FormatDisk;`n"
        if (-not (Wait-Appended $base 'fs: formatted and remounted')) {
            throw 'phase-B FormatDisk did not report success'
        }

        # ---- M5: next-command predictor (bigram over REPL history) ----
        # Seed a deterministic command sequence with valid expressions, then
        # Predict; must suggest the most frequent follower of the last command.
        # History (after ClearHist): 9*9; 7*7; 9*9; 7*7; 9*9;
        #   bigrams: 9*9;->7*7; x2, 7*7;->9*9; x2, last = 9*9; => predict 7*7;
        Send-Keys "ClearHist;`n"
        if (-not (Wait-LogPattern 'hist: cleared')) {
            throw 'ClearHist did not report success'
        }
        Send-Keys "9*9;`n"
        Send-Keys "7*7;`n"
        Send-Keys "9*9;`n"
        Send-Keys "7*7;`n"
        Send-Keys "9*9;`n"
        Send-Keys "Predict;`n"
        if (-not (Wait-LogPattern 'predict: 7\*7;')) {
            throw 'Predict did not suggest the most frequent follower'
        }

        # ---- M5: page-access predictor (prefetcher learning half) ----
        # Spawn user processes that deliberately fault on a known address
        # sequence. Each fault is recorded by the predictor before the process
        # is killed, so PgPred; must suggest the page that follows the last
        # fault. Fault stream: 0x400000 0x500000 0x400000 0x500000 0x400000
        #   bigrams: 0x400000->0x500000 x2, last = 0x400000 => pgpred 0x500000
        Send-Keys "Spawn(`"PageFault(0x400000);`");`n"
        if (-not (Wait-LogPattern 'killed: Page Fault')) {
            throw 'page-fault proc 1 did not fault and die'
        }
        Send-Keys "Spawn(`"PageFault(0x500000);`");`n"
        if (-not (Wait-LogPattern 'killed: Page Fault')) {
            throw 'page-fault proc 2 did not fault and die'
        }
        Send-Keys "Spawn(`"PageFault(0x400000);`");`n"
        if (-not (Wait-LogPattern 'killed: Page Fault')) {
            throw 'page-fault proc 3 did not fault and die'
        }
        Send-Keys "Spawn(`"PageFault(0x500000);`");`n"
        if (-not (Wait-LogPattern 'killed: Page Fault')) {
            throw 'page-fault proc 4 did not fault and die'
        }
        Send-Keys "Spawn(`"PageFault(0x400000);`");`n"
        if (-not (Wait-LogPattern 'killed: Page Fault')) {
            throw 'page-fault proc 5 did not fault and die'
        }
        Send-Keys "PgPred;`n"
        if (-not (Wait-LogPattern 'pgpred: 0x500000')) {
            throw 'PgPred did not predict the next page from fault history'
        }

        # ---- M5: model_budget syscall (per-process weight RAM budget) ----
        # The REPL task defaults to 8192 KB; setting 8 KB and committing 2
        # weight pages (8 KB) must fit and show in the accounting. A spawned
        # user process then proves enforcement is per-process: with a 16 KB
        # budget, committing 3 pages (12 KB) fits (a=0) and 5 more (20 KB) is
        # rejected (b=-1), which is how the model gracefully degrades.
        Send-Keys "ModelInfo;`n"
        if (-not (Wait-LogPattern 'model: budget=8192 KB used=0 KB')) {
            throw 'ModelInfo did not show the default 8192 KB budget'
        }
        Send-Keys "ModelBudget(8);`n"
        Send-Keys "ModelInfo;`n"
        if (-not (Wait-LogPattern 'model: budget=8 KB used=0 KB')) {
            throw 'ModelBudget did not lower the budget to 8 KB'
        }
        Send-Keys "ModelCommit(2);`n"
        Send-Keys "ModelInfo;`n"
        if (-not (Wait-LogPattern 'model: budget=8 KB used=8 KB')) {
            throw 'ModelCommit(2) did not account 8 KB of weight pages'
        }
        Send-Keys "Spawn(`"ModelBudget(16); PrintLn(\`"a=%d\`", ModelCommit(3)); PrintLn(\`"b=%d\`", ModelCommit(5));`");`n"
        if (-not (Wait-LogPattern 'a=0')) {
            throw 'user process commit under budget was not allowed'
        }
        if (-not (Wait-LogPattern 'b=-1')) {
            throw 'user process commit over budget was not rejected'
        }

        # ---- M5: interaction log (persistent training corpus) ----
        # Every REPL command is captured as [CMD] + [OUT]/[ERR] records in a
        # 64 KiB in-memory ring. LogDump proves the record layout; LogSave
        # persists a 4 KiB checkpoint (the FS file cap is 5 KiB) so a reboot
        # restores the corpus. The marker command's records must survive the
        # reset and show up in a post-boot LogDump.
        Send-Keys "LogInfo;`n"
        if (-not (Wait-LogPattern 'log: [0-9]+ records, [0-9]+ bytes of 65536')) {
            throw 'LogInfo did not report interaction log stats'
        }
        Send-Keys "PrintLn(`"IL-PERSIST`");`n"
        if (-not (Wait-LogPattern 'IL-PERSIST')) {
            throw 'interaction marker PrintLn output did not appear'
        }
        Send-Keys "LogDump;`n"
        if (-not (Wait-LogPattern '\[CMD\] PrintLn\("IL-PERSIST"\);')) {
            throw 'LogDump did not include the marker command record'
        }
        if (-not (Wait-LogPattern '\[OUT\] IL-PERSIST')) {
            throw 'LogDump did not include the marker output record'
        }
        if (-not (Wait-LogPattern '\[SPAWN\] 6 user')) {
            throw 'LogDump did not include the spawn event record'
        }
        if (-not (Wait-LogPattern '\[EXIT\] 6 0')) {
            throw 'LogDump did not include the exit event record'
        }
        Send-Keys "LogSave;`n"
        if (-not (Wait-LogPattern 'log: saved interact\.log \(\d+ bytes\)')) {
            throw 'LogSave did not persist the interaction log'
        }
        $base2 = Get-Log
        Send-Monitor 'system_reset'
        if (-not (Wait-Appended $base2 'keyboard echo test' 30)) {
            throw 'guest did not reboot after second system_reset'
        }
        Send-Keys "ok`n`n"
        if (-not (Wait-Appended $base2 'boot-test-ok')) {
            throw 'phase-C keyboard echo did not yield boot-test-ok'
        }
        if (-not (Wait-Appended $base2 'log: restored')) {
            throw 'guest did not restore the interaction log at boot'
        }
        if (-not (Wait-Appended $base2 'no/os> ')) {
            throw 'phase-C shell prompt did not appear'
        }
        Send-Keys "LogDump;`n"
        if (-not (Wait-Appended $base2 '\[CMD\] PrintLn\("IL-PERSIST"\);')) {
            throw 'persisted interaction log did not survive reboot'
        }
        if (-not (Wait-Appended $base2 '\[EXIT\] 6 0')) {
            throw 'persisted event records did not survive reboot'
        }

        # ---- M5: idle-retrain loop (byte-bigram model) ----
        # Train; forces a pass over the interaction log and reports the
        # integer fixed-point cross-entropy loss. The model accumulates
        # counts, so consecutive passes over an already-seen corpus reach a
        # deterministic fixed point (loss is invariant under count doubling)
        # rather than strictly decreasing -- assert stability/determinism
        # instead. TrainIdle lowers the idle trigger threshold so the harness
        # can prove the auto-retrain fires without waiting 30 s.
        $baseT = Get-Log
        Send-Keys "Train;`n"
        if (-not (Wait-Appended $baseT 'train: ok \(\d+ passes, \d+ bytes, loss=\d+\.\d+ bits/byte\)')) {
            throw 'Train did not run and report a loss'
        }
        $c1 = Get-Log
        $m1 = [regex]::Match($c1.Substring($baseT.Length), 'loss=(\d+\.\d+)')
        if (-not $m1.Success) { throw 'could not read the first training loss' }
        $loss1 = [double]$m1.Groups[1].Value

        $baseT = Get-Log
        Send-Keys "Train;`n"
        if (-not (Wait-Appended $baseT 'train: ok \(\d+ passes, \d+ bytes, loss=\d+\.\d+ bits/byte\)')) {
            throw 'second Train did not run'
        }
        $c2 = Get-Log
        $m2 = [regex]::Match($c2.Substring($baseT.Length), 'loss=(\d+\.\d+)')
        if (-not $m2.Success) { throw 'could not read the second training loss' }
        $loss2 = [double]$m2.Groups[1].Value
        if ([math]::Abs($loss2 - $loss1) -gt 0.15) {
            throw "training loss was not stable across identical retrains ($loss1 -> $loss2)"
        }

        Send-Keys "ModelInfo;`n"
        if (-not (Wait-LogPattern 'model: trained [0-9]+ passes, [0-9]+ bytes, loss=\d+\.\d+ bits/byte, idle=\d+ s')) {
            throw 'ModelInfo did not report byte-model stats'
        }

        $baseT = Get-Log
        Send-Keys "TrainIdle(1);`n"
        if (-not (Wait-Appended $baseT 'train: idle threshold 1 s')) {
            throw 'TrainIdle did not set the idle threshold'
        }
        $baseI = Get-Log
        Start-Sleep -Seconds 2   # sit idle past the 1 s threshold
        if (-not (Wait-Appended $baseI 'train: idle \(\d+ passes, \d+ bytes, loss=\d+\.\d+ bits/byte\)')) {
            throw 'idle retrain did not fire after the idle threshold elapsed'
        }

        # Deliberate fault as the final check: #UD must trap, not triple-fault.
        Send-Keys "FaultTest`n"
        if (-not (Wait-LogPattern 'EXCEPTION: Invalid Opcode')) {
            throw 'deliberate fault was not trapped'
        }

        $content = Get-Content -Raw $log -ErrorAction SilentlyContinue
        Write-Host $content
        Write-Host 'TEST PASS: boot self-test, NOC shell (bare commands, history, ctrl-c, interrupt), filesystem persistence across reboot, interaction log persistence, idle byte-model retraining (deterministic fixed point; auto-trigger fires), and fault trapping all verified'
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
