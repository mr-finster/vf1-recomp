# The TrimUI build's container (tools/trimui/build.sh): Debian 11 arm64, GCC 10 and glibc 2.31 (the consoles' firmware
# has 2.33), so the game runs on the stock firmware and the custom ones. Native on Apple silicon and arm64 Linux;
# emulated (slower) on x86-64. Debian 11 is in the archive: its packages come from archive.debian.org.
FROM debian:bullseye
RUN printf 'deb http://archive.debian.org/debian bullseye main\ndeb http://archive.debian.org/debian-security bullseye-security main\n' > /etc/apt/sources.list \
 && echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99archive \
 && apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
    build-essential cmake ninja-build pkg-config git ca-certificates zlib1g-dev libasound2-dev xz-utils zip file \
 && rm -rf /var/lib/apt/lists/*
