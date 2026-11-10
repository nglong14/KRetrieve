# One image for every role: `docker run IMAGE shard|coordinator|loadgen|prepare|query ...`.
# Data is not baked in: mount the output of `kretrieve prepare` at /data.
FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++-13 cmake make \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
RUN cmake -S . -B build -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=g++-13 \
 && cmake --build build -j"$(nproc)"

FROM ubuntu:24.04
# The base image already ships a non-root `ubuntu` user (uid 1000).
COPY --from=build /src/build/kretrieve /usr/local/bin/kretrieve
USER ubuntu
WORKDIR /data
# Exec form so the process is PID 1 and receives SIGTERM from `docker stop`.
ENTRYPOINT ["kretrieve"]
