# Deployment Guide

## Overview

This guide covers deploying InstantDB in various environments, from development to production. The current version (v0.1.0) supports single-node deployments only.

## Single Node Deployment

### Development Environment

**Quick Start:**
```bash
# Build the project
cmake -B build -S .
cmake --build build

# Run demo (development/testing)
./build/instantdb_demo 8080

# Run production server
./build/instantdb_server -p 8080 -d ./data
```

**Environment Variables:**
```bash
export INSTANTDB_WS_PORT=8080
export INSTANTDB_DATA_DIR=/var/lib/instantdb
export INSTANTDB_LOG_LEVEL=info
```

### Production Environment

#### System Requirements

**Minimum Requirements:**
- **CPU**: 2 cores, 2.0 GHz
- **Memory**: 4 GB RAM
- **Storage**: 10 GB available space
- **Network**: 1 Gbps network interface
- **OS**: Linux (Ubuntu 20.04+, CentOS 8+, RHEL 8+)

**Recommended Production:**
- **CPU**: 8+ cores, 3.0+ GHz
- **Memory**: 16+ GB RAM
- **Storage**: 100+ GB SSD storage
- **Network**: 10 Gbps network interface
- **OS**: Ubuntu 22.04 LTS or CentOS 8 Stream

#### Installation Steps

**1. Prepare System:**
```bash
# Update system packages
sudo apt update && sudo apt upgrade -y

# Install dependencies
sudo apt install -y build-essential cmake git libssl-dev

# Create system user
sudo useradd -r -s /bin/false instantdb
sudo mkdir -p /var/lib/instantdb
sudo chown instantdb:instantdb /var/lib/instantdb

# Create log directory
sudo mkdir -p /var/log/instantdb
sudo chown instantdb:instantdb /var/log/instantdb
```

**2. Build Application:**
```bash
# Clone and build
git clone <repository-url> /opt/instantdb
cd /opt/instantdb

# Build for production
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Install binaries
sudo cp build/instantdb_server /usr/local/bin/
sudo chmod +x /usr/local/bin/instantdb_server
```

**3. Create Configuration:**
```bash
# Create config directory
sudo mkdir -p /etc/instantdb

# Create basic configuration file
sudo tee /etc/instantdb/config.env << EOF
INSTANTDB_WS_PORT=8080
INSTANTDB_DATA_DIR=/var/lib/instantdb
INSTANTDB_LOG_LEVEL=info
EOF
```

**4. Create systemd Service:**
```bash
sudo tee /etc/systemd/system/instantdb.service << EOF
[Unit]
Description=InstantDB Server
After=network.target
Wants=network.target

[Service]
Type=simple
User=instantdb
Group=instantdb
WorkingDirectory=/var/lib/instantdb
EnvironmentFile=/etc/instantdb/config.env
ExecStart=/usr/local/bin/instantdb_server
Restart=always
RestartSec=5
LimitNOFILE=65536

# Security hardening
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/instantdb /var/log/instantdb

[Install]
WantedBy=multi-user.target
EOF
```

**5. Start Service:**
```bash
# Reload systemd and start service
sudo systemctl daemon-reload
sudo systemctl enable instantdb
sudo systemctl start instantdb

# Check status
sudo systemctl status instantdb
```

### Docker Deployment

#### Dockerfile

```dockerfile
# Dockerfile
FROM ubuntu:22.04 AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy source code
WORKDIR /app
COPY . .

# Build application
RUN cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
RUN cmake --build build

# Production image
FROM ubuntu:22.04

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    libssl3 \
    && rm -rf /var/lib/apt/lists/*

# Create user
RUN useradd -r -s /bin/false instantdb

# Copy binary
COPY --from=builder /app/build/instantdb_server /usr/local/bin/

# Create directories
RUN mkdir -p /var/lib/instantdb /var/log/instantdb
RUN chown instantdb:instantdb /var/lib/instantdb /var/log/instantdb

# Switch to non-root user
USER instantdb

# Expose ports (WebSocket + gRPC)
EXPOSE 8080 50051

# Set working directory
WORKDIR /var/lib/instantdb

# Start server
CMD ["/usr/local/bin/instantdb_server"]
```

#### Docker Compose

