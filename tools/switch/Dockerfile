# Switch build tools, following R-drg/the-simpsons-game-nx's toolchain.
# Pin the same devkitA64 base digest already used by this machine's prior Switch builds.
FROM devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        clang-19 \
        glslang-tools \
        cmake \
        ninja-build \
        python3 \
        python3-pip \
        git \
        pkg-config \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /project
