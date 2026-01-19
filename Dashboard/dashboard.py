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
import psutil

class Dashboard:
    def __init__(self):
        # setting up connection
        self.channel = None #grpc.aio.insecure_channel("localhost:50051")
        self.stub = None #afs_operation_pb2_grpc.operatorStub(self.channel)
        self.connected_clients: List[str] = []
        self.file_to_clients: Dict[str, List[str]] = {}
        self.server_base_dir: str

    def __enter__(self):
        print("Connecting to the AFS server...")
        self.channel = grpc.insecure_channel("localhost:50051")
        self.stub = afs_operation_pb2_grpc.operatorsStub(self.channel)
        self.get_root_dir()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        print("system shutdown, closing grpc connection")
        self.channel.close()
    
    def get_root_dir(self):
        print("Dashboard getting root directory of the server")
        request = afs_operation_pb2.InitialiseRequest()
        request.code_to_initialise = "I want input/output directory"
        try:
            response = self.stub.request_dir(request)
            self.server_base_dir = response.root_path
        except grpc.RpcError as e:
            print("gRPC error: ", e)
            self.server_base_dir = ""
    
    
    def get_process_metric(self):
        server_process = None
        print("Track the server's process states")
        for proc in psutil.process_iter(['pid', 'name']):
            # Check for afs_server (the actual binary name)
            if proc.info['name'] == 'afs_server':
                server_process = proc
                break
        if not server_process:
            print("Server process not found!")
            return None

        cpu_percent = server_process.cpu_percent(interval=0.1)  # shorter interval for responsiveness
        memory_info = server_process.memory_info()
        num_threads = server_process.num_threads()

        return {
            "pid": server_process.pid,
            "cpu_percent": cpu_percent,
            "memory_rss_mb": round(memory_info.rss / (1024 * 1024), 2),  # Convert to MB
            "memory_vms_mb": round(memory_info.vms / (1024 * 1024), 2),
            "num_threads": num_threads
        }


        
    def get_application_metric(self):
        print("Retrieve data from the filesystem server ...")
        request = afs_operation_pb2.GetStatusRequest()
        try:
            response = self.stub.GetStatus(request)
        except grpc.RpcError as e:
            print("gRPC error:", e)

        # Access the repeated string field
        connected_clients = list(response.connected_clients)
        self.connected_clients = connected_clients

        # Access the map<string, FileUsers> field and strip base directory
        file_to_clients_dict = {}
        for filepath, users in response.file_to_clients.items():
            # Strip the server base directory from the filepath
            if self.server_base_dir and filepath.startswith(self.server_base_dir):
                stripped_path = filepath[len(self.server_base_dir):]
                # Ensure path starts with / if not empty
                if stripped_path and not stripped_path.startswith('/'):
                    stripped_path = '/' + stripped_path
                file_to_clients_dict[stripped_path or '/'] = list(users.users)
            else:
                file_to_clients_dict[filepath] = list(users.users)

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
        dashboard.get_application_metric()
        
        process_metric = dashboard.get_process_metric()
        data = {
            "connected_clients": dashboard.connected_clients,
            "file_to_clients": dashboard.file_to_clients,
            "process": process_metric  # Will be None if server not found
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