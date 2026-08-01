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
    'noc\trans.c'
    'noc\corpus.c'
    'mm\pmm.c'
    'mm\heap.c'
    'mm\vmm.c'
    'mm\model.c'
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
        # GNU objcopy (binutils 2.46) mis-sizes flat output for the 1 TiB
        # user VMA: it pre-extends the file to the raw section address instead
        # of base-subtracting, so the write fails with ENOSPC. zig's LLVM
        # objcopy base-subtracts correctly.
        & zig objcopy -O binary user.elf nocproc.bin
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

        # ---- M5: generate from the trained byte-bigram model ----
        # TrainReset + LogClear build a clean controlled corpus of 5 copies of
        # one command; Train; then fits the model to exactly that text.
        # The [CMD]/[OUT] records live only in the interaction-log ring, so the
        # serial stream shows the bare PrintLn output ("xyz" on its own line),
        # never "[OUT] ..." -- wait on those bare output lines instead.
        # PredictBigram seeds greedy next-byte generation from "xyz". Each
        # command contributes exactly one "[TICK]" (C->K) and one "[CMD]"
        # (C->M) record, so on 'C' those counts tie and the lowest byte wins
        # ('K' < 'M'). The model must therefore emit '\n' after 'z' (xyz\n
        # from the [OUT] records, beating '"'), then '[CK] Prin("' -- proving
        # the weights genuinely learned the interaction log.
        $baseT = Get-Log
        Send-Keys "TrainReset;`n"
        if (-not (Wait-Appended $baseT 'model: reset')) {
            throw 'TrainReset did not run'
        }
        Send-Keys "LogClear;`n"
        if (-not (Wait-Appended $baseT 'log: cleared')) {
            throw 'LogClear did not run'
        }
        $baseC = Get-Log
        foreach ($i in 1..5) {
            Send-Keys "PrintLn(`"xyz`");`n"
            Start-Sleep -Milliseconds 250
        }
        if (-not (Wait-Appended $baseC '(?m)^xyz$')) {
            throw 'seed PrintLn commands did not run'
        }
        $nxyz = [regex]::Matches((Get-Log).Substring($baseC.Length), '(?m)^xyz$').Count
        if ($nxyz -lt 5) {
            throw "seed PrintLn outputs: expected 5, saw $nxyz (dropped keystrokes?)"
        }
        $baseT = Get-Log
        Send-Keys "Train;`n"
        if (-not (Wait-Appended $baseT 'train: ok \(\d+ passes, \d+ bytes, loss=\d+\.\d+ bits/byte\)')) {
            throw 'Train did not run on the controlled corpus'
        }
        $baseP = Get-Log
        Send-Keys "PredictBigram(`"xyz`");`n"
        if (-not (Wait-Appended $baseP 'pred: xyz\n\[CK\] Prin\("')) {
            throw 'PredictBigram did not generate the expected continuation'
        }

        # Slice 8: model-drafted NOC program. Reset the model, build a
        # controlled corpus of PrintLn("DRAFT-OK"); x5, then DraftRun with
        # the seed PrintLn("DRAFT-OK" (balanced string, closing quote already
        # in the seed) so generation only adds ');' -- the unambiguous
        # CMD-record tail "->) -> ; -> \n. Seeding past the closing quote
        # skips the K transition, where the [TICK] records' K->] (7) and the
        # [OUT] records' K->\n (5) would both out-vote the close-quote K->"
        # (5). The draft is syntax-checked, spawns as a ring-3 user process,
        # and its output reaches the shell as a bare DRAFT-OK line.
        $baseR = Get-Log
        Send-Keys "TrainReset;`n"
        if (-not (Wait-Appended $baseR 'model: reset')) {
            throw 'TrainReset did not reset the model'
        }
        $baseC = Get-Log
        Send-Keys "LogClear;`n"
        if (-not (Wait-Appended $baseC 'log: cleared')) {
            throw 'LogClear did not clear the interaction log'
        }
        $baseD = Get-Log
        foreach ($i in 1..5) {
            Send-Keys "PrintLn(`"DRAFT-OK`");`n"
            if (-not (Wait-Appended $baseD 'DRAFT-OK')) {
                throw "draft corpus: DRAFT-OK #$i did not echo"
            }
            $baseD = Get-Log
        }
        $baseT2 = Get-Log
        Send-Keys "Train;`n"
        if (-not (Wait-Appended $baseT2 'train: ok \(\d+ passes, \d+ bytes, loss=\d+\.\d+ bits/byte\)')) {
            throw 'Train did not run on the draft corpus'
        }
        $baseN = Get-Log
        Send-Keys "DraftRun(`"PrintLn(`\`"DRAFT-OK`\`"`");`n"
        if (-not (Wait-Appended $baseN 'draft: PrintLn\("DRAFT-OK"\);')) {
            throw 'DraftRun did not print the completed draft'
        }
        if (-not (Wait-Appended $baseN 'draft: spawned pid \d+')) {
            throw 'DraftRun did not spawn the draft as a user process'
        }
        if (-not (Wait-Appended $baseN '(?m)DRAFT-OK$')) {
            throw 'the spawned draft output did not reach the shell'
        }

        # Slice 9: versioned, rollback-safe generation corpus. Every
        # successful DraftRun commits the generated source to a versioned
        # corpus file (corp%04u.noc) with a metadata header, advances the
        # persisted corpus.seq counter, and refreshes last_known_good.noc.
        # A rejected draft writes nothing (failed experiments cannot corrupt
        # the corpus); CorpusRollback re-runs the last known good generation.
        # The slice-8 DraftRun already committed corp0001.noc, so this block
        # works relative to whatever seq is current.
        $baseK = Get-Log
        Send-Keys "CorpusInfo;`n"
        if (-not (Wait-Appended $baseK 'corpus: versions=\d+ next=(\d+) lkg=\w+')) {
            throw 'CorpusInfo did not report the generation corpus state'
        }
        $ci = (Get-Log).Substring($baseK.Length)
        $mCi = [regex]::Match($ci, 'next=(\d+)')
        if (-not $mCi.Success) { throw 'could not read the current corpus seq' }
        $seqNext = [int]$mCi.Groups[1].Value
        $corpFile = 'corp{0:d4}.noc' -f $seqNext

        # Fresh controlled corpus, then a successful draft of PrintLn("CORP-OK");.
        $baseR = Get-Log
        Send-Keys "TrainReset;`n"
        if (-not (Wait-Appended $baseR 'model: reset')) {
            throw 'TrainReset did not reset the model'
        }
        Send-Keys "LogClear;`n"
        if (-not (Wait-Appended $baseR 'log: cleared')) {
            throw 'LogClear did not clear the interaction log'
        }
        $baseC = Get-Log
        foreach ($i in 1..5) {
            Send-Keys "PrintLn(`"CORP-OK`");`n"
            if (-not (Wait-Appended $baseC 'CORP-OK')) {
                throw "corpus slice: CORP-OK #$i did not echo"
            }
            $baseC = Get-Log
        }
        $baseT2 = Get-Log
        Send-Keys "Train;`n"
        if (-not (Wait-Appended $baseT2 'train: ok \(\d+ passes, \d+ bytes, loss=\d+\.\d+ bits/byte\)')) {
            throw 'Train did not run on the corpus slice'
        }
        $baseN = Get-Log
        Send-Keys "DraftRun(`"PrintLn(`\`"CORP-OK`\`"`");`n"
        if (-not (Wait-Appended $baseN 'draft: PrintLn\("CORP-OK"\);')) {
            throw 'corpus DraftRun did not print the completed draft'
        }
        if (-not (Wait-Appended $baseN "corpus: saved $corpFile \(seq $seqNext\)")) {
            throw 'corpus DraftRun did not commit the versioned corpus file'
        }
        if (-not (Wait-Appended $baseN 'draft: spawned pid \d+')) {
            throw 'corpus DraftRun did not spawn the draft'
        }
        if (-not (Wait-Appended $baseN '(?m)CORP-OK$')) {
            throw 'corpus DraftRun output did not reach the shell'
        }

        # Versioning advanced: next seq bumped, the saved version is now the
        # last known good.
        $baseK = Get-Log
        Send-Keys "CorpusInfo;`n"
        if (-not (Wait-Appended $baseK "corpus: versions=$seqNext next=$($seqNext + 1) lkg=yes")) {
            throw 'CorpusInfo did not advance after the committed draft'
        }

        # A rejected draft must not touch the corpus (rollback-safe).
        $baseR = Get-Log
        Send-Keys "DraftRun(`"zzz`");`n"
        if (-not (Wait-Appended $baseR 'draft: rejected')) {
            throw 'corpus DraftRun did not reject the zzz draft'
        }
        $baseK = Get-Log
        Send-Keys "CorpusInfo;`n"
        if (-not (Wait-Appended $baseK "corpus: versions=$seqNext next=$($seqNext + 1) lkg=yes")) {
            throw 'a rejected draft changed the corpus (rollback-safety violated)'
        }

        # The versioned file holds the metadata header plus the draft source.
        $baseF = Get-Log
        Send-Keys "PrintLn(`"%s`", ReadFile(`"$corpFile`"));`n"
        if (-not (Wait-Appended $baseF '@@ GENERATED:')) {
            throw 'corpus file did not carry the GENERATED metadata header'
        }
        if (-not (Wait-Appended $baseF 'PrintLn\("CORP-OK"\);')) {
            throw 'corpus file did not carry the draft source'
        }

        # Rollback re-runs the last known good generation.
        $baseB = Get-Log
        Send-Keys "CorpusRollback;`n"
        if (-not (Wait-Appended $baseB 'corpus: rollback spawned pid \d+')) {
            throw 'CorpusRollback did not re-spawn the last known good generation'
        }
        if (-not (Wait-Appended $baseB '(?m)CORP-OK$')) {
            throw 'the rolled-back generation output did not reach the shell'
        }

        # ---- M5: demand-paged read-only weight pages (evictable) ----
        # Reset the model and train on a tiny "AAAA" corpus so the A->A
        # bigram weight (offset 0x4141) is deterministically nonzero while
        # other weights (e.g. byte 0) stay zero. A spawned process then reads
        # those bytes through the demand-paged mapping: the first access to
        # each page faults it in read-only (budget-charged), and Len() on the
        # copied byte proves the page holds the canonical weight bytes.
        Send-Keys "TrainReset;`n"
        Send-Keys "PrintLn(`"AAAA`");`n"
        Send-Keys "PrintLn(`"AAAA`");`n"
        Send-Keys "PrintLn(`"AAAA`");`n"
        Send-Keys "Train;`n"
        if (-not (Wait-LogPattern 'train: ok \(\d+ passes, \d+ bytes, loss=\d+\.\d+ bits/byte\)')) {
            throw 'slice-10 corpus training did not run'
        }

        $baseR = Get-Log
        Send-Keys "TransInfo;`n"
        if (-not (Wait-Appended $baseR 'trans: model L=\d+ ctx=\d+ d=\d+ ff=\d+ heads=\d+ pages=\d+ weights=\d+ KB')) {
            throw 'TransInfo did not report the transformer model'
        }
        # The model window serves the transformer blob (trans_weights), not
        # the legacy bigram weights. Its canonical bytes are deterministic
        # regardless of the PRNG: byte 0 is the header magic ('N'=0x4E), the
        # final-LN gamma is 1.0 (16, Q4.4) and its bias is 0. The offsets are
        # config-independent in D/L/F (the final-LN sits at TRANS_HDR+V*D+
        # L*LAYSA). A spawned process reads those bytes through the demand-
        # paged mapping: the first access to each page faults it in read-only
        # (budget-charged) and the read back proves the frame holds the
        # canonical weight bytes.
        Send-Keys "Spawn(`"Str S = Alloc(2); MemCpy(S, 0x100F1000000, 1); PrintLn(\`"magic=%d\`", Len(S)); MemCpy(S, 0x100F1000000+411680, 1); PrintLn(\`"gamma=%d\`", Len(S)); MemCpy(S, 0x100F1000000+411744, 1); PrintLn(\`"bias=%d\`", Len(S));`");`n"
        if (-not (Wait-Appended $baseR 'model: fault-in pg=0 resident=1 used=4 KB')) {
            throw 'reading a cold model page did not demand-page it in'
        }
        if (-not (Wait-Appended $baseR 'model: fault-in pg=100 resident=2 used=8 KB')) {
            throw 'reading a deep weight page did not demand-page it in'
        }
        if (-not (Wait-Appended $baseR 'magic=1')) {
            throw 'the transformer header magic did not read back through the window'
        }
        if (-not (Wait-Appended $baseR 'gamma=1')) {
            throw 'the final-LN gamma did not read back as 1.0 (16) through the mapping'
        }
        if (-not (Wait-Appended $baseR 'bias=0')) {
            throw 'the final-LN bias did not read back as zero through the mapping'
        }

        # A write to a resident (read-only) model page must fault and kill the
        # process: the fault is present+write so it is NOT a demand-page
        # request and is refused.
        $baseW = Get-Log
        Send-Keys "Spawn(`"MemCpy(Alloc(1), 0x100F1000000, 1); MemSet(0x100F1000000, 0x41, 1);`");`n"
        if (-not (Wait-Appended $baseW 'killed: Page Fault')) {
            throw 'write to a read-only weight page was not trapped'
        }
        if (-not (Wait-Appended $baseW 'cr2=0x100f1000000')) {
            throw 'write-kill did not report the model address'
        }

        # Budget pressure + eviction + refault: with a tiny budget the second
        # page cannot fault in until an earlier page is evicted. Eviction
        # refunds the budget; a later touch re-faults the page in again.
        $baseE = Get-Log
        Send-Keys "Spawn(`"ModelBudget(4); ModelTouch(0); ModelTouch(1); ModelEvict(0); ModelTouch(1); PrintLn(\`"PRESSURE-OK\`");`");`n"
        if (-not (Wait-Appended $baseE 'model: fault-in pg=0 resident=1 used=4 KB')) {
            throw 'budgeted touch 0 did not fault in'
        }
        if (-not (Wait-Appended $baseE 'model: pg 1 denied \(budget 4 KB\)')) {
            throw 'budget did not deny the second page under pressure'
        }
        if (-not (Wait-Appended $baseE 'model: evict pg=0 resident=0 used=0 KB')) {
            throw 'ModelEvict did not evict and refund the budget'
        }
        if (-not (Wait-Appended $baseE 'model: fault-in pg=1 resident=1 used=4 KB')) {
            throw 'eviction did not free budget for the needed page'
        }
        if (-not (Wait-Appended $baseE 'PRESSURE-OK')) {
            throw 'evict-under-pressure process did not complete'
        }

        # ---- M5: fixed-point transformer training / eval ----
        # TransReset deterministically re-randomizes the weights, TransTrain
        # runs full fixed-point backprop over the tail of the interaction log
        # and reports a loss, and TransEval reports in-corpus accuracy. After a
        # reset, re-training on the same log must reproduce the identical loss
        # (deterministic SGD), and TransPredict must still answer from the
        # trained weights.
        $baseTr = Get-Log
        Send-Keys "TransReset;`n"
        if (-not (Wait-Appended $baseTr 'trans: reset')) {
            throw 'TransReset did not report the transformer reset'
        }
        Send-Keys "LogClear;`n"
        if (-not (Wait-Appended $baseTr 'log: cleared')) {
            throw 'LogClear did not clear the interaction log'
        }
        $baseF = Get-Log
        foreach ($i in 1..8) {
            Send-Keys "PrintLn(`"ABABABAB`");`n"
            if (-not (Wait-Appended $baseF 'ABABABAB')) {
                throw "trans corpus line #$i did not echo"
            }
            $baseF = Get-Log
        }
        $baseT = Get-Log
        Send-Keys "TransTrain(`"harness`", 2);`n"
        if (-not (Wait-Appended $baseT 'trans: train harness \(2 passes, \d+ bytes, loss=\d+\.\d+ bits/byte\)' 30)) {
            throw 'TransTrain did not run and report a loss'
        }
        $mLoss = [regex]::Match((Get-Log).Substring($baseT.Length),
                                'loss=(\d+\.\d+) bits/byte')
        if (-not $mLoss.Success) { throw 'could not capture the training loss' }
        $loss1 = $mLoss.Groups[1].Value
        $baseE2 = Get-Log
        Send-Keys "TransEval;`n"
        if (-not (Wait-Appended $baseE2 'trans: eval \d+ bytes, acc=\d+%, loss=\d+\.\d+ bits/byte' 30)) {
            throw 'TransEval did not report accuracy'
        }
        # Determinism re-run: reset weights AND rebuild the exact same
        # interaction-log tail (the training window), so the fixed-point SGD
        # must reproduce the identical loss.
        $baseD = Get-Log
        Send-Keys "TransReset;`n"
        if (-not (Wait-Appended $baseD 'trans: reset')) {
            throw 'determinism TransReset did not report'
        }
        Send-Keys "LogClear;`n"
        if (-not (Wait-Appended $baseD 'log: cleared')) {
            throw 'determinism LogClear did not clear the interaction log'
        }
        $baseF2 = Get-Log
        foreach ($i in 1..8) {
            Send-Keys "PrintLn(`"ABABABAB`");`n"
            if (-not (Wait-Appended $baseF2 'ABABABAB')) {
                throw "determinism corpus line #$i did not echo"
            }
            $baseF2 = Get-Log
        }
        Send-Keys "TransTrain(`"harness`", 2);`n"
        if (-not (Wait-Appended $baseD 'trans: train harness \(2 passes, \d+ bytes, loss=\d+\.\d+ bits/byte\)' 30)) {
            throw 'determinism TransTrain did not run'
        }
        $mLoss2 = [regex]::Match((Get-Log).Substring($baseD.Length),
                                 'loss=(\d+\.\d+) bits/byte')
        if (-not $mLoss2.Success) { throw 'could not capture the second loss' }
        if ($mLoss2.Groups[1].Value -ne $loss1) {
            throw "trans training was not deterministic: loss $loss1 vs $($mLoss2.Groups[1].Value)"
        }
        $baseP = Get-Log
        Send-Keys "TransPredict(`"ABAB`");`n"
        if (-not (Wait-Appended $baseP 'trans: pred \d+ bytes: ')) {
            throw 'TransPredict did not answer after training'
        }

        # Deliberate fault as the final check: #UD must trap, not triple-fault.
        Send-Keys "FaultTest`n"
        if (-not (Wait-LogPattern 'EXCEPTION: Invalid Opcode')) {
            throw 'deliberate fault was not trapped'
        }

        $content = Get-Content -Raw $log -ErrorAction SilentlyContinue
        Write-Host $content
        Write-Host 'TEST PASS: boot self-test, NOC shell (bare commands, history, ctrl-c, interrupt), filesystem persistence across reboot, interaction log persistence, idle byte-model retraining (deterministic fixed point; auto-trigger fires), bigram generation from the trained model, model-drafted NOC program spawned as a ring-3 user process with output reaching the shell, demand-paged read-only transformer weight pages (fault-in, write-trap, budget eviction/refault), deterministic fixed-point transformer training/eval (TransTrain/TransEval/TransReset), and fault trapping all verified'
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
