param(
    [Parameter(Mandatory=$true)][string]$InFile,
    [Parameter(Mandatory=$true)][string]$OutFile
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $InFile)) {
    Write-Error "Input file does not exist: $InFile"
    exit 1
}

$inBytes = [System.IO.File]::ReadAllBytes($InFile)
$outStream = [System.IO.File]::Create($OutFile)
try {
    $gzipStream = New-Object System.IO.Compression.GZipStream($outStream, [System.IO.Compression.CompressionLevel]::Optimal)
    try {
        $gzipStream.Write($inBytes, 0, $inBytes.Length)
    }
    finally {
        $gzipStream.Close()
    }
}
finally {
    $outStream.Close()
}

$origLen = $inBytes.Length
$compLen = (Get-Item -LiteralPath $OutFile).Length
Write-Host "Compressed: $InFile ($origLen bytes) -> $OutFile ($compLen bytes, $([math]::Round($compLen / $origLen * 100, 1))%)"
exit 0
