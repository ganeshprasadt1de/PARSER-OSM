#ifndef OSM_DATA_MODEL_H
#define OSM_DATA_MODEL_H

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using StringRef = uint32_t;

constexpr int32_t kCoordinateScale = 10000000;
constexpr StringRef kEmptyStringRef = 0;

struct Coordinate {
    int32_t latE7 = 0;
    int32_t lonE7 = 0;
};

inline double coordinateToDouble(int32_t value) {
    return static_cast<double>(value) / static_cast<double>(kCoordinateScale);
}

inline double latitudeOf(const Coordinate& coordinate) {
    return coordinateToDouble(coordinate.latE7);
}

inline double longitudeOf(const Coordinate& coordinate) {
    return coordinateToDouble(coordinate.lonE7);
}

struct BoundingBox {
    int32_t minLatE7 = std::numeric_limits<int32_t>::max();
    int32_t maxLatE7 = std::numeric_limits<int32_t>::min();
    int32_t minLonE7 = std::numeric_limits<int32_t>::max();
    int32_t maxLonE7 = std::numeric_limits<int32_t>::min();
    bool valid = false;

    void expand(const Coordinate& point) {
        if (!valid) {
            minLatE7 = maxLatE7 = point.latE7;
            minLonE7 = maxLonE7 = point.lonE7;
            valid = true;
            return;
        }

        if (point.latE7 < minLatE7) {
            minLatE7 = point.latE7;
        }
        if (point.latE7 > maxLatE7) {
            maxLatE7 = point.latE7;
        }
        if (point.lonE7 < minLonE7) {
            minLonE7 = point.lonE7;
        }
        if (point.lonE7 > maxLonE7) {
            maxLonE7 = point.lonE7;
        }
    }

    bool contains(const Coordinate& point) const {
        if (!valid) {
            return false;
        }
        return point.latE7 >= minLatE7 && point.latE7 <= maxLatE7 &&
               point.lonE7 >= minLonE7 && point.lonE7 <= maxLonE7;
    }

    bool intersects(const BoundingBox& other) const {
        if (!valid || !other.valid) {
            return false;
        }
        return !(maxLatE7 < other.minLatE7 || minLatE7 > other.maxLatE7 ||
                 maxLonE7 < other.minLonE7 || minLonE7 > other.maxLonE7);
    }
};

enum class FeatureSource : uint8_t {
    Node = 1,
    Way = 2,
    Relation = 3
};

struct HouseRecord {
    uint64_t osmId = 0;
    Coordinate point;
    StringRef houseNumber = kEmptyStringRef;
    StringRef streetName = kEmptyStringRef;
    StringRef postcode = kEmptyStringRef;
    StringRef city = kEmptyStringRef;
    StringRef country = kEmptyStringRef;
    uint32_t adminAreaOffset = 0;
    uint32_t adminAreaSize = 0;
    FeatureSource source = FeatureSource::Node;
};

struct StreetRecord {
    uint64_t osmId = 0;
    StringRef name = kEmptyStringRef;
    StringRef highwayType = kEmptyStringRef;
    uint32_t geometryOffset = 0;
    uint32_t geometrySize = 0;
    uint32_t adminAreaOffset = 0;
    uint32_t adminAreaSize = 0;
    BoundingBox bbox;
};

struct AdminAreaRecord {
    uint64_t osmId = 0;
    StringRef name = kEmptyStringRef;
    uint8_t adminLevel = 0;
    FeatureSource source = FeatureSource::Way;
    uint32_t geometryOffset = 0;
    uint32_t geometrySize = 0;
    uint32_t ringOffset = 0;
    uint32_t ringSize = 0;
    uint32_t parentAreaOffset = 0;
    uint32_t parentAreaSize = 0;
    BoundingBox bbox;
};

struct AdminRingRecord {
    uint32_t geometryOffset = 0;
    uint32_t geometrySize = 0;
    uint32_t adminAreaIndex = 0;
    uint8_t role = 0;
};

struct PoiRecord {
    uint64_t osmId = 0;
    Coordinate point;
    BoundingBox bbox;
    StringRef name = kEmptyStringRef;
    StringRef category = kEmptyStringRef;
    StringRef tagKey = kEmptyStringRef;
    StringRef tagValue = kEmptyStringRef;
    StringRef brand = kEmptyStringRef;
    uint32_t adminAreaOffset = 0;
    uint32_t adminAreaSize = 0;
    FeatureSource source = FeatureSource::Node;
};

