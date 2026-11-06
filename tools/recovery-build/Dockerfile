FROM debian:bookworm-slim@sha256:734a72b3d6142a71993b4bc580ca01d8ca687f77a2c422d96a961d3376b179bc
RUN apt-get update && apt-get install -y --no-install-recommends \
    autoconf automake libtool pkg-config make gcc g++ gcc-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu linux-libc-dev-arm64-cross qemu-user python3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
