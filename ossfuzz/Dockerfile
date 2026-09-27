FROM gcr.io/oss-fuzz-base/base-builder
RUN apt-get update && apt-get install -y cmake
RUN git clone --depth 1 https://github.com/unum-cloud/USearch.git usearch
WORKDIR usearch
COPY build.sh $SRC/
