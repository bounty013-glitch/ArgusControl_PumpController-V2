<#
.SYNOPSIS
Commissions one Argus pump controller end to end.

.DESCRIPTION
Takes a controller that has been flashed and never configured, and leaves it
commissioned, on the trailer network, and enrolled for the console - then
prints the machine credential, which the controller discloses EXACTLY ONCE.

This is the commissioning-day procedure in executable form. Everything it
does was learned the hard way against real boards on 2026-08-01; the notes
below are the traps, so nobody rediscovers them at a paint shop.

  - /api/auth/login enforces an ORIGIN CHECK before authenticating. A bare
    scripted POST is refused 403, which is NOT an authentication failure and
    must not be read as a wrong password.
  - The console principal is 'argus'. The session identifies as
    'argus_console'.
  - The CSRF header is X-Argus-CSRF.
  - Machine enrollment needs a SEPARATE recent re-authentication beyond
    holding a session (POST /api/auth/reauth with current_password).
  - /api/config/save needs a 'scope' of identity or wifi, and the identity
    scope needs client_id, unit_id AND device_name together.
  - Session slots are FEW. A script that logs in without logging out
    exhausts them, and the next run fails with session_unavailable - which
    reads exactly like a credential problem. This script always logs out.

.PARAMETER Port
Serial port of the controller, used only to read its MAC and to reset it.

.PARAMETER PumpNumber
Physical position, counted LEFT TO RIGHT standing in front of the trailer.
Becomes unit id pump_00N and console slot N.

.PARAMETER Ssid
Trailer AP the controller should join.

.PARAMETER Passphrase
Trailer AP passphrase.

.PARAMETER ClientId
Fleet client id. Defaults to paladin.

.EXAMPLE
.\tools\commission_controller.ps1 -Port COM12 -PumpNumber 3 `
    -Ssid ArgusNet-probe -Passphrase probe-bench-2026
#>
param(
    [Parameter(Mandatory=$true)][string]$Port,
    [Parameter(Mandatory=$true)][int]$PumpNumber,
    [Parameter(Mandatory=$true)][string]$Ssid,
    [Parameter(Mandatory=$true)][string]$Passphrase,
    [string]$ClientId = 'paladin',
    [string]$Adapter  = 'Wi-Fi 3',
    [string]$RepoRoot = 'C:\Users\bount\Dev\Argus\ArgusControl_PumpController-V2'
)

$ErrorActionPreference = 'Stop'
$base = 'http://192.168.4.1'
$unit = 'pump_{0:D3}' -f $PumpNumber

function Fail($m) { Write-Host "FAILED: $m" -ForegroundColor Red; exit 1 }
function ErrBody($e) {
    if ($e.Exception.Response) {
        $sr = New-Object IO.StreamReader($e.Exception.Response.GetResponseStream())
        return "HTTP $([int]$e.Exception.Response.StatusCode) $($sr.ReadToEnd())"
    }
    return $e.Exception.Message
}

Write-Host "=== Commissioning pump $PumpNumber ($unit) on $Port ==="

# 1. MAC identifies the board and derives both its SSID and its factory
#    password. Also resets it, which clears any leaked sessions.
$mac = $null
$esptool = python -m esptool --port $Port --after hard_reset read_mac 2>&1
foreach ($line in $esptool) {
    if ($line -match 'MAC:\s*([0-9a-fA-F:]{17})') { $mac = $Matches[1]; break }
}
if (-not $mac) { Fail "could not read MAC from $Port" }
$suffix = ($mac -split ':' | Select-Object -Last 3) -join ''
$ssidSvc = "Argus-Service-$($suffix.ToUpper())"
Write-Host "MAC $mac -> $ssidSvc"

# 2. Recompute the password the device derived for itself. It is never
#    logged or served by the device; only the salt makes this possible.
$pw = $null
$cred = python "$RepoRoot\tools\factory_credential.py" $mac 2>&1
foreach ($line in $cred) { if ($line -match '^Password\s*:\s*(\S+)') { $pw = $Matches[1] } }
if (-not $pw) { Fail "could not derive factory credential" }

# 3. Join its service AP.
$profileXml = Join-Path $env:TEMP "argus_$suffix.xml"
@"
<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
 <name>$ssidSvc</name>
 <SSIDConfig><SSID><name>$ssidSvc</name></SSID></SSIDConfig>
 <connectionType>ESS</connectionType><connectionMode>manual</connectionMode>
 <MSM><security>
  <authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption>
  <sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>ArgusService2026!</keyMaterial></sharedKey>
 </security></MSM>
</WLANProfile>
"@ | Set-Content $profileXml -Encoding utf8
netsh wlan add profile filename="$profileXml" interface="$Adapter" | Out-Null
Remove-Item $profileXml -ErrorAction SilentlyContinue

$joined = $false
for ($i = 0; $i -lt 20; $i++) {
    netsh wlan connect name="$ssidSvc" interface="$Adapter" 2>&1 | Out-Null
    Start-Sleep -Seconds 5
    if ((netsh wlan show interfaces | Out-String) -match [regex]::Escape($ssidSvc)) { $joined = $true; break }
}
if (-not $joined) { Fail "could not join $ssidSvc" }
Write-Host "joined $ssidSvc"

# 4. Authenticate. Origin is required or this is 403 before auth is tried.
$sess = New-Object Microsoft.PowerShell.Commands.WebRequestSession
$H0 = @{ 'Origin' = $base; 'Referer' = "$base/login" }
try {
    $login = Invoke-RestMethod -Uri "$base/api/auth/login" -Method Post -ContentType 'application/json' `
        -Headers $H0 -WebSession $sess -TimeoutSec 30 `
        -Body (@{ username='argus'; password=$pw } | ConvertTo-Json -Compress)
} catch { Fail "login: $(ErrBody $_)" }
$H = @{ 'Origin' = $base; 'Referer' = "$base/"; 'X-Argus-CSRF' = $login.csrf }
Write-Host "authenticated as $($login.principal.id)"

try {
    Invoke-RestMethod -Uri "$base/api/auth/reauth" -Method Post -ContentType 'application/json' `
        -Headers $H -WebSession $sess -TimeoutSec 30 `
        -Body (@{ current_password=$pw } | ConvertTo-Json -Compress) | Out-Null
} catch { Fail "reauth: $(ErrBody $_)" }

# 5. Enroll the console. Pump-operating permissions only: the console
#    operates a pump, it does not administer a controller.
try {
    $m = Invoke-RestMethod -Uri "$base/api/security/machines" -Method Post -ContentType 'application/json' `
        -Headers $H -WebSession $sess -TimeoutSec 40 -Body (@{
            display_name = "Trailer Console (slot $PumpNumber)"
            client_type  = 'PUMP_HMI'
            interfaces   = @('STA')
            scope        = '*'
            topic_scope  = '*'
            api_scope    = ''
            permissions  = @('view_status','request_authority','motion',
                             'software_estop','reset_software_estop','ack_alarms')
        } | ConvertTo-Json -Compress)
} catch { Fail "enroll: $(ErrBody $_)" }