```yaml
# docker-compose.yml
version: '3.8'

services:
  instantdb:
    build: .
    ports:
      - "8080:8080"      # WebSocket
      - "50051:50051"    # gRPC (SQL + WASM module deployment)
    environment:
      - INSTANTDB_WS_PORT=8080
      - INSTANTDB_GRPC_PORT=50051
      - INSTANTDB_DATA_DIR=/var/lib/instantdb
      - INSTANTDB_LOG_LEVEL=info
    volumes:
      - instantdb_data:/var/lib/instantdb
      - instantdb_logs:/var/log/instantdb
    restart: unless-stopped
    healthcheck:
      test: ["CMD", "nc", "-z", "localhost", "8080"]
      interval: 30s
      timeout: 10s
      retries: 3

volumes:
  instantdb_data:
  instantdb_logs:
```

#### Build and Run

```bash
# Build image
docker build -t instantdb:latest .

# Run with Docker Compose
docker-compose up -d

# Check logs
docker-compose logs -f instantdb

# Stop service
docker-compose down
```

### Kubernetes Deployment (planned — example manifests)

> **Note:** the repository does not ship a `k8s/` directory; first-class
> Kubernetes support is planned. The manifests below are illustrative
> examples you would need to create and maintain yourself.

#### Namespace and ConfigMap

```yaml
# k8s/namespace.yaml (example — not in the repo)
apiVersion: v1
kind: Namespace
metadata:
  name: instantdb

---
# k8s/configmap.yaml
apiVersion: v1
kind: ConfigMap
metadata:
  name: instantdb-config
  namespace: instantdb
data:
  INSTANTDB_WS_PORT: "8080"
  INSTANTDB_DATA_DIR: "/var/lib/instantdb"
  INSTANTDB_LOG_LEVEL: "info"
```

#### Deployment

```yaml
# k8s/deployment.yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: instantdb
  namespace: instantdb
spec:
  replicas: 1  # Single node only in v0.1.0
  selector:
    matchLabels:
      app: instantdb
  template:
    metadata:
      labels:
        app: instantdb
    spec:
      containers:
      - name: instantdb
        image: instantdb:latest
        ports:
        - containerPort: 8080
          name: websocket
        envFrom:
        - configMapRef:
            name: instantdb-config
        volumeMounts:
        - name: data
          mountPath: /var/lib/instantdb
        - name: logs
          mountPath: /var/log/instantdb
        resources:
          requests:
            memory: "1Gi"
            cpu: "500m"
          limits:
            memory: "4Gi"
            cpu: "2000m"
        livenessProbe:
          tcpSocket:
            port: 8080
          initialDelaySeconds: 30
          periodSeconds: 10
        readinessProbe:
          tcpSocket:
            port: 8080
          initialDelaySeconds: 5
          periodSeconds: 5
      volumes:
      - name: data
        persistentVolumeClaim:
          claimName: instantdb-data
      - name: logs
        persistentVolumeClaim:
          claimName: instantdb-logs
```

#### Persistent Volumes

```yaml
# k8s/pvc.yaml
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: instantdb-data
  namespace: instantdb
spec:
  accessModes:
    - ReadWriteOnce
  resources:
    requests:
      storage: 50Gi
  storageClassName: fast-ssd

---
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: instantdb-logs
  namespace: instantdb
spec:
  accessModes:
    - ReadWriteOnce
  resources:
    requests:
      storage: 10Gi
  storageClassName: standard
```

#### Service

```yaml
# k8s/service.yaml
apiVersion: v1
kind: Service
metadata:
  name: instantdb-service
  namespace: instantdb
spec:
  selector:
    app: instantdb
  ports:
  - name: websocket
    port: 8080
    targetPort: 8080
  type: LoadBalancer
```

#### Deploy to Kubernetes

```bash
# Apply all configurations
kubectl apply -f k8s/

# Check deployment status
kubectl get pods -n instantdb
kubectl get services -n instantdb

# Check logs
kubectl logs -f deployment/instantdb -n instantdb

# Port forward for testing
kubectl port-forward service/instantdb-service 8080:8080 -n instantdb
```

