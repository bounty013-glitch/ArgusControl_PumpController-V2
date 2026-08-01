<#
.SYNOPSIS
Change a commissioned controller's client_id / unit_id via its service AP.

.DESCRIPTION
Identity only. Does NOT re-enroll: the console's machine credential is
unrelated to identity and survives the change untouched.

IMPORTANT: identity determines the MQTT topic root, `argus/<client>/<unit>`,
which BOTH sides build independently. Change it here and every console slot
bound to this controller must be updated to match, or the console will
authenticate and subscribe successfully to a tree nobody publishes to -
connected, and deaf.

.EXAMPLE
.\tools\set_identity.ps1 -Ssid Argus-Service-448994 -Mac 10:20:ba:44:89:94 `
    -ClientId stealth -UnitId pump_002
#>
param(
    [Parameter(Mandatory=$true)][string]$Ssid,
    [Parameter(Mandatory=$true)][string]$Mac,
    [Parameter(Mandatory=$true)][string]$ClientId,
    [Parameter(Mandatory=$true)][string]$UnitId,
    [string]$Password,
    [string]$Adapter = 'Wi-Fi 3',
    [string]$RepoRoot = 'C:\Users\bount\Dev\Argus\ArgusControl_PumpController-V2'
)
$ErrorActionPreference = 'Stop'
$base = 'http://192.168.4.1'

function ErrBody($e) {
    if ($e.Exception.Response) {
        $sr = New-Object IO.StreamReader($e.Exception.Response.GetResponseStream())
        return "HTTP $([int]$e.Exception.Response.StatusCode) $($sr.ReadToEnd())"
    }
    return $e.Exception.Message
}

if (-not $Password) {
    $out = python "$RepoRoot\tools\factory_credential.py" $Mac 2>&1
    foreach ($l in $out) { if ($l -match '^Password\s*:\s*(\S+)') { $Password = $Matches[1] } }
}
if (-not $Password) { Write-Host "no password"; exit 1 }

$joined = $false
for ($i = 0; $i -lt 12; $i++) {
    netsh wlan connect name="$Ssid" interface="$Adapter" 2>&1 | Out-Null
    Start-Sleep -Seconds 5
    if ((netsh wlan show interfaces | Out-String) -match [regex]::Escape($Ssid)) { $joined = $true; break }
}
if (-not $joined) { Write-Host "could not join $Ssid"; exit 1 }

$sess = New-Object Microsoft.PowerShell.Commands.WebRequestSession
try {
    $login = Invoke-RestMethod -Uri "$base/api/auth/login" -Method Post -ContentType 'application/json' `
        -Headers @{ 'Origin'=$base; 'Referer'="$base/login" } -WebSession $sess -TimeoutSec 30 `
        -Body (@{ username='argus'; password=$Password } | ConvertTo-Json -Compress)
} catch { Write-Host "login: $(ErrBody $_)"; exit 1 }
$H = @{ 'Origin'=$base; 'Referer'="$base/"; 'X-Argus-CSRF'=$login.csrf }

try {
    Invoke-RestMethod -Uri "$base/api/auth/reauth" -Method Post -ContentType 'application/json' `
        -Headers $H -WebSession $sess -TimeoutSec 30 `
        -Body (@{ current_password=$Password } | ConvertTo-Json -Compress) | Out-Null
} catch { Write-Host "reauth: $(ErrBody $_)"; exit 1 }

# Identity scope requires client_id, unit_id AND device_name together.
try {
    $r = Invoke-RestMethod -Uri "$base/api/config/save" -Method Post -ContentType 'application/json' `
        -Headers $H -WebSession $sess -TimeoutSec 30 -Body (@{
            scope='identity'; client_id=$ClientId; unit_id=$UnitId
            device_name='Argus Peristaltic Pump V2' } | ConvertTo-Json -Compress)
    Write-Host "$Ssid -> $ClientId/$UnitId  ($($r.status), restart_required=$($r.restart_required))"
} catch { Write-Host "identity: $(ErrBody $_)"; exit 1 }

try { Invoke-RestMethod -Uri "$base/api/auth/logout" -Method Post -Headers $H -WebSession $sess -TimeoutSec 15 | Out-Null } catch {}
Write-Host "  new topic root: argus/$ClientId/$UnitId   (console slot MUST be updated to match)"
