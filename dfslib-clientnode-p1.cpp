#include <regex>
#include <vector>
#include <string>
#include <thread>
#include <cstdio>
#include <chrono>
#include <errno.h>
#include <csignal>
#include <iostream>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <getopt.h>
#include <unistd.h>
#include <limits.h>
#include <sys/inotify.h>
#include <grpcpp/grpcpp.h>

#include "dfslib-shared-p1.h"
#include "dfslib-clientnode-p1.h"
#include "proto-src/dfs-service.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientWriter;
using grpc::Status;
using grpc::StatusCode;

using dfs_service::DFSService;
using dfs_service::FileChunk;
using dfs_service::FileInfo;
using dfs_service::FilePath;
using dfs_service::FileStatus;
using dfs_service::ListFilesRequest;
using dfs_service::LSResponse;
using dfs_service::ResponseStatus;

//
// STUDENT INSTRUCTION:
//
// You may want to add aliases to your namespaced service methods here.
// All of the methods will be under the `dfs_service` namespace.
//
// For example, if you have a method named MyMethod, add
// the following:
//
//      using dfs_service::MyMethod
//

DFSClientNodeP1::DFSClientNodeP1() : DFSClientNode() {}

DFSClientNodeP1::~DFSClientNodeP1() noexcept {}

StatusCode DFSClientNodeP1::Store(const std::string &filename)
{

    //
    // STUDENT INSTRUCTION:
    //
    // Add your request to store a file here. This method should
    // connect to your gRPC service implementation method
    // that can accept and store a file.
    //
    // When working with files in gRPC you'll need to stream
    // the file contents, so consider the use of gRPC's ClientWriter.
    //
    // The StatusCode response should be:
    //
    // StatusCode::OK - if all went well
    // StatusCode::DEADLINE_EXCEEDED - if the deadline timeout occurs
    // StatusCode::NOT_FOUND - if the file cannot be found on the client
    // StatusCode::CANCELLED otherwise
    //
    std::string local_filepath = WrapPath(filename);
    // Check if the file exists
    std::ifstream infile(local_filepath, std::ios::in | std::ios::binary);
    if (!infile.is_open())
    {
        dfs_log(LL_ERROR) << "File not found: " << local_filepath;
        return StatusCode::NOT_FOUND;
    }

    // Create the context
    grpc::ClientContext context;
    // Set the metadata
    context.AddMetadata("filename", filename);
    // Set the deadline
    std::chrono::system_clock::time_point deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(deadline_timeout);
    context.set_deadline(deadline);
    // prepare the response
    ResponseStatus response;

    // Create a ClientWriter for the streaming call
    std::unique_ptr<grpc::ClientWriter<dfs_service::FileChunk>> writer(
        service_stub->storeFile(&context, &response));

    // Create a buffer for the file chunk
    char buffer[BUF_SIZE];
    int32_t chunk_num = 0;

    while (infile.read(buffer, BUF_SIZE) || infile.gcount())
    {
        // Create a FileChunk message
        dfs_service::FileChunk chunk;
        chunk.set_content(buffer, infile.gcount());
        chunk.set_chunk_num(chunk_num++);
        // Write the chunk
        if (!writer->Write(chunk))
        {
            dfs_log(LL_ERROR) << "Failed to write chunk to server";
            break;
        }
        dfs_log(LL_DEBUG) << "Sending chunk No. " << chunk_num << " size: " << infile.gcount();
    }

    // Close the writer
    writer->WritesDone();

    grpc::Status status = writer->Finish();
    infile.close();

    if (status.ok())
    {
        dfs_log(LL_SYSINFO) << "File stored successfully";
        return StatusCode::OK;
    }

    if (status.error_code() == grpc::DEADLINE_EXCEEDED)
    {
        dfs_log(LL_ERROR) << "Deadline exceeded";
        return StatusCode::DEADLINE_EXCEEDED;
    }
    else
    {
        dfs_log(LL_ERROR) << "Failed to store file: " << status.error_message();
        return StatusCode::CANCELLED;
    }
}

