<#
  Windows code signing helper for CI (Azure Artifact Signing, formerly "Trusted
  Signing"). Identical copy lives in spasynth, SPAGlitch and spastrip under
  scripts/windows-signing.ps1 -- keep the three in step.

  Actions:
    Setup   Install SignTool (Windows SDK Build Tools) + the Artifact Signing
            dlib from NuGet, write metadata.json, and export to GITHUB_ENV:
              SPA_SIGNTOOL        full path to signtool.exe
              SPA_SIGN_DLIB       full path to Azure.CodeSigning.Dlib.dll
              SPA_SIGN_METADATA   full path to metadata.json
              SPA_ISCC_SIGN_CMD   the command line Inno Setup runs for /S
                                  (Inno substitutes $f with each file)
    Sign    Sign the given files (SHA256, RFC 3161 timestamp). Retries 3x.
    Verify  signtool verify /pa /v on the given files; throws on failure.

  Authentication: the workflow runs azure/login (OIDC) first; the dlib then
  picks that up through DefaultAzureCredential's AzureCliCredential. Every
  other credential type is excluded so it never stalls probing them.

  Needs env for Setup: SIGN_ENDPOINT, SIGN_ACCOUNT, SIGN_PROFILE.
#>
param(
  [Parameter(Mandatory)][ValidateSet('Setup', 'Sign', 'Verify')][string]$Action,
  [string[]]$Files = @()
)
$ErrorActionPreference = 'Stop'
$timestampUrl = 'http://timestamp.acs.microsoft.com'
$tools = Join-Path $env:RUNNER_TEMP 'artifact-signing'

function Resolve-Files([string[]]$patterns) {
  $out = @()
  foreach ($p in $patterns) {
    $hits = @(Get-ChildItem -Path $p -File -ErrorAction SilentlyContinue)
    if ($hits.Count -eq 0) { throw "No file matches '$p' -- refusing to continue with nothing to sign/verify." }
    $out += $hits.FullName
  }
  return $out
}

function Invoke-Native([string]$exe, [string[]]$arguments) {
  & $exe @arguments
  if ($LASTEXITCODE -ne 0) { throw "$exe exited with $LASTEXITCODE" }
}

switch ($Action) {
  'Setup' {
    foreach ($v in 'SIGN_ENDPOINT', 'SIGN_ACCOUNT', 'SIGN_PROFILE') {
      if (-not (Get-Item "env:$v" -ErrorAction SilentlyContinue).Value) { throw "$v is not set" }
    }
    New-Item -ItemType Directory -Force -Path $tools | Out-Null
    # Both packages are unpinned on purpose: Microsoft's docs recommend the
    # latest of each, and the dlib requires a recent SignTool (>= 10.0.2261.755).
    foreach ($pkg in 'Microsoft.Windows.SDK.BuildTools', 'Microsoft.ArtifactSigning.Client') {
      Invoke-Native 'nuget' @('install', $pkg, '-OutputDirectory', $tools, '-ExcludeVersion', '-NonInteractive')
    }
    $signtool = Get-ChildItem -Path (Join-Path $tools 'Microsoft.Windows.SDK.BuildTools') -Recurse -Filter signtool.exe |
      Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1
    $dlib = Get-ChildItem -Path (Join-Path $tools 'Microsoft.ArtifactSigning.Client') -Recurse -Filter Azure.CodeSigning.Dlib.dll |
      Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1
    if (-not $signtool) { throw 'signtool.exe (x64) not found in Microsoft.Windows.SDK.BuildTools' }
    if (-not $dlib) { throw 'Azure.CodeSigning.Dlib.dll (x64) not found in Microsoft.ArtifactSigning.Client' }

    $meta = Join-Path $tools 'metadata.json'
    [ordered]@{
      Endpoint               = $env:SIGN_ENDPOINT.TrimEnd('/')
      CodeSigningAccountName = $env:SIGN_ACCOUNT
      CertificateProfileName = $env:SIGN_PROFILE
      CorrelationId          = "$env:GITHUB_REPOSITORY@$env:GITHUB_RUN_ID"
      ExcludeCredentials     = @(
        'EnvironmentCredential', 'ManagedIdentityCredential', 'WorkloadIdentityCredential',
        'SharedTokenCacheCredential', 'VisualStudioCredential', 'VisualStudioCodeCredential',
        'AzurePowerShellCredential', 'AzureDeveloperCliCredential', 'InteractiveBrowserCredential')
    } | ConvertTo-Json | Set-Content -Path $meta -Encoding utf8

    # The Inno /S command. Paths under RUNNER_TEMP contain no spaces, so no
    # inner quoting is needed; Inno replaces $f with the file to sign.
    $iscc = "$($signtool.FullName) sign /v /fd SHA256 /tr $timestampUrl /td SHA256 /dlib $($dlib.FullName) /dmdf $meta " + '$f'
    @(
      "SPA_SIGNTOOL=$($signtool.FullName)",
      "SPA_SIGN_DLIB=$($dlib.FullName)",
      "SPA_SIGN_METADATA=$meta",
      "SPA_ISCC_SIGN_CMD=$iscc"
    ) | Add-Content -Path $env:GITHUB_ENV -Encoding utf8
    Write-Host "SignTool: $($signtool.FullName)"
    Write-Host "Dlib:     $($dlib.FullName)"
  }
  'Sign' {
    foreach ($f in Resolve-Files $Files) {
      Write-Host "Signing $f"
      $ok = $false
      for ($i = 1; $i -le 3 -and -not $ok; $i++) {
        & $env:SPA_SIGNTOOL sign /v /fd SHA256 /tr $timestampUrl /td SHA256 /dlib $env:SPA_SIGN_DLIB /dmdf $env:SPA_SIGN_METADATA $f
        if ($LASTEXITCODE -eq 0) { $ok = $true } else { Write-Host "Attempt $i failed ($LASTEXITCODE)"; Start-Sleep -Seconds (5 * $i) }
      }
      if (-not $ok) { throw "Signing failed: $f" }
    }
  }
  'Verify' {
    foreach ($f in Resolve-Files $Files) {
      Write-Host "Verifying $f"
      Invoke-Native $env:SPA_SIGNTOOL @('verify', '/pa', '/v', $f)
    }
  }
}
