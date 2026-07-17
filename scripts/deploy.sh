#!/bin/bash
# Deployment script for install.origindb.com

set -e

# Configuration
DOMAIN="install.origindb.com"
WEB_ROOT="/var/www/$DOMAIN"
NGINX_SITES_AVAILABLE="/etc/nginx/sites-available"
NGINX_SITES_ENABLED="/etc/nginx/sites-enabled"

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m'

log() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    error "Please run as root (use sudo)"
    exit 1
fi

log "Deploying install.origindb.com..."

# Create web directory
log "Creating web directory..."
mkdir -p "$WEB_ROOT"

# Copy files
log "Copying website files..."
cp web/index.html "$WEB_ROOT/"
cp install.sh "$WEB_ROOT/"
cp install.ps1 "$WEB_ROOT/"
cp install.bat "$WEB_ROOT/"

# Set permissions
log "Setting permissions..."
chown -R www-data:www-data "$WEB_ROOT"
chmod -R 644 "$WEB_ROOT"/*
chmod +x "$WEB_ROOT"/*.sh

# Install Nginx configuration
log "Installing Nginx configuration..."
cp web/nginx.conf "$NGINX_SITES_AVAILABLE/$DOMAIN"

# Enable site
if [ ! -L "$NGINX_SITES_ENABLED/$DOMAIN" ]; then
    ln -s "$NGINX_SITES_AVAILABLE/$DOMAIN" "$NGINX_SITES_ENABLED/"
fi

# Test Nginx configuration
log "Testing Nginx configuration..."
nginx -t

# Reload Nginx
log "Reloading Nginx..."
systemctl reload nginx

# Install SSL certificate (using Let's Encrypt)
if command -v certbot >/dev/null 2>&1; then
    log "Installing SSL certificate..."
    certbot --nginx -d "$DOMAIN" --non-interactive --agree-tos --email admin@origindb.com
else
    log "Certbot not found. Please install SSL certificate manually."
fi

success "Deployment completed!"
echo ""
echo "The install site is now available at: https://$DOMAIN"
echo ""
echo "Test the install scripts:"
echo "  curl -sSf https://$DOMAIN | sh"
echo "  iwr -useb https://$DOMAIN/windows | iex"
echo ""