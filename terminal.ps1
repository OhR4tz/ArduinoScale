param(
    [Parameter(Mandatory = $true)]
    [string]$Port,
    [ValidateRange(300, 2000000)]
    [int]$BaudRate = 9600
)

$ErrorActionPreference = 'Stop'
if ([Console]::IsInputRedirected) {
    throw 'Run this script in an interactive PowerShell terminal, or use Arduino Serial Monitor.'
}

$serial = New-Object System.IO.Ports.SerialPort $Port, $BaudRate, None, 8, One
$serial.DtrEnable = $true
$serial.ReadTimeout = 100
$serial.WriteTimeout = 1000
try {
    try {
        $serial.Open()
    } catch {
        throw "Cannot open ${Port}. Close Arduino Serial Monitor and other serial terminals, check the port, and retry. $($_.Exception.Message)"
    }
    Write-Host "Connected to $Port at $BaudRate baud. Opening the port may reset the Uno."
    Start-Sleep -Seconds 2
    Write-Host 'Type tare, calibrate, or help and press Enter. Type quit to close.'
    $line = ''
    $running = $true
    while ($running) {
        $received = $serial.ReadExisting()
        if ($received.Length -gt 0) {
            [Console]::Write($received)
        }
        while ([Console]::KeyAvailable) {
            $key = [Console]::ReadKey($true)
            if ($key.Key -eq [ConsoleKey]::Enter) {
                [Console]::WriteLine()
                if ($line.Trim() -eq 'quit') {
                    $running = $false
                    break
                }
                $serial.Write($line + "`n")
                $line = ''
            } elseif ($key.Key -eq [ConsoleKey]::Backspace) {
                if ($line.Length -gt 0) {
                    $line = $line.Substring(0, $line.Length - 1)
                    [Console]::Write("`b `b")
                }
            } elseif (-not [char]::IsControl($key.KeyChar)) {
                $line += $key.KeyChar
                [Console]::Write($key.KeyChar)
            }
        }
        Start-Sleep -Milliseconds 20
    }
} finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
    $serial.Dispose()
}
