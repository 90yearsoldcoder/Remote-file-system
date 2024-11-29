#include <map>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <errno.h>
#include <iostream>
#include <fstream>
#include <getopt.h>
#include <dirent.h>
#include <sys/stat.h>
#include <grpcpp/grpcpp.h>

#include "src/dfs-utils.h"
#include "dfslib-shared-p1.h"
#include "dfslib-servernode-p1.h"
#include "proto-src/dfs-service.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerWriter;
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
// DFSServiceImpl is the implementation service for the rpc methods
// and message types you defined in the `dfs-service.proto` file.
//
// You should add your definition overrides here for the specific
// methods that you created for your GRPC service protocol. The
// gRPC tutorial described in the readme is a good place to get started
// when trying to understand how to implement this class.
//
// The method signatures generated can be found in `proto-src/dfs-service.grpc.pb.h` file.
//
// Look for the following section:
//
//      class Service : public ::grpc::Service {
//
// The methods returning grpc::Status are the methods you'll want to override.
//
// In C++, you'll want to use the `override` directive as well. For example,
// if you have a service method named MyMethod that takes a MyMessageType
// and a ServerWriter, you'll want to override it similar to the following:
//
//      Status MyMethod(ServerContext* context,
//                      const MyMessageType* request,
//                      ServerWriter<MySegmentType> *writer) override {
//
//          /** code implementation here **/
//      }
//
class DFSServiceImpl final : public DFSService::Service
{

private:
    /** The mount path for the server **/
    std::string mount_path;

    /**
     * Prepend the mount path to the filename.
     *
     * @param filepath
     * @return
     */
    const std::string WrapPath(const std::string &filepath)
    {
        return this->mount_path + filepath;
    }

public:
    DFSServiceImpl(const std::string &mount_path) : mount_path(mount_path)
    {
    }

    ~DFSServiceImpl() {}

    //
    // STUDENT INSTRUCTION:
    //
    // Add your additional code here, including
    // implementations of your protocol service methods
    //
    ::grpc::Status storeFile(::grpc::ServerContext *context,
                             ::grpc::ServerReader<::dfs_service::FileChunk> *reader,
                             ::dfs_service::ResponseStatus *response) override
    {
        // get the filename from the metadata
        const auto &metadata = context->client_metadata();
        auto iter = metadata.find("filename");

        if (iter == metadata.end())
        {
            dfs_log(LL_ERROR) << "Filename not found in metadata";
            return grpc::Status(StatusCode::CANCELLED, "Filename not found in metadata");
        }

        std::string filename = std::string(iter->second.data(), iter->second.size());
        // wrap the path
        std::string filepath = WrapPath(filename);

        // open the file to writie the chunks
        std::ofstream outfile(filepath, std::ios::out | std::ios::binary);
        if (!outfile.is_open())
        {
            dfs_log(LL_ERROR) << "Failed to open file for writing: " << filepath;
            return grpc::Status(StatusCode::INTERNAL, "Failed to open file for writing");
        }

        dfs_service::FileChunk chunk;
        int64_t bytes_written = 0;
        while (reader->Read(&chunk))
        {
            const std::string &content = chunk.content();
            outfile.write(content.data(), content.size());
            bytes_written += content.size();

            // Check for client cancellation
            if (context->IsCancelled())
            {
                dfs_log(LL_SYSINFO) << "Client cancelled the request.";
                outfile.close();
                return grpc::Status(StatusCode::DEADLINE_EXCEEDED, "Client cancelled the request.");
            }

            // For debugging purposes
            dfs_log(LL_DEBUG) << "Writing " << chunk.chunk_num() << " chunk: " << content.size() << " bytes";
        }
        outfile.close();

        response->set_descstatus("File stored successfully");

        return grpc::Status::OK;
    }

    ::grpc::Status fetchFile(::grpc::ServerContext *context,
                             const ::dfs_service::FilePath *request,
                             ::grpc::ServerWriter<::dfs_service::FileChunk> *writer) override
    {
        std::string wrapedPath = WrapPath(request->path());
        std::ifstream infile(wrapedPath, std::ios::in | std::ios::binary);
        // check if the file exists
        if (!infile.is_open())
        {
            dfs_log(LL_ERROR) << "File not found: " << wrapedPath;
            return grpc::Status(StatusCode::NOT_FOUND, "File not found");
        }

        // prepare the buffer to read the file
        char buffer[BUF_SIZE];
        int32_t chunk_num = 0;
        while (infile.read(buffer, BUF_SIZE) || infile.gcount())
        {
            dfs_service::FileChunk chunk;
            chunk.set_content(buffer, infile.gcount());
            chunk.set_chunk_num(chunk_num++);
            if (context->IsCancelled())
            {
                dfs_log(LL_SYSINFO) << "Client cancelled the request.";
                infile.close();
                return grpc::Status(StatusCode::DEADLINE_EXCEEDED, "Client cancelled the request.");
            }
            if (!writer->Write(chunk))
            {
                dfs_log(LL_ERROR) << "Failed to write chunk to stream(from server to clinet)";
                infile.close();
                return grpc::Status(StatusCode::CANCELLED, "Failed to write chunk to client");
            }
            dfs_log(LL_DEBUG) << "Writing chunk: " << chunk_num << " size: " << infile.gcount();
        }
        infile.close();

        return grpc::Status(StatusCode::OK, "File sent successfully");
    }

