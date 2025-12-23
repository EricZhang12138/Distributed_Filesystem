FROM ubuntu:latest

RUN apt-get update && apt-get install -y \ 
    build-essential \
    cmake \
    pkg-config \
    libfuse-dev \
    protobuf-compiler \
    libprotobuf-dev \
    libgrpc++-dev \
    libprotoc-dev \
    protobuf-compiler-grpc \
    libboost-all-dev 

# create Filesystems and then move into Filesystems
WORKDIR /Filesystems

# copy the contents in the directory where Dockerfile resides (first dot) into the /Filesystems directory
COPY . .

WORKDIR /Filesystems/build

EXPOSE 50051

RUN cmake .. && make
CMD ["./afs_server", "../Filesystem_server"]


