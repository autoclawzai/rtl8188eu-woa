# relay.ps1 - outbound-only command relay over GitHub (no tunnel / DNS / open ports).
# Polls branch "relay" (file relay/cmd.ps1) in this repo, runs HMAC-signed commands, writes AES-encrypted output
# to relay/out/<id>.enc. Needs C:\ProgramData\pcmcp\relay.cfg = {"pat":"<fine-grained token>","key":"<shared secret>"}.
$ErrorActionPreference = 'Continue'
$dir = 'C:\ProgramData\pcmcp'
$cfg = Get-Content "$dir\relay.cfg" -Raw | ConvertFrom-Json
$repo = 'autoclawzai/rtl8188eu-woa'; $br = 'relay'
$H = @{ Authorization = "Bearer $($cfg.pat)"; 'User-Agent' = 'pcmcp-relay'; Accept = 'application/vnd.github+json' }
$keyBytes = [Text.Encoding]::UTF8.GetBytes([string]$cfg.key)
$aesKey = [Security.Cryptography.SHA256]::Create().ComputeHash($keyBytes)
$statePath = "$dir\relay.state"
$script:last = 0; if (Test-Path $statePath) { $script:last = [int](Get-Content $statePath -Raw) }
function Log($m) { try { Add-Content "$dir\relay.log" ("{0} {1}" -f (Get-Date -f 's'), $m); if ((Get-Item "$dir\relay.log").Length -gt 300000) { Remove-Item "$dir\relay.log" -Force } } catch {} }
function HmacHex($s) { $h = New-Object Security.Cryptography.HMACSHA256 (, $keyBytes); ([BitConverter]::ToString($h.ComputeHash([Text.Encoding]::UTF8.GetBytes($s))) -replace '-', '').ToLower() }
function Encrypt($text) {
  $aes = [Security.Cryptography.Aes]::Create(); $aes.Key = $aesKey; $aes.GenerateIV()
  $b = [Text.Encoding]::UTF8.GetBytes($text); $ct = $aes.CreateEncryptor().TransformFinalBlock($b, 0, $b.Length)
  [Convert]::ToBase64String([byte[]]($aes.IV + $ct))
}
function PutFile($path, $text, $msg) {
  $uri = "https://api.github.com/repos/$repo/contents/$path"
  $body = @{ message = $msg; branch = $br; content = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($text)) }
  try { $ex = Invoke-RestMethod "$uri`?ref=$br" -Headers $H -EA Stop; $body.sha = $ex.sha } catch {}
  Invoke-RestMethod $uri -Method Put -Headers $H -Body ($body | ConvertTo-Json) -ContentType 'application/json' -EA Stop | Out-Null
}
function RunCmd($txt) {
  $nl = $txt.IndexOf("`n"); if ($nl -lt 0) { return }
  $first = $txt.Substring(0, $nl).Trim(); $rest = $txt.Substring($nl + 1)
  if ($first -notmatch '^#relay id=(\d+) timeout=(\d+) sig=([0-9a-f]{64})$') { Log 'bad header'; return }
  $id = [int]$Matches[1]; $to = [int]$Matches[2]; $sig = $Matches[3]
  if ($id -le $script:last) { return }
  $script:last = $id; Set-Content $statePath $id            # at-most-once
  if ((HmacHex "$id|$to|$rest") -ne $sig) { Log "bad signature id=$id"; return }
  Log "run id=$id timeout=$to"
  $job = "$dir\relay_job.ps1"; $o = "$dir\relay_out.txt"; $e = "$dir\relay_err.txt"
  Set-Content $job $rest -Encoding UTF8; Remove-Item $o, $e -EA 0
  $p = Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $job -RedirectStandardOutput $o -RedirectStandardError $e -WindowStyle Hidden -PassThru
  $note = ''
  if (-not $p.WaitForExit($to * 1000)) { & taskkill /T /F /PID $p.Id | Out-Null; $note = "`n[relay] TIMEOUT after ${to}s" }
  $out = "[relay] id=$id host=$env:COMPUTERNAME time=$(Get-Date -f s) exit=$($p.ExitCode)`n"
  if (Test-Path $o) { $out += (Get-Content $o -Raw) }
  if ((Test-Path $e) -and (Get-Item $e).Length -gt 0) { $out += "`n--stderr--`n" + (Get-Content $e -Raw) }
  $out += $note
  if ($out.Length -gt 900000) { $out = $out.Substring(0, 900000) + "`n[relay] truncated" }
  for ($try = 0; $try -lt 5; $try++) { try { PutFile "relay/out/$id.enc" (Encrypt $out) "out $id"; Log "out $id ok"; break } catch { Log "put err $($_.Exception.Message)"; Start-Sleep 10 } }
}
Log "relay start last=$script:last"
$etag = $null
while ($true) {
  try {
    $h2 = $H.Clone(); if ($etag) { $h2['If-None-Match'] = $etag }
    $r = Invoke-WebRequest "https://api.github.com/repos/$repo/contents/relay/cmd.ps1?ref=$br" -Headers $h2 -UseBasicParsing -EA Stop
    $etag = $r.Headers['ETag']
    $j = $r.Content | ConvertFrom-Json
    $txt = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String(($j.content -replace '\s', '')))
    RunCmd $txt
  } catch {
    $sc = 0; try { $sc = [int]$_.Exception.Response.StatusCode } catch {}
    if ($sc -ne 304) { Log "poll err $sc $($_.Exception.Message)" }
  }
  Start-Sleep 20
}
