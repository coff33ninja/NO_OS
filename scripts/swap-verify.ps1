param([string]$monPort = '4455')
$ErrorActionPreference = 'Stop'
$qemu = 'C:\Program Files\qemu\qemu-system-x86_64.exe'
$disk = 'E:\SCRIPTS\Servers\NO_OS\build\disk.img'
$kern = 'E:\SCRIPTS\Servers\NO_OS\build\kernel.elf'
$log = 'C:\Users\DRAGOHN\AppData\Local\Temp\opencode\swap-verify.log'
Remove-Item -Force $log -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $qemu -ArgumentList @('-kernel',$kern,'-m','64M','-display','none','-drive',"file=$disk,format=raw,if=ide",'-serial',"file:$log",'-monitor',"tcp:127.0.0.1:$monPort,server,nowait") -PassThru

function Get-Log2 { Get-Content -Raw $log -ErrorAction SilentlyContinue }
function Wait-Log2([string]$pat,[int]$sec=25) {
    $dl=(Get-Date).AddSeconds($sec)
    while((Get-Date)-lt$dl){ Start-Sleep -Milliseconds 200; $c=Get-Log2; if($c -and $c -match $pat){ return $true }; if($p.HasExited){ return $false } }
    return $false
}

# persistent monitor connection
$client = [System.Net.Sockets.TcpClient]::new('127.0.0.1',[int]$monPort)
$stream = $client.GetStream()
$sw = [System.IO.StreamWriter]::new($stream); $sw.AutoFlush = $true

function Send-Key([string]$n){ $sw.Write("sendkey $n`n"); Start-Sleep -Milliseconds 110 }
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
            default {
                $c = $ch.ToString()
                if ($c -cmatch '[A-Z]') { "shift-$($c.ToLower())" } else { $c }
            }
        }
        Send-Key $name
    }
}

try {
    if (-not (Wait-Log2 'keyboard echo test')) { throw 'no boot prompt' }
    Send-Text "ok`n`n"
    if (-not (Wait-Log2 'no/os> ')) { throw 'no REPL prompt' }
    if (-not (Wait-Log2 'noc-self-test-done')) { throw 'noc self-test did not finish' }

    $script = 'Spawn("p=Alloc(64);MemSet(p,65,63);MemSet(p+63,0,1);r=SwapOut(p);SwapInfo();v=Len(p);PrintLn(v);SwapInfo();if(r==0)PrintLn(\"SWAP-OK\")else PrintLn(\"SWAP-FAIL\");");'
    Send-Text $script + "`n"

    $checks = @(
        @('swap: 2048 slots \(8192 KB\) at LBA', 'boot swap init'),
        @('swap: 1/2048 slots, [0-9]+ out, [0-9]+ in, [0-9]+ reclaim', 'SwapOut evicted the page'),
        @("`n63`n", 'swap-in restored exact bytes (Len=63)'),
        @('swap: 0/2048 slots, [0-9]+ out, [0-9]+ in, [0-9]+ reclaim', 'swap-in freed the slot'),
        @("`nSWAP-OK`n", 'SwapOut returned 0')
    )
    $pass = $true
    foreach ($chk in $checks) {
        $ok = Wait-Log2 $chk[0]
        "{0,-40} {1}" -f $chk[1], $(if($ok){'PASS'}else{'FAIL'})
        if (-not $ok) { $pass = $false }
    }
    "RESULT: $(if($pass){'PASS'}else{'FAIL'})"
} catch {
    "ERROR: $_"
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
