#include "parser.h"
#include "server.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

std::atomic<bool> keepRunning{true};

void signalHandler(int) {
    keepRunning = false;
}

void printUsage(const std::string& program) {
    std::cout << "Usage:\n";
    std::cout << "  " << program << " --parse <input.osm.pbf> [options]\n";
    std::cout << "  " << program << " --load-binary <dataset.bin> [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --lang <code>              Select one metadata language (default: de)\n";
    std::cout << "  --pbf-threads <n>          PBF blob decoder threads: 0=libosmium default, 1=serial, 2-32=parallel\n";
    std::cout << "  --low-memory              Use extra PBF scans to lower peak RAM during extraction\n";
    std::cout << "  --connect-streets          Merge connected street segments with the same label and highway type\n";
    std::cout << "  --save-binary <file>       Save compact binary snapshot after parsing or loading\n";
    std::cout << "  --load-binary <file>       Load compact binary snapshot instead of parsing PBF\n";
    std::cout << "  --geojson <file>           Export sampled GeoJSON for inspection\n";
    std::cout << "  --geojson-houses <n>       Max houses in GeoJSON export (default: 25000)\n";
    std::cout << "  --geojson-streets <n>      Max streets in GeoJSON export (default: 5000)\n";
    std::cout << "  --geojson-admin <n>        Max admin areas in GeoJSON export (default: 500)\n";
    std::cout << "  --server [port]            Start backend + Leaflet frontend (default: 8080)\n";
    std::cout << "  --viewport-limit <n>       Max houses returned for one viewport request (default: 1500)\n";
    std::cout << "  --help                     Show this help message\n";
}

bool parsePositiveInt(const std::string& text, int& value) {
    try {
        value = std::stoi(text);
    } catch (...) {
        return false;
    }
    return value > 0;
}

bool parsePositiveSize(const std::string& text, size_t& value) {
    try {
        value = static_cast<size_t>(std::stoull(text));
    } catch (...) {
        return false;
    }
    return value > 0;
}