StatusCode DFSClientNodeP1::Fetch(const std::string &filename)
{

    //
    // STUDENT INSTRUCTION:
    //
    // Add your request to fetch a file here. This method should
    // connect to your gRPC service implementation method
    // that can accept a file request and return the contents
    // of a file from the service.
    //
    // As with the store function, you'll need to stream the
    // contents, so consider the use of gRPC's ClientReader.
    //
    // The StatusCode response should be:
    //
    // StatusCode::OK - if all went well
    // StatusCode::DEADLINE_EXCEEDED - if the deadline timeout occurs
    // StatusCode::NOT_FOUND - if the file cannot be found on the server
    // StatusCode::CANCELLED otherwise
    //
    //
    std::string local_filepath = WrapPath(filename);

    // Create the context
    grpc::ClientContext context;
    // Set the deadline
    std::chrono::system_clock::time_point deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(deadline_timeout);
    context.set_deadline(deadline);
    // prepare request
    FilePath request;
    request.set_path(filename);

    // Start request
    std::unique_ptr<grpc::ClientReader<dfs_service::FileChunk>> reader(service_stub->fetchFile(&context, request));

    // Create a buffer for the file chunk
    dfs_service::FileChunk chunk;
    int64_t bytes_written = 0;
    bool local_file_exists = false;
    std::ofstream outfile;

    try
    {
        while (reader->Read(&chunk))
        {
            if (local_file_exists == false)
            {
                local_file_exists = true;
                // prepare local file for writing
                outfile.open(local_filepath, std::ios::out | std::ios::binary);
                if (!outfile.is_open())
                {
                    dfs_log(LL_ERROR) << "Failed to open file for writing: " << local_filepath;
                    return StatusCode::INTERNAL;
                }
            }
            if (!outfile.is_open())
            {
                dfs_log(LL_ERROR) << "Failed to open file for writing: " << local_filepath;
                return StatusCode::INTERNAL;
            }
            const std::string &content = chunk.content();
            outfile.write(content.data(), content.size());
            bytes_written += content.size();
            dfs_log(LL_DEBUG) << "Receiving No." << chunk.chunk_num() << " chunk: " << content.size() << " bytes";
        }
        outfile.close();
    }
    catch (const std::exception &e)
    {
        dfs_log(LL_ERROR) << "Failed to send chunk to stream(from client to server)";
        outfile.close();
        return StatusCode::CANCELLED;
    }

    grpc::Status status = reader->Finish();
    if (status.ok())
    {
        dfs_log(LL_SYSINFO) << "File received successfully";
        return StatusCode::OK;
    }
    else if (status.error_code() == grpc::DEADLINE_EXCEEDED)
    {
        dfs_log(LL_ERROR) << "Deadline exceeded";
        return StatusCode::DEADLINE_EXCEEDED;
    }
    else if (status.error_code() == grpc::NOT_FOUND)
    {
        dfs_log(LL_ERROR) << "File not found";
        return StatusCode::NOT_FOUND;
    }
    else
    {
        dfs_log(LL_ERROR) << "Other Errors: " << status.error_message();
        return StatusCode::CANCELLED;
    }
}

StatusCode DFSClientNodeP1::Delete(const std::string &filename)
{

    //
    // STUDENT INSTRUCTION:
    //
    // Add your request to delete a file here. Refer to the Part 1
    // student instruction for details on the basics.
    //
    // The StatusCode response should be:
    //
    // StatusCode::OK - if all went well
    // StatusCode::DEADLINE_EXCEEDED - if the deadline timeout occurs
    // StatusCode::NOT_FOUND - if the file cannot be found on the server
    // StatusCode::CANCELLED otherwise
    //

    // Create the context
    grpc::ClientContext context;
    // Set the deadline
    std::chrono::system_clock::time_point deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(deadline_timeout);
    context.set_deadline(deadline);
    // prepare request
    dfs_service::FilePath request;
    request.set_path(filename);
    // prepare response
    dfs_service::ResponseStatus response;

    // Call the service
    grpc::Status status = service_stub->deleteFile(&context, request, &response);

    if (!status.ok())
    {
        if (status.error_code() == grpc::DEADLINE_EXCEEDED)
        {
            dfs_log(LL_ERROR) << "Deadline exceeded";
            return StatusCode::DEADLINE_EXCEEDED;
        }
        else if (status.error_code() == grpc::NOT_FOUND)
        {
            dfs_log(LL_ERROR) << "File not found";
            return StatusCode::NOT_FOUND;
        }
        else
        {
            dfs_log(LL_ERROR) << "Failed to delete file: " << status.error_message();
            return StatusCode::CANCELLED;
        }
    }

    return StatusCode::OK;
}

