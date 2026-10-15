# pcmcp server: minimal MCP (streamable HTTP, JSON responses) over HttpListener. No dependencies.
# Endpoint: http://127.0.0.1:<Port>/mcp/<token>   (token is the only auth, keep the full URL secret)
param([int]$Port = 8787, [string]$Dir = "$env:ProgramData\pcmcp")
$ErrorActionPreference = 'Stop'
$token = (Get-Content (Join-Path $Dir 'token.txt') -Raw).Trim()
$logFile = Join-Path $Dir 'server.log'
$MAXOUT = 60000

function Log($m) { try { Add-Content -Path $logFile -Value ("{0} {1}" -f (Get-Date -Format s), $m) } catch {} }

function Send($ctx, [int]$code, $obj) {
    $r = $ctx.Response
    $r.StatusCode = $code
    if ($null -ne $obj) {
        $json = if ($obj -is [string]) { $obj } else { $obj | ConvertTo-Json -Depth 20 -Compress }
        $b = [Text.Encoding]::UTF8.GetBytes($json)
        $r.ContentType = 'application/json'
        $r.ContentLength64 = $b.Length
        $r.OutputStream.Write($b, 0, $b.Length)
    } else { $r.ContentLength64 = 0 }
    $r.Close()
}

function Tail([string]$s) {
    if ($s.Length -gt $MAXOUT) { return "[... " + ($s.Length - $MAXOUT) + " chars cut ...]`n" + $s.Substring($s.Length - $MAXOUT) }
    return $s
}

function RunPS([string]$cmd, [int]$timeout) {
    if ($timeout -lt 1) { $timeout = 60 }
    if ($timeout -gt 1800) { $timeout = 1800 }
    $base = Join-Path $env:TEMP ('mcp_' + [guid]::NewGuid().ToString('N'))
    Set-Content -Path "$base.ps1" -Value $cmd -Encoding UTF8
    $p = Start-Process -FilePath powershell.exe -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$base.ps1" `
        -RedirectStandardOutput "$base.out" -RedirectStandardError "$base.err" -WindowStyle Hidden -PassThru
    $timedOut = $false
    if (-not $p.WaitForExit($timeout * 1000)) {
        $timedOut = $true
        try { & taskkill.exe /F /T /PID $p.Id | Out-Null } catch {}
    }
    $out = ''; $err = ''
    if (Test-Path "$base.out") { $out = Get-Content "$base.out" -Raw -ErrorAction SilentlyContinue }
    if (Test-Path "$base.err") { $err = Get-Content "$base.err" -Raw -ErrorAction SilentlyContinue }
    $code = if ($timedOut) { 'TIMEOUT' } else { $p.ExitCode }
    Remove-Item "$base.ps1", "$base.out", "$base.err" -ErrorAction SilentlyContinue
    $text = "exit: $code`n--- stdout ---`n" + (Tail ([string]$out))
    if ($err) { $text += "`n--- stderr ---`n" + (Tail ([string]$err)) }
    return $text
}

$tools = @(
    @{ name = 'run_powershell'; description = 'Run a PowerShell script on the PC (elevated, Windows PowerShell 5.1). Returns exit code, stdout, stderr. For long scripts write a file first, then run it.';
       inputSchema = @{ type = 'object'; properties = @{ command = @{ type = 'string' }; timeout_sec = @{ type = 'integer'; description = 'default 60, max 1800' } }; required = @('command') } },
    @{ name = 'read_file'; description = 'Read a text file (UTF-8). Returns at most max_bytes from the end if tail=true, else from the start.';
       inputSchema = @{ type = 'object'; properties = @{ path = @{ type = 'string' }; max_bytes = @{ type = 'integer' }; tail = @{ type = 'boolean' } }; required = @('path') } },
    @{ name = 'write_file'; description = 'Write (or append to) a UTF-8 text file. Creates parent folders.';
       inputSchema = @{ type = 'object'; properties = @{ path = @{ type = 'string' }; content = @{ type = 'string' }; append = @{ type = 'boolean' } }; required = @('path', 'content') } },
    @{ name = 'list_dir'; description = 'List a directory: name, type, size, last write time.';
       inputSchema = @{ type = 'object'; properties = @{ path = @{ type = 'string' } }; required = @('path') } }
)

