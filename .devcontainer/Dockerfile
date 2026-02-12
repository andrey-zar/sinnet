FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
         build-essential cmake ninja-build gdb lldb clang clang-tidy \
         pkg-config git python3 \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*
