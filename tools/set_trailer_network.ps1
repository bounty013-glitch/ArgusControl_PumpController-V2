<#
.SYNOPSIS
Re-point a commissioned controller at a different trailer AP.

.DESCRIPTION
Wi-Fi scope only: identity, enrollment and machine credentials are all
untouched. Applies live, no reboot.

Needed whenever the console's AP identity changes - which it does when a
console is first commissioned, because the SSID is derived from the
console's own MAC rather than being a fleet-wide constant. Every controller
must be re-pointed, one portal visit each.
#>
param(
    [Parameter(Mandatory=$true)][string]$Port,
    [Parameter(Mandatory=$true)][string]$Ssid,
    [Parameter(Mandatory=$true)][string]$Passphrase,
    [string]$Adapter  = 'Wi-Fi 3',
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

$mac = $null
foreach ($l in (python -m esptool --port $Port --after no_reset read_mac 2>&1)) {
    if ($l -match 'MAC:\s*([0-9a-fA-F:]{17})') { $mac = $Matches[1]; break }
}
if (-not $mac) { Write-Host "$Port : could not read MAC"; exit 1 }
$suffix = (($mac -split ':' | Select-Object -Last 3) -join '').ToUpper()
$svc = "Argus-Service-$suffix"

$pw = $null
foreach ($l in (python "$RepoRoot\tools\factory_credential.py" $mac 2>&1)) {
    if ($l -match '^Password\s*:\s*(\S+)') { $pw = $Matches[1] }
}
if (-not $pw) { Write-Host "$Port : could not derive credential"; exit 1 }

$joined = $false
for ($i = 0; $i -lt 15; $i++) {
    netsh wlan connect name="$svc" interface="$Adapter" 2>&1 | Out-Null
    Start-Sleep -Seconds 4
    if ((netsh wlan show interfaces | Out-String) -match [regex]::Escape($svc)) { $joined = $true; break }
}
if (-not $joined) { Write-Host "$svc : could not join"; exit 1 }

$sess = New-Object Microsoft.PowerShell.Commands.WebRequestSession
try {
    $login = Invoke-RestMethod -Uri "$base/api/auth/login" -Method Post -ContentType 'application/json' `
        -Headers @{ Origin=$base; Referer="$base/login" } -WebSession $sess -TimeoutSec 30 `
        -Body (@{ username='argus'; password=$pw } | ConvertTo-Json -Compress)
} catch { Write-Host "$svc login: $(ErrBody $_)"; exit 1 }
$H = @{ Origin=$base; Referer="$base/"; 'X-Argus-CSRF'=$login.csrf }

try {
    Invoke-RestMethod -Uri "$base/api/auth/reauth" -Method Post -ContentType 'application/json' `
        -Headers $H -WebSession $sess -TimeoutSec 30 `
        -Body (@{ current_password=$pw } | ConvertTo-Json -Compress) | Out-Null
    Invoke-RestMethod -Uri "$base/api/config/save" -Method Post -ContentType 'application/json' `
        -Headers $H -WebSession $sess -TimeoutSec 30 `
        -Body (@{ scope='wifi'; sta_ssid=$Ssid; sta_pass=$Passphrase } | ConvertTo-Json -Compress) | Out-Null
} catch { Write-Host "$svc wifi: $(ErrBody $_)"; exit 1 }

Start-Sleep -Seconds 8
try {
    $st = Invoke-RestMethod -Uri "$base/api/status" -Headers $H -WebSession $sess -TimeoutSec 20
    Write-Host ("{0} ({1}) -> {2}  sta={3} ip={4} broker={5}" -f $svc, $st.machine.unit_id, $Ssid,
                $st.network.sta_connected, $st.network.sta_ip_address, $st.broker.running)
} catch { Write-Host "$svc : re-pointed, status unavailable" }

# Always release the session, and drop the service-AP association so the
# host is not left multi-homed - that routing ambiguity has twice looked
# like a dead console when the console was fine.
try { Invoke-RestMethod -Uri "$base/api/auth/logout" -Method Post -Headers $H -WebSession $sess -TimeoutSec 15 | Out-Null } catch {}
netsh wlan disconnect interface="$Adapter" | Out-Null
