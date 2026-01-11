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
RUN rm -rf build && mkdir build

WORKDIR /Filesystems/build

EXPOSE 50051

RUN cmake .. && make
CMD ["./afs_server", "../Test_env/Filesystem_server"]

# at run time for the built image, you need to run docker run -p 50051:50051 -v $(pwd):/Filesystems/Test_env/Filesystem_server my-image

