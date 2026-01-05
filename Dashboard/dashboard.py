import grpc
import sys
from pathlib import Path
sys.path.append(str(Path(__file__).parent.parent)) # root directory
from build import afs_operation_pb2_grpc, afs_operation_pb2
from fastapi import FastAPI
from contextlib import asynccontextmanager

grpc_clients = {}  # global variable to hold the shared connection

@asynccontextmanager
async def lifespan(app: FastAPI):   # fastAPI's way to handle start up and shutdown 
    print("Connecting to the AFS server.......")
    channel = grpc.aio.insecure_channel("localhost:50051")
    stub = afs_operation_pb2_grpc.operatorsStub(channel)
    grpc_clients["client"] = channel
    grpc_clients["stub"] = stub

    yield # the app runs while paused here

    # Shutdown
    print("system shutdown, closing grpc connection")
    await channel.close()