## Configuration Management

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `INSTANTDB_WS_PORT` | 8080 | WebSocket server port |
| `INSTANTDB_GRPC_PORT` | 50051 | gRPC server port (SQL + WASM services) |
| `INSTANTDB_DATA_DIR` | ./instantdb_data | Data directory path (WAL + persisted WASM modules under `modules/`) |
| `INSTANTDB_LOG_LEVEL` | info | Logging level (trace, debug, info, warn, error) |

### Command Line Options

```bash
Usage: instantdb_server [OPTIONS]

Options:
  -p, --port PORT          WebSocket port (default: 8080)
  -g, --grpc-port PORT     gRPC port (default: 50051)
  -d, --data-dir DIR       Data directory (default: ./instantdb_data)
  -l, --log-level LEVEL    Log level (default: info)
  -c, --config FILE        Config file path (default: instantdb.conf)
  -h, --help               Show help message

Examples:
  instantdb_server                    # Start with defaults
  instantdb_server -p 9090 -g 50052   # Custom ports
  instantdb_server --data-dir /data   # Custom data directory
```

### File Permissions

**Data Directory:**
```bash
# Set proper ownership and permissions
sudo chown -R instantdb:instantdb /var/lib/instantdb
sudo chmod 755 /var/lib/instantdb
sudo chmod 644 /var/lib/instantdb/*
```

**Log Directory:**
```bash
# Set log permissions
sudo chown -R instantdb:instantdb /var/log/instantdb
sudo chmod 755 /var/log/instantdb
sudo chmod 644 /var/log/instantdb/*
```

## Monitoring and Observability

### Health Checks

**Simple Health Check:**
```bash
# Check if server is responding
nc -z localhost 8080 && echo "Server is running" || echo "Server is down"

# Test WebSocket connection
curl -i -N -H "Connection: Upgrade" \
     -H "Upgrade: websocket" \
     -H "Sec-WebSocket-Version: 13" \
     -H "Sec-WebSocket-Key: test" \
     http://localhost:8080/
```

**Health Check Script:**
```bash
#!/bin/bash
# health-check.sh

PORT=${INSTANTDB_WS_PORT:-8080}
TIMEOUT=5

# Check if port is listening
if ! timeout $TIMEOUT nc -z localhost $PORT; then
    echo "ERROR: Server not responding on port $PORT"
    exit 1
fi

# Check WebSocket handshake
response=$(curl -s -o /dev/null -w "%{http_code}" \
    --max-time $TIMEOUT \
    -H "Connection: Upgrade" \
    -H "Upgrade: websocket" \
    -H "Sec-WebSocket-Version: 13" \
    -H "Sec-WebSocket-Key: test" \
    http://localhost:$PORT/)

if [ "$response" = "101" ]; then
    echo "OK: Server is healthy"
    exit 0
else
    echo "ERROR: WebSocket handshake failed (HTTP $response)"
    exit 1
fi
```

### Log Management

**Log Rotation (logrotate):**
```bash
# /etc/logrotate.d/instantdb
/var/log/instantdb/*.log {
    daily
    missingok
    rotate 30
    compress
    delaycompress
    notifempty
    sharedscripts
    postrotate
        systemctl reload instantdb
    endscript
}
```

**Centralized Logging:**
```bash
# Configure rsyslog to forward logs
echo "*.* @@log-server:514" >> /etc/rsyslog.conf
systemctl restart rsyslog
```

### Performance Monitoring

**Resource Usage Script:**
```bash
#!/bin/bash
# monitor.sh

PID=$(pgrep instantdb_server)

if [ -z "$PID" ]; then
    echo "InstantDB server not running"
    exit 1
fi

echo "InstantDB Server Monitoring"
echo "=========================="
echo "PID: $PID"
echo "CPU Usage: $(ps -p $PID -o %cpu --no-headers)%"
echo "Memory Usage: $(ps -p $PID -o %mem --no-headers)%"
echo "Memory (MB): $(ps -p $PID -o rss --no-headers | awk '{print $1/1024}')"
echo "Open Files: $(lsof -p $PID | wc -l)"
echo "Network Connections: $(netstat -pan | grep $PID | wc -l)"
```

## Security Considerations

### Network Security

