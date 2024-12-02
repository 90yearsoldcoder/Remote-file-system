# 1. The design of the Project

Overrall, this is a remote file system that support basic operations, including fetch, store, list and status.

## 1.1 The design of RPC

This project uses gRPC (Google Remote Procedure Call) to implement remote calls, with the IDL (Interface Definition Language) based on Protocol Buffers.

### 1.1.1 rpc: Store File

```
rpc storeFile(stream FileChunk) returns (ResponseStatus){}
```

This rpc sends a file to server using a `steam` to the server chunk by chunk. And the file name is saved into the request's metadata. After saving the file, the server will return an unary called `ResponseStatus` to the client, indicating the result of the store operation in server.

### 1.1.2 rpc: Fetch File

```
rpc fetchFile(FilePath) returns (stream FileChunk){}
```

This rpc request a file from client to server with an unary called `FilePath` that refers to the file's name(or filepath if needed). Then, the server will send a stream called `FileChunk` back to client.

### 1.1.3 rpc: Delete File

```
rpc deleteFile(FilePath) returns (ResponseStatus){}
```

This rpc sends a filename(or filepath in future) from client to server, asking the server to delete the file. After the server removed the file, it will send an unbary `ResponseStatus` to client, indicating the result of the delete operation.

### 1.1.4 rpc: List out files

```
rpc listFiles(ListFilesRequest) returns (LSResponse){}
message FileInfo{
    string fileName = 1;
    int64 modified_time = 2;
}

message LSResponse{
    repeated FileInfo filesInfoList = 1;
}
```

The client first sends an empty `ListFilesRequest` to server. Then, server will send back a list of `fileInfo`(or `repeated type` in protocol buffer) to client, indicating the information of all files in server.
**Note:** Since the rpc itself defines the order, client do not need to send extra information.

### 1.1.5 rpc: Request file status

```
rpc statusFile(FilePath) returns (FileStatus){}
```

This rpc sends a filename from client to server, asking the server the detail status about the file. Sever then send back the detail in `FileStatus` to client.

## 1.2 The design of the client

Overrall, the design of the client is `quite simple`. Basicly, you can say there is no specific design for the client.

- First, the `src\dfs-client-p1.cpp` will parse the parameters(e.g. the server address, mount path, command) and set the `clientNode`.
- According to the command, the `clientNode` (`dfslib-clientnode-p1.cpp`) will run different function. For example, `Fetch` command will be done by `Fetch` function.
- Those function will implement the specific rpc to do the task.

## 1.3 The design of the server

The server is quite straightforward as well.

- First, the server (`src\dfs-server-p1.cpp`) will parse the parameters(e.g. mount path, server address), and start a `DFSServerNode`(`dfslib-servernode-p1.cpp`).
- The `DFSServerNode` builds a gRPC server with `DFSServiceImpl` service.
- `DFSServiceImpl` handles the gRPC, following the gRPC function format.

# 2. Flow Control

## 2.1 Flow Control for client

- Execute the appropriate function based on the command. For example, the `Fetch` command will invoke the Fetch function.
- Create a context for RPC;
- Set the metadata, such as the file name, if required.
- Send the request via gRPC;
- Handle the response, which could be unary or a stream
  - Use a writer to send the file stream chunk by chunk.
  - Use a reader to process the received stream and write the content to a file.
- Handle the errors;
- Return the final status of operation's result;

## 2.2 Flow Control for server

- Set up the server
- The functions are in gRPC format.
- Get the metadata, if necessary.
- Handle the request with writer or reader.
  - For `Store` request, use reader to read the stream and save it to the file;
  - For `Fetch` request, use writer to write the file content to the stream;
  - For other requests, collect the information and send them to client via `&response`
- set error/ok informtion to response appropriately.
- return `grpc::status` appropriately.

# 3. Implementation

To compile

```
make protos
make part1
```

To run the server

```
./bin/dfs-server-p1
```

To run the client

```
./bin/dfs-client-p1 <command> <optional, file>
```

# 4. Test

I checked the mount folders after runing a following command.

## 4.1 Test `list` command

```
./bin/dfs-client-p1 list
./bin/dfs-client-p1 -t 1 list
```

**Note:** The second command is for test if the server/client acts right when the requst is abolished

## 4.2 Test `status` command

```
./bin/dfs-client-p1 stat testfile_cloud.txt
./bin/dfs-client-p1 stat wrong_file
./bin/dfs-client-p1 -t 1 stat testfile_cloud.txt
```

**Note:** `wrong_file` does not exist.

## 4.3 Test `store` command

```
./bin/dfs-client-p1 store testfile_local.txt
./bin/dfs-client-p1 store build_db.py
./bin/dfs-client-p1 store not_exist_file.txt
./bin/dfs-client-p1 -t 1 store testfile_local.txt
```

**Note:** The first two command is for testing different size/type of files.

## 4.4 Test `delete` command

```
./bin/dfs-client-p1 delete testfile_local.txt
./bin/dfs-client-p1 delete not_exist_file.txt
./bin/dfs-client-p1 delete -t 1 build_db.py
```

## 4.5 Test `fetch` command

```
./bin/dfs-client-p1 fetch testfile_cloud.txt
./bin/dfs-client-p1 fetch not_exist_file.txt
./bin/dfs-client-p1 fetch -t 1 testfile_cloud.txt
```

**Note:** not_exist_file.txt does not exist in both client and server.