bool parseNonNegativeInt(const std::string& text, int& value) {
    try {
        value = std::stoi(text);
    } catch (...) {
        return false;
    }
    return value >= 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argc > 0 ? argv[0] : "osm_parser");
        return 1;
    }

    Parser::Options options;
    std::string parsePbfPath;
    std::string loadBinaryPath;
    std::string saveBinaryPath;
    std::string geoJsonPath;
    size_t geoJsonHouseLimit = 25000;
    size_t geoJsonStreetLimit = 5000;
    size_t geoJsonAdminLimit = 500;
    bool startServer = false;
    int serverPort = 8080;
    int viewportLimit = 1500;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--parse" && i + 1 < argc) {
            parsePbfPath = argv[++i];
        } else if (arg == "--load-binary" && i + 1 < argc) {
            loadBinaryPath = argv[++i];
        } else if (arg == "--save-binary" && i + 1 < argc) {
            saveBinaryPath = argv[++i];
        } else if (arg == "--lang" && i + 1 < argc) {
            options.nameLanguage = argv[++i];
        } else if (arg == "--pbf-threads" && i + 1 < argc) {
            if (!parseNonNegativeInt(argv[++i], options.pbfThreads) || options.pbfThreads > 32) {
                std::cerr << "Invalid value for --pbf-threads; use 0 to 32" << std::endl;
                return 1;
            }
        } else if (arg == "--low-memory") {
            options.lowMemory = true;
        } else if (arg == "--connect-streets") {
            options.connectStreets = true;
        } else if (arg == "--geojson" && i + 1 < argc) {
            geoJsonPath = argv[++i];
        } else if (arg == "--geojson-houses" && i + 1 < argc) {
            if (!parsePositiveSize(argv[++i], geoJsonHouseLimit)) {
                std::cerr << "Invalid value for --geojson-houses" << std::endl;
                return 1;
            }
        } else if (arg == "--geojson-streets" && i + 1 < argc) {
            if (!parsePositiveSize(argv[++i], geoJsonStreetLimit)) {
                std::cerr << "Invalid value for --geojson-streets" << std::endl;
                return 1;
            }
        } else if (arg == "--geojson-admin" && i + 1 < argc) {
            if (!parsePositiveSize(argv[++i], geoJsonAdminLimit)) {
                std::cerr << "Invalid value for --geojson-admin" << std::endl;
                return 1;
            }
        } else if (arg == "--viewport-limit" && i + 1 < argc) {
            if (!parsePositiveInt(argv[++i], viewportLimit)) {
                std::cerr << "Invalid value for --viewport-limit" << std::endl;
                return 1;
            }
        } else if (arg == "--server") {
            startServer = true;
            if (i + 1 < argc) {
                const std::string candidate = argv[i + 1];
                if (!candidate.empty() && candidate[0] != '-') {
                    if (!parsePositiveInt(candidate, serverPort) || serverPort > 65535) {
                        std::cerr << "Invalid server port" << std::endl;
                        return 1;
                    }
                    ++i;
                }
            }
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown or incomplete option: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }

    if (parsePbfPath.empty() == loadBinaryPath.empty()) {
        std::cerr << "Choose exactly one input source: --parse or --load-binary" << std::endl;
        return 1;
    }

    try {
        Parser parser(options);

        if (!parsePbfPath.empty()) {
            if (!std::filesystem::exists(parsePbfPath)) {
                std::cerr << "Input PBF not found: " << parsePbfPath << std::endl;
                return 1;
            }

            std::cout << "Parsing PBF: " << parsePbfPath << std::endl;
            parser.parsePbf(parsePbfPath);

            if (!saveBinaryPath.empty()) {
                if (parser.writeBinarySnapshot(saveBinaryPath)) {
                    std::cout << "Binary snapshot written to: " << saveBinaryPath << std::endl;
                } else {
                    std::cerr << "Failed to write binary snapshot: " << saveBinaryPath << std::endl;
                    return 1;
                }
            }
        } else {
            if (!std::filesystem::exists(loadBinaryPath)) {
                std::cerr << "Binary snapshot not found: " << loadBinaryPath << std::endl;
                return 1;
            }

            std::cout << "Loading binary snapshot: " << loadBinaryPath << std::endl;
            if (!parser.loadBinarySnapshot(loadBinaryPath)) {
                std::cerr << "Failed to load binary snapshot" << std::endl;
                return 1;
            }

            if (options.connectStreets) {
                parser.connectStreets();
            }

            if (!saveBinaryPath.empty()) {
                if (parser.writeBinarySnapshot(saveBinaryPath)) {
                    std::cout << "Binary snapshot written to: " << saveBinaryPath << std::endl;
                } else {
                    std::cerr << "Failed to write binary snapshot: " << saveBinaryPath << std::endl;
                    return 1;
                }
            }
        }

        if (!loadBinaryPath.empty()) {
            parser.printStats();
        }

        if (!geoJsonPath.empty()) {
            if (parser.exportGeoJson(geoJsonPath, geoJsonHouseLimit, geoJsonStreetLimit, geoJsonAdminLimit)) {
                std::cout << "GeoJSON export written to: " << geoJsonPath << std::endl;
            } else {
                std::cerr << "Failed to write GeoJSON: " << geoJsonPath << std::endl;
                return 1;
            }
        }

        if (startServer) {
            parser.ensureForwardGeocodeIndex(true);
            std::signal(SIGINT, signalHandler);
            std::cout << "Building server indexes before starting http://localhost:" << serverPort << std::endl;
            Server server(parser.data(), serverPort, viewportLimit);
            std::cout << "Server indexes ready" << std::endl;
            if (!server.start()) {
                std::cerr << "Failed to start server" << std::endl;
                return 1;
            }

            std::cout << "Open http://localhost:" << serverPort << " in your browser" << std::endl;
            std::cout << "Press Ctrl+C to stop the server" << std::endl;

            while (keepRunning) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }

            server.stop();
            std::cout << "Server stopped" << std::endl;
        }
    } catch (const std::bad_alloc&) {
        std::cerr << "Fatal error: out of memory while processing the dataset" << std::endl;
        return 1;
    } catch (const std::exception& exception) {
        std::cerr << "Fatal error: " << exception.what() << std::endl;
        return 1;
    }

    return 0;
}
