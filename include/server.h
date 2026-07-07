#ifndef OSM_SERVER_H
#define OSM_SERVER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "data_model.h"

namespace httplib {
class Server;
}

class Server {
public:
    Server(const OSMDataset& data, int port = 8080, int defaultHouseLimit = 1500);
    ~Server();

    bool start();
    void stop();

private:
    struct ServerResponse {
        int statusCode;
        std::string contentType;
        std::string body;
    };
    struct ServerBuildMetrics {
        double spatialIndexBuildMs = 0.0;
        double geocodeIndexBuildMs = 0.0;
        uint64_t geocodePostingRefs = 0;
        uint64_t geocodePrimaryPostingRefs = 0;
        uint64_t geocodeLargestPostingList = 0;
        uint64_t geocodeLargestPrimaryPostingList = 0;
        uint64_t geocodeEstimatedBytes = 0;
        uint64_t houseGridCells = 0;
        uint64_t streetGridCells = 0;
        uint64_t adminGridCells = 0;
        uint64_t poiGridCells = 0;
    };

    const OSMDataset& data_;
    int port_;
    int defaultHouseLimit_;
    int32_t houseIndexCellSizeE7_;
    int32_t streetIndexCellSizeE7_;
    int32_t adminIndexCellSizeE7_;
    std::unordered_map<int64_t, std::vector<uint32_t>> houseIndex_;
    std::unordered_map<int64_t, std::vector<uint32_t>> streetIndex_;
    std::unordered_map<int64_t, std::vector<uint32_t>> adminIndex_;
    std::unordered_map<int64_t, std::vector<uint32_t>> poiIndex_;
    const ForwardGeocodeIndex* forwardIndex_ = nullptr;
    ServerBuildMetrics buildMetrics_;
    bool houseIndexHasBounds_ = false;
    int32_t minHouseLatCell_ = 0;
    int32_t maxHouseLatCell_ = 0;
    int32_t minHouseLonCell_ = 0;
    int32_t maxHouseLonCell_ = 0;

    std::atomic<bool> running_{false};
    std::unique_ptr<httplib::Server> httpServer_;
    std::unique_ptr<std::thread> serverThread_;
    int managedOllamaPid_ = -1;
    bool managedOllamaStarted_ = false;

    void configureRoutes(httplib::Server& server);
    void serverLoop();
    ServerResponse handleRequest(const std::string& path,
                                 const std::unordered_map<std::string, std::string>& params);
    void buildSpatialIndexes();
    void buildGeocodeIndex();
    void startManagedOllama();
    void stopManagedOllama();
    bool launchManagedOllama(std::string* errorMessage);
    ServerResponse handleOllamaStartRequest();
    std::mutex ollamaMutex_;

    static ServerResponse httpResponse(int statusCode,
                                       const std::string& contentType,
                                       const std::string& body);
};

#endif
