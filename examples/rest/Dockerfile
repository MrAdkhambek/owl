# syntax=docker/dockerfile:1
#
# The build context is the repository root, because the example vendors
# owl with add_subdirectory(../..). From the repository root:
#
#   docker compose -f examples/rest/docker-compose.yml up --build
#
# Two stages: everything owl needs to compile, then a slim image with the
# binary and its shared libraries. Two things are not in Ubuntu's archive at
# the versions owl needs and are fetched here: CMake 4.3 (apt has 3.28) and
# libh2o-evloop from h2o's master (apt has h2o 2.2; owl targets 2.3). The
# compiler is GCC 14: Ubuntu's clang 18 cannot use libstdc++'s <expected>
# (it reports __cpp_concepts below what the header demands), and libc++ 18
# has no floating-point from_chars.

FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git pkg-config ninja-build \
        gcc-14 g++-14 \
        libssl-dev zlib1g-dev libuv1-dev libpq-dev libhiredis-dev \
    && rm -rf /var/lib/apt/lists/*

ARG CMAKE_VERSION=4.3.0
RUN arch="$(uname -m)" \
    && curl -fsSL "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${arch}.tar.gz" \
       | tar -xz -C /opt \
    && ln -s "/opt/cmake-${CMAKE_VERSION}-linux-${arch}/bin/"* /usr/local/bin/

ENV CC=gcc-14 CXX=g++-14

ARG H2O_REF=master
RUN git clone --depth 1 --branch "${H2O_REF}" --recurse-submodules --shallow-submodules https://github.com/h2o/h2o.git /tmp/h2o \
    && cmake -S /tmp/h2o -B /tmp/h2o/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DWITH_MRUBY=OFF \
    && cmake --build /tmp/h2o/build \
    && cmake --install /tmp/h2o/build \
    && rm -rf /tmp/h2o

WORKDIR /src
COPY . /src
# No C++ modules here: scanning for them would need clang-scan-deps.
RUN cmake -S examples/rest -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    && cmake --build /build --target rest

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3t64 zlib1g libpq5 libhiredis1.1.0 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /build/rest /usr/local/bin/rest
# The compose file sets these; the defaults make a bare `docker run` work
# against a postgres and a redis on the same network.
ENV REST_ADDRESS=0.0.0.0 REST_PORT=8080 \
    REST_PG=postgres://rest:rest@postgres:5432/rest \
    REST_REDIS=redis:6379
EXPOSE 8080
CMD ["rest"]
