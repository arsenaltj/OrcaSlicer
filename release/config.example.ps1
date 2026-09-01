# Copy this file to config.local.ps1 and fill values for this computer.
# config.local.ps1 is ignored by Git. Never put provider keys or SSH secrets here
# if the file may be copied to an untrusted location.

$BuildRelease = @{
    BuildDir            = 'build-commercial-review'
    InternalDefaultsFile = '' # Absolute path outside this Git worktree.
    OutputDir           = 'build\windows-installer'
    CMakeExecutable     = '' # Optional; auto-detected when empty.
    NsisDir             = '' # Optional; auto-detected by the package script.
}

$UploadRelease = @{
    SshTarget        = '' # Local SSH alias or user@host; never commit the real value.
    EmployeeId       = '' # Example forms: 12345, 00012345, or s00012345.
    RestrictedPublisher = $true
    RestrictedChunkSizeBytes = 4MB # Resumable chunks; keep between 1 and 8 MiB.
    RemoteDownloadDir = '/srv/3dprint-beer/data/downloads'
    RemoteOwner      = 'web'
    RemoteGroup      = 'web'
}

$PublicRelease = @{
    BaseUrl = 'https://3dprint.beer'
}

# The website source is a separate checkout and may be anywhere on each machine.
$WebsiteWorktree = ''

# Non-secret server-side website layout used by the deployment runbook.
$WebsiteServer = @{
    SourceDir    = '/home/web/3dprint-web'
    CurrentLink  = '/srv/3dprint-beer/current'
    ReleasesDir  = '/srv/3dprint-beer/releases'
    DeployScript = '/home/web/bin/deploy-3dprint-beer'
    ServiceUser  = 'web'
}