    ::grpc::Status deleteFile(::grpc::ServerContext *context,
                              const ::dfs_service::FilePath *request,
                              ::dfs_service::ResponseStatus *response) override
    {
        // check if the context is cancelled
        if (context->IsCancelled())
        {
            dfs_log(LL_SYSINFO) << "Client cancelled the request.";
            return grpc::Status(StatusCode::DEADLINE_EXCEEDED, "Client cancelled the request.");
        }

        std::string path = WrapPath(request->path());
        // check if the file exists
        struct stat file_stat;
        if (stat(path.c_str(), &file_stat) != 0)
        {
            dfs_log(LL_ERROR) << "File not found: " << path;
            return grpc::Status(StatusCode::NOT_FOUND, "File not found");
        }

        // remove the file
        if (std::remove(path.c_str()) != 0)
        {
            dfs_log(LL_ERROR) << "Failed to delete file: " << path;
            return grpc::Status(StatusCode::CANCELLED, "Failed to delete file");
        }

        dfs_log(LL_SYSINFO) << "File deleted successfully";
        return grpc::Status::OK;
    }

    ::grpc::Status listFiles(::grpc::ServerContext *context,
                             const ::dfs_service::ListFilesRequest *request,
                             ::dfs_service::LSResponse *response) override
    {
        // Open the directory
        DIR *dir = opendir(mount_path.c_str());
        if (dir == nullptr)
        {
            dfs_log(LL_ERROR) << "Failed to open directory: " << strerror(errno);
            return grpc::Status(grpc::INTERNAL, "Failed to open directory.");
        }

        // Read the directory
        struct dirent *entry;
        struct stat file_stat;
        while ((entry = readdir(dir)) != nullptr)
        {
            if (context->IsCancelled())
            {
                dfs_log(LL_SYSINFO) << "Client cancelled the request.";
                closedir(dir);
                return grpc::Status(StatusCode::DEADLINE_EXCEEDED, "Client cancelled the request.");
            }

            const std::string filename = entry->d_name;
            const std::string wrapped_path = WrapPath(filename);
            if (stat(wrapped_path.c_str(), &file_stat) != 0)
            {
                dfs_log(LL_ERROR) << "Failed to stat file: " << wrapped_path;
                continue;
            }

            // record the file info
            dfs_service::FileInfo *file_info = response->add_filesinfolist();
            file_info->set_filename(filename);
            file_info->set_modified_time(file_stat.st_mtime);
        }

        closedir(dir);
        dfs_log(LL_SYSINFO) << "Sent file lists successfully";

        return grpc::Status::OK;
    }

    ::grpc::Status statusFile(::grpc::ServerContext *context,
                              const ::dfs_service::FilePath *request,
                              ::dfs_service::FileStatus *response) override
    {
        std::string path = WrapPath(request->path());
        struct stat file_stat;

        if (context->IsCancelled())
        {
            dfs_log(LL_SYSINFO) << "Client cancelled the request.";
            return grpc::Status(StatusCode::DEADLINE_EXCEEDED, "Client cancelled the request.");
        }

        if (stat(path.c_str(), &file_stat) != 0)
        {
            dfs_log(LL_ERROR) << "Failed to stat file: " << path;
            return grpc::Status(StatusCode::NOT_FOUND, "File not found");
        }

        response->set_size(file_stat.st_size);
        response->set_modified_time(file_stat.st_mtime);
        response->set_creation_time(file_stat.st_ctime);
        dfs_log(LL_DEBUG) << "File " << path << " size: " << file_stat.st_size << " mtime: " << file_stat.st_mtime << " ctime: " << file_stat.st_ctime;
        dfs_log(LL_SYSINFO) << "File status retrieved successfully";

        return grpc::Status::OK;
    }
};

//
// STUDENT INSTRUCTION:
//
// The following three methods are part of the basic DFSServerNode
// structure. You may add additional methods or change these slightly,
// but be aware that the testing environment is expecting these three
// methods as-is.
//
/**
 * The main server node constructor
 *
 * @param server_address
 * @param mount_path
 */
DFSServerNode::DFSServerNode(const std::string &server_address,
                             const std::string &mount_path,
                             std::function<void()> callback) : server_address(server_address), mount_path(mount_path), grader_callback(callback) {}

/**
 * Server shutdown
 */
DFSServerNode::~DFSServerNode() noexcept
{
    dfs_log(LL_SYSINFO) << "DFSServerNode shutting down";
    this->server->Shutdown();
}

/** Server start **/
void DFSServerNode::Start()
{
    DFSServiceImpl service(this->mount_path);
    ServerBuilder builder;
    builder.AddListeningPort(this->server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    this->server = builder.BuildAndStart();
    dfs_log(LL_SYSINFO) << "DFSServerNode server listening on " << this->server_address;
    this->server->Wait();
}

//
// STUDENT INSTRUCTION:
//
// Add your additional DFSServerNode definitions here
//
