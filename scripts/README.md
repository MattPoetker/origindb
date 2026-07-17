# OriginDB Installation Scripts

This directory contains the one-liner installation scripts and deployment infrastructure for `install.origindb.com`.

## 🚀 One-Liner Install Commands

### macOS & Linux
```bash
curl -sSf https://install.origindb.com | sh
```

### Windows (PowerShell)
```powershell
iwr -useb https://install.origindb.com/windows | iex
```

### Windows (Command Prompt)
```cmd
curl -sSf https://install.origindb.com/windows.bat | cmd
```

## 📁 Files

### Install Scripts
- **`install.sh`** - Unix/Linux/macOS installation script
- **`install.ps1`** - Windows PowerShell installation script
- **`install.bat`** - Windows Batch/Command Prompt installation script

### Web Infrastructure
- **`web/index.html`** - Landing page for install.origindb.com
- **`web/nginx.conf`** - Nginx configuration for hosting the install site
- **`deploy.sh`** - Deployment script for setting up the install server

## 🔧 How It Works

The install scripts:

1. **Auto-detect** the user's OS and architecture
2. **Download** the latest release from GitHub releases
3. **Extract** and install to the appropriate system location
4. **Add** OriginDB to the user's PATH
5. **Verify** the installation works correctly

## 🌐 Hosting Setup

The install scripts are hosted at `install.origindb.com` with the following URL routing:

- `https://install.origindb.com/` → `install.sh` (Unix/Linux/macOS)
- `https://install.origindb.com/windows` → `install.ps1` (PowerShell)
- `https://install.origindb.com/windows.bat` → `install.bat` (Batch)

### Deploy to Server

1. Set up a server with Nginx installed
2. Configure DNS for `install.origindb.com`
3. Run the deployment script:

```bash
sudo ./deploy.sh
```

This will:
- Copy all files to the web server
- Configure Nginx with SSL
- Set up proper routing and headers
- Install SSL certificates via Let's Encrypt

## 🔒 Security Features

- **HTTPS only** - All scripts served over secure connections
- **Integrity checks** - Scripts verify downloads and checksums
- **Source verification** - Downloads only from official GitHub releases
- **Permission handling** - Proper file permissions and PATH management
- **Error handling** - Comprehensive error checking and user feedback

## 🧪 Testing

Test the install scripts locally:

```bash
# Test Unix script
./install.sh

# Test PowerShell script (Windows)
powershell -ExecutionPolicy Bypass -File install.ps1

# Test Batch script (Windows)
install.bat
```

## 📊 Analytics

The install scripts include optional analytics to track:
- OS and architecture distribution
- Installation success/failure rates
- Geographic distribution of users

Analytics are privacy-focused and don't collect personal information.

## 🔄 Updates

When a new version of OriginDB is released:

1. GitHub Actions automatically builds and releases binaries
2. The install scripts automatically fetch the latest version
3. No manual updates needed for the install infrastructure

## 🐛 Error Reporting

Common installation issues and solutions:

**Permission Denied:**
- Run with appropriate privileges (sudo on Unix, Administrator on Windows)
- Check file permissions and PATH configuration

**Network Issues:**
- Verify internet connectivity
- Check firewall settings
- Try alternative download methods

**Missing Dependencies:**
- Install curl/wget (Unix) or PowerShell 3+ (Windows)
- Verify tar/unzip utilities are available

## 📚 Related Documentation

- [INSTALL.md](../INSTALL.md) - Complete installation guide
- [.github/workflows/ci.yml](../.github/workflows/ci.yml) - CI/CD pipeline
- [unity/README.md](../unity/README.md) - Unity integration guide