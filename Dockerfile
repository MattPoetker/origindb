# Multi-stage build for OriginDB
FROM ubuntu:22.04 as builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libprotobuf-dev \
    protobuf-compiler \
    libgrpc++-dev \
    libssl-dev \
    pkg-config \
    ninja-build \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /src

# Copy source code
COPY . .

# Build OriginDB
RUN mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release -GNinja && \
    ninja

# Runtime stage
FROM ubuntu:22.04

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    libprotobuf32 \
    libgrpc++1.45 \
    libssl3 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Create OriginDB user
RUN groupadd -r origindb && useradd -r -g origindb origindb

# Create data directory
RUN mkdir -p /data && chown origindb:origindb /data

# Copy binaries from builder
COPY --from=builder /src/build/origindb_server /usr/local/bin/
COPY --from=builder /src/build/origindb_sql /usr/local/bin/
COPY --from=builder /src/build/origindb /usr/local/bin/

# Make binaries executable
RUN chmod +x /usr/local/bin/origindb*

# Switch to non-root user
USER origindb

# Expose ports
EXPOSE 8080 50051

# Create volume for data persistence
VOLUME ["/data"]

# Health check
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD origindb_sql "SELECT 1" || exit 1

# Default command
CMD ["origindb_server", "--data-dir", "/data", "--log-level", "info"]