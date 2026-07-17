# OriginDB Installation Script for Windows (PowerShell)
# Usage: iwr -useb https://install.origindb.com/windows | iex

param(
    [string]$InstallDir = "$env:ProgramFiles\OriginDB",
    [string]$Version = "latest"
)

# Configuration
$GitHubRepo = "your-org/origindb"
$BinaryName = "origindb.exe"
$TmpDir = "$env:TEMP\origindb-install"

# Colors for console output
function Write-ColorOutput {
    param([string]$Message, [string]$Color = "White")

    $colors = @{
        "Red" = [ConsoleColor]::Red
        "Green" = [ConsoleColor]::Green
        "Yellow" = [ConsoleColor]::Yellow
        "Blue" = [ConsoleColor]::Blue
        "White" = [ConsoleColor]::White
    }

    Write-Host $Message -ForegroundColor $colors[$Color]
}

function Write-Info { Write-ColorOutput "[INFO] $args" "Blue" }
function Write-Warn { Write-ColorOutput "[WARN] $args" "Yellow" }
function Write-Error { Write-ColorOutput "[ERROR] $args" "Red" }
function Write-Success { Write-ColorOutput "[SUCCESS] $args" "Green" }

# Check if running as administrator
function Test-Administrator {
    $currentUser = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($currentUser)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Check prerequisites
function Test-Prerequisites {
    Write-Info "Checking prerequisites..."

    # Check PowerShell version
    if ($PSVersionTable.PSVersion.Major -lt 3) {
        Write-Error "PowerShell 3.0 or higher is required"
        exit 1
    }

    # Check if we can access the internet
    try {
        Invoke-WebRequest -Uri "https://api.github.com" -Method Head -TimeoutSec 10 | Out-Null
    }
    catch {
        Write-Error "Cannot access GitHub API. Please check your internet connection."
        exit 1
    }

    Write-Success "Prerequisites check passed"
}

# Detect architecture
function Get-Architecture {
    $arch = $env:PROCESSOR_ARCHITECTURE
    switch ($arch) {
        "AMD64" { return "x64" }
        "ARM64" { return "arm64" }
        default {
            Write-Error "Unsupported architecture: $arch"
            exit 1
        }
    }
}

# Get latest release version
function Get-LatestVersion {
    Write-Info "Fetching latest release version..."

    try {
        $response = Invoke-RestMethod -Uri "https://api.github.com/repos/$GitHubRepo/releases/latest"
        $version = $response.tag_name
        Write-Info "Latest version: $version"
        return $version
    }
    catch {
        Write-Warn "Could not fetch latest version, using 'latest'"
        return "latest"
    }
}

# Download and extract
function Install-OriginDB {
    param([string]$Architecture, [string]$Version)

    Write-Info "Downloading OriginDB for Windows $Architecture..."

    # Create temporary directory
    if (Test-Path $TmpDir) {
        Remove-Item $TmpDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $TmpDir | Out-Null

    # Download URL
    $downloadUrl = if ($Version -eq "latest") {
        "https://github.com/$GitHubRepo/releases/latest/download/origindb-windows-$Architecture.zip"
    } else {
        "https://github.com/$GitHubRepo/releases/download/$Version/origindb-windows-$Architecture.zip"
    }

    $zipPath = "$TmpDir\origindb.zip"

    try {
        # Download file
        Write-Info "Downloading from: $downloadUrl"
        Invoke-WebRequest -Uri $downloadUrl -OutFile $zipPath -ErrorAction Stop

        # Extract archive
        Write-Info "Extracting archive..."
        Expand-Archive -Path $zipPath -DestinationPath $TmpDir -Force

        Write-Success "Download completed"
    }
    catch {
        Write-Error "Failed to download OriginDB: $($_.Exception.Message)"
        exit 1
    }
}

# Install binary to system
function Install-Binary {
    Write-Info "Installing OriginDB..."

    # Create install directory
    if (!(Test-Path $InstallDir)) {
        try {
            New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
        }
        catch {
            Write-Error "Failed to create install directory: $InstallDir"
            Write-Error "Error: $($_.Exception.Message)"
            exit 1
        }
    }

    # Copy binary
    $sourcePath = "$TmpDir\$BinaryName"
    $destPath = "$InstallDir\$BinaryName"

    try {
        Copy-Item $sourcePath $destPath -Force
        Write-Success "OriginDB installed to $destPath"
    }
    catch {
        Write-Error "Failed to install binary: $($_.Exception.Message)"
        exit 1
    }
}

# Add to PATH
function Add-ToPath {
    Write-Info "Adding OriginDB to PATH..."

    # Get current PATH
    $currentPath = [Environment]::GetEnvironmentVariable("PATH", "Machine")

    # Check if already in PATH
    if ($currentPath -like "*$InstallDir*") {
        Write-Info "OriginDB is already in PATH"
        return
    }

    # Add to PATH
    try {
        $newPath = "$currentPath;$InstallDir"
        [Environment]::SetEnvironmentVariable("PATH", $newPath, "Machine")

        # Also add to current session
        $env:PATH += ";$InstallDir"

        Write-Success "Added OriginDB to system PATH"
        Write-Info "You may need to restart your terminal for PATH changes to take effect"
    }
    catch {
        Write-Warn "Could not add to system PATH automatically. Please add manually: $InstallDir"
        Write-Info "To add manually:"
        Write-Info "1. Open System Properties > Advanced > Environment Variables"
        Write-Info "2. Edit the PATH variable and add: $InstallDir"
    }
}

# Verify installation
function Test-Installation {
    Write-Info "Verifying installation..."

    # Check if binary exists
    $binaryPath = "$InstallDir\$BinaryName"
    if (!(Test-Path $binaryPath)) {
        Write-Error "Installation verification failed. Binary not found at $binaryPath"
        exit 1
    }

    # Test if we can run it
    try {
        $version = & $binaryPath --version 2>$null
        if ($LASTEXITCODE -eq 0) {
            Write-Success "OriginDB installed successfully!"
            Write-Success "Version: $version"
            Write-Success "Location: $binaryPath"
        } else {
            Write-Warn "Binary installed but version check failed"
        }
    }
    catch {
        Write-Warn "Binary installed but could not verify version"
    }
}

# Cleanup
function Remove-TempFiles {
    Write-Info "Cleaning up..."
    if (Test-Path $TmpDir) {
        Remove-Item $TmpDir -Recurse -Force
    }
}

# Main installation function
function Install-Main {
    Write-Info "Starting OriginDB installation..."

    try {
        # Check if running as admin
        if (!(Test-Administrator)) {
            Write-Warn "Running without administrator privileges"
            Write-Warn "Installation may fail or require manual PATH configuration"
        }

        # Check prerequisites
        Test-Prerequisites

        # Detect architecture
        $architecture = Get-Architecture
        Write-Info "Detected architecture: $architecture"

        # Get version
        $version = if ($Version -eq "latest") { Get-LatestVersion } else { $Version }
        Write-Info "Installing version: $version"

        # Download and extract
        Install-OriginDB $architecture $version

        # Install binary
        Install-Binary

        # Add to PATH
        Add-ToPath

        # Verify installation
        Test-Installation

        Write-Success "🚀 OriginDB installation completed!"
        Write-Host ""
        Write-Host "Next steps:" -ForegroundColor Yellow
        Write-Host "  1. Restart your terminal or open a new one"
        Write-Host "  2. Start the OriginDB server: origindb server"
        Write-Host "  3. Check out the documentation: https://docs.origindb.com"
        Write-Host "  4. Try the C# quickstart: https://docs.origindb.com/csharp"
        Write-Host ""
    }
    catch {
        Write-Error "Installation failed: $($_.Exception.Message)"
        exit 1
    }
    finally {
        Remove-TempFiles
    }
}

# Run main installation
Install-Main