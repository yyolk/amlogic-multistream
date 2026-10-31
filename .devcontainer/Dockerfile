FROM mcr.microsoft.com/devcontainers/cpp:ubuntu24.04

# Compiler, CMake, pkg-config, FFmpeg, libmosquitto, and libdrm so
# `cmake`, `layout_engine_tests`, and the player all configure.
# curl, patchelf, file, and binutils are for scripts/build-appimage.sh.
# Keep this list aligned with .github/workflows/ci.yml.
USER root
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get install -y --no-install-recommends \
        binutils \
        ca-certificates \
        cmake \
        curl \
        ffmpeg \
        file \
        g++ \
        libavcodec-dev \
        libavformat-dev \
        libavutil-dev \
        libdrm-dev \
        libfuse2t64 \
        libmosquitto-dev \
        libswscale-dev \
        patchelf \
        pkg-config \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*
USER vscode
