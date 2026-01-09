import grpc
import sys
from pathlib import Path
from typing import List, Dict
sys.path.append(str(Path(__file__).parent.parent)) # root directory
sys.path.append(str(Path(__file__).parent.parent / "build"))
from build import afs_operation_pb2_grpc, afs_operation_pb2
from fastapi import FastAPI
from contextlib import asynccontextmanager
from fastapi.middleware.cors import CORSMiddleware 

class Dashboard:
    def __init__(self):
        # setting up connection
        self.channel = None #grpc.aio.insecure_channel("localhost:50051")
        self.stub = None #afs_operation_pb2_grpc.operatorStub(self.channel)
        self.connected_clients: List[str] = []
        self.file_to_clients: Dict[str, List[str]] = {}
    
    def __enter__(self):
        print("Connecting to the AFS server...")
        self.channel = grpc.insecure_channel("localhost:50051")
        self.stub = afs_operation_pb2_grpc.operatorsStub(self.channel)
        return self
    
    def __exit__(self, exc_type, exc_val, exc_tb):
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

# this FastAPI is like a API for the frontend and backend 
app = FastAPI()
dashboard = Dashboard()

# allow frontend to call this API from a different origin
# in this case, we allow anyone to access this backend with any methods and any headers
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"], #In production, we could specify the specific frontend URL
    allow_credentials = True,
    allow_methods = ["*"],
    allow_headers=["*"],
)

@app.get("/api/status")
def get_status():
    " Get current filesystem status "
    with dashboard: 
        dashboard.get_data()
        data = {
            "connected_clients": dashboard.connected_clients,
            "file_to_clients": dashboard.file_to_clients
        }
        return data


# uvicorn dashboard:app --reload --host 0.0.0.0 --port 8000
# host 0.0.0.0 means it listens for all ip addresses 
# --port 8000, listens on port 8000
















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