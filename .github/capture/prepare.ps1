param([string]$Game)
# The runner has no Vulkan: use the DirectX 9 renderer through Windows' own d3d9.
(Get-Content "$Game\SunsetRacer2-fx.ini") -replace '^renderer=.*','renderer=0' | Set-Content "$Game\SunsetRacer2-fx.ini"
Rename-Item "$Game\d3d9.dll" d3d9.dll.dxvk
Get-ChildItem $Game | Format-Table Name, Length | Out-String | Write-Host