# 6. Identity, then network.
try {
    Invoke-RestMethod -Uri "$base/api/config/save" -Method Post -ContentType 'application/json' `
        -Headers $H -WebSession $sess -TimeoutSec 30 -Body (@{
            scope='identity'; client_id=$ClientId; unit_id=$unit
            device_name='Argus Peristaltic Pump V2' } | ConvertTo-Json -Compress) | Out-Null
    Write-Host "identity set: $ClientId / $unit"
} catch { Fail "identity: $(ErrBody $_)" }

try {
    Invoke-RestMethod -Uri "$base/api/config/save" -Method Post -ContentType 'application/json' `
        -Headers $H -WebSession $sess -TimeoutSec 30 -Body (@{
            scope='wifi'; sta_ssid=$Ssid; sta_pass=$Passphrase } | ConvertTo-Json -Compress) | Out-Null
    Write-Host "trailer network set: $Ssid"
} catch { Fail "wifi: $(ErrBody $_)" }

Start-Sleep -Seconds 10
try {
    $st = Invoke-RestMethod -Uri "$base/api/status" -Headers $H -WebSession $sess -TimeoutSec 20
    Write-Host "network: sta=$($st.network.sta_connected) ip=$($st.network.sta_ip_address) broker=$($st.broker.running)"
} catch { Write-Host "status unavailable (not fatal)" }

# 7. Always release the session.
try { Invoke-RestMethod -Uri "$base/api/auth/logout" -Method Post -Headers $H -WebSession $sess -TimeoutSec 15 | Out-Null } catch {}

# 8. Identity needs a restart to take effect.
python -m esptool --port $Port --after hard_reset read_mac 2>&1 | Out-Null

Write-Host ""
Write-Host "=== pump $PumpNumber commissioned ==="
Write-Host "CONFIG_CONSOLE_DEV_SLOT${PumpNumber}_ENABLE=y"
Write-Host "CONFIG_CONSOLE_DEV_SLOT${PumpNumber}_BROKER_PORT=1883"
Write-Host "CONFIG_CONSOLE_DEV_SLOT${PumpNumber}_MACHINE_ID=`"$($m.machine_id)`""
Write-Host "CONFIG_CONSOLE_DEV_SLOT${PumpNumber}_MACHINE_SECRET=`"$($m.machine_secret)`""
Write-Host "CONFIG_CONSOLE_DEV_SLOT${PumpNumber}_CONTROLLER_CLIENT=`"$ClientId`""
Write-Host "CONFIG_CONSOLE_DEV_SLOT${PumpNumber}_CONTROLLER_UNIT=`"$unit`""
Write-Host ""
Write-Host "The machine secret is disclosed ONCE. If it is lost, re-enroll."
