# ClusterFuzzLite base image
FROM gcr.io/oss-fuzz-base/base-builder:latest
RUN apt-get update && apt-get install -y cmake
COPY . $SRC/usearch
WORKDIR $SRC/usearch
COPY .clusterfuzzlite/build.sh $SRC/build.sh
