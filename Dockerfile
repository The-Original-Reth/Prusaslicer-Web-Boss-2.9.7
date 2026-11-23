# Prusaslicer - Web Boss 2.9.7.
#
# Compiles PrusaSlicer BOSS 2.9.7's slicing library (libslic3r) from source, using its own
# static dependency build, and links our engine (engine/) against it, inside BOSS's source
# tree, the same way BOSS builds its own command-line slicer. No GUI is built. The final
# image runs the gateway (gateway/), which serves the web page (web/) and starts one engine
# per open project.
#
# BOSS is pinned to its 2.9.7 release. To move to a newer 2.9.x release: build with
# --build-arg SLICER_COMMIT=<the release's commit>, test it, then change the default below.
# BOSS's dependencies are rebuilt only when a new commit changes them (deps/, cmake/).

ARG SLICER_OWNER_REPO=mjonuschat/PrusaSlicer
# BOSS 2.9.7, tag version_2.9.7+boss (2026-11-02)
ARG SLICER_COMMIT=2d0b5b5c86d6b9683799780ff7ba94a1dd41d4b4

########## the build tools ##########
# Debian bookworm: GCC 12 and CMake 3.25. Some of BOSS 2.9.7's bundled dependencies (Blosc's
# zlib) are old C that GCC 14 refuses, so the newer trixie toolchain doesn't build them.
FROM debian:bookworm AS toolchain

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates git build-essential autoconf automake libtool cmake ninja-build \
        pkg-config texinfo wget file m4 python3 nasm \
        zlib1g-dev libglu1-mesa-dev libgl1-mesa-dev libegl-dev libdbus-1-dev \
        libx11-dev libxext-dev libxi-dev libxrandr-dev libudev-dev && \
    rm -rf /var/lib/apt/lists/*

# Some dependency archives have non-ASCII file names; CMake can't unpack them under
# the default POSIX locale. C.UTF-8 is built into Debian, no locales package needed.
ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

########## BOSS's source ##########
FROM toolchain AS source
ARG SLICER_OWNER_REPO
ARG SLICER_COMMIT
# Just that commit; the history isn't needed, and dropping .git keeps the layers below
# depending on the source alone
RUN git init -q /src && \
    git -C /src fetch -q --depth 1 "https://github.com/${SLICER_OWNER_REPO}" "${SLICER_COMMIT}" && \
    git -C /src checkout -q FETCH_HEAD && \
    git -C /src log -1 --format='%H %cs' > /src/.source-commit && \
    rm -rf /src/.git

########## BOSS's dependencies ##########
# Boost, CGAL, OpenVDB, TBB, OCCT, ... built statically. The slow part. The GUI-only ones
# (wxWidgets, OpenCSG) are left out. Only deps/ and cmake/ are copied in, so a new release
# that leaves them alone reuses this layer. Only the installed result (destdir) is kept:
# the build trees are many GB, and a layer that big gets evicted from Docker's build cache.
FROM toolchain AS deps
# Parallel compile jobs. Each job can use ~2GB RAM while linking; lower this on
# small servers.
ARG BUILD_JOBS=4
COPY --from=source /src/deps /src/deps
COPY --from=source /src/cmake /src/cmake
WORKDIR /src/deps/build
# OpenSSL first, on one job: its old makefiles race when built in parallel
RUN cmake .. "-DPrusaSlicer_deps_PACKAGE_EXCLUDES=wxWidgets|OpenCSG" \
        -DDESTDIR=/src/deps/build/destdir && \
    cmake --build . --target dep_OpenSSL --parallel 1 && \
    cmake --build . --parallel "${BUILD_JOBS}" && \
    find . -mindepth 1 -maxdepth 1 ! -name destdir -exec rm -rf {} +

########## BOSS's libraries ##########
FROM deps AS build
ARG BUILD_JOBS=4
# BOSS takes libpng from the system on Linux; installed here rather than with the tools, so
# the dependency build above stays cached
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends libpng-dev && \
    rm -rf /var/lib/apt/lists/*
COPY --from=source /src /src-new
RUN cp -a /src-new/. /src/ && rm -rf /src-new

# The engine joins BOSS's source tree as one more target next to BOSS's own command-line
# slicer, and links the same libraries. They (and BOSS's CLI, handy for comparing results)
# are built here, before the engine's source is copied in, so changing the engine only
# recompiles the engine.
RUN echo 'if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/web-engine)\n  add_subdirectory(web-engine)\nendif()' \
        >> /src/src/CMakeLists.txt
WORKDIR /src
RUN cmake --preset linux-x86_64 -DSLIC3R_GUI=OFF -DSLIC3R_BUILD_TESTS=OFF && \
    cmake --build build --target PrusaSlicer --parallel "${BUILD_JOBS}"

########## the engine ##########
FROM build AS engine-build
ARG BUILD_JOBS=4
COPY engine /src/src/web-engine
# The settings tabs' layout comes from BOSS's own GUI source (see engine/tools/tab_layout.py)
RUN cmake --preset linux-x86_64 -DSLIC3R_GUI=OFF -DSLIC3R_BUILD_TESTS=OFF && \
    cmake --build build --target prusaslicer-web-engine --parallel "${BUILD_JOBS}" && \
    install -D build/src/web-engine/prusaslicer-web-engine /opt/engine/bin/prusaslicer-web-engine && \
    install -D build/src/prusa-slicer /opt/engine/bin/prusa-slicer && \
    cp /src/.source-commit /opt/engine/SOURCE && \
    cp -r /src/resources /opt/engine/resources && \
    mkdir -p /opt/engine/resources/web && \
    python3 /src/src/web-engine/tools/tab_layout.py /src/src/slic3r/GUI/Tab.cpp \
        /src/src/slic3r/GUI/GUI_Factories.cpp /opt/engine/resources/web/tab_layout.json

########## the web page ##########
FROM node:22-bookworm-slim AS web-build
WORKDIR /web
COPY web/package.json web/package-lock.json ./
RUN npm ci
COPY web ./
RUN npm run build

########## the engine alone (for engine tests) ##########
FROM debian:bookworm-slim AS engine

ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    ENGINE_RESOURCES=/opt/engine/resources

# BOSS's own libraries are linked statically; a few system ones are left
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libpng16-16 libdbus-1-3 libgl1 libglu1-mesa libegl1 libudev1 ca-certificates && \
    rm -rf /var/lib/apt/lists/*

# The engine, plus the profiles, shapes and G-code templates it reads at run time
COPY --from=engine-build /opt/engine /opt/engine

# One project per process; JSON commands on stdin, events on stdout (see engine/src/main.cpp)
ENTRYPOINT ["/opt/engine/bin/prusaslicer-web-engine"]

########## the app: gateway + web page + engine ##########
FROM engine AS app

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends python3 python3-venv && \
    rm -rf /var/lib/apt/lists/*

COPY gateway/requirements.txt /opt/gateway/requirements.txt
RUN python3 -m venv /opt/gateway/venv && \
    /opt/gateway/venv/bin/pip install --no-cache-dir -r /opt/gateway/requirements.txt

COPY gateway /opt/gateway
COPY --from=web-build /web/dist /opt/web

ENV ENGINE_BIN=/opt/engine/bin/prusaslicer-web-engine \
    WEB_DIR=/opt/web \
    DATA_DIR=/data
VOLUME /data
EXPOSE 8080
WORKDIR /opt/gateway
ENTRYPOINT []
CMD ["/opt/gateway/venv/bin/uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8080"]