function CallTool([string]$name, $a) {
    switch ($name) {
        'run_powershell' { return RunPS ([string]$a.command) ([int]$a.timeout_sec) }
        'read_file' {
            $max = if ($a.max_bytes) { [int]$a.max_bytes } else { 60000 }
            $t = [IO.File]::ReadAllText([string]$a.path)
            if ($t.Length -le $max) { return $t }
            if ($a.tail) { return "[tail $max of $($t.Length) chars]`n" + $t.Substring($t.Length - $max) }
            return $t.Substring(0, $max) + "`n[... cut, file has $($t.Length) chars]"
        }
        'write_file' {
            $dir = Split-Path -Parent ([string]$a.path)
            if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
            if ($a.append) { Add-Content -Path ([string]$a.path) -Value ([string]$a.content) -Encoding UTF8 -NoNewline }
            else { [IO.File]::WriteAllText([string]$a.path, [string]$a.content, (New-Object Text.UTF8Encoding($false))) }
            return "ok: " + (Get-Item ([string]$a.path)).Length + " bytes"
        }
        'list_dir' {
            return (Get-ChildItem -Force ([string]$a.path) | ForEach-Object {
                "{0}`t{1}`t{2}`t{3:s}" -f $_.Name, $(if ($_.PSIsContainer) { 'dir' } else { 'file' }), $(if ($_.PSIsContainer) { '' } else { $_.Length }), $_.LastWriteTime
            }) -join "`n"
        }
        default { throw "unknown tool $name" }
    }
}

$listener = New-Object Net.HttpListener
$listener.Prefixes.Add("http://127.0.0.1:$Port/")
$listener.Start()
Log "listening on 127.0.0.1:$Port"

while ($listener.IsListening) {
    $ctx = $listener.GetContext()
    try {
        $path = $ctx.Request.Url.AbsolutePath
        if ($path -ne "/mcp/$token") { Send $ctx 404 $null; continue }
        $method = $ctx.Request.HttpMethod
        if ($method -eq 'DELETE') { Send $ctx 200 $null; continue }
        if ($method -ne 'POST') { Send $ctx 405 $null; continue }
        $sr = New-Object IO.StreamReader($ctx.Request.InputStream, [Text.Encoding]::UTF8)
        $body = $sr.ReadToEnd()
        $req = $body | ConvertFrom-Json
        $m = [string]$req.method
        $hasId = $null -ne $req.PSObject.Properties['id']
        if (-not $hasId) { Send $ctx 202 $null; continue }   # notification
        $resp = @{ jsonrpc = '2.0'; id = $req.id }
        try {
            switch ($m) {
                'initialize' {
                    $pv = if ($req.params.protocolVersion) { [string]$req.params.protocolVersion } else { '2025-03-26' }
                    $resp.result = @{ protocolVersion = $pv; capabilities = @{ tools = @{ listChanged = $false } }; serverInfo = @{ name = 'pcmcp'; version = '1.0' } }
                }
                'ping' { $resp.result = @{} }
                'tools/list' { $resp.result = @{ tools = $tools } }
                'tools/call' {
                    Log ("call " + $req.params.name)
                    try {
                        $txt = CallTool ([string]$req.params.name) $req.params.arguments
                        $resp.result = @{ content = @(@{ type = 'text'; text = [string]$txt }); isError = $false }
                    } catch {
                        $resp.result = @{ content = @(@{ type = 'text'; text = ("error: " + $_.Exception.Message) }); isError = $true }
                    }
                }
                default { $resp.error = @{ code = -32601; message = "method not found: $m" } }
            }
        } catch {
            $resp.error = @{ code = -32603; message = $_.Exception.Message }
        }
        Send $ctx 200 $resp
    } catch {
        Log ("request error: " + $_.Exception.Message)
        try { Send $ctx 500 $null } catch {}
    }
}
