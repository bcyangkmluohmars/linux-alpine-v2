FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    gcc-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    bc \
    bison \
    flex \
    libssl-dev \
    libelf-dev \
    kmod \
    cpio \
    device-tree-compiler \
    python3 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ENV ARCH=arm64
ENV CROSS_COMPILE=aarch64-linux-gnu-

WORKDIR /build
