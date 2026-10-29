FROM --platform=linux/amd64 ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential git python3 bzip2 unzip ca-certificates libncurses5 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
