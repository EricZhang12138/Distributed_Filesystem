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
    protobuf-compiler-grpc

# create Filesystems and then move into Filesystems
WORKDIR /FileSystems

# copy the contents in the directory where Dockerfile resides (first dot) into the /Filesystems directory
COPY . .




#WORKDIR /build

#RUN cmake .. && make


