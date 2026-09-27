# syntax=docker/dockerfile:1
# Optional full image (FetchContent llama.cpp). Prefer deploy/k3s hostPath + .local/api on WSL.
FROM ubuntu:26.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git libssl-dev pkg-config ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -B /build -DUSEARCH_BUILD_API=ON -DUSEARCH_BUILD_TEST_CPP=OFF -DUSEARCH_BUILD_BENCH_CPP=OFF \
        -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build --config Release --target api -j"$(nproc)"

FROM ubuntu:26.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 libgomp1 libssl3t64 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /build/api /app/api
ENV HOME=/app
EXPOSE 8088
ENTRYPOINT ["/app/api"]
CMD ["serve"]
