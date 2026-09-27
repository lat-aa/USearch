# syntax=docker/dockerfile:1
# Optional full image build (FetchContent llama.cpp). Prefer deploy/k3s hostPath + .local/api for WSL.
FROM alpine:3.20 AS build
RUN apk add --no-cache build-base cmake git openssl-dev linux-headers
WORKDIR /src
COPY . .
RUN cmake -B /build -DUSEARCH_BUILD_API=ON -DUSEARCH_BUILD_TEST_CPP=OFF -DUSEARCH_BUILD_BENCH_CPP=OFF \
        -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build --config Release --target api -j"$(nproc)"

FROM alpine:3.20
RUN apk add --no-cache libstdc++ libgomp openssl ca-certificates
WORKDIR /app
COPY --from=build /build/api /app/api
ENV HOME=/app
EXPOSE 8088
ENTRYPOINT ["/app/api"]
CMD ["serve"]