StatusCode DFSClientNodeP1::List(std::map<std::string, int> *file_map, bool display)
{

    //
    // STUDENT INSTRUCTION:
    //
    // Add your request to list all files here. This method
    // should connect to your service's list method and return
    // a list of files using the message type you created.
    //
    // The file_map parameter is a simple map of files. You should fill
    // the file_map with the list of files you receive with keys as the
    // file name and values as the modified time (mtime) of the file
    // received from the server.
    //
    // The StatusCode response should be:
    //
    // StatusCode::OK - if all went well
    // StatusCode::DEADLINE_EXCEEDED - if the deadline timeout occurs
    // StatusCode::CANCELLED otherwise
    //
    //

    // Create the context
    grpc::ClientContext context;
    // Set the deadline
    std::chrono::system_clock::time_point deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(deadline_timeout);
    context.set_deadline(deadline);
    // prepare request
    dfs_service::ListFilesRequest request;
    // prepare response
    dfs_service::LSResponse response;

    // Call the service
    grpc::Status status = service_stub->listFiles(&context, request, &response);

    if (!status.ok())
    {
        if (status.error_code() == grpc::DEADLINE_EXCEEDED)
        {
            dfs_log(LL_ERROR) << "Deadline exceeded";
            return StatusCode::DEADLINE_EXCEEDED;
        }
        else
        {
            dfs_log(LL_ERROR) << "Failed to list files: " << status.error_message();
            return StatusCode::CANCELLED;
        }
    }

    // fill the file_map
    for (const auto &file_info : response.filesinfolist())
    {
        dfs_log(LL_DEBUG) << "File: " << file_info.filename() << " - " << file_info.modified_time();
        file_map->insert(std::pair<std::string, int>(file_info.filename(), file_info.modified_time()));
    }

    return StatusCode::OK;
}

StatusCode DFSClientNodeP1::Stat(const std::string &filename, void *file_status)
{

    //
    // STUDENT INSTRUCTION:
    //
    // Add your request to get the status of a file here. This method should
    // retrieve the status of a file on the server. Note that you won't be
    // tested on this method, but you will most likely find that you need
    // a way to get the status of a file in order to synchronize later.
    //
    // The status might include data such as name, size, mtime, crc, etc.
    //
    // The file_status is left as a void* so that you can use it to pass
    // a message type that you defined. For instance, may want to use that message
    // type after calling Stat from another method.
    //
    // The StatusCode response should be:
    //
    // StatusCode::OK - if all went well
    // StatusCode::DEADLINE_EXCEEDED - if the deadline timeout occurs
    // StatusCode::NOT_FOUND - if the file cannot be found on the server
    // StatusCode::CANCELLED otherwise
    //
    //

    // Create the context
    grpc::ClientContext context;
    // Set the deadline
    std::chrono::system_clock::time_point deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(deadline_timeout);
    context.set_deadline(deadline);
    // prepare request
    dfs_service::FilePath request;
    request.set_path(filename);
    // prepare response
    dfs_service::FileStatus response;

    // Call the service
    grpc::Status status = service_stub->statusFile(&context, request, &response);

    if (!status.ok())
    {
        if (status.error_code() == grpc::DEADLINE_EXCEEDED)
        {
            dfs_log(LL_ERROR) << "Deadline exceeded";
            return StatusCode::DEADLINE_EXCEEDED;
        }
        else if (status.error_code() == grpc::NOT_FOUND)
        {
            dfs_log(LL_ERROR) << "File not found";
            return StatusCode::NOT_FOUND;
        }
        else
        {
            dfs_log(LL_ERROR) << "Failed to get file status: " << status.error_message();
            return StatusCode::CANCELLED;
        }
    }

    // define file_status as dfs_service::FileStatus
    file_status = &response;

    dfs_log(LL_DEBUG) << "File " << filename << " size: " << response.size() << " mtime: " << response.modified_time() << " ctime: " << response.creation_time();

    return StatusCode::OK;
}

//
// STUDENT INSTRUCTION:
//
// Add your additional code here, including
// implementations of your client methods
//
