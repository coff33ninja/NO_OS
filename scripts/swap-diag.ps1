param([string]$monPort = '4455')
$ErrorActionPreference = 'Stop'
$qemu = 'C:\Program Files\qemu\qemu-system-x86_64.exe'
$disk = 'E:\SCRIPTS\Servers\NO_OS\build\disk.img'
$kern = 'E:\SCRIPTS\Servers\NO_OS\build\kernel.elf'
$log = 'C:\Users\DRAGOHN\AppData\Local\Temp\opencode\swap-diag.log'
Remove-Item -Force $log -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $qemu -ArgumentList @('-kernel',$kern,'-m','64M','-display','none','-drive',"file=$disk,format=raw,if=ide",'-serial',"file:$log",'-monitor',"tcp:127.0.0.1:$monPort,server,nowait") -PassThru
function Get-Log2 { Get-Content -Raw $log -ErrorAction SilentlyContinue }
function Wait-Log2([string]$pat,[int]$sec=20) {
    $dl=(Get-Date).AddSeconds($sec)
    while((Get-Date)-lt$dl){ Start-Sleep -Milliseconds 200; $c=Get-Log2; if($c -and $c -match $pat){ return $true }; if($p.HasExited){ return $false } }
    return $false
}
$client = [System.Net.Sockets.TcpClient]::new('127.0.0.1',[int]$monPort)
$stream = $client.GetStream()
$sw = [System.IO.StreamWriter]::new($stream); $sw.AutoFlush = $true
function Send-Key([string]$n){ $sw.Write("sendkey $n`n"); Start-Sleep -Milliseconds 100 }
function Send-Text([string]$text){
    foreach ($ch in $text.ToCharArray()) {
        $name = switch -Exact ($ch.ToString()) {
            ' ' { 'spc' }; "`n" { 'ret' }; '+' { 'shift-equal' }; '-' { 'minus' }
            ';' { 'semicolon' }; ',' { 'comma' }; '.' { 'dot' }; '/' { 'slash' }
            '\' { 'backslash' }; '(' { 'shift-9' }; ')' { 'shift-0' }; '*' { 'shift-8' }
            '"' { 'shift-apostrophe' }; '%' { 'shift-5' }; '=' { 'equal' }
            '{' { 'shift-bracket_left' }; '}' { 'shift-bracket_right' }
            '[' { 'bracket_left' }; ']' { 'bracket_right' }; '<' { 'shift-comma' }
            '>' { 'shift-dot' }; '!' { 'shift-1' }; '?' { 'shift-slash' }
            '_' { 'shift-minus' }; ':' { 'shift-semicolon' }; '&' { 'shift-7' }
            '|' { 'shift-backslash' }; '^' { 'shift-6' }; '~' { 'shift-grave_accent' }
            '#' { 'shift-3' }; '@' { 'shift-2' }; '$' { 'shift-4' }; "'" { 'apostrophe' }
            default { $c = $ch.ToString(); if ($c -cmatch '[A-Z]') { "shift-$($c.ToLower())" } else { $c } }
        }
        Send-Key $name
    }
}
try {
    if (-not (Wait-Log2 'keyboard echo test')) { throw 'no boot prompt' }
    Send-Text "ok`n`n"
    if (-not (Wait-Log2 'no/os> ')) { throw 'no REPL prompt' }

    # 1) simple command sanity
    Send-Text "PrintLn(`"ALPHA`");`n"
    "{0,-38} {1}" -f 'simple PrintLn', $(if(Wait-Log2 'ALPHA'){'PASS'}else{'FAIL'})

    # 2) Spawn a trivial script
    Send-Text 'Spawn("PrintLn(\"BETA\");");' + "`n"
    "{0,-38} {1}" -f 'Spawn trivial (BETA)', $(if(Wait-Log2 'BETA'){'PASS'}else{'FAIL'})

    # 3) process list
    Send-Text 'Ps;' + "`n"
    "{0,-38} {1}" -f 'Ps processed', $(if(Wait-Log2 'pid  state'){'PASS'}else{'FAIL'})

    # 4) Spawn the swap script
    $script = 'Spawn("Str p = Alloc(64); MemSet(p, 65, 63); MemSet(p+63, 0, 1); I64 r = SwapOut(p); SwapInfo(); I64 v = Len(p); PrintLn(\"%d\", v); SwapInfo(); if(r==0) PrintLn(\"SWAP-OK\"); else PrintLn(\"SWAP-FAIL\");");'
    Send-Text $script + "`n"
    Start-Sleep -Seconds 5
    $c = Get-Log2
    $c = $c -replace "`r`n", "`n"
    "{0,-38} {1}" -f 'swap round-trip (0 slots,1 out,1 in)', $(if($c -match 'swap: 0/2048 slots, [1-9][0-9]* out, [1-9][0-9]* in'){'PASS'}else{'FAIL'})
    "{0,-38} {1}" -f 'Len restored 63', $(if($c -match "`n63`n"){'PASS'}else{'FAIL'})
    "{0,-38} {1}" -f 'SWAP-OK (real output)', $(if($c -match '(?m)^SWAP-OK'){'PASS'}else{'FAIL'})

    # 5) REPL still alive after swap spawn?
    Send-Text "PrintLn(`"POST`");`n"
    "{0,-38} {1}" -f 'REPL alive (POST)', $(if(Wait-Log2 'POST'){'PASS'}else{'FAIL'})
    Start-Sleep -Milliseconds 500
    $c = Get-Log2
    "log tail:"
    (($c -split "`n") | Select-Object -Last 10) -join "`n"
} catch {
    "ERROR: $_"
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