struct DatasetStats {
    uint64_t scannedNodes = 0;
    uint64_t scannedWays = 0;
    uint64_t scannedRelations = 0;

    uint64_t housesTotal = 0;
    uint64_t housesFromNodes = 0;
    uint64_t housesFromWays = 0;
    uint64_t housesFromRelations = 0;
    uint64_t housesMissingStreet = 0;
    uint64_t housesMissingHouseNumber = 0;
    uint64_t housesWithAdminAreas = 0;
    uint64_t houseAdminAreaLinks = 0;

    uint64_t streetsWithAdminAreas = 0;
    uint64_t streetAdminAreaLinks = 0;
    uint64_t adminAreasWithParents = 0;
    uint64_t adminParentAreaLinks = 0;

    uint64_t streetsTotal = 0;
    uint64_t adminAreasFromWays = 0;
    uint64_t adminAreasFromRelations = 0;
    uint64_t adminAreasTotal = 0;
    uint64_t poisTotal = 0;
    uint64_t poisFromNodes = 0;
    uint64_t poisFromWays = 0;
    uint64_t poisWithAdminAreas = 0;
    uint64_t poiAdminAreaLinks = 0;

    uint64_t inputFileBytes = 0;
    double inputFileMB = 0.0;

    double relationScanSeconds = 0.0;
    double extractionSeconds = 0.0;
    double relationAssemblySeconds = 0.0;
    double adminAttributionSeconds = 0.0;
    double connectionSeconds = 0.0;
    double totalSeconds = 0.0;

    double inputMegabytesPerSecond = 0.0;
    double objectsPerSecond = 0.0;

    double datasetBytes = 0.0;
    double datasetMB = 0.0;
    double rssStartMB = 0.0;
    double rssEndMB = 0.0;
    double rssPeakMB = 0.0;
};

#pragma pack(push, 1)
struct GeocodeRef {
    uint8_t type = 0;
    uint32_t index = 0;
};
#pragma pack(pop)

static_assert(sizeof(GeocodeRef) == 5, "GeocodeRef must stay packed; Europe indexes depend on this size.");

struct GeocodePostingList {
    std::string token;
    std::vector<GeocodeRef> refs;
};

struct GeocodeSuffixEntry {
    uint32_t tokenIndex = 0;
    uint16_t offset = 0;
};

struct ForwardGeocodeIndex {
    bool available = false;
    std::vector<GeocodePostingList> context;
    std::vector<GeocodePostingList> primary;
    std::vector<std::string> vocabulary;
    std::vector<GeocodeSuffixEntry> suffixArray;
    uint64_t postingRefs = 0;
    uint64_t primaryPostingRefs = 0;
    uint64_t largestPostingList = 0;
    uint64_t largestPrimaryPostingList = 0;
    uint64_t estimatedBytes = 0;

    void clear() {
        available = false;
        context.clear();
        primary.clear();
        vocabulary.clear();
        suffixArray.clear();
        postingRefs = 0;
        primaryPostingRefs = 0;
        largestPostingList = 0;
        largestPrimaryPostingList = 0;
        estimatedBytes = 0;
    }
};

struct OSMDataset {
    std::vector<std::string> strings;
    std::vector<HouseRecord> houses;
    std::vector<StreetRecord> streets;
    std::vector<AdminAreaRecord> adminAreas;
    std::vector<PoiRecord> pois;
    std::vector<uint32_t> houseAdminAreaIndexes;
    std::vector<uint32_t> streetAdminAreaIndexes;
    std::vector<uint32_t> poiAdminAreaIndexes;
    std::vector<uint32_t> adminParentAreaIndexes;
    std::vector<Coordinate> streetGeometry;
    std::vector<Coordinate> adminGeometry;
    std::vector<AdminRingRecord> adminRings;
    ForwardGeocodeIndex forwardGeocodeIndex;
    DatasetStats stats;
    std::string selectedNameLanguage = "de";

    const std::string& resolve(StringRef ref) const {
        static const std::string empty;
        if (ref >= strings.size()) {
            return empty;
        }
        return strings[ref];
    }
};

#endif
