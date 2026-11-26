param([Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference = 'Stop'
$framework = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
$wpf = Join-Path $framework 'WPF'
$source = Join-Path $PSScriptRoot 'Launcher.cs'
$xaml = Join-Path $PSScriptRoot 'Launcher.xaml'
$court = (Resolve-Path (Join-Path $PSScriptRoot '..\..\docs\screenshot-match.jpg')).Path
$icon = (Resolve-Path (Join-Path $PSScriptRoot '..\..\icons\256.ico')).Path
$destination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Output)
$compilerArgs = @(
    '/nologo','/target:winexe','/platform:x64','/optimize+',
    "/out:$destination", "/win32icon:$icon",
    "/reference:$wpf\PresentationFramework.dll", "/reference:$wpf\PresentationCore.dll",
    "/reference:$wpf\WindowsBase.dll", "/reference:$framework\System.Xaml.dll",
    "/resource:$xaml,Launcher.xaml", "/resource:$court,Court.jpg", $source
)
& (Join-Path $framework 'csc.exe') @compilerArgs
exit $LASTEXITCODE
