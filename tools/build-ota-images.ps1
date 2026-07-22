param(
    [ValidateSet('r2','s3')][string]$Board = 's3',
    [string]$OutDir = '.'
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path 'config.toml')) { throw 'config.toml is required for the with-config image' }

if ($Board -eq 's3') {
    $target = 'xtensa-esp32s3-espidf'; $chip = 'esp32s3'; $flash = '16mb'
    $layout = '16m'; $partitions = 'partitions_ota_16m.csv'; $sdk = "$PWD\sdkconfig.esp32s3.defaults"
} else {
    $target = 'xtensa-esp32-espidf'; $chip = 'esp32'; $flash = '4mb'
    $layout = '4m'; $partitions = 'partitions_ota_4m.csv'; $sdk = "$PWD\sdkconfig.defaults"
}
$targetDir = if ($env:CARGO_TARGET_DIR) {
    $env:CARGO_TARGET_DIR
} elseif ($Board -eq 's3') {
    'C:\ts3'
} else {
    'C:\t'
}
$env:CARGO_TARGET_DIR = $targetDir
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$env:SMSGATE_FLASH_LAYOUT = $layout
$env:ESP_IDF_SDKCONFIG_DEFAULTS = $sdk

foreach ($variant in @(@{Apply='0'; Name='software-only'}, @{Apply='1'; Name='with-config'})) {
    $env:SMSGATE_APPLY_COMPILED_CONFIG = $variant.Apply
    cargo +esp build --release --target $target
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $elf = Join-Path $targetDir "$target\release\smsgate"
    $output = Join-Path $OutDir "smsgate-$Board-ota-$($variant.Name).bin"
    espflash save-image --chip $chip --flash-size $flash --partition-table $partitions --target-app-partition ota_0 $elf $output
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
