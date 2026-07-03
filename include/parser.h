#ifndef OSM_PARSER_H
#define OSM_PARSER_H

#include <string>

#include "data_model.h"

class Parser {
public:
    struct Options {
        std::string nameLanguage = "de";
        int pbfThreads = 0;
        bool connectStreets = false;
        bool lowMemory = false;
    };

    Parser();
    explicit Parser(Options options);

    void parsePbf(const std::string& pbfPath);
    void connectStreets();
    void ensureForwardGeocodeIndex(bool showProgress = true);
    bool writeBinarySnapshot(const std::string& outputPath);
    bool loadBinarySnapshot(const std::string& inputPath);
    bool exportGeoJson(const std::string& outputPath,
                       size_t maxHouses = 25000,
                       size_t maxStreets = 5000,
                       size_t maxAdminAreas = 500) const;
    void printStats() const;

    const OSMDataset& data() const {
        return data_;
    }

private:
    Options options_;
    OSMDataset data_;

    double estimateDatasetBytes() const;
};

#endif
