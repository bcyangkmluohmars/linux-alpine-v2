FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    wget \
    qemu-user-static \
    binfmt-support \
    tar \
    gzip \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY build-rootfs.sh /build/build-rootfs.sh
RUN chmod +x /build/build-rootfs.sh

WORKDIR /build
CMD ["/build/build-rootfs.sh"]
