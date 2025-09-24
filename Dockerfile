# Multi-stage build for InstantDB
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

# Build InstantDB
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

# Create InstantDB user
RUN groupadd -r instantdb && useradd -r -g instantdb instantdb

# Create data directory
RUN mkdir -p /data && chown instantdb:instantdb /data

# Copy binaries from builder
COPY --from=builder /src/build/instantdb_server /usr/local/bin/
COPY --from=builder /src/build/instantdb_sql /usr/local/bin/
COPY --from=builder /src/build/instantdb /usr/local/bin/

# Make binaries executable
RUN chmod +x /usr/local/bin/instantdb*

# Switch to non-root user
USER instantdb

# Expose ports
EXPOSE 8080 50051

# Create volume for data persistence
VOLUME ["/data"]

# Health check
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD instantdb_sql "SELECT 1" || exit 1

# Default command
CMD ["instantdb_server", "--data-dir", "/data", "--log-level", "info"]