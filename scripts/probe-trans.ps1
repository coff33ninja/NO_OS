param(
    [string]$kern = 'E:\SCRIPTS\Servers\NO_OS\build\kernel.elf',
    [string]$disk = 'E:\SCRIPTS\Servers\NO_OS\build\disk.img',
    [string]$out = 'C:\Users\DRAGOHN\AppData\Local\Temp\opencode\probe.log',
    [ValidateSet('sweep', 'timing')]
    [string]$Mode = 'sweep',
    [int]$Passes = 4,
    [int]$Rounds = 8
)
$qemu = 'C:\Program Files\qemu\qemu-system-x86_64.exe'
if (-not (Test-Path $qemu)) { $qemu = (Get-Command qemu-system-x86_64).Source }
Remove-Item -Force $out -ErrorAction SilentlyContinue
$monPort = 4555
$p = Start-Process -FilePath $qemu -ArgumentList @(
    '-kernel', $kern, '-m', '64M',
    '-display', 'none',
    '-drive', "file=$disk,format=raw,if=ide",
    '-serial', "file:$out",
    '-monitor', "tcp:127.0.0.1:$monPort,server,nowait"
) -PassThru
try {
    function Send-Keys([string]$text) {
        $client = [System.Net.Sockets.TcpClient]::new('127.0.0.1', $monPort)
        $stream = $client.GetStream()
        $sw = [System.IO.StreamWriter]::new($stream)
        $sw.AutoFlush = $true
        foreach ($ch in $text.ToCharArray()) {
            $name = switch -Exact ($ch.ToString()) {
                ' ' { 'spc' } "`n" { 'ret' } '+' { 'shift-equal' } '-' { 'minus' }
                ';' { 'semicolon' } ',' { 'comma' } '.' { 'dot' } '/' { 'slash' }
                '\' { 'backslash' } '(' { 'shift-9' } ')' { 'shift-0' } '*' { 'shift-8' }
                '"' { 'shift-apostrophe' } '%' { 'shift-5' } '=' { 'equal' }
                '{' { 'shift-bracket_left' } '}' { 'shift-bracket_right' }
                '[' { 'bracket_left' } ']' { 'bracket_right' } '<' { 'shift-comma' }
                '>' { 'shift-dot' } '!' { 'shift-1' } '?' { 'shift-slash' }
                '_' { 'shift-minus' } ':' { 'shift-semicolon' } '&' { 'shift-7' }
                '|' { 'shift-backslash' } '^' { 'shift-6' } '~' { 'shift-grave_accent' }
                '#' { 'shift-3' } '@' { 'shift-2' } '$' { 'shift-4' }
                "'" { 'apostrophe' }
                default {
                    $c = $ch.ToString()
                    if ($c -cmatch '[A-Z]') { "shift-$($c.ToLower())" } else { $c }
                }
            }
            $sw.Write("sendkey $name`n")
            Start-Sleep -Milliseconds 120
        }
        Start-Sleep -Milliseconds 200
        $client.Close()
    }
    function Get-Log {
        $c = Get-Content -Raw $out -ErrorAction SilentlyContinue
        if ($c) { $c = $c -replace "`r`n", "`n" }
        return $c
    }
    function Wait-New([string]$base, [string]$pattern, [int]$timeoutSec = 40) {
        $deadline = (Get-Date).AddSeconds($timeoutSec)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 200
            if ($p.HasExited) { break }
            $c = Get-Log
            if ($c.Length -gt $base.Length) {
                $seg = $c.Substring($base.Length)
                if ($seg -match $pattern) { return $true }
            }
        }
        return $false
    }

    $b = Get-Log
    if (-not (Wait-New $b 'keyboard echo test' 40)) { throw 'boot failed' }
    Send-Keys "ok`n"
    if (-not (Wait-New $b 'boot-test-ok' 30)) { throw 'echo test failed' }
    if (-not (Wait-New $b 'no/os> ' 15)) { throw 'no shell prompt' }
    Start-Sleep -Seconds 1

    Send-Keys "TrainReset;`n"; Start-Sleep -Seconds 1
    Send-Keys "TransReset;`n"; Start-Sleep -Seconds 1
    Send-Keys "LogClear;`n"; Start-Sleep -Seconds 1
    foreach ($i in 1..5) {
        Send-Keys "PrintLn(`"ABAB`");`n"
        Start-Sleep -Milliseconds 400
    }

    if ($Mode -eq 'sweep') {
        # Repeated capped passes: measure how loss falls as the same corpus
        # is trained repeatedly (establishes the fixed-point loss floor).
        for ($i = 1; $i -le $Rounds; $i++) {
            $before = Get-Log
            Send-Keys "TransTrain(`"probe$i`", $Passes);`n"
            if (-not (Wait-New $before 'trans: train probe')) { throw "train $i failed" }
            Start-Sleep -Milliseconds 300
        }
    } else {
        # One longer run, wall-clock timed (larger budgets -> bigger hardware).
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $before = Get-Log
        Send-Keys "TransTrain(`"mid`", $Passes);`n"
        if (-not (Wait-New $before 'trans: train mid' 300)) { throw 'train mid failed' }
        $sw.Stop()
        Write-Host ("TransTrain($Passes) took {0:n0}s" -f $sw.Elapsed.TotalSeconds)
        Start-Sleep -Milliseconds 300
    }

    $before = Get-Log
    Send-Keys "TransEval;`n"
    if (-not (Wait-New $before 'trans: eval')) { throw 'eval failed' }
    $before = Get-Log
    Send-Keys "TransPredict(`"PrintLn(`\`"ABAB`\`"`");`n"
    Start-Sleep -Milliseconds 500
    $before = Get-Log
    Send-Keys "TransPredict(`"ABAB`");`n"
    Start-Sleep -Seconds 1
    Write-Host '---- probe log tail ----'
    $c = Get-Log
    $c.Substring([Math]::Max(0, $c.Length - 3000))
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
