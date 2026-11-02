# AMEGA Windows LAN firewall rules (private networks only).
# Run this script from the folder containing AMEGA.exe.

$ErrorActionPreference = "Stop"
$exePath = Join-Path $PSScriptRoot "AMEGA.exe"

if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    Write-Host "AMEGA.exe was not found beside this script." -ForegroundColor Red
    Write-Host "Extract the complete AMEGA Windows build, then run this script from that folder."
    Read-Host "Press Enter to close"
    exit 1
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
$isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host "Windows will ask for administrator permission to add the AMEGA firewall rules."
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $arguments
    exit
}

$rules = @(
    @{ Name = "AMEGA-LAN-Discovery-UDP-38472"; DisplayName = "AMEGA LAN discovery (UDP 38472)"; Protocol = "UDP"; Port = "38472"; Description = "Allow AMEGA to receive discovery replies from a PS4 on a private LAN." },
    @{ Name = "AMEGA-LAN-HTTP-TCP-38471"; DisplayName = "AMEGA local transfer host (TCP 38471)"; Protocol = "TCP"; Port = "38471"; Description = "Allow a paired local PS4 to reach the AMEGA HTTP host on a private LAN." }
)

foreach ($rule in $rules) {
    Get-NetFirewallRule -Name $rule.Name -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction SilentlyContinue
    New-NetFirewallRule -Name $rule.Name -DisplayName $rule.DisplayName -Direction Inbound -Program $exePath -Protocol $rule.Protocol -LocalPort $rule.Port -Action Allow -Profile Private -EdgeTraversalPolicy Block -Description $rule.Description | Out-Null
}

Write-Host ""
Write-Host "AMEGA firewall rules were added for PRIVATE networks only." -ForegroundColor Green
Write-Host "Allowed inbound UDP port: 38472"
Write-Host "Allowed inbound TCP port: 38471"
Write-Host "Program: $exePath"
Write-Host ""
$profiles = Get-NetConnectionProfile -ErrorAction SilentlyContinue
$publicProfiles = @($profiles | Where-Object { $_.NetworkCategory -eq "Public" })
if ($publicProfiles.Count -gt 0) {
    Write-Host "WARNING: At least one connected network is marked PUBLIC." -ForegroundColor Yellow
    Write-Host "For your trusted home Wi-Fi only, set its Windows network profile to PRIVATE."
    Write-Host "Do not enable these rules for public or guest Wi-Fi."
}
Read-Host "Press Enter to close"