**Firewall Configuration:**
```bash
# Allow only necessary ports
sudo ufw default deny incoming
sudo ufw default allow outgoing
sudo ufw allow 8080/tcp comment "InstantDB WebSocket"
# Only open the gRPC port to trusted hosts — it is unauthenticated and
# allows SQL execution and WASM module deployment:
sudo ufw allow from 10.0.0.0/8 to any port 50051 comment "InstantDB gRPC"
sudo ufw enable
```

**Reverse Proxy (nginx):**
```nginx
# /etc/nginx/sites-available/instantdb
upstream instantdb {
    server 127.0.0.1:8080;
}

server {
    listen 80;
    server_name your-domain.com;

    location / {
        proxy_pass http://instantdb;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
    }
}
```

### File System Security

**Secure Directories:**
```bash
# Restrict access to data directory
sudo chmod 700 /var/lib/instantdb
sudo chown instantdb:instantdb /var/lib/instantdb

# Secure log files
sudo chmod 640 /var/log/instantdb/*
sudo chown instantdb:adm /var/log/instantdb/*
```

**SELinux/AppArmor (if enabled):**
```bash
# Basic SELinux policy (example)
setsebool -P httpd_can_network_connect 1
semanage port -a -t http_port_t -p tcp 8080
```

## Backup and Recovery

### Data Backup

**Simple Backup Script:**
```bash
#!/bin/bash
# backup.sh

DATA_DIR="/var/lib/instantdb"
BACKUP_DIR="/backup/instantdb"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# Create backup directory
mkdir -p "$BACKUP_DIR"

# Stop service
systemctl stop instantdb

# Create backup
tar -czf "$BACKUP_DIR/instantdb_backup_$TIMESTAMP.tar.gz" -C "$(dirname $DATA_DIR)" "$(basename $DATA_DIR)"

# Start service
systemctl start instantdb

echo "Backup completed: $BACKUP_DIR/instantdb_backup_$TIMESTAMP.tar.gz"
```

**Automated Backup (cron):**
```bash
# Add to crontab
echo "0 2 * * * /usr/local/bin/backup.sh" | crontab -
```

### Disaster Recovery

**Recovery Steps:**
1. Stop the service
2. Restore data directory from backup
3. Verify file permissions
4. Start the service
5. Verify data integrity

```bash
#!/bin/bash
# restore.sh

BACKUP_FILE="$1"
DATA_DIR="/var/lib/instantdb"

if [ -z "$BACKUP_FILE" ]; then
    echo "Usage: $0 <backup_file>"
    exit 1
fi

# Stop service
systemctl stop instantdb

# Backup current data (safety)
mv "$DATA_DIR" "${DATA_DIR}.old.$(date +%s)"

# Restore from backup
mkdir -p "$(dirname $DATA_DIR)"
tar -xzf "$BACKUP_FILE" -C "$(dirname $DATA_DIR)"

# Fix permissions
chown -R instantdb:instantdb "$DATA_DIR"
chmod 755 "$DATA_DIR"

# Start service
systemctl start instantdb

echo "Recovery completed from $BACKUP_FILE"
```

## Troubleshooting

### Common Issues

**Server Won't Start:**
1. Check port availability: `netstat -tuln | grep 8080`
2. Verify data directory permissions
3. Check system logs: `journalctl -u instantdb -f`
4. Validate configuration

**High Memory Usage:**
1. Monitor with: `top -p $(pgrep instantdb_server)`
2. Check for memory leaks
3. Review data volume and retention
4. Consider memory limits

**Connection Issues:**
1. Verify firewall settings
2. Check network connectivity
3. Validate WebSocket handshake
4. Review nginx/proxy configuration

### Performance Tuning

**System Limits:**
```bash
# Increase file descriptor limits
echo "instantdb soft nofile 65536" >> /etc/security/limits.conf
echo "instantdb hard nofile 65536" >> /etc/security/limits.conf

# Increase network buffer sizes
echo "net.core.rmem_max = 268435456" >> /etc/sysctl.conf
echo "net.core.wmem_max = 268435456" >> /etc/sysctl.conf
```

**Application Tuning:**
- Use appropriate log levels (avoid debug in production)
- Monitor memory usage and tune accordingly
- Consider SSD storage for WAL files
- Optimize network settings for high-throughput scenarios