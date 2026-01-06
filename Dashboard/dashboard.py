import grpc
import sys
from pathlib import Path
from typing import List, Dict
sys.path.append(str(Path(__file__).parent.parent)) # root directory
from build import afs_operation_pb2_grpc, afs_operation_pb2
from fastapi import FastAPI
from contextlib import asynccontextmanager

class Dashboard:
    def __init__(self):
        # setting up connection
        self.channel = grpc.aio.insecure_channel("localhost:50051")
        stub = afs_operation_pb2_grpc.operatorStub(self.channel)
        self.connected_clients: List[str] = []
        self.file_to_clients: Dict[str, List[str]] = {}
    
    def __enter__(self):
        return self
    
    def __exit__(self):
        print("system shutdown, closing grpc connection")
        self.channel.close()

    def get_data(self):
        print("Retrieve data from the filesystem server ...")
        request = afs_operation_pb2.GetStatusRequest()
        try:
            response = self.stub.GetStatus(request)
        except grpc.RpcError as e:
            print("gRPC error:", e)
        
        # Access the repeated string field 
        connected_clients = list(response.connected_clients)
        self.connected_clients = connected_clients
        # Access the map<string, FileUsers> field
        file_to_clients_dict = {k: list(v.users) for k, v in response.file_to_clients.items()}
        self.file_to_clients = file_to_clients_dict

app = FastAPI()
dashboard = Dashboard()


















#grpc_clients = {}  # global variable to hold the shared connection

#@asynccontextmanager
#async def lifespan(app: FastAPI):   # fastAPI's way to handle start up and shutdown 
#    print("Connecting to the AFS server.......")
#    channel = grpc.aio.insecure_channel("localhost:50051")
#    stub = afs_operation_pb2_grpc.operatorsStub(channel)
#    grpc_clients["client"] = channel
#    grpc_clients["stub"] = stub

#    yield # the app runs while paused here

    # Shutdown
#    print("system shutdown, closing grpc connection")
#    await channel.close()
#