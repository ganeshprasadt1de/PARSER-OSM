#include "server.h"

#include "build_config.h"
#include "geocode_index.h"
#include "httplib.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <cstdlib>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

constexpr const char* kHtmlContentType = "text/html; charset=UTF-8";
constexpr const char* kJsonContentType = "application/json; charset=UTF-8";
constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusMeters = 6371000.0;

struct NearestHouseResult {
    bool found = false;
    uint32_t index = 0;
    double distanceMeters = std::numeric_limits<double>::max();
};

struct NearestStreetResult {
    bool found = false;
    uint32_t index = 0;
    double distanceMeters = std::numeric_limits<double>::max();
};

struct NearestAdminResult {
    bool found = false;
    uint32_t index = 0;
    double distanceMeters = std::numeric_limits<double>::max();
    bool inside = false;
};

struct NearestPoiResult {
    bool found = false;
    uint32_t index = 0;
    double distanceMeters = std::numeric_limits<double>::max();
};

struct GeocodeCandidate {
    uint8_t type = 0;
    uint32_t index = 0;
    uint16_t matchedTokens = 0;
    int score = 0;
};

struct NaturalPoiCandidate {
    double distanceMeters = 0.0;
    uint32_t index = 0;
    int weight = 0;
};

enum class NaturalIntentType {
    Unknown,
    NamedPoiInPlace,
    NearestCategoryToAddress,
    NearestConceptToAddress
};

struct NaturalIntent {
    NaturalIntentType type = NaturalIntentType::Unknown;
    std::string poiName;
    std::string place;
    std::string category;
    std::string concept;
    std::string productFamily;
    std::string address;
    bool fromLlm = false;
    bool verifiedByLlm = false;
};

struct OllamaSettings {
    std::string model;
    std::string host;
    int port = 11434;
    int readTimeoutSeconds = 8;
};

struct QueryAnalysis {
    std::vector<std::string> tokens;
    std::vector<std::string> numberTokens;
    std::vector<std::string> postcodeTokens;
    std::vector<std::string> textTokens;
    bool hasNumber = false;
    bool hasPostcode = false;
};

struct GeometryWriteStats {
    uint32_t sourcePoints = 0;
    uint32_t writtenPoints = 0;
    bool simplified = false;
};

std::string formatProgressDuration(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return "unknown";
    }

    const auto rounded = static_cast<uint64_t>(std::llround(seconds));
    const uint64_t hours = rounded / 3600;
    const uint64_t minutes = (rounded % 3600) / 60;
    const uint64_t remainingSeconds = rounded % 60;
    std::ostringstream stream;
    if (hours > 0) {
        stream << hours << "h " << minutes << "m " << remainingSeconds << "s";
    } else if (minutes > 0) {
        stream << minutes << "m " << remainingSeconds << "s";
    } else {
        stream << remainingSeconds << "s";
    }
    return stream.str();
}

bool stdoutIsTerminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

class ConsoleProgress {
public:
    ConsoleProgress(std::string label, uint64_t total, std::string unit)
        : label_(std::move(label)),
          total_(total),
          unit_(std::move(unit)),
          interactive_(stdoutIsTerminal()),
          start_(std::chrono::steady_clock::now()),
          lastPrint_(start_) {
        update(0, true);
    }

    void update(uint64_t completed) {
        update(completed, false);
    }

    void finish(uint64_t completed = std::numeric_limits<uint64_t>::max()) {
        if (completed == std::numeric_limits<uint64_t>::max()) {
            completed = total_;
        }
        if (completed == lastCompleted_) {
            if (lineOpen_) {
                std::cout << "\n";
                lineOpen_ = false;
            }
            return;
        }
        update(completed, true);
        if (lineOpen_) {
            std::cout << "\n";
            lineOpen_ = false;
        }
    }

private:
    static constexpr double kPrintIntervalSeconds = 1.0;
    static constexpr int kBarWidth = 24;

    void update(uint64_t completed, bool force) {
        if (total_ > 0) {
            completed = std::min(completed, total_);
        }
        if (!force && completed == lastCompleted_) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force && completed < total_ &&
            std::chrono::duration<double>(now - lastPrint_).count() < kPrintIntervalSeconds) {
            return;
        }

        const double elapsed = std::chrono::duration<double>(now - start_).count();
        const double fraction = total_ > 0 ? static_cast<double>(completed) / static_cast<double>(total_) : 0.0;
        const int filled = static_cast<int>(std::round(fraction * kBarWidth));
        const double rate = elapsed > 0.0 ? static_cast<double>(completed) / elapsed : 0.0;
        const bool hasEta = total_ > 0 && completed > 0 && completed < total_ && rate > 0.0;
        const double eta = hasEta ? static_cast<double>(total_ - completed) / rate : 0.0;

        std::ostringstream line;
        if (interactive_) {
            line << "\r\033[2K";
        }
        line << label_ << " [";
        for (int i = 0; i < kBarWidth; ++i) {
            line << (i < filled ? '#' : '-');
        }
        line << "] " << std::fixed << std::setprecision(1) << (fraction * 100.0)
             << "% | " << completed << " / " << total_ << " " << unit_;
        if (rate > 0.0) {
            line << " | " << std::fixed << std::setprecision(0) << rate << " " << unit_ << "/s";
        }
        line << " | elapsed " << formatProgressDuration(elapsed);
        if (completed >= total_) {
            line << " | ETA 0s";
        } else if (hasEta) {
            line << " | ETA " << formatProgressDuration(eta);
        } else {
            line << " | ETA unknown";
        }

        const std::string text = line.str();
        std::cout << text;
        if (interactive_ && lastLength_ > text.size()) {
            std::cout << std::string(lastLength_ - text.size(), ' ');
        }
        if (!interactive_) {
            std::cout << "\n";
        }
        std::cout << std::flush;

        lastLength_ = text.size();
        lastCompleted_ = completed;
        lastPrint_ = now;
        lineOpen_ = true;
        if (force && total_ > 0 && completed >= total_) {
            if (interactive_) {
                std::cout << "\n";
            }
            lineOpen_ = false;
            lastLength_ = 0;
        }
    }

    std::string label_;
    uint64_t total_ = 0;
    std::string unit_;
    bool interactive_ = false;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point lastPrint_;
    uint64_t lastCompleted_ = std::numeric_limits<uint64_t>::max();
    size_t lastLength_ = 0;
    bool lineOpen_ = false;
};

std::string jsonEscape(const std::string& input) {
    std::ostringstream out;
    for (char c : input) {
        switch (c) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default: out << c; break;
        }
    }
    return out.str();
}

bool isNumberToken(const std::string& token) {
    return std::any_of(token.begin(), token.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c));
    });
}

bool isPostcodeToken(const std::string& token) {
    return token.size() == 5 && std::all_of(token.begin(), token.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c));
    });
}

QueryAnalysis analyzeQuery(const std::string& query) {
    QueryAnalysis analysis;
    analysis.tokens = tokenizeSearchText(query);
    for (const std::string& token : analysis.tokens) {
        if (isPostcodeToken(token)) {
            analysis.postcodeTokens.push_back(token);
            analysis.hasPostcode = true;
        } else if (isNumberToken(token)) {
            analysis.numberTokens.push_back(token);
            analysis.hasNumber = true;
        } else {
            analysis.textTokens.push_back(token);
        }
    }
    return analysis;
}

bool containsToken(const std::vector<std::string>& haystack, const std::string& needle) {
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

bool containsAnyToken(const std::vector<std::string>& haystack, const std::vector<std::string>& needles) {
    return std::any_of(needles.begin(), needles.end(), [&](const std::string& token) {
        return containsToken(haystack, token);
    });
}

uint16_t countMatchingSortedTokens(const std::vector<std::string>& tokens,
                                   const std::vector<std::string>& available) {
    uint16_t count = 0;
    for (const std::string& token : tokens) {
        if (std::binary_search(available.begin(), available.end(), token)) {
            ++count;
        }
    }
    return count;
}

bool tryGetDouble(const std::unordered_map<std::string, std::string>& params,
                  const std::string& key,
                  double& valueOut) {
    const auto found = params.find(key);
    if (found == params.end()) {
        return false;
    }
    try {
        valueOut = std::stod(found->second);
        return true;
    } catch (...) {
        return false;
    }
}

bool tryGetUint64(const std::unordered_map<std::string, std::string>& params,
                  const std::string& key,
                  uint64_t& valueOut) {
    const auto found = params.find(key);
    if (found == params.end()) {
        return false;
    }
    try {
        size_t consumed = 0;
        valueOut = std::stoull(found->second, &consumed);
        return consumed == found->second.size();
    } catch (...) {
        return false;
    }
}

bool tryGetUint32(const std::unordered_map<std::string, std::string>& params,
                  const std::string& key,
                  uint32_t& valueOut) {
    const auto found = params.find(key);
    if (found == params.end()) {
        return false;
    }
    try {
        size_t consumed = 0;
        const unsigned long value = std::stoul(found->second, &consumed);
        if (consumed != found->second.size() || value > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        valueOut = static_cast<uint32_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

int getIntOrDefault(const std::unordered_map<std::string, std::string>& params,
                    const std::string& key,
                    int defaultValue,
                    int minValue,
                    int maxValue) {
    const auto found = params.find(key);
    if (found == params.end()) {
        return defaultValue;
    }
    try {
        return std::max(minValue, std::min(maxValue, std::stoi(found->second)));
    } catch (...) {
        return defaultValue;
    }
}

BoundingBox makeBoundingBox(double minLat, double maxLat, double minLon, double maxLon) {
    BoundingBox bbox;
    bbox.valid = true;
    bbox.minLatE7 = static_cast<int32_t>(std::llround(std::min(minLat, maxLat) * kCoordinateScale));
    bbox.maxLatE7 = static_cast<int32_t>(std::llround(std::max(minLat, maxLat) * kCoordinateScale));
    bbox.minLonE7 = static_cast<int32_t>(std::llround(std::min(minLon, maxLon) * kCoordinateScale));
    bbox.maxLonE7 = static_cast<int32_t>(std::llround(std::max(minLon, maxLon) * kCoordinateScale));
    return bbox;
}

Coordinate makeCoordinate(double lat, double lon) {
    return Coordinate{
        static_cast<int32_t>(std::llround(lat * kCoordinateScale)),
        static_cast<int32_t>(std::llround(lon * kCoordinateScale))
    };
}

int32_t floorDiv(int32_t value, int32_t divisor) {
    int64_t quotient = static_cast<int64_t>(value) / divisor;
    const int64_t remainder = static_cast<int64_t>(value) % divisor;
    if (remainder != 0 && ((remainder < 0) != (divisor < 0))) {
        --quotient;
    }
    return static_cast<int32_t>(quotient);
}

int64_t gridKey(int32_t latCell, int32_t lonCell) {
    const uint64_t lat = static_cast<uint32_t>(latCell);
    const uint64_t lon = static_cast<uint32_t>(lonCell);
    return static_cast<int64_t>((lat << 32) | lon);
}

double distanceMeters(const Coordinate& left, const Coordinate& right) {
    const double lat1 = latitudeOf(left) * kPi / 180.0;
    const double lat2 = latitudeOf(right) * kPi / 180.0;
    const double lon1 = longitudeOf(left) * kPi / 180.0;
    const double lon2 = longitudeOf(right) * kPi / 180.0;
    const double x = (lon2 - lon1) * std::cos((lat1 + lat2) * 0.5);
    const double y = lat2 - lat1;
    return std::sqrt(x * x + y * y) * kEarthRadiusMeters;
}

double pointToSegmentDistanceMeters(const Coordinate& point, const Coordinate& start, const Coordinate& end) {
    const double latRad = latitudeOf(point) * kPi / 180.0;
    const double metersPerDegreeLat = 111320.0;
    const double metersPerDegreeLon = metersPerDegreeLat * std::max(0.01, std::abs(std::cos(latRad)));

    const double px = longitudeOf(point) * metersPerDegreeLon;
    const double py = latitudeOf(point) * metersPerDegreeLat;
    const double ax = longitudeOf(start) * metersPerDegreeLon;
    const double ay = latitudeOf(start) * metersPerDegreeLat;
    const double bx = longitudeOf(end) * metersPerDegreeLon;
    const double by = latitudeOf(end) * metersPerDegreeLat;

    const double dx = bx - ax;
    const double dy = by - ay;
    const double lengthSquared = dx * dx + dy * dy;
    if (lengthSquared <= 0.0) {
        const double x = px - ax;
        const double y = py - ay;
        return std::sqrt(x * x + y * y);
    }

    const double t = std::max(0.0, std::min(1.0, ((px - ax) * dx + (py - ay) * dy) / lengthSquared));
    const double closestX = ax + t * dx;
    const double closestY = ay + t * dy;
    const double x = px - closestX;
    const double y = py - closestY;
    return std::sqrt(x * x + y * y);
}

double distanceToStreetMeters(const Coordinate& point, const OSMDataset& data, const StreetRecord& street) {
    if (street.geometrySize == 0 || street.geometryOffset >= data.streetGeometry.size()) {
        return std::numeric_limits<double>::max();
    }
    const uint32_t available = std::min<uint32_t>(street.geometrySize,
        static_cast<uint32_t>(data.streetGeometry.size() - street.geometryOffset));
    if (available == 1) {
        return distanceMeters(point, data.streetGeometry[street.geometryOffset]);
    }

    double best = std::numeric_limits<double>::max();
    for (uint32_t i = 1; i < available; ++i) {
        best = std::min(best, pointToSegmentDistanceMeters(point,
            data.streetGeometry[street.geometryOffset + i - 1],
            data.streetGeometry[street.geometryOffset + i]));
    }
    return best;
}

double distanceToAdminAreaMeters(const Coordinate& point, const OSMDataset& data, const AdminAreaRecord& area) {
    if (area.geometrySize == 0 || area.geometryOffset >= data.adminGeometry.size()) {
        return std::numeric_limits<double>::max();
    }
    const uint32_t available = std::min<uint32_t>(area.geometrySize,
        static_cast<uint32_t>(data.adminGeometry.size() - area.geometryOffset));
    if (available == 1) {
        return distanceMeters(point, data.adminGeometry[area.geometryOffset]);
    }

    double best = std::numeric_limits<double>::max();
    for (uint32_t i = 0; i < available; ++i) {
        const uint32_t next = i + 1 < available ? i + 1 : 0;
        best = std::min(best, pointToSegmentDistanceMeters(point,
            data.adminGeometry[area.geometryOffset + i],
            data.adminGeometry[area.geometryOffset + next]));
    }
    return best;
}

bool streetHasPointInViewport(const OSMDataset& data, const StreetRecord& street, const BoundingBox& viewport) {
    if (street.geometryOffset >= data.streetGeometry.size()) {
        return false;
    }
    const uint32_t available = std::min<uint32_t>(street.geometrySize,
        static_cast<uint32_t>(data.streetGeometry.size() - street.geometryOffset));
    for (uint32_t i = 0; i < available; ++i) {
        if (viewport.contains(data.streetGeometry[street.geometryOffset + i])) {
            return true;
        }
    }
    return false;
}

bool pointOnProjectedSegment(double pointX, double pointY,
                             double startX, double startY,
                             double endX, double endY) {
    const double dx = endX - startX;
    const double dy = endY - startY;
    const double cross = (pointX - startX) * dy - (pointY - startY) * dx;
    if (std::abs(cross) > 1.0) {
        return false;
    }
    const double dot = (pointX - startX) * dx + (pointY - startY) * dy;
    if (dot < 0.0) {
        return false;
    }
    return dot <= dx * dx + dy * dy;
}

bool pointInRing(const Coordinate& point,
                 const std::vector<Coordinate>& geometry,
                 uint32_t offset,
                 uint32_t size) {
    if (size < 4 || offset >= geometry.size()) {
        return false;
    }
    const uint32_t available = std::min<uint32_t>(size, static_cast<uint32_t>(geometry.size() - offset));
    const double queryLatRadians = latitudeOf(point) * kPi / 180.0;
    const double lonScale = std::cos(queryLatRadians);
    const double pointX = static_cast<double>(point.lonE7) * lonScale;
    const double pointY = static_cast<double>(point.latE7);

    bool inside = false;
    for (uint32_t current = 0; current < available; ++current) {
        const uint32_t previous = current == 0 ? available - 1 : current - 1;
        const Coordinate& a = geometry[offset + current];
        const Coordinate& b = geometry[offset + previous];
        const double ax = static_cast<double>(a.lonE7) * lonScale;
        const double ay = static_cast<double>(a.latE7);
        const double bx = static_cast<double>(b.lonE7) * lonScale;
        const double by = static_cast<double>(b.latE7);
        if (pointOnProjectedSegment(pointX, pointY, ax, ay, bx, by)) {
            return true;
        }
        if ((ay > pointY) != (by > pointY)) {
            const double edgeX = ax + (pointY - ay) * (bx - ax) / (by - ay);
            if (edgeX >= pointX) {
                inside = !inside;
            }
        }
    }
    return inside;
}

std::string readFrontendHtml() {
    static std::string cachedHtml;
    if (!cachedHtml.empty()) {
        return cachedHtml;
    }

    const std::vector<std::string> candidates = {
        OSM_FRONTEND_INDEX_PATH,
        "frontend/index.html",
        "../frontend/index.html",
        "../../frontend/index.html"
    };
    for (const std::string& path : candidates) {
        std::ifstream in(path);
        if (!in.is_open()) {
            continue;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        cachedHtml = buffer.str();
        return cachedHtml;
    }
    cachedHtml = "<!doctype html><html><body><h1>frontend/index.html not found</h1></body></html>";
    return cachedHtml;
}

std::unordered_map<std::string, std::string> paramsFromRequest(const httplib::Request& request) {
    std::unordered_map<std::string, std::string> params;
    for (const auto& param : request.params) {
        params[param.first] = param.second;
    }
    return params;
}

void cellRangeForBox(const BoundingBox& bbox,
                     int32_t cellSizeE7,
                     int32_t& minLatCell,
                     int32_t& maxLatCell,
                     int32_t& minLonCell,
                     int32_t& maxLonCell) {
    minLatCell = floorDiv(bbox.minLatE7, cellSizeE7);
    maxLatCell = floorDiv(bbox.maxLatE7, cellSizeE7);
    minLonCell = floorDiv(bbox.minLonE7, cellSizeE7);
    maxLonCell = floorDiv(bbox.maxLonE7, cellSizeE7);
}

void addBoxToIndex(const BoundingBox& bbox,
                   uint32_t index,
                   int32_t cellSizeE7,
                   std::unordered_map<int64_t, std::vector<uint32_t>>& grid) {
    if (!bbox.valid) {
        return;
    }
    int32_t minLatCell = 0;
    int32_t maxLatCell = 0;
    int32_t minLonCell = 0;
    int32_t maxLonCell = 0;
    cellRangeForBox(bbox, cellSizeE7, minLatCell, maxLatCell, minLonCell, maxLonCell);
    for (int32_t latCell = minLatCell; latCell <= maxLatCell; ++latCell) {
        for (int32_t lonCell = minLonCell; lonCell <= maxLonCell; ++lonCell) {
            grid[gridKey(latCell, lonCell)].push_back(index);
        }
    }
}

std::vector<uint32_t> queryBoxIndex(const BoundingBox& viewport,
                                    int32_t cellSizeE7,
                                    const std::unordered_map<int64_t, std::vector<uint32_t>>& grid) {
    std::vector<uint32_t> result;
    std::unordered_set<uint32_t> seen;
    int32_t minLatCell = 0;
    int32_t maxLatCell = 0;
    int32_t minLonCell = 0;
    int32_t maxLonCell = 0;
    cellRangeForBox(viewport, cellSizeE7, minLatCell, maxLatCell, minLonCell, maxLonCell);
    for (int32_t latCell = minLatCell; latCell <= maxLatCell; ++latCell) {
        for (int32_t lonCell = minLonCell; lonCell <= maxLonCell; ++lonCell) {
            const auto bucket = grid.find(gridKey(latCell, lonCell));
            if (bucket == grid.end()) {
                continue;
            }
            for (uint32_t item : bucket->second) {
                if (seen.insert(item).second) {
                    result.push_back(item);
                }
            }
        }
    }
    return result;
}

void writeAdminRefsJson(std::ostringstream& json,
                        const OSMDataset& data,
                        const std::vector<uint32_t>& links,
                        uint32_t offset,
                        uint32_t size) {
    json << "[";
    bool first = true;
    for (uint32_t i = 0; i < size; ++i) {
        const size_t linkIndex = static_cast<size_t>(offset) + i;
        if (linkIndex >= links.size()) {
            continue;
        }
        const uint32_t areaIndex = links[linkIndex];
        if (areaIndex >= data.adminAreas.size()) {
            continue;
        }
        const AdminAreaRecord& area = data.adminAreas[areaIndex];
        if (!first) {
            json << ",";
        }
        first = false;
        json << "{";
        json << "\"index\":" << areaIndex << ",";
        json << "\"id\":" << area.osmId << ",";
        json << "\"name\":\"" << jsonEscape(data.resolve(area.name)) << "\",";
        json << "\"adminLevel\":" << static_cast<int>(area.adminLevel);
        json << "}";
    }
    json << "]";
}

std::vector<uint32_t> adminRefsFromLinks(const std::vector<uint32_t>& links,
                                         uint32_t offset,
                                         uint32_t size,
                                         size_t adminAreaCount) {
    std::vector<uint32_t> refs;
    refs.reserve(size);
    for (uint32_t i = 0; i < size; ++i) {
        const size_t linkIndex = static_cast<size_t>(offset) + i;
        if (linkIndex >= links.size()) {
            continue;
        }
        const uint32_t areaIndex = links[linkIndex];
        if (areaIndex < adminAreaCount) {
            refs.push_back(areaIndex);
        }
    }
    return refs;
}

std::string houseAddressLine(const OSMDataset& data, const HouseRecord& house) {
    const std::string& street = data.resolve(house.streetName);
    const std::string& number = data.resolve(house.houseNumber);
    if (!street.empty() && !number.empty()) {
        return street + " " + number;
    }
    if (!street.empty()) {
        return street;
    }
    return number;
}

std::string streetDisplayLabel(const OSMDataset& data, const StreetRecord& street) {
    const std::string& name = data.resolve(street.name);
    if (!name.empty()) {
        return name;
    }
    const std::string& highway = data.resolve(street.highwayType);
    if (!highway.empty()) {
        return "unnamed " + highway + " road";
    }
    return "unnamed road";
}

std::string poiDisplayLabel(const OSMDataset& data, const PoiRecord& poi) {
    const std::string& name = data.resolve(poi.name);
    if (!name.empty()) {
        return name;
    }
    const std::string& brand = data.resolve(poi.brand);
    if (!brand.empty()) {
        return brand;
    }
    const std::string& category = data.resolve(poi.category);
    if (!category.empty()) {
        return category;
    }
    return "POI";
}

void appendTokensFromText(std::vector<std::string>& tokens, const std::string& text) {
    std::vector<std::string> next = tokenizeSearchText(text);
    tokens.insert(tokens.end(), next.begin(), next.end());
}

void appendAdminTokens(std::vector<std::string>& tokens,
                       const OSMDataset& data,
                       const std::vector<uint32_t>& links,
                       uint32_t offset,
                       uint32_t size) {
    for (uint32_t i = 0; i < size; ++i) {
        const size_t linkIndex = static_cast<size_t>(offset) + i;
        if (linkIndex >= links.size()) {
            continue;
        }
        const uint32_t areaIndex = links[linkIndex];
        if (areaIndex >= data.adminAreas.size()) {
            continue;
        }
        appendTokensFromText(tokens, data.resolve(data.adminAreas[areaIndex].name));
    }
}

std::vector<std::string> tokensFromText(const std::string& text) {
    return tokenizeSearchText(text);
}

std::string trimCopy(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> orderedSearchTokens(const std::string& input) {
    std::istringstream stream(normalizeSearchText(input));
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token) {
        if (token == "str" || token == "strasse" || token == "strasze") {
            token = "strasse";
        }
        tokens.push_back(token);
    }
    return tokens;
}

std::string normalizeCategoryAlias(const std::string& text) {
    const std::string normalized = normalizeSearchText(text);
    if (normalized == "park" || normalized == "parks" || normalized == "garden" ||
        normalized == "green space" || normalized == "green area" ||
        normalized == "garten" || normalized == "gruenflaeche" || normalized == "grunflache" ||
        normalized == "gruenanlage" || normalized == "grunanlage") {
        return "park";
    }
    if (normalized == "restaurant" || normalized == "restaurants") {
        return "restaurant";
    }
    if (normalized == "fast food" || normalized == "fastfood" ||
        normalized == "schnellrestaurant" || normalized == "imbiss") {
        return "fast_food";
    }
    if (normalized == "shop" || normalized == "laden" || normalized == "geschaeft" ||
        normalized == "geschaft") {
        return "shop";
    }
    if (normalized == "tankstelle" || normalized == "fuel" ||
        normalized == "fuel station" || normalized == "gas station" ||
        normalized == "petrol station") {
        return "fuel";
    }
    if (normalized == "hotel") {
        return "hotel";
    }
    if (normalized == "hospital" || normalized == "hospitals" || normalized == "krankenhaus") {
        return "hospital";
    }
    if (normalized == "school" || normalized == "schools" || normalized == "schule") {
        return "school";
    }
    return "";
}

struct PoiTagRule {
    const char* key;
    const char* value;
    int weight = 100;
};

struct ConceptRule {
    const char* name;
    const char* label;
    std::vector<PoiTagRule> tags;
};

struct ProductFamilyRule {
    const char* name;
    const char* concept;
    const char* label;
    std::vector<PoiTagRule> tags;
};

const std::vector<ConceptRule>& conceptRules() {
    static const std::vector<ConceptRule> rules = {
        {"personal_care", "personal care products", {{"shop", "chemist", 100}, {"shop", "cosmetics", 95}, {"amenity", "pharmacy", 85}, {"shop", "supermarket", 55}}},
        {"medicine", "medicine and pharmacy products", {{"amenity", "pharmacy", 100}}},
        {"bakery_food", "bread and bakery products", {{"shop", "bakery", 100}, {"shop", "supermarket", 65}, {"shop", "convenience", 50}}},
        {"groceries", "groceries", {{"shop", "supermarket", 100}, {"shop", "convenience", 75}, {"shop", "greengrocer", 70}, {"shop", "deli", 60}}},
        {"electronics_accessories", "electronics accessories", {{"shop", "electronics", 100}, {"shop", "mobile_phone", 95}, {"shop", "computer", 90}, {"shop", "supermarket", 45}}},
        {"clothing", "clothing", {{"shop", "clothes", 100}, {"shop", "fashion", 95}, {"shop", "department_store", 75}, {"shop", "sports", 45}}},
        {"shoes", "shoes", {{"shop", "shoes", 100}, {"shop", "sports", 65}, {"shop", "department_store", 55}}},
        {"books_stationery", "books and stationery", {{"shop", "books", 100}, {"shop", "stationery", 95}, {"shop", "department_store", 45}}},
        {"flowers", "flowers", {{"shop", "florist", 100}, {"shop", "garden_centre", 80}, {"shop", "supermarket", 40}}},
        {"bike_service", "bike parts and repair", {{"shop", "bicycle", 100}}},
        {"car_service", "car parts and repair", {{"shop", "car_repair", 100}, {"shop", "car_parts", 95}, {"shop", "tyres", 90}}},
        {"fuel", "fuel", {{"amenity", "fuel", 100}}},
        {"restaurant", "restaurants", {{"amenity", "restaurant", 100}, {"amenity", "cafe", 70}, {"amenity", "fast_food", 65}}},
        {"fast_food", "fast food", {{"amenity", "fast_food", 100}}},
        {"hotel", "hotels", {{"tourism", "hotel", 100}}},
        {"hospital", "hospitals", {{"amenity", "hospital", 100}}},
        {"school", "schools", {{"amenity", "school", 100}, {"amenity", "university", 60}}},
        {"park", "parks", {{"leisure", "park", 100}, {"leisure", "garden", 80}, {"landuse", "recreation_ground", 70}, {"landuse", "village_green", 70}}},
        {"banking", "banking", {{"amenity", "bank", 100}}},
        {"parking", "parking", {{"amenity", "parking", 100}}},
        {"sports", "sports equipment and facilities", {{"shop", "sports", 100}, {"leisure", "sports_centre", 70}, {"leisure", "swimming_pool", 55}}},
        {"toys_games", "toys and games", {{"shop", "toys", 100}, {"shop", "games", 95}, {"shop", "department_store", 50}}},
        {"jewelry", "jewelry and watches", {{"shop", "jewelry", 100}, {"shop", "watches", 95}}},
        {"hardware", "hardware and DIY", {{"shop", "hardware", 100}, {"shop", "doityourself", 95}, {"shop", "paint", 85}, {"shop", "garden_centre", 60}}},
        {"furniture_home", "furniture and home goods", {{"shop", "furniture", 100}, {"shop", "houseware", 85}, {"shop", "interior_decoration", 80}}},
        {"optician", "glasses and contact lenses", {{"shop", "optician", 100}}},
        {"pet_supplies", "pet supplies", {{"shop", "pet", 100}}},
        {"photo_printing", "photo and printing services", {{"shop", "photo", 100}, {"shop", "copyshop", 95}, {"shop", "stationery", 50}}},
        {"hair_beauty", "hair and beauty services", {{"shop", "hairdresser", 100}, {"shop", "beauty", 90}, {"shop", "cosmetics", 60}}}
    };
    return rules;
}

const std::vector<ProductFamilyRule>& productFamilyRules() {
    static const std::vector<ProductFamilyRule> rules = {
        {"cosmetics_removal", "personal_care", "cosmetics removal", {{"shop", "chemist", 100}, {"shop", "cosmetics", 95}, {"amenity", "pharmacy", 70}, {"shop", "supermarket", 45}}},
        {"hair_body_care", "personal_care", "hair and body care", {{"shop", "chemist", 100}, {"shop", "cosmetics", 90}, {"shop", "supermarket", 65}, {"amenity", "pharmacy", 50}}},
        {"dental_care", "personal_care", "dental care", {{"shop", "chemist", 100}, {"amenity", "pharmacy", 90}, {"shop", "supermarket", 65}}},
        {"baby_hygiene", "personal_care", "baby hygiene", {{"shop", "chemist", 100}, {"shop", "supermarket", 90}, {"amenity", "pharmacy", 75}}},
        {"cold_medicine", "medicine", "cold medicine", {{"amenity", "pharmacy", 100}}},
        {"pain_allergy_medicine", "medicine", "pain and allergy medicine", {{"amenity", "pharmacy", 100}}},
        {"first_aid", "medicine", "first aid", {{"amenity", "pharmacy", 100}, {"shop", "chemist", 70}}},
        {"bread_pastry", "bakery_food", "bread and pastry", {{"shop", "bakery", 100}, {"shop", "supermarket", 65}, {"shop", "convenience", 45}}},
        {"cake_sweets", "bakery_food", "cakes and sweets", {{"shop", "bakery", 100}, {"shop", "confectionery", 95}, {"shop", "supermarket", 55}}},
        {"daily_groceries", "groceries", "daily groceries", {{"shop", "supermarket", 100}, {"shop", "convenience", 75}, {"shop", "deli", 55}}},
        {"fruit_vegetables", "groceries", "fruit and vegetables", {{"shop", "greengrocer", 100}, {"shop", "supermarket", 80}}},
        {"pet_food", "pet_supplies", "pet food", {{"shop", "pet", 100}, {"shop", "supermarket", 55}}},
        {"phone_accessories", "electronics_accessories", "phone accessories", {{"shop", "mobile_phone", 100}, {"shop", "electronics", 95}, {"shop", "computer", 70}, {"shop", "supermarket", 35}}},
        {"computer_accessories", "electronics_accessories", "computer accessories", {{"shop", "computer", 100}, {"shop", "electronics", 95}, {"shop", "supermarket", 35}}},
        {"printer_supplies", "electronics_accessories", "printer supplies", {{"shop", "computer", 100}, {"shop", "electronics", 90}, {"shop", "stationery", 70}}},
        {"outerwear", "clothing", "outerwear", {{"shop", "clothes", 100}, {"shop", "fashion", 95}, {"shop", "department_store", 80}, {"shop", "sports", 50}}},
        {"basic_clothing", "clothing", "basic clothing", {{"shop", "clothes", 100}, {"shop", "fashion", 90}, {"shop", "department_store", 75}}},
        {"running_shoes", "shoes", "running shoes", {{"shop", "shoes", 100}, {"shop", "sports", 95}, {"shop", "department_store", 50}}},
        {"books", "books_stationery", "books", {{"shop", "books", 100}, {"shop", "department_store", 40}}},
        {"stationery", "books_stationery", "stationery", {{"shop", "stationery", 100}, {"shop", "books", 70}, {"shop", "department_store", 40}}},
        {"flowers_plants", "flowers", "flowers and plants", {{"shop", "florist", 100}, {"shop", "garden_centre", 85}, {"shop", "supermarket", 35}}},
        {"garden_supplies", "hardware", "garden supplies", {{"shop", "garden_centre", 100}, {"shop", "doityourself", 90}, {"shop", "hardware", 70}}},
        {"diy_tools", "hardware", "DIY tools", {{"shop", "hardware", 100}, {"shop", "doityourself", 95}}},
        {"paint_supplies", "hardware", "paint supplies", {{"shop", "paint", 100}, {"shop", "doityourself", 90}, {"shop", "hardware", 70}}},
        {"bike_parts_repair", "bike_service", "bike parts and repair", {{"shop", "bicycle", 100}}},
        {"car_parts", "car_service", "car parts", {{"shop", "car_parts", 100}, {"shop", "tyres", 95}, {"shop", "car_repair", 85}}},
        {"glasses_lenses", "optician", "glasses and lenses", {{"shop", "optician", 100}}},
        {"photo_documents", "photo_printing", "photo and document services", {{"shop", "photo", 100}, {"shop", "copyshop", 95}, {"shop", "stationery", 45}}},
        {"hair_services", "hair_beauty", "hair services", {{"shop", "hairdresser", 100}, {"shop", "beauty", 50}}},
        {"beauty_products", "hair_beauty", "beauty products", {{"shop", "beauty", 100}, {"shop", "cosmetics", 95}, {"shop", "chemist", 70}}},
        {"toys", "toys_games", "toys", {{"shop", "toys", 100}, {"shop", "department_store", 50}}},
        {"games", "toys_games", "games", {{"shop", "games", 100}, {"shop", "toys", 80}, {"shop", "department_store", 45}}},
        {"jewelry_watches", "jewelry", "jewelry and watches", {{"shop", "jewelry", 100}, {"shop", "watches", 90}}},
        {"watch_repair_battery", "jewelry", "watch repair and batteries", {{"shop", "watches", 100}, {"shop", "jewelry", 80}}},
        {"home_furniture", "furniture_home", "home furniture", {{"shop", "furniture", 100}, {"shop", "interior_decoration", 75}, {"shop", "houseware", 70}}},
        {"home_textiles", "furniture_home", "home textiles", {{"shop", "houseware", 100}, {"shop", "furniture", 80}, {"shop", "interior_decoration", 80}}},
        {"sports_equipment", "sports", "sports equipment", {{"shop", "sports", 100}, {"leisure", "sports_centre", 40}}}
    };
    return rules;
}

const ConceptRule* findConceptRule(const std::string& concept) {
    std::string normalized = normalizeSearchText(concept);
    std::replace(normalized.begin(), normalized.end(), ' ', '_');
    for (const ConceptRule& rule : conceptRules()) {
        if (normalized == rule.name) {
            return &rule;
        }
    }
    return nullptr;
}

const ProductFamilyRule* findProductFamilyRule(const std::string& family) {
    std::string normalized = normalizeSearchText(family);
    std::replace(normalized.begin(), normalized.end(), ' ', '_');
    for (const ProductFamilyRule& rule : productFamilyRules()) {
        if (normalized == rule.name) {
            return &rule;
        }
    }
    return nullptr;
}

std::string normalizeConceptAlias(const std::string& text) {
    std::string normalized = normalizeSearchText(text);
    std::replace(normalized.begin(), normalized.end(), ' ', '_');
    if (findConceptRule(normalized) != nullptr) {
        return normalized;
    }
    if (normalized == "cosmetics" || normalized == "drugstore" || normalized == "drogerie" ||
        normalized == "koerperpflege" || normalized == "korperpflege" || normalized == "hygiene") {
        return "personal_care";
    }
    if (normalized == "pharmacy" || normalized == "medicine" || normalized == "medication" ||
        normalized == "apotheke" || normalized == "medizin" || normalized == "arznei") {
        return "medicine";
    }
    if (normalized == "bakery" || normalized == "bread" || normalized == "baeckerei" ||
        normalized == "backerei" || normalized == "brot") {
        return "bakery_food";
    }
    if (normalized == "food" || normalized == "groceries" || normalized == "supermarket" ||
        normalized == "lebensmittel" || normalized == "supermarkt") {
        return "groceries";
    }
    if (normalized == "electronics" || normalized == "phone_accessories" ||
        normalized == "elektronik" || normalized == "handy_zubehoer" || normalized == "handy_zubehor") {
        return "electronics_accessories";
    }
    if (normalized == "bike" || normalized == "bicycle" || normalized == "fahrrad") {
        return "bike_service";
    }
    if (normalized == "car" || normalized == "auto") {
        return "car_service";
    }
    if (normalized == "books" || normalized == "stationery" || normalized == "buch" ||
        normalized == "buecher" || normalized == "bucher" || normalized == "schreibwaren") {
        return "books_stationery";
    }
    if (normalized == "flower" || normalized == "blumen") {
        return "flowers";
    }
    return "";
}

std::string normalizeProductFamilyAlias(const std::string& text) {
    std::string normalized = normalizeSearchText(text);
    std::replace(normalized.begin(), normalized.end(), ' ', '_');
    if (findProductFamilyRule(normalized) != nullptr) {
        return normalized;
    }
    if (normalized == "nail_polish_remover" || normalized == "makeup_remover" ||
        normalized == "nagellackentferner" || normalized == "abschminke") {
        return "cosmetics_removal";
    }
    if (normalized == "shampoo" || normalized == "deodorant" || normalized == "sunscreen" ||
        normalized == "sonnencreme" || normalized == "deo") {
        return "hair_body_care";
    }
    if (normalized == "toothpaste" || normalized == "zahnpasta") {
        return "dental_care";
    }
    if (normalized == "diapers" || normalized == "windeln" || normalized == "baby_diapers" ||
        normalized == "babywindeln") {
        return "baby_hygiene";
    }
    if (normalized == "cough_syrup" || normalized == "hustensaft" || normalized == "nasal_spray" ||
        normalized == "throat_lozenges") {
        return "cold_medicine";
    }
    if (normalized == "painkillers" || normalized == "schmerztabletten" ||
        normalized == "allergy_tablets" || normalized == "allergietabletten") {
        return "pain_allergy_medicine";
    }
    if (normalized == "bandages" || normalized == "verband") {
        return "first_aid";
    }
    if (normalized == "bread" || normalized == "croissants" || normalized == "brot") {
        return "bread_pastry";
    }
    if (normalized == "cake" || normalized == "birthday_cake" || normalized == "geburtstagstorte") {
        return "cake_sweets";
    }
    if (normalized == "phone_charger" || normalized == "usb_cable" ||
        normalized == "handy_ladegeraet" || normalized == "handy_ladegerat" ||
        normalized == "usb_kabel") {
        return "phone_accessories";
    }
    if (normalized == "printer_ink" || normalized == "druckerpatronen") {
        return "printer_supplies";
    }
    if (normalized == "bike_repair" || normalized == "bicycle_inner_tube" ||
        normalized == "fahrrad_reparatur" || normalized == "fahrradschlauch") {
        return "bike_parts_repair";
    }
    return "";
}

bool containsNormalizedPhrase(const std::string& normalizedText, const std::string& phrase) {
    return normalizedText.find(phrase) != std::string::npos;
}

std::string inferProductFamilyFromQuery(const std::string& rawQuery) {
    const std::string q = normalizeSearchText(rawQuery);
    const std::vector<std::pair<std::string, std::string>> phrases = {
        {"nail polish remover", "cosmetics_removal"}, {"nagellackentferner", "cosmetics_removal"},
        {"shampoo", "hair_body_care"}, {"deodorant", "hair_body_care"}, {" deo ", "hair_body_care"},
        {"sunscreen", "hair_body_care"}, {"sonnencreme", "hair_body_care"},
        {"toothpaste", "dental_care"}, {"zahnpasta", "dental_care"}, {"dental floss", "dental_care"},
        {"zahnseide", "dental_care"},
        {"diapers", "baby_hygiene"}, {"windeln", "baby_hygiene"}, {"babywindeln", "baby_hygiene"},
        {"cough syrup", "cold_medicine"}, {"hustensaft", "cold_medicine"}, {"nasenspray", "cold_medicine"},
        {"nasal spray", "cold_medicine"},
        {"painkillers", "pain_allergy_medicine"}, {"schmerztabletten", "pain_allergy_medicine"},
        {"allergy tablets", "pain_allergy_medicine"}, {"allergietabletten", "pain_allergy_medicine"},
        {"bandages", "first_aid"}, {"verband", "first_aid"},
        {"fresh bread", "bread_pastry"}, {"bread", "bread_pastry"}, {"brot", "bread_pastry"},
        {"croissants", "bread_pastry"},
        {"birthday cake", "cake_sweets"}, {"cake", "cake_sweets"}, {"geburtstagstorte", "cake_sweets"},
        {"milk", "daily_groceries"}, {"milch", "daily_groceries"}, {"kaffeebohnen", "daily_groceries"}, {"coffee beans", "daily_groceries"},
        {"bananas", "fruit_vegetables"}, {"bananen", "fruit_vegetables"},
        {"cat food", "pet_food"}, {"dog food", "pet_food"}, {"katzenfutter", "pet_food"},
        {"hundefutter", "pet_food"}, {"hundeleckerlis", "pet_food"},
        {"phone charger", "phone_accessories"}, {"usb cable", "phone_accessories"},
        {"handy ladegeraet", "phone_accessories"}, {"handy ladegerat", "phone_accessories"},
        {"usb kabel", "phone_accessories"}, {"headphones", "phone_accessories"}, {"kopfhoerer", "phone_accessories"},
        {"kopfhorer", "phone_accessories"},
        {"laptop mouse", "computer_accessories"}, {"computermaus", "computer_accessories"},
        {"printer ink", "printer_supplies"}, {"druckerpatronen", "printer_supplies"},
        {"winter jacket", "outerwear"}, {"winterjacke", "outerwear"},
        {"socks", "basic_clothing"}, {"socken", "basic_clothing"},
        {"running shoes", "running_shoes"}, {"laufschuhe", "running_shoes"},
        {"novel", "books"}, {"roman", "books"},
        {"notebook", "stationery"}, {"notizbuch", "stationery"}, {"envelopes", "stationery"}, {"briefumschlaege", "stationery"},
        {"briefumschlage", "stationery"},
        {"roses", "flowers_plants"}, {"rosen", "flowers_plants"},
        {"potting soil", "garden_supplies"}, {"blumenerde", "garden_supplies"},
        {"screwdriver", "diy_tools"}, {"schraubenzieher", "diy_tools"},
        {"wall paint", "paint_supplies"}, {"wandfarbe", "paint_supplies"},
        {"bike", "bike_parts_repair"}, {"bicycle", "bike_parts_repair"}, {"fahrrad", "bike_parts_repair"},
        {"fahrradschlauch", "bike_parts_repair"},
        {"wiper blades", "car_parts"}, {"scheibenwischer", "car_parts"},
        {"reading glasses", "glasses_lenses"}, {"lesebrille", "glasses_lenses"},
        {"contact lenses", "glasses_lenses"}, {"kontaktlinsen", "glasses_lenses"},
        {"passport photos", "photo_documents"}, {"passfotos", "photo_documents"},
        {"document copies", "photo_documents"}, {"dokumente", "photo_documents"}, {"kopieren", "photo_documents"},
        {"haircut", "hair_services"}, {"haare schneiden", "hair_services"},
        {"hair dye", "beauty_products"}, {"haarfarbe", "beauty_products"},
        {"toy car", "toys"}, {"spielzeugauto", "toys"},
        {"board game", "games"}, {"brettspiel", "games"},
        {"necklace", "jewelry_watches"}, {"halskette", "jewelry_watches"},
        {"watch battery", "watch_repair_battery"}, {"uhrenbatterie", "watch_repair_battery"},
        {"sofa", "home_furniture"}, {"pillow", "home_textiles"}, {"kissen", "home_textiles"},
        {"football", "sports_equipment"}, {"fussball", "sports_equipment"}
    };
    const std::string padded = " " + q + " ";
    for (const auto& phrase : phrases) {
        const std::string normalizedPhrase = normalizeSearchText(phrase.first);
        if (containsNormalizedPhrase(padded, " " + normalizedPhrase + " ") ||
            containsNormalizedPhrase(q, normalizedPhrase)) {
            return phrase.second;
        }
    }
    return "";
}

void appendConceptNames(std::ostringstream& out) {
    bool first = true;
    for (const ConceptRule& rule : conceptRules()) {
        if (!first) {
            out << ", ";
        }
        first = false;
        out << rule.name;
    }
}

void appendProductFamilyNames(std::ostringstream& out) {
    bool first = true;
    for (const ProductFamilyRule& rule : productFamilyRules()) {
        if (!first) {
            out << ", ";
        }
        first = false;
        out << rule.name;
    }
}

bool startsWithWord(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

NaturalIntent parseDeterministicNaturalIntent(const std::string& rawQuery) {
    NaturalIntent intent;
    const std::string normalized = normalizeSearchText(rawQuery);
    const std::vector<std::string> tokens = orderedSearchTokens(rawQuery);

    const bool nearestPrefix = startsWithWord(normalized, "closest ") ||
                               startsWithWord(normalized, "nearest ") ||
                               startsWithWord(normalized, "naechster ") ||
                               startsWithWord(normalized, "nachster ") ||
                               startsWithWord(normalized, "naechste ") ||
                               startsWithWord(normalized, "nachste ");
    if (nearestPrefix) {
        const std::vector<std::string> separators = {" to ", " near ", " nearby ", " bei ", " an ", " in der naehe von ", " in der nahe von "};
        std::string lowered = rawQuery;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        size_t separatorPos = std::string::npos;
        std::string separator;
        for (const std::string& current : separators) {
            separatorPos = normalizeSearchText(rawQuery).find(trimCopy(current));
            if (separatorPos != std::string::npos) {
                separator = trimCopy(current);
                break;
            }
        }

        if (!separator.empty()) {
            const std::string normalizedQuery = normalizeSearchText(rawQuery);
            const size_t sep = normalizedQuery.find(separator);
            const std::string before = trimCopy(normalizedQuery.substr(0, sep));
            const std::string after = trimCopy(normalizedQuery.substr(sep + separator.size()));
            std::vector<std::string> beforeTokens = orderedSearchTokens(before);
            if (beforeTokens.size() >= 2 && !after.empty()) {
                const std::string category = normalizeCategoryAlias(beforeTokens.back());
                if (!category.empty()) {
                    intent.type = NaturalIntentType::NearestCategoryToAddress;
                    intent.category = category;
                    intent.address = after;
                    return intent;
                }
            }
        }

        if (tokens.size() >= 2) {
            const std::string category = normalizeCategoryAlias(tokens.back());
            if (!category.empty()) {
                intent.type = NaturalIntentType::NearestCategoryToAddress;
                intent.category = category;
                return intent;
            }
        }
    }

    const auto inIt = std::find(tokens.begin(), tokens.end(), "in");
    if (inIt != tokens.end() && inIt != tokens.begin() && std::next(inIt) != tokens.end()) {
        std::vector<std::string> nameTokens(tokens.begin(), inIt);
        std::vector<std::string> placeTokens(std::next(inIt), tokens.end());
        intent.type = NaturalIntentType::NamedPoiInPlace;
        std::ostringstream name;
        for (size_t i = 0; i < nameTokens.size(); ++i) {
            if (i > 0) {
                name << " ";
            }
            name << nameTokens[i];
        }
        std::ostringstream place;
        for (size_t i = 0; i < placeTokens.size(); ++i) {
            if (i > 0) {
                place << " ";
            }
            place << placeTokens[i];
        }
        intent.poiName = name.str();
        intent.place = place.str();
        return intent;
    }

    if (tokens.size() >= 3) {
        const std::string lastTwo = tokens[tokens.size() - 2] + " " + tokens[tokens.size() - 1];
        if (lastTwo == "burger king") {
            intent.type = NaturalIntentType::NamedPoiInPlace;
            intent.poiName = "burger king";
            std::ostringstream place;
            for (size_t i = 0; i + 2 < tokens.size(); ++i) {
                if (i > 0) {
                    place << " ";
                }
                place << tokens[i];
            }
            intent.place = place.str();
            return intent;
        }
    }

    return intent;
}

std::string jsonStringField(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) {
        return "";
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return "";
    }
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) {
        return "";
    }
    std::string value;
    bool escaped = false;
    for (size_t i = pos + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (escaped) {
            if (c == 'n') {
                value.push_back('\n');
            } else {
                value.push_back(c);
            }
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            break;
        }
        value.push_back(c);
    }
    return value;
}

int envIntOrDefault(const char* name, int defaultValue, int minValue, int maxValue) {
    if (const char* value = std::getenv(name); value != nullptr && *value != '\0') {
        try {
            return std::clamp(std::stoi(value), minValue, maxValue);
        } catch (...) {
            return defaultValue;
        }
    }
    return defaultValue;
}

OllamaSettings ollamaSettingsFromEnvironment() {
    OllamaSettings settings;
    const char* modelEnv = std::getenv("OSM_OLLAMA_MODEL");
    settings.model = modelEnv != nullptr && *modelEnv != '\0' ? modelEnv : "llama3.1:8b";
    const char* hostEnv = std::getenv("OSM_OLLAMA_HOST");
    settings.host = hostEnv != nullptr && *hostEnv != '\0' ? hostEnv : "localhost";
    settings.port = envIntOrDefault("OSM_OLLAMA_PORT", 11434, 1, 65535);
    settings.readTimeoutSeconds = envIntOrDefault("OSM_OLLAMA_TIMEOUT_SECONDS", 8, 1, 60);
    return settings;
}

bool isLocalOllamaHost(const std::string& host) {
    return host == "localhost" || host == "127.0.0.1" || host == "::1";
}

bool ollamaResponds(const OllamaSettings& settings, int timeoutMilliseconds) {
    httplib::Client client(settings.host, settings.port);
    client.set_connection_timeout(0, timeoutMilliseconds * 1000);
    client.set_read_timeout(0, timeoutMilliseconds * 1000);
    auto response = client.Get("/api/tags");
    return response && response->status == 200;
}

bool envFlagDisabled(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return false;
    }
    const std::string text = normalizeSearchText(value);
    return text == "0" || text == "false" || text == "off" || text == "no";
}

std::string ollamaBinaryPath() {
    if (const char* value = std::getenv("OSM_OLLAMA_BIN"); value != nullptr && *value != '\0') {
        return value;
    }
#ifndef _WIN32
    constexpr const char* kBundledPath = "/home/ganesh/opt/ollama-linux/bin/ollama";
    if (::access(kBundledPath, X_OK) == 0) {
        return kBundledPath;
    }
    if (::access("/usr/local/bin/ollama", X_OK) == 0) {
        return "/usr/local/bin/ollama";
    }
    if (::access("/usr/bin/ollama", X_OK) == 0) {
        return "/usr/bin/ollama";
    }
#endif
    return "ollama";
}

#ifndef _WIN32
bool waitForOllamaReady(const OllamaSettings& settings, int pid, int timeoutSeconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ollamaResponds(settings, 500)) {
            return true;
        }
        int status = 0;
        const pid_t result = ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
        if (result == static_cast<pid_t>(pid)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return ollamaResponds(settings, 500);
}

void terminateProcessGroup(int pid) {
    if (pid <= 0) {
        return;
    }
    const pid_t group = -static_cast<pid_t>(pid);
    ::kill(group, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        int status = 0;
        const pid_t result = ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
        if (result == static_cast<pid_t>(pid) || result == -1) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    ::kill(group, SIGKILL);
    int status = 0;
    ::waitpid(static_cast<pid_t>(pid), &status, 0);
}
#endif

bool warmOllamaModel(const OllamaSettings& settings) {
    std::ostringstream body;
    body << "{\"model\":\"" << jsonEscape(settings.model) << "\",";
    body << "\"stream\":false,";
    body << "\"format\":\"json\",";
    body << "\"options\":{\"temperature\":0,\"top_p\":1,\"seed\":0,\"num_predict\":8},";
    body << "\"prompt\":\"Return {\\\"ready\\\":true} as JSON only.\"}";
    httplib::Client client(settings.host, settings.port);
    client.set_connection_timeout(1, 0);
    client.set_read_timeout(settings.readTimeoutSeconds, 0);
    auto response = client.Post("/api/generate", body.str(), kJsonContentType);
    return response && response->status == 200;
}

NaturalIntent parseIntentJson(const std::string& json) {
    NaturalIntent intent;
    const std::string intentName = jsonStringField(json, "intent");
    if (intentName == "named_poi_in_place") {
        intent.type = NaturalIntentType::NamedPoiInPlace;
        intent.poiName = normalizeSearchText(jsonStringField(json, "poiName"));
        intent.place = normalizeSearchText(jsonStringField(json, "place"));
    } else if (intentName == "nearest_category_to_address") {
        intent.category = normalizeCategoryAlias(jsonStringField(json, "category"));
        intent.address = normalizeSearchText(jsonStringField(json, "address"));
        if (!intent.category.empty()) {
            intent.type = NaturalIntentType::NearestCategoryToAddress;
        } else {
            intent.concept = normalizeConceptAlias(jsonStringField(json, "category"));
            if (!intent.concept.empty()) {
                intent.type = NaturalIntentType::NearestConceptToAddress;
            }
        }
    } else if (intentName == "nearest_concept_to_address" ||
               intentName == "nearest_product_to_address" ||
               intentName == "nearest_service_to_address" ||
               intentName == "nearest_need_to_address") {
        intent.productFamily = normalizeProductFamilyAlias(jsonStringField(json, "productFamily"));
        if (intent.productFamily.empty()) {
            intent.productFamily = normalizeProductFamilyAlias(jsonStringField(json, "family"));
        }
        intent.concept = normalizeConceptAlias(jsonStringField(json, "concept"));
        if (intent.concept.empty()) {
            intent.concept = normalizeConceptAlias(jsonStringField(json, "category"));
        }
        if (intent.concept.empty() && !intent.productFamily.empty()) {
            if (const ProductFamilyRule* family = findProductFamilyRule(intent.productFamily)) {
                intent.concept = family->concept;
            }
        }
        intent.address = normalizeSearchText(jsonStringField(json, "address"));
        if (!intent.concept.empty()) {
            intent.type = NaturalIntentType::NearestConceptToAddress;
        }
    }
    return intent;
}

std::string ollamaGenerateJson(const std::string& model,
                               const std::string& host,
                               int port,
                               int readTimeoutSeconds,
                               const std::string& prompt,
                               int maxTokens) {
    httplib::Client client(host, port);
    client.set_connection_timeout(1, 0);
    client.set_read_timeout(readTimeoutSeconds, 0);
    std::ostringstream body;
    body << "{\"model\":\"" << jsonEscape(model) << "\",";
    body << "\"stream\":false,";
    body << "\"format\":\"json\",";
    body << "\"options\":{\"temperature\":0,\"top_p\":1,\"seed\":0,\"num_predict\":" << maxTokens << "},";
    body << "\"prompt\":\"" << jsonEscape(prompt) << "\"}";
    auto response = client.Post("/api/generate", body.str(), kJsonContentType);
    if (!response || response->status != 200) {
        return "";
    }
    return jsonStringField(response->body, "response");
}

std::string intentPromptPrefix() {
    std::ostringstream prompt;
    prompt << "Convert this English or German map query into one JSON object only. "
           << "Allowed intents: named_poi_in_place, nearest_category_to_address, nearest_concept_to_address. "
           << "Allowed categories: park, restaurant, fast_food, shop, fuel, hotel, hospital, school. "
           << "Allowed concepts: ";
    appendConceptNames(prompt);
    prompt << ". Allowed productFamily values: ";
    appendProductFamilyNames(prompt);
    prompt << ". "
           << "If the query asks for a broad nearby category, use nearest_category_to_address. "
           << "If the query asks where to buy, find, get, repair, print, or use a product or service, use nearest_concept_to_address. "
           << "For nearest_concept_to_address, include concept, productFamily, and address. "
           << "For nearest_category_to_address, include category and address. "
           << "For named_poi_in_place, include poiName and place. "
           << "Use keys exactly: intent, poiName, place, category, concept, productFamily, address. "
           << "Do not invent coordinates, shops, or results. "
           << "Examples: show me the nearest park around Koenigstrasse 1 Stuttgart => "
           << "{\"intent\":\"nearest_category_to_address\",\"category\":\"park\",\"address\":\"Koenigstrasse 1 Stuttgart\"}. "
           << "where can I buy nail polish remover near Koenigstrasse 1 Stuttgart => "
           << "{\"intent\":\"nearest_concept_to_address\",\"concept\":\"personal_care\",\"productFamily\":\"cosmetics_removal\",\"address\":\"Koenigstrasse 1 Stuttgart\"}. "
           << "wo bekomme ich Hustensaft nahe Hauptstrasse 1 Heidelberg => "
           << "{\"intent\":\"nearest_concept_to_address\",\"concept\":\"medicine\",\"productFamily\":\"cold_medicine\",\"address\":\"Hauptstrasse 1 Heidelberg\"}. "
           << "where can I repair my bike near Koenigstrasse 1 Stuttgart => "
           << "{\"intent\":\"nearest_concept_to_address\",\"concept\":\"bike_service\",\"productFamily\":\"bike_parts_repair\",\"address\":\"Koenigstrasse 1 Stuttgart\"}. "
           << "Burger King branches in Stuttgart => "
           << "{\"intent\":\"named_poi_in_place\",\"poiName\":\"Burger King\",\"place\":\"Stuttgart\"}. ";
    return prompt.str();
}

NaturalIntent parseOllamaIntent(const std::string& rawQuery) {
    NaturalIntent intent;
    const OllamaSettings settings = ollamaSettingsFromEnvironment();

    const std::string prefix = intentPromptPrefix();
    const std::string generated = ollamaGenerateJson(settings.model, settings.host, settings.port, settings.readTimeoutSeconds,
        prefix + "Query: " + rawQuery, 120);
    if (generated.empty()) {
        return intent;
    }

    std::ostringstream verifyPrompt;
    verifyPrompt << prefix
                 << "Verify and correct this draft JSON for the original query. "
                 << "Return one JSON object only. Keep only allowed intent, concept, productFamily, category, and address values. "
                 << "If the query is about buying/finding/repairing/printing a product or service, use nearest_concept_to_address with the best productFamily. "
                 << "Original query: " << rawQuery << ". Draft JSON: " << generated;
    const std::string verified = ollamaGenerateJson(settings.model, settings.host, settings.port, settings.readTimeoutSeconds, verifyPrompt.str(), 120);
    const std::string finalJson = verified.empty() ? generated : verified;
    intent = parseIntentJson(finalJson);
    const std::string inferredFamily = inferProductFamilyFromQuery(rawQuery);
    if (!inferredFamily.empty()) {
        if (const ProductFamilyRule* family = findProductFamilyRule(inferredFamily)) {
            intent.type = NaturalIntentType::NearestConceptToAddress;
            intent.productFamily = inferredFamily;
            intent.concept = family->concept;
            if (intent.address.empty()) {
                intent.address = normalizeSearchText(jsonStringField(finalJson, "address"));
            }
        }
    }
    intent.fromLlm = intent.type != NaturalIntentType::Unknown;
    intent.verifiedByLlm = !verified.empty() && intent.fromLlm;
    return intent;
}

bool extractFirstLatLon(const std::string& json, Coordinate& pointOut) {
    const size_t latKey = json.find("\"lat\":");
    const size_t lonKey = json.find("\"lon\":");
    if (latKey == std::string::npos || lonKey == std::string::npos) {
        return false;
    }
    try {
        const double lat = std::stod(json.substr(latKey + 6));
        const double lon = std::stod(json.substr(lonKey + 6));
        pointOut.latE7 = static_cast<int32_t>(std::llround(lat * kCoordinateScale));
        pointOut.lonE7 = static_cast<int32_t>(std::llround(lon * kCoordinateScale));
        return true;
    } catch (...) {
        return false;
    }
}

bool coordinateFromLatLonParams(const std::unordered_map<std::string, std::string>& params,
                                Coordinate& pointOut) {
    double lat = 0.0;
    double lon = 0.0;
    if (!tryGetDouble(params, "lat", lat) || !tryGetDouble(params, "lon", lon)) {
        return false;
    }
    if (!std::isfinite(lat) || !std::isfinite(lon) || lat < -90.0 || lat > 90.0 ||
        lon < -180.0 || lon > 180.0) {
        return false;
    }
    pointOut.latE7 = static_cast<int32_t>(std::llround(lat * kCoordinateScale));
    pointOut.lonE7 = static_cast<int32_t>(std::llround(lon * kCoordinateScale));
    return true;
}

bool queryContainsAddressTokens(const std::string& rawQuery, const std::string& address) {
    const std::vector<std::string> queryTokens = orderedSearchTokens(rawQuery);
    const std::vector<std::string> addressTokens = orderedSearchTokens(address);
    if (addressTokens.empty()) {
        return false;
    }
    for (const std::string& token : addressTokens) {
        if (std::find(queryTokens.begin(), queryTokens.end(), token) == queryTokens.end()) {
            return false;
        }
    }
    return true;
}

bool poiMatchesCategory(const OSMDataset& data, const PoiRecord& poi, const std::string& category) {
    if (category.empty()) {
        return false;
    }
    const std::string poiCategory = normalizeSearchText(data.resolve(poi.category));
    const std::string tagValue = normalizeSearchText(data.resolve(poi.tagValue));
    if (poiCategory == category || tagValue == category) {
        return true;
    }
    if (category == "park" && (poiCategory == "garden" || tagValue == "park" ||
                               tagValue == "recreation ground" || tagValue == "village green")) {
        return true;
    }
    return false;
}

int poiConceptMatchWeight(const OSMDataset& data,
                          const PoiRecord& poi,
                          const std::string& concept,
                          const std::string& productFamily);

int poiConceptMatchWeight(const OSMDataset& data,
                          const PoiRecord& poi,
                          const std::string& concept,
                          const std::string& productFamily) {
    const std::string tagKey = normalizeSearchText(data.resolve(poi.tagKey));
    const std::string tagValue = normalizeSearchText(data.resolve(poi.tagValue));
    if (const ProductFamilyRule* family = findProductFamilyRule(productFamily)) {
        for (const PoiTagRule& allowed : family->tags) {
            if (tagKey == allowed.key && tagValue == allowed.value) {
                return allowed.weight;
            }
        }
    }
    const ConceptRule* rule = findConceptRule(concept);
    if (rule == nullptr) {
        return 0;
    }
    for (const PoiTagRule& allowed : rule->tags) {
        if (tagKey == allowed.key && tagValue == allowed.value) {
            return allowed.weight;
        }
    }
    return 0;
}

std::vector<std::string> adminTokensForLinks(const OSMDataset& data,
                                             const std::vector<uint32_t>& links,
                                             uint32_t offset,
                                             uint32_t size) {
    std::vector<std::string> tokens;
    appendAdminTokens(tokens, data, links, offset, size);
    uniqueTokens(tokens);
    return tokens;
}

const std::vector<GeocodeRef>* findPostingList(const std::vector<GeocodePostingList>& lists,
                                               const std::string& token) {
    const auto it = std::lower_bound(lists.begin(), lists.end(), token,
        [](const GeocodePostingList& list, const std::string& value) {
            return list.token < value;
        });
    if (it == lists.end() || it->token != token) {
        return nullptr;
    }
    return &it->refs;
}

Coordinate streetRepresentativePoint(const OSMDataset& data, const StreetRecord& street) {
    if (street.geometrySize == 0 || street.geometryOffset >= data.streetGeometry.size()) {
        return Coordinate{};
    }
    const uint32_t available = std::min<uint32_t>(street.geometrySize,
        static_cast<uint32_t>(data.streetGeometry.size() - street.geometryOffset));
    return data.streetGeometry[street.geometryOffset + available / 2];
}

Coordinate adminRepresentativePoint(const OSMDataset& data, const AdminAreaRecord& area) {
    Coordinate point{};
    if (area.geometrySize == 0 || area.geometryOffset >= data.adminGeometry.size()) {
        return point;
    }
    int64_t lat = 0;
    int64_t lon = 0;
    const uint32_t available = std::min<uint32_t>(area.geometrySize,
        static_cast<uint32_t>(data.adminGeometry.size() - area.geometryOffset));
    for (uint32_t i = 0; i < available; ++i) {
        const Coordinate& current = data.adminGeometry[area.geometryOffset + i];
        lat += current.latE7;
        lon += current.lonE7;
    }
    point.latE7 = static_cast<int32_t>(lat / static_cast<int64_t>(available));
    point.lonE7 = static_cast<int32_t>(lon / static_cast<int64_t>(available));
    return point;
}

void writeHouseJson(std::ostringstream& json, const OSMDataset& data, uint32_t index, double distance) {
    const HouseRecord& house = data.houses[index];
    json << "{";
    json << "\"index\":" << index << ",";
    json << "\"id\":" << house.osmId << ",";
    json << "\"lat\":" << std::setprecision(10) << latitudeOf(house.point) << ",";
    json << "\"lon\":" << longitudeOf(house.point) << ",";
    json << "\"distanceMeters\":" << std::fixed << std::setprecision(2) << distance << ",";
    json << "\"addressLine\":\"" << jsonEscape(houseAddressLine(data, house)) << "\",";
    json << "\"street\":\"" << jsonEscape(data.resolve(house.streetName)) << "\",";
    json << "\"number\":\"" << jsonEscape(data.resolve(house.houseNumber)) << "\",";
    json << "\"postcode\":\"" << jsonEscape(data.resolve(house.postcode)) << "\",";
    json << "\"city\":\"" << jsonEscape(data.resolve(house.city)) << "\",";
    json << "\"country\":\"" << jsonEscape(data.resolve(house.country)) << "\",";
    json << "\"adminAreas\":";
    writeAdminRefsJson(json, data, data.houseAdminAreaIndexes, house.adminAreaOffset, house.adminAreaSize);
    json << "}";
}

void writeStreetJson(std::ostringstream& json, const OSMDataset& data, uint32_t index, bool includeGeometry, double distance) {
    const StreetRecord& street = data.streets[index];
    json << "{";
    json << "\"index\":" << index << ",";
    json << "\"id\":" << street.osmId << ",";
    json << "\"name\":\"" << jsonEscape(data.resolve(street.name)) << "\",";
    json << "\"label\":\"" << jsonEscape(streetDisplayLabel(data, street)) << "\",";
    json << "\"highway\":\"" << jsonEscape(data.resolve(street.highwayType)) << "\",";
    if (distance >= 0.0) {
        json << "\"distanceMeters\":" << std::fixed << std::setprecision(2) << distance << ",";
    }
    json << "\"adminAreas\":";
    writeAdminRefsJson(json, data, data.streetAdminAreaIndexes, street.adminAreaOffset, street.adminAreaSize);
    if (includeGeometry) {
        json << ",\"geometry\":[";
        for (uint32_t i = 0; i < street.geometrySize; ++i) {
            if (street.geometryOffset + i >= data.streetGeometry.size()) {
                break;
            }
            if (i > 0) {
                json << ",";
            }
            const Coordinate& point = data.streetGeometry[street.geometryOffset + i];
            json << "[" << std::setprecision(10) << latitudeOf(point) << "," << longitudeOf(point) << "]";
        }
        json << "]";
    }
    json << "}";
}

void writePoiJson(std::ostringstream& json, const OSMDataset& data, uint32_t index, double distance) {
    const PoiRecord& poi = data.pois[index];
    json << "{";
    json << "\"index\":" << index << ",";
    json << "\"id\":" << poi.osmId << ",";
    json << "\"name\":\"" << jsonEscape(data.resolve(poi.name)) << "\",";
    json << "\"label\":\"" << jsonEscape(poiDisplayLabel(data, poi)) << "\",";
    json << "\"category\":\"" << jsonEscape(data.resolve(poi.category)) << "\",";
    json << "\"tagKey\":\"" << jsonEscape(data.resolve(poi.tagKey)) << "\",";
    json << "\"tagValue\":\"" << jsonEscape(data.resolve(poi.tagValue)) << "\",";
    json << "\"brand\":\"" << jsonEscape(data.resolve(poi.brand)) << "\",";
    json << "\"lat\":" << std::setprecision(10) << latitudeOf(poi.point) << ",";
    json << "\"lon\":" << longitudeOf(poi.point) << ",";
    if (distance >= 0.0) {
        json << "\"distanceMeters\":" << std::fixed << std::setprecision(2) << distance << ",";
    }
    json << "\"adminAreas\":";
    writeAdminRefsJson(json, data, data.poiAdminAreaIndexes, poi.adminAreaOffset, poi.adminAreaSize);
    json << "}";
}

void writeStreetViewportJson(std::ostringstream& json,
                             const OSMDataset& data,
                             uint32_t index,
                             const BoundingBox& viewport) {
    const StreetRecord& street = data.streets[index];
    json << "{";
    json << "\"index\":" << index << ",";
    json << "\"id\":" << street.osmId << ",";
    json << "\"name\":\"" << jsonEscape(data.resolve(street.name)) << "\",";
    json << "\"label\":\"" << jsonEscape(streetDisplayLabel(data, street)) << "\",";
    json << "\"highway\":\"" << jsonEscape(data.resolve(street.highwayType)) << "\",";
    json << "\"adminAreas\":";
    writeAdminRefsJson(json, data, data.streetAdminAreaIndexes, street.adminAreaOffset, street.adminAreaSize);
    json << ",\"geometry\":[";
    bool firstPoint = true;
    const uint32_t available = street.geometryOffset < data.streetGeometry.size()
        ? std::min<uint32_t>(street.geometrySize, static_cast<uint32_t>(data.streetGeometry.size() - street.geometryOffset))
        : 0;
    for (uint32_t i = 0; i < available; ++i) {
        const Coordinate& point = data.streetGeometry[street.geometryOffset + i];
        if (!viewport.contains(point)) {
            continue;
        }
        if (!firstPoint) {
            json << ",";
        }
        firstPoint = false;
        json << "[" << std::setprecision(10) << latitudeOf(point) << "," << longitudeOf(point) << "]";
    }
    json << "]}";
}

GeometryWriteStats writeGeometryJson(std::ostringstream& json,
                                      const std::vector<Coordinate>& geometry,
                                      uint32_t offset,
                                      uint32_t size,
                                      uint32_t maxPoints) {
    GeometryWriteStats stats;
    if (offset >= geometry.size() || size == 0) {
        json << "[]";
        return stats;
    }

    const uint32_t available = std::min<uint32_t>(size, static_cast<uint32_t>(geometry.size() - offset));
    stats.sourcePoints = available;
    const uint32_t targetPoints = maxPoints == 0 ? available : std::min(maxPoints, available);
    const uint32_t step = targetPoints >= available ? 1 :
        static_cast<uint32_t>(std::ceil(static_cast<double>(available) / static_cast<double>(targetPoints)));
    stats.simplified = step > 1;

    json << "[";
    bool first = true;
    uint32_t written = 0;
    for (uint32_t i = 0; i < available; i += step) {
        if (!first) {
            json << ",";
        }
        first = false;
        const Coordinate& point = geometry[offset + i];
        json << "[" << std::setprecision(10) << latitudeOf(point) << "," << longitudeOf(point) << "]";
        ++written;
    }
    if (available > 1 && step > 1 && (available - 1) % step != 0) {
        if (!first) {
            json << ",";
        }
        const Coordinate& point = geometry[offset + available - 1];
        json << "[" << std::setprecision(10) << latitudeOf(point) << "," << longitudeOf(point) << "]";
        ++written;
    }
    json << "]";
    stats.writtenPoints = written;
    return stats;
}

void writeAdminAreaJson(std::ostringstream& json,
                        const OSMDataset& data,
                        uint32_t index,
                        bool includeGeometry,
                        uint32_t maxGeometryPoints = 0) {
    const AdminAreaRecord& area = data.adminAreas[index];
    json << "{";
    json << "\"index\":" << index << ",";
    json << "\"id\":" << area.osmId << ",";
    json << "\"name\":\"" << jsonEscape(data.resolve(area.name)) << "\",";
    json << "\"adminLevel\":" << static_cast<int>(area.adminLevel) << ",";
    json << "\"parentAreas\":";
    writeAdminRefsJson(json, data, data.adminParentAreaIndexes, area.parentAreaOffset, area.parentAreaSize);
    if (includeGeometry) {
        json << ",\"geometry\":";
        const GeometryWriteStats stats = writeGeometryJson(json, data.adminGeometry,
                                                           area.geometryOffset,
                                                           area.geometrySize,
                                                           maxGeometryPoints);
        json << ",\"geometryMeta\":{";
        json << "\"sourcePoints\":" << stats.sourcePoints << ",";
        json << "\"writtenPoints\":" << stats.writtenPoints << ",";
        json << "\"simplified\":" << (stats.simplified ? "true" : "false");
        json << "}";
    }
    json << "}";
}

double bboxAreaScore(const BoundingBox& bbox) {
    if (!bbox.valid) {
        return std::numeric_limits<double>::max();
    }
    const double latSpan = static_cast<double>(std::max<int32_t>(1, bbox.maxLatE7 - bbox.minLatE7));
    const double lonSpan = static_cast<double>(std::max<int32_t>(1, bbox.maxLonE7 - bbox.minLonE7));
    return latSpan * lonSpan;
}

std::vector<uint32_t> sortedUniqueAdminRefs(const OSMDataset& data, std::vector<uint32_t> refs) {
    refs.erase(std::remove_if(refs.begin(), refs.end(), [&data](uint32_t index) {
        return index >= data.adminAreas.size();
    }), refs.end());
    std::sort(refs.begin(), refs.end(), [&data](uint32_t left, uint32_t right) {
        const AdminAreaRecord& leftArea = data.adminAreas[left];
        const AdminAreaRecord& rightArea = data.adminAreas[right];
        if (leftArea.adminLevel != rightArea.adminLevel) {
            return leftArea.adminLevel < rightArea.adminLevel;
        }
        return leftArea.osmId < rightArea.osmId;
    });
    refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
    return refs;
}

std::vector<uint32_t> compactAdminRefsByLevel(const OSMDataset& data, const std::vector<uint32_t>& refs) {
    std::vector<uint32_t> compact;
    compact.reserve(refs.size());
    for (uint32_t areaIndex : refs) {
        if (compact.empty() ||
            data.adminAreas[compact.back()].adminLevel != data.adminAreas[areaIndex].adminLevel) {
            compact.push_back(areaIndex);
            continue;
        }
        if (bboxAreaScore(data.adminAreas[areaIndex].bbox) < bboxAreaScore(data.adminAreas[compact.back()].bbox)) {
            compact.back() = areaIndex;
        }
    }
    return compact;
}

std::vector<uint32_t> adminChainForArea(const OSMDataset& data, uint32_t areaIndex) {
    if (areaIndex >= data.adminAreas.size()) {
        return {};
    }
    const AdminAreaRecord& area = data.adminAreas[areaIndex];
    std::vector<uint32_t> refs = adminRefsFromLinks(data.adminParentAreaIndexes,
                                                   area.parentAreaOffset,
                                                   area.parentAreaSize,
                                                   data.adminAreas.size());
    refs.push_back(areaIndex);
    return compactAdminRefsByLevel(data, sortedUniqueAdminRefs(data, std::move(refs)));
}

std::vector<uint32_t> coherentAdminChainForRawMatches(const OSMDataset& data, const std::vector<uint32_t>& matches) {
    uint32_t anchor = std::numeric_limits<uint32_t>::max();
    for (uint32_t areaIndex : matches) {
        if (areaIndex >= data.adminAreas.size()) {
            continue;
        }
        const AdminAreaRecord& area = data.adminAreas[areaIndex];
        if (anchor == std::numeric_limits<uint32_t>::max()) {
            anchor = areaIndex;
            continue;
        }
        const AdminAreaRecord& selected = data.adminAreas[anchor];
        if (area.adminLevel > selected.adminLevel ||
            (area.adminLevel == selected.adminLevel && bboxAreaScore(area.bbox) < bboxAreaScore(selected.bbox))) {
            anchor = areaIndex;
        }
    }
    if (anchor != std::numeric_limits<uint32_t>::max()) {
        std::vector<uint32_t> chain = adminChainForArea(data, anchor);
        if (chain.size() > 1 || data.adminAreas[anchor].parentAreaSize > 0) {
            return chain;
        }
    }

    std::vector<uint32_t> bestByLevel(13, std::numeric_limits<uint32_t>::max());
    for (uint32_t areaIndex : matches) {
        if (areaIndex >= data.adminAreas.size()) {
            continue;
        }
        const AdminAreaRecord& area = data.adminAreas[areaIndex];
        if (area.adminLevel >= bestByLevel.size()) {
            continue;
        }
        uint32_t& selected = bestByLevel[area.adminLevel];
        if (selected == std::numeric_limits<uint32_t>::max() ||
            bboxAreaScore(area.bbox) < bboxAreaScore(data.adminAreas[selected].bbox)) {
            selected = areaIndex;
        }
    }

    std::vector<uint32_t> chain;
    for (uint32_t areaIndex : bestByLevel) {
        if (areaIndex != std::numeric_limits<uint32_t>::max()) {
            chain.push_back(areaIndex);
        }
    }
    std::sort(chain.begin(), chain.end(), [&data](uint32_t left, uint32_t right) {
        const AdminAreaRecord& leftArea = data.adminAreas[left];
        const AdminAreaRecord& rightArea = data.adminAreas[right];
        if (leftArea.adminLevel != rightArea.adminLevel) {
            return leftArea.adminLevel < rightArea.adminLevel;
        }
        return leftArea.osmId < rightArea.osmId;
    });
    return chain;
}

}  // namespace

Server::Server(const OSMDataset& data, int port, int defaultHouseLimit)
    : data_(data),
      port_(port),
      defaultHouseLimit_(defaultHouseLimit),
      houseIndexCellSizeE7_(kCoordinateScale / 50),
      streetIndexCellSizeE7_(kCoordinateScale / 20),
      adminIndexCellSizeE7_(kCoordinateScale / 4) {
    const auto spatialStarted = std::chrono::steady_clock::now();
    buildSpatialIndexes();
    const auto spatialFinished = std::chrono::steady_clock::now();
    buildMetrics_.spatialIndexBuildMs =
        std::chrono::duration<double, std::milli>(spatialFinished - spatialStarted).count();

    const auto geocodeStarted = std::chrono::steady_clock::now();
    buildGeocodeIndex();
    const auto geocodeFinished = std::chrono::steady_clock::now();
    buildMetrics_.geocodeIndexBuildMs =
        std::chrono::duration<double, std::milli>(geocodeFinished - geocodeStarted).count();

    std::cout << "Server index build timings:\n";
    std::cout << "  Spatial indexes:             " << std::fixed << std::setprecision(3)
              << buildMetrics_.spatialIndexBuildMs << " ms\n";
    std::cout << "  Forward geocode index:       " << buildMetrics_.geocodeIndexBuildMs << " ms\n";
    std::cout << "  Forward geocode terms:       "
              << (forwardIndex_ == nullptr ? 0 : forwardIndex_->context.size()) << "\n";
    std::cout << "  Forward geocode postings:    " << buildMetrics_.geocodePostingRefs << "\n";
    std::cout << "  Primary geocode terms:       "
              << (forwardIndex_ == nullptr ? 0 : forwardIndex_->primary.size()) << "\n";
    std::cout << "  Primary geocode postings:    " << buildMetrics_.geocodePrimaryPostingRefs << "\n";
    std::cout << "  Largest geocode posting:     " << buildMetrics_.geocodeLargestPostingList << "\n";
    std::cout << "  Estimated geocode index RAM: "
              << (static_cast<double>(buildMetrics_.geocodeEstimatedBytes) / (1024.0 * 1024.0))
              << " MB\n";
}

Server::~Server() {
    stop();
}

void Server::startManagedOllama() {
    if (envFlagDisabled("OSM_AUTO_OLLAMA")) {
        std::cout << "Ollama auto-start disabled by OSM_AUTO_OLLAMA=0" << std::endl;
        return;
    }

    const OllamaSettings settings = ollamaSettingsFromEnvironment();
    if (!isLocalOllamaHost(settings.host)) {
        std::cout << "Ollama auto-start skipped for non-local host " << settings.host << std::endl;
        return;
    }

    if (ollamaResponds(settings, 500)) {
        std::cout << "Ollama already running at " << settings.host << ":" << settings.port << std::endl;
        if (!envFlagDisabled("OSM_OLLAMA_WARMUP")) {
            std::cout << "Warming Ollama model " << settings.model << std::endl;
            if (!warmOllamaModel(settings)) {
                std::cerr << "Ollama model warmup failed; natural-language queries may be slow or unavailable" << std::endl;
            }
        }
        return;
    }

#ifdef _WIN32
    std::cerr << "Ollama auto-start is only implemented for the Ubuntu/Linux server runtime" << std::endl;
#else
    const std::string binary = ollamaBinaryPath();
    std::cout << "Starting Ollama server: " << binary << " serve" << std::endl;
    const pid_t child = ::fork();
    if (child < 0) {
        std::cerr << "Failed to fork Ollama server process: " << std::strerror(errno) << std::endl;
        return;
    }
    if (child == 0) {
        ::setsid();
        const std::string bindAddress = settings.host + ":" + std::to_string(settings.port);
        ::setenv("OLLAMA_HOST", bindAddress.c_str(), 1);
        if (std::freopen("/tmp/osm-ollama-serve.log", "a", stdout) == nullptr) {
            std::cerr << "Failed to redirect Ollama stdout" << std::endl;
        }
        if (std::freopen("/tmp/osm-ollama-serve.log", "a", stderr) == nullptr) {
            std::cerr << "Failed to redirect Ollama stderr" << std::endl;
        }
        if (binary.find('/') != std::string::npos) {
            ::execl(binary.c_str(), binary.c_str(), "serve", static_cast<char*>(nullptr));
        } else {
            ::execlp(binary.c_str(), binary.c_str(), "serve", static_cast<char*>(nullptr));
        }
        std::cerr << "Failed to exec Ollama binary " << binary << ": " << std::strerror(errno) << std::endl;
        std::_Exit(127);
    }

    managedOllamaPid_ = static_cast<int>(child);
    managedOllamaStarted_ = true;
    if (!waitForOllamaReady(settings, managedOllamaPid_, 30)) {
        std::cerr << "Ollama did not become ready on " << settings.host << ":" << settings.port << std::endl;
        stopManagedOllama();
        return;
    }

    std::cout << "Ollama server ready at " << settings.host << ":" << settings.port << std::endl;
    if (!envFlagDisabled("OSM_OLLAMA_WARMUP")) {
        std::cout << "Warming Ollama model " << settings.model << std::endl;
        if (!warmOllamaModel(settings)) {
            std::cerr << "Ollama model warmup failed; natural-language queries may be slow or unavailable" << std::endl;
        }
    }
#endif
}

void Server::stopManagedOllama() {
    if (!managedOllamaStarted_ || managedOllamaPid_ <= 0) {
        return;
    }
#ifndef _WIN32
    std::cout << "Stopping managed Ollama server" << std::endl;
    terminateProcessGroup(managedOllamaPid_);
#endif
    managedOllamaStarted_ = false;
    managedOllamaPid_ = -1;
}

bool Server::start() {
    if (running_) {
        return true;
    }
    httpServer_ = std::make_unique<httplib::Server>();
    configureRoutes(*httpServer_);
    if (!httpServer_->is_valid()) {
        std::cerr << "Failed to initialize HTTP server" << std::endl;
        httpServer_.reset();
        return false;
    }
    if (!httpServer_->bind_to_port("0.0.0.0", port_)) {
        std::cerr << "Failed to bind to port " << port_ << std::endl;
        httpServer_.reset();
        return false;
    }
    startManagedOllama();
    running_ = true;
    serverThread_ = std::make_unique<std::thread>(&Server::serverLoop, this);
    return true;
}

void Server::stop() {
    if (!running_ && (!serverThread_ || !serverThread_->joinable())) {
        stopManagedOllama();
        return;
    }
    running_ = false;
    if (httpServer_) {
        httpServer_->stop();
    }
    if (serverThread_ && serverThread_->joinable()) {
        serverThread_->join();
    }
    httpServer_.reset();
    stopManagedOllama();
}

void Server::configureRoutes(httplib::Server& server) {
    server.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"}
    });

    auto handleGet = [this](const httplib::Request& request, httplib::Response& response) {
        const ServerResponse payload = handleRequest(request.path, paramsFromRequest(request));
        response.status = payload.statusCode;
        response.set_content(payload.body, payload.contentType);
    };

    server.Get(R"(/.*)", handleGet);
    server.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& response) { response.status = 204; });
    server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
        response.status = response.status >= 400 ? response.status : 404;
        if (response.body.empty()) {
            response.set_content("{\"error\":\"endpoint not found\"}", kJsonContentType);
        }
    });
    server.set_exception_handler([](const httplib::Request&, httplib::Response& response, std::exception_ptr) {
        response.status = 500;
        response.set_content("{\"error\":\"internal server error\"}", kJsonContentType);
    });
}

void Server::serverLoop() {
    std::cout << "Server listening on http://localhost:" << port_ << std::endl;
    if (httpServer_ && !httpServer_->listen_after_bind() && running_) {
        std::cerr << "HTTP server stopped unexpectedly" << std::endl;
    }
    running_ = false;
}

void Server::buildSpatialIndexes() {
    houseIndex_.clear();
    streetIndex_.clear();
    adminIndex_.clear();
    poiIndex_.clear();
    houseIndexHasBounds_ = false;
    buildMetrics_.houseGridCells = 0;
    buildMetrics_.streetGridCells = 0;
    buildMetrics_.adminGridCells = 0;
    buildMetrics_.poiGridCells = 0;

    const uint64_t totalRecords = static_cast<uint64_t>(data_.houses.size() + data_.streets.size() +
                                                        data_.adminAreas.size() + data_.pois.size());
    ConsoleProgress progress("Server spatial index build", totalRecords, "records");
    uint64_t processedRecords = 0;
    const auto updateProgress = [&]() {
        if (processedRecords % 65536 == 0 || processedRecords == totalRecords) {
            progress.update(processedRecords);
        }
    };

    houseIndex_.reserve(std::max<size_t>(1024, data_.houses.size() / 64));
    for (size_t i = 0; i < data_.houses.size(); ++i) {
        const HouseRecord& house = data_.houses[i];
        const int32_t latCell = floorDiv(house.point.latE7, houseIndexCellSizeE7_);
        const int32_t lonCell = floorDiv(house.point.lonE7, houseIndexCellSizeE7_);
        houseIndex_[gridKey(latCell, lonCell)].push_back(static_cast<uint32_t>(i));
        if (!houseIndexHasBounds_) {
            minHouseLatCell_ = maxHouseLatCell_ = latCell;
            minHouseLonCell_ = maxHouseLonCell_ = lonCell;
            houseIndexHasBounds_ = true;
        } else {
            minHouseLatCell_ = std::min(minHouseLatCell_, latCell);
            maxHouseLatCell_ = std::max(maxHouseLatCell_, latCell);
            minHouseLonCell_ = std::min(minHouseLonCell_, lonCell);
            maxHouseLonCell_ = std::max(maxHouseLonCell_, lonCell);
        }
        ++processedRecords;
        updateProgress();
    }

    streetIndex_.reserve(std::max<size_t>(1024, data_.streets.size() / 8));
    for (size_t i = 0; i < data_.streets.size(); ++i) {
        addBoxToIndex(data_.streets[i].bbox, static_cast<uint32_t>(i), streetIndexCellSizeE7_, streetIndex_);
        ++processedRecords;
        updateProgress();
    }

    adminIndex_.reserve(std::max<size_t>(256, data_.adminAreas.size() * 4));
    for (size_t i = 0; i < data_.adminAreas.size(); ++i) {
        addBoxToIndex(data_.adminAreas[i].bbox, static_cast<uint32_t>(i), adminIndexCellSizeE7_, adminIndex_);
        ++processedRecords;
        updateProgress();
    }

    poiIndex_.reserve(std::max<size_t>(1024, data_.pois.size() / 16));
    for (size_t i = 0; i < data_.pois.size(); ++i) {
        addBoxToIndex(data_.pois[i].bbox, static_cast<uint32_t>(i), houseIndexCellSizeE7_, poiIndex_);
        ++processedRecords;
        updateProgress();
    }
    progress.finish(processedRecords);
    buildMetrics_.houseGridCells = houseIndex_.size();
    buildMetrics_.streetGridCells = streetIndex_.size();
    buildMetrics_.adminGridCells = adminIndex_.size();
    buildMetrics_.poiGridCells = poiIndex_.size();
}

void Server::buildGeocodeIndex() {
    buildMetrics_.geocodePostingRefs = 0;
    buildMetrics_.geocodePrimaryPostingRefs = 0;
    buildMetrics_.geocodeLargestPostingList = 0;
    buildMetrics_.geocodeLargestPrimaryPostingList = 0;
    buildMetrics_.geocodeEstimatedBytes = 0;
    forwardIndex_ = nullptr;

    if (!data_.forwardGeocodeIndex.available) {
        throw std::runtime_error(
            "Binary snapshot has no embedded forward index. Re-parse the PBF and save a new binary snapshot.");
    }

    forwardIndex_ = &data_.forwardGeocodeIndex;
    buildMetrics_.geocodePostingRefs = forwardIndex_->postingRefs;
    buildMetrics_.geocodePrimaryPostingRefs = forwardIndex_->primaryPostingRefs;
    buildMetrics_.geocodeLargestPostingList = forwardIndex_->largestPostingList;
    buildMetrics_.geocodeLargestPrimaryPostingList = forwardIndex_->largestPrimaryPostingList;
    buildMetrics_.geocodeEstimatedBytes = forwardIndex_->estimatedBytes;
}

Server::ServerResponse Server::handleRequest(const std::string& path,
        const std::unordered_map<std::string, std::string>& params) const {
    if (path == "/" || path == "/index.html") {
        return httpResponse(200, kHtmlContentType, readFrontendHtml());
    }

    if (path == "/api/stats") {
        std::ostringstream json;
        json << "{";
        json << "\"houses\":" << data_.stats.housesTotal << ",";
        json << "\"housesFromNodes\":" << data_.stats.housesFromNodes << ",";
        json << "\"housesFromWays\":" << data_.stats.housesFromWays << ",";
        json << "\"housesFromRelations\":" << data_.stats.housesFromRelations << ",";
        json << "\"housesMissingStreet\":" << data_.stats.housesMissingStreet << ",";
        json << "\"housesMissingNumber\":" << data_.stats.housesMissingHouseNumber << ",";
        json << "\"housesWithAdminAreas\":" << data_.stats.housesWithAdminAreas << ",";
        json << "\"houseAdminAreaLinks\":" << data_.stats.houseAdminAreaLinks << ",";
        json << "\"streets\":" << data_.stats.streetsTotal << ",";
        json << "\"streetsWithAdminAreas\":" << data_.stats.streetsWithAdminAreas << ",";
        json << "\"streetAdminAreaLinks\":" << data_.stats.streetAdminAreaLinks << ",";
        json << "\"adminAreas\":" << data_.stats.adminAreasTotal << ",";
        json << "\"pois\":" << data_.stats.poisTotal << ",";
        json << "\"poisFromNodes\":" << data_.stats.poisFromNodes << ",";
        json << "\"poisFromWays\":" << data_.stats.poisFromWays << ",";
        json << "\"poisWithAdminAreas\":" << data_.stats.poisWithAdminAreas << ",";
        json << "\"poiAdminAreaLinks\":" << data_.stats.poiAdminAreaLinks << ",";
        json << "\"adminAreasWithParents\":" << data_.stats.adminAreasWithParents << ",";
        json << "\"adminParentAreaLinks\":" << data_.stats.adminParentAreaLinks << ",";
        json << "\"geocodeIndexTerms\":" << (forwardIndex_ == nullptr ? 0 : forwardIndex_->context.size()) << ",";
        json << "\"geocodeIndexPostings\":" << buildMetrics_.geocodePostingRefs << ",";
        json << "\"geocodeLargestPostingList\":" << buildMetrics_.geocodeLargestPostingList << ",";
        json << "\"geocodePrimaryIndexTerms\":" << (forwardIndex_ == nullptr ? 0 : forwardIndex_->primary.size()) << ",";
        json << "\"geocodePrimaryIndexPostings\":" << buildMetrics_.geocodePrimaryPostingRefs << ",";
        json << "\"geocodeLargestPrimaryPostingList\":" << buildMetrics_.geocodeLargestPrimaryPostingList << ",";
        json << "\"timings\":{";
        json << "\"relationScan\":" << std::fixed << std::setprecision(3) << data_.stats.relationScanSeconds << ",";
        json << "\"extraction\":" << data_.stats.extractionSeconds << ",";
        json << "\"relationAssembly\":" << data_.stats.relationAssemblySeconds << ",";
        json << "\"adminLookup\":" << data_.stats.adminAttributionSeconds << ",";
        json << "\"streetConnection\":" << data_.stats.connectionSeconds << ",";
        json << "\"serverSpatialIndexBuildMs\":" << buildMetrics_.spatialIndexBuildMs << ",";
        json << "\"serverGeocodeIndexBuildMs\":" << buildMetrics_.geocodeIndexBuildMs << ",";
        json << "\"parserComponentSeconds\":" <<
            (data_.stats.relationScanSeconds + data_.stats.extractionSeconds +
             data_.stats.relationAssemblySeconds + data_.stats.adminAttributionSeconds +
             data_.stats.connectionSeconds) << ",";
        json << "\"totalSeconds\":" << data_.stats.totalSeconds << ",";
        json << "\"totalMinutes\":" << (data_.stats.totalSeconds / 60.0) << "},";
        json << "\"memory\":{";
        json << "\"datasetMB\":" << std::setprecision(2) << data_.stats.datasetMB << ",";
        json << "\"rssPeakMB\":" << data_.stats.rssPeakMB << ",";
        json << "\"geocodeIndexEstimatedMB\":" <<
            (static_cast<double>(buildMetrics_.geocodeEstimatedBytes) / (1024.0 * 1024.0)) << "},";
        json << "\"indexes\":{";
        json << "\"houseGridCells\":" << buildMetrics_.houseGridCells << ",";
        json << "\"streetGridCells\":" << buildMetrics_.streetGridCells << ",";
        json << "\"adminGridCells\":" << buildMetrics_.adminGridCells << ",";
        json << "\"poiGridCells\":" << buildMetrics_.poiGridCells << "},";
        json << "\"throughput\":{";
        json << "\"inputMBps\":" << data_.stats.inputMegabytesPerSecond << ",";
        json << "\"objectsPerSecond\":" << data_.stats.objectsPerSecond << "}";
        json << "}";
        return httpResponse(200, kJsonContentType, json.str());
    }

    if (path == "/api/geocode") {
        const auto queryIt = params.find("q") != params.end() ? params.find("q") : params.find("query");
        if (queryIt == params.end() || queryIt->second.empty()) {
            return httpResponse(400, kJsonContentType, "{\"error\":\"missing q parameter\"}");
        }

        const auto started = std::chrono::steady_clock::now();
        const QueryAnalysis query = analyzeQuery(queryIt->second);
        const std::vector<std::string>& queryTokens = query.tokens;
        const int limit = getIntOrDefault(params, "limit", 20, 1, 100);
        const bool usePrimaryIndex = queryTokens.size() == 1 && !query.hasNumber && !query.hasPostcode;
        const std::vector<GeocodePostingList>& searchIndex =
            usePrimaryIndex ? forwardIndex_->primary : forwardIndex_->context;

        std::unordered_map<uint64_t, GeocodeCandidate> candidates;
        bool missingToken = queryTokens.empty();
        std::vector<const std::vector<GeocodeRef>*> postingLists;
        std::vector<std::vector<GeocodeRef>> expandedPostingLists;
        std::vector<std::vector<std::string>> queryTokenAlternatives;
        postingLists.reserve(queryTokens.size());
        expandedPostingLists.reserve(queryTokens.size());
        queryTokenAlternatives.reserve(queryTokens.size());
        for (const std::string& token : queryTokens) {
            std::vector<std::string> alternatives;
            alternatives.push_back(token);
            const std::vector<GeocodeRef>* postings = findPostingList(searchIndex, token);
            if (postings != nullptr) {
                postingLists.push_back(postings);
                queryTokenAlternatives.push_back(std::move(alternatives));
                continue;
            }

            std::vector<GeocodeRef> mergedRefs;
            std::vector<std::string> expansions = substringTokenExpansions(*forwardIndex_, token, 12);
            if (expansions.empty()) {
                expansions = fuzzyTokenExpansions(*forwardIndex_, token, 8);
            }

            for (const std::string& expandedToken : expansions) {
                const std::vector<GeocodeRef>* expandedPostings = findPostingList(searchIndex, expandedToken);
                if (expandedPostings == nullptr) {
                    continue;
                }
                alternatives.push_back(expandedToken);
                mergedRefs.insert(mergedRefs.end(), expandedPostings->begin(), expandedPostings->end());
            }

            if (mergedRefs.empty()) {
                missingToken = true;
                break;
            }

            std::sort(mergedRefs.begin(), mergedRefs.end(), geocodeRefLess);
            mergedRefs.erase(std::unique(mergedRefs.begin(), mergedRefs.end(), [](const GeocodeRef& left, const GeocodeRef& right) {
                return left.type == right.type && left.index == right.index;
            }), mergedRefs.end());
            expandedPostingLists.push_back(std::move(mergedRefs));
            postingLists.push_back(&expandedPostingLists.back());
            uniqueTokens(alternatives);
            queryTokenAlternatives.push_back(std::move(alternatives));
        }

        uint64_t inspectedPostingRefs = 0;
        uint64_t anchorPostingRefs = 0;
        if (!missingToken) {
            std::sort(postingLists.begin(), postingLists.end(), [](const auto* left, const auto* right) {
                return left->size() < right->size();
            });
            const std::vector<GeocodeRef>& anchor = *postingLists.front();
            anchorPostingRefs = anchor.size();
            for (const GeocodeRef& ref : anchor) {
                uint16_t matched = 1;
                for (size_t postingListIndex = 1; postingListIndex < postingLists.size(); ++postingListIndex) {
                    ++inspectedPostingRefs;
                    if (std::binary_search(postingLists[postingListIndex]->begin(),
                                           postingLists[postingListIndex]->end(),
                                           ref,
                                           geocodeRefLess)) {
                        ++matched;
                    } else {
                        break;
                    }
                }
                if (matched != queryTokens.size()) {
                    continue;
                }
                const uint64_t key = geocodeKey(ref.type, ref.index);
                GeocodeCandidate& candidate = candidates[key];
                candidate.type = ref.type;
                candidate.index = ref.index;
                candidate.matchedTokens = matched;
                ++inspectedPostingRefs;
            }
        }

        std::vector<GeocodeCandidate> results;
        if (!missingToken) {
            results.reserve(std::min<size_t>(candidates.size(), static_cast<size_t>(limit) * 4));
            for (auto& item : candidates) {
                GeocodeCandidate candidate = item.second;
                if (candidate.matchedTokens != queryTokens.size()) {
                    continue;
                }

                if (candidate.type == kGeocodeHouse) {
                    if (candidate.index >= data_.houses.size() || !query.hasNumber) {
                        continue;
                    }
                    const HouseRecord& house = data_.houses[candidate.index];
                    const std::vector<std::string> houseNumberTokens = tokensFromText(data_.resolve(house.houseNumber));
                    const std::vector<std::string> streetTokens = tokensFromText(data_.resolve(house.streetName));
                    const std::vector<std::string> postcodeTokens = tokensFromText(data_.resolve(house.postcode));
                    const std::vector<std::string> cityTokens = tokensFromText(data_.resolve(house.city));
                    const std::vector<std::string> adminTokens = adminTokensForLinks(data_, data_.houseAdminAreaIndexes,
                                                                                    house.adminAreaOffset,
                                                                                    house.adminAreaSize);
                    const bool numberMatches = containsAnyToken(houseNumberTokens, query.numberTokens);
                    const bool postcodeMatches = !query.hasPostcode || containsAnyToken(postcodeTokens, query.postcodeTokens);
                    if (!numberMatches || !postcodeMatches) {
                        continue;
                    }
                    const uint16_t streetMatches = countMatchingSortedTokens(query.textTokens, streetTokens);
                    const uint16_t cityMatches = countMatchingSortedTokens(query.textTokens, cityTokens);
                    const uint16_t adminMatches = countMatchingSortedTokens(query.textTokens, adminTokens);
                    candidate.score = 120 + static_cast<int>(candidate.matchedTokens) * 15 +
                        (numberMatches ? 80 : 0) +
                        (query.hasPostcode ? 45 : 0) +
                        static_cast<int>(streetMatches) * 25 +
                        static_cast<int>(cityMatches + adminMatches) * 12;
                } else if (candidate.type == kGeocodeStreet) {
                    if (candidate.index >= data_.streets.size()) {
                        continue;
                    }
                    const StreetRecord& street = data_.streets[candidate.index];
                    if (queryTokens.size() == 1) {
                        std::vector<std::string> ownTokens = tokenizeSearchText(data_.resolve(street.name));
                        const std::vector<std::string>& alternatives = queryTokenAlternatives.empty()
                            ? queryTokens
                            : queryTokenAlternatives.front();
                        if (!containsAnyToken(ownTokens, alternatives)) {
                            continue;
                        }
                    }
                    const bool exactStreetName = normalizeSearchText(data_.resolve(street.name)) ==
                        normalizeSearchText(queryIt->second);
                    const std::vector<std::string> streetTokens = tokensFromText(data_.resolve(street.name));
                    const std::vector<std::string> adminTokens = adminTokensForLinks(data_, data_.streetAdminAreaIndexes,
                                                                                    street.adminAreaOffset,
                                                                                    street.adminAreaSize);
                    const uint16_t streetMatches = countMatchingSortedTokens(query.textTokens, streetTokens);
                    const uint16_t adminMatches = countMatchingSortedTokens(query.textTokens, adminTokens);
                    candidate.score = (query.hasNumber ? 35 : 95) +
                        static_cast<int>(candidate.matchedTokens) * 15 +
                        static_cast<int>(streetMatches) * 25 +
                        static_cast<int>(adminMatches) * 10 +
                        (exactStreetName ? 50 : 0);
                } else if (candidate.type == kGeocodeAdmin) {
                    if (candidate.index >= data_.adminAreas.size()) {
                        continue;
                    }
                    const AdminAreaRecord& area = data_.adminAreas[candidate.index];
                    if (queryTokens.size() == 1) {
                        std::vector<std::string> ownTokens = tokenizeSearchText(data_.resolve(area.name));
                        const std::vector<std::string>& alternatives = queryTokenAlternatives.empty()
                            ? queryTokens
                            : queryTokenAlternatives.front();
                        if (!containsAnyToken(ownTokens, alternatives)) {
                            continue;
                        }
                    }
                    const bool exactAreaName = normalizeSearchText(data_.resolve(area.name)) ==
                        normalizeSearchText(queryIt->second);
                    const std::vector<std::string> nameTokens = tokensFromText(data_.resolve(area.name));
                    const std::vector<std::string> parentTokens = adminTokensForLinks(data_, data_.adminParentAreaIndexes,
                                                                                     area.parentAreaOffset,
                                                                                     area.parentAreaSize);
                    const uint16_t nameMatches = countMatchingSortedTokens(query.textTokens, nameTokens);
                    const uint16_t parentMatches = countMatchingSortedTokens(query.textTokens, parentTokens);
                    candidate.score = (query.hasNumber ? 10 : (queryTokens.size() == 1 ? 135 : 75)) +
                        static_cast<int>(candidate.matchedTokens) * 15 +
                        static_cast<int>(nameMatches) * 25 +
                        static_cast<int>(parentMatches) * 8 +
                        static_cast<int>(area.adminLevel) +
                        (exactAreaName ? 60 : 0);
                } else if (candidate.type == kGeocodePoi) {
                    if (candidate.index >= data_.pois.size()) {
                        continue;
                    }
                    const PoiRecord& poi = data_.pois[candidate.index];
                    if (queryTokens.size() == 1) {
                        std::vector<std::string> ownTokens;
                        appendTokensFromText(ownTokens, data_.resolve(poi.name));
                        appendTokensFromText(ownTokens, data_.resolve(poi.brand));
                        appendTokensFromText(ownTokens, data_.resolve(poi.category));
                        uniqueTokens(ownTokens);
                        const std::vector<std::string>& alternatives = queryTokenAlternatives.empty()
                            ? queryTokens
                            : queryTokenAlternatives.front();
                        if (!containsAnyToken(ownTokens, alternatives)) {
                            continue;
                        }
                    }
                    std::vector<std::string> poiTokens;
                    appendTokensFromText(poiTokens, data_.resolve(poi.name));
                    appendTokensFromText(poiTokens, data_.resolve(poi.brand));
                    appendTokensFromText(poiTokens, data_.resolve(poi.category));
                    appendTokensFromText(poiTokens, data_.resolve(poi.tagValue));
                    uniqueTokens(poiTokens);
                    const std::vector<std::string> adminTokens = adminTokensForLinks(data_, data_.poiAdminAreaIndexes,
                                                                                    poi.adminAreaOffset,
                                                                                    poi.adminAreaSize);
                    const uint16_t poiMatches = countMatchingSortedTokens(query.textTokens, poiTokens);
                    const uint16_t adminMatches = countMatchingSortedTokens(query.textTokens, adminTokens);
                    const bool exactPoiName = normalizeSearchText(poiDisplayLabel(data_, poi)) ==
                        normalizeSearchText(queryIt->second);
                    candidate.score = (query.hasNumber ? 20 : 105) +
                        static_cast<int>(candidate.matchedTokens) * 15 +
                        static_cast<int>(poiMatches) * 30 +
                        static_cast<int>(adminMatches) * 12 +
                        (exactPoiName ? 55 : 0);
                } else {
                    continue;
                }
                results.push_back(candidate);
            }
        }

        std::sort(results.begin(), results.end(), [](const GeocodeCandidate& left, const GeocodeCandidate& right) {
            if (left.score != right.score) {
                return left.score > right.score;
            }
            if (left.type != right.type) {
                return left.type < right.type;
            }
            return left.index < right.index;
        });
        if (results.size() > static_cast<size_t>(limit)) {
            results.resize(static_cast<size_t>(limit));
        }

        const auto finished = std::chrono::steady_clock::now();
        const double elapsedMs = std::chrono::duration<double, std::milli>(finished - started).count();

        std::ostringstream json;
        json << "{";
        json << "\"query\":\"" << jsonEscape(queryIt->second) << "\",";
        json << "\"normalized\":\"" << jsonEscape(normalizeSearchText(queryIt->second)) << "\",";
        json << "\"queryTimeMs\":" << std::fixed << std::setprecision(3) << elapsedMs << ",";
        json << "\"indexMode\":\"" << (usePrimaryIndex ? "primary" : "context") << "\",";
        json << "\"inspectedPostingRefs\":" << inspectedPostingRefs << ",";
        json << "\"anchorPostingRefs\":" << anchorPostingRefs << ",";
        json << "\"candidateCountBeforeRanking\":" << candidates.size() << ",";
        json << "\"count\":" << results.size() << ",";
        json << "\"results\":[";
        bool first = true;
        for (const GeocodeCandidate& result : results) {
            if (!first) {
                json << ",";
            }
            first = false;
            json << "{";
            json << "\"score\":" << result.score << ",";
            if (result.type == kGeocodeHouse) {
                const HouseRecord& house = data_.houses[result.index];
                json << "\"type\":\"house\",";
                json << "\"label\":\"" << jsonEscape(houseAddressLine(data_, house)) << "\",";
                json << "\"lat\":" << std::setprecision(10) << latitudeOf(house.point) << ",";
                json << "\"lon\":" << longitudeOf(house.point) << ",";
                json << "\"house\":";
                writeHouseJson(json, data_, result.index, 0.0);
            } else if (result.type == kGeocodeStreet) {
                const StreetRecord& street = data_.streets[result.index];
                const Coordinate point = streetRepresentativePoint(data_, street);
                json << "\"type\":\"street\",";
                json << "\"label\":\"" << jsonEscape(streetDisplayLabel(data_, street)) << "\",";
                json << "\"lat\":" << std::setprecision(10) << latitudeOf(point) << ",";
                json << "\"lon\":" << longitudeOf(point) << ",";
                json << "\"street\":";
                writeStreetJson(json, data_, result.index, true, -1.0);
            } else if (result.type == kGeocodeAdmin) {
                const AdminAreaRecord& area = data_.adminAreas[result.index];
                const Coordinate point = adminRepresentativePoint(data_, area);
                json << "\"type\":\"admin_area\",";
                json << "\"label\":\"" << jsonEscape(data_.resolve(area.name)) << "\",";
                json << "\"lat\":" << std::setprecision(10) << latitudeOf(point) << ",";
                json << "\"lon\":" << longitudeOf(point) << ",";
                json << "\"adminArea\":";
                writeAdminAreaJson(json, data_, result.index, false);
            } else if (result.type == kGeocodePoi) {
                const PoiRecord& poi = data_.pois[result.index];
                json << "\"type\":\"poi\",";
                json << "\"label\":\"" << jsonEscape(poiDisplayLabel(data_, poi)) << "\",";
                json << "\"lat\":" << std::setprecision(10) << latitudeOf(poi.point) << ",";
                json << "\"lon\":" << longitudeOf(poi.point) << ",";
                json << "\"poi\":";
                writePoiJson(json, data_, result.index, -1.0);
            }
            json << "}";
        }
        json << "]}";
        return httpResponse(200, kJsonContentType, json.str());
    }

    if (path == "/api/natural-geocode") {
        const auto queryIt = params.find("q") != params.end() ? params.find("q") : params.find("query");
        if (queryIt == params.end() || queryIt->second.empty()) {
            return httpResponse(400, kJsonContentType, "{\"error\":\"missing q parameter\"}");
        }

        const auto started = std::chrono::steady_clock::now();
        const bool useLlm = params.find("useLlm") != params.end() &&
            (params.at("useLlm") == "1" || params.at("useLlm") == "true");
        NaturalIntent intent = useLlm ? parseOllamaIntent(queryIt->second) : NaturalIntent{};
        if (intent.type == NaturalIntentType::Unknown) {
            intent = parseDeterministicNaturalIntent(queryIt->second);
        }
        if (intent.type == NaturalIntentType::Unknown) {
            return httpResponse(400, kJsonContentType, "{\"error\":\"query intent not understood\"}");
        }

        if (intent.type == NaturalIntentType::NamedPoiInPlace) {
            std::unordered_map<std::string, std::string> geocodeParams;
            geocodeParams.emplace("q", trimCopy(intent.place + " " + intent.poiName));
            geocodeParams.emplace("limit", params.find("limit") == params.end() ? "20" : params.at("limit"));
            ServerResponse response = handleRequest("/api/geocode", geocodeParams);
            if (response.statusCode == 200) {
                const std::string prefix = "{\"naturalIntent\":\"named_poi_in_place\",\"usedLlm\":";
                response.body.replace(0, 1, prefix + std::string(intent.fromLlm ? "true" : "false") + ",");
            }
            return response;
        }

        Coordinate origin;
        std::string originSource;
        const bool canUseAddressOrigin = !intent.address.empty() &&
            (!intent.fromLlm || queryContainsAddressTokens(queryIt->second, intent.address));
        if (canUseAddressOrigin) {
            std::unordered_map<std::string, std::string> addressParams;
            addressParams.emplace("q", intent.address);
            addressParams.emplace("limit", "1");
            const ServerResponse addressResponse = handleRequest("/api/geocode", addressParams);
            if (addressResponse.statusCode == 200 && extractFirstLatLon(addressResponse.body, origin)) {
                originSource = "address";
            }
        }
        if (originSource.empty() && coordinateFromLatLonParams(params, origin)) {
            originSource = "viewport";
        }
        if (originSource.empty()) {
            const bool conceptIntent = intent.type == NaturalIntentType::NearestConceptToAddress;
            std::ostringstream json;
            json << "{\"query\":\"" << jsonEscape(queryIt->second) << "\",";
            json << "\"naturalIntent\":\"" << (conceptIntent ? "nearest_concept_to_address" : "nearest_category_to_address") << "\",";
            json << "\"usedLlm\":" << (intent.fromLlm ? "true" : "false") << ",";
            json << "\"llmVerified\":" << (intent.verifiedByLlm ? "true" : "false") << ",";
            json << "\"category\":\"" << jsonEscape(intent.category) << "\",";
            json << "\"concept\":\"" << jsonEscape(intent.concept) << "\",";
            json << "\"productFamily\":\"" << jsonEscape(intent.productFamily) << "\",";
            json << "\"address\":\"" << jsonEscape(intent.address) << "\",";
            json << "\"originSource\":\"none\",";
            json << "\"count\":0,\"results\":[]}";
            return httpResponse(200, kJsonContentType, json.str());
        }

        const int limit = getIntOrDefault(params, "limit", 10, 1, 50);
        const int32_t centerLatCell = floorDiv(origin.latE7, houseIndexCellSizeE7_);
        const int32_t centerLonCell = floorDiv(origin.lonE7, houseIndexCellSizeE7_);
        std::unordered_set<uint32_t> seen;
        std::vector<NaturalPoiCandidate> nearest;
        const int maxPoiSearchRadius = intent.type == NaturalIntentType::NearestConceptToAddress ? 192 : 64;
        for (int radius = 0; radius <= maxPoiSearchRadius && nearest.size() < static_cast<size_t>(limit) * 8; ++radius) {
            for (int32_t latCell = centerLatCell - radius; latCell <= centerLatCell + radius; ++latCell) {
                for (int32_t lonCell = centerLonCell - radius; lonCell <= centerLonCell + radius; ++lonCell) {
                    if (radius > 0 && latCell != centerLatCell - radius && latCell != centerLatCell + radius &&
                        lonCell != centerLonCell - radius && lonCell != centerLonCell + radius) {
                        continue;
                    }
                    const auto bucket = poiIndex_.find(gridKey(latCell, lonCell));
                    if (bucket == poiIndex_.end()) {
                        continue;
                    }
                    for (uint32_t poiIndex : bucket->second) {
                        if (poiIndex >= data_.pois.size() || !seen.insert(poiIndex).second) {
                            continue;
                        }
                        const PoiRecord& poi = data_.pois[poiIndex];
                        int matchWeight = 100;
                        if (intent.type == NaturalIntentType::NearestConceptToAddress) {
                            matchWeight = poiConceptMatchWeight(data_, poi, intent.concept, intent.productFamily);
                        } else if (!poiMatchesCategory(data_, poi, intent.category)) {
                            matchWeight = 0;
                        }
                        if (matchWeight <= 0) {
                            continue;
                        }
                        nearest.push_back({distanceMeters(origin, poi.point), poiIndex, matchWeight});
                    }
                }
            }
        }
        std::sort(nearest.begin(), nearest.end(), [](const NaturalPoiCandidate& left, const NaturalPoiCandidate& right) {
            const double leftScore = left.distanceMeters / (1.0 + static_cast<double>(left.weight) / 100.0);
            const double rightScore = right.distanceMeters / (1.0 + static_cast<double>(right.weight) / 100.0);
            if (leftScore != rightScore) {
                return leftScore < rightScore;
            }
            if (left.weight != right.weight) {
                return left.weight > right.weight;
            }
            return left.index < right.index;
        });
        if (nearest.size() > static_cast<size_t>(limit)) {
            nearest.resize(static_cast<size_t>(limit));
        }

        const auto finished = std::chrono::steady_clock::now();
        const double elapsedMs = std::chrono::duration<double, std::milli>(finished - started).count();
        const bool conceptIntent = intent.type == NaturalIntentType::NearestConceptToAddress;
        const ConceptRule* conceptRule = conceptIntent ? findConceptRule(intent.concept) : nullptr;
        std::ostringstream json;
        json << "{\"query\":\"" << jsonEscape(queryIt->second) << "\",";
        json << "\"naturalIntent\":\"" << (conceptIntent ? "nearest_concept_to_address" : "nearest_category_to_address") << "\",";
        json << "\"usedLlm\":" << (intent.fromLlm ? "true" : "false") << ",";
        json << "\"llmVerified\":" << (intent.verifiedByLlm ? "true" : "false") << ",";
        json << "\"category\":\"" << jsonEscape(intent.category) << "\",";
        json << "\"concept\":\"" << jsonEscape(intent.concept) << "\",";
        json << "\"conceptLabel\":\"" << jsonEscape(conceptRule == nullptr ? "" : conceptRule->label) << "\",";
        json << "\"productFamily\":\"" << jsonEscape(intent.productFamily) << "\",";
        json << "\"address\":\"" << jsonEscape(intent.address) << "\",";
        json << "\"originSource\":\"" << originSource << "\",";
        json << "\"origin\":{\"lat\":" << std::setprecision(10) << latitudeOf(origin)
             << ",\"lon\":" << longitudeOf(origin) << "},";
        json << "\"queryTimeMs\":" << std::fixed << std::setprecision(3) << elapsedMs << ",";
        json << "\"count\":" << nearest.size() << ",\"results\":[";
        for (size_t i = 0; i < nearest.size(); ++i) {
            if (i > 0) {
                json << ",";
            }
            json << "{\"type\":\"poi\",\"label\":\""
                 << jsonEscape(poiDisplayLabel(data_, data_.pois[nearest[i].index])) << "\",";
            json << "\"matchWeight\":" << nearest[i].weight << ",";
            json << "\"lat\":" << std::setprecision(10) << latitudeOf(data_.pois[nearest[i].index].point) << ",";
            json << "\"lon\":" << longitudeOf(data_.pois[nearest[i].index].point) << ",";
            json << "\"poi\":";
            writePoiJson(json, data_, nearest[i].index, nearest[i].distanceMeters);
            json << "}";
        }
        json << "]}";
        return httpResponse(200, kJsonContentType, json.str());
    }

    if (path == "/api/reverse") {
        const auto reverseStarted = std::chrono::steady_clock::now();
        double lat = 0.0;
        double lon = 0.0;
        if (!tryGetDouble(params, "lat", lat) || !tryGetDouble(params, "lon", lon)) {
            return httpResponse(400, kJsonContentType, "{\"error\":\"missing lat or lon parameter\"}");
        }

        const Coordinate query = makeCoordinate(lat, lon);
        NearestHouseResult houseResult;
        if (houseIndexHasBounds_) {
            const int32_t centerLatCell = floorDiv(query.latE7, houseIndexCellSizeE7_);
            const int32_t centerLonCell = floorDiv(query.lonE7, houseIndexCellSizeE7_);
            const int maxSearchRadius = std::max({
                std::abs(centerLatCell - minHouseLatCell_),
                std::abs(centerLatCell - maxHouseLatCell_),
                std::abs(centerLonCell - minHouseLonCell_),
                std::abs(centerLonCell - maxHouseLonCell_)
            });
            const double cellDegrees = static_cast<double>(houseIndexCellSizeE7_) / kCoordinateScale;
            const double cellMeters = cellDegrees * 111320.0 * std::max(0.01, std::abs(std::cos(lat * kPi / 180.0)));
            for (int radius = 0; radius <= maxSearchRadius; ++radius) {
                for (int32_t latCell = centerLatCell - radius; latCell <= centerLatCell + radius; ++latCell) {
                    for (int32_t lonCell = centerLonCell - radius; lonCell <= centerLonCell + radius; ++lonCell) {
                        if (radius > 0 && latCell != centerLatCell - radius && latCell != centerLatCell + radius &&
                            lonCell != centerLonCell - radius && lonCell != centerLonCell + radius) {
                            continue;
                        }
                        const auto bucket = houseIndex_.find(gridKey(latCell, lonCell));
                        if (bucket == houseIndex_.end()) {
                            continue;
                        }
                        for (uint32_t houseIndex : bucket->second) {
                            if (houseIndex >= data_.houses.size()) {
                                continue;
                            }
                            const double currentDistance = distanceMeters(query, data_.houses[houseIndex].point);
                            if (currentDistance < houseResult.distanceMeters) {
                                houseResult = {true, houseIndex, currentDistance};
                            }
                        }
                    }
                }
                if (houseResult.found && radius >= 2 && static_cast<double>(radius - 1) * cellMeters > houseResult.distanceMeters) {
                    break;
                }
            }
        }

        NearestStreetResult streetResult;
        const int32_t streetLatCell = floorDiv(query.latE7, streetIndexCellSizeE7_);
        const int32_t streetLonCell = floorDiv(query.lonE7, streetIndexCellSizeE7_);
        std::unordered_set<uint32_t> seenStreets;
        for (int radius = 0; radius <= 8; ++radius) {
            for (int32_t latCell = streetLatCell - radius; latCell <= streetLatCell + radius; ++latCell) {
                for (int32_t lonCell = streetLonCell - radius; lonCell <= streetLonCell + radius; ++lonCell) {
                    if (radius > 0 && latCell != streetLatCell - radius && latCell != streetLatCell + radius &&
                        lonCell != streetLonCell - radius && lonCell != streetLonCell + radius) {
                        continue;
                    }
                    const auto bucket = streetIndex_.find(gridKey(latCell, lonCell));
                    if (bucket == streetIndex_.end()) {
                        continue;
                    }
                    for (uint32_t streetIndex : bucket->second) {
                        if (streetIndex >= data_.streets.size() || !seenStreets.insert(streetIndex).second) {
                            continue;
                        }
                        const double currentDistance = distanceToStreetMeters(query, data_, data_.streets[streetIndex]);
                        if (currentDistance < streetResult.distanceMeters) {
                            streetResult = {true, streetIndex, currentDistance};
                        }
                    }
                }
            }
            const double lowerBoundMeters = static_cast<double>(radius) * 0.05 * 111320.0 *
                std::max(0.01, std::abs(std::cos(lat * kPi / 180.0)));
            if (streetResult.found && radius >= 2 && lowerBoundMeters > streetResult.distanceMeters) {
                break;
            }
        }

        std::vector<uint32_t> adminMatches;
        const int32_t adminLatCell = floorDiv(query.latE7, adminIndexCellSizeE7_);
        const int32_t adminLonCell = floorDiv(query.lonE7, adminIndexCellSizeE7_);
        const auto adminBucket = adminIndex_.find(gridKey(adminLatCell, adminLonCell));
        if (adminBucket != adminIndex_.end()) {
            for (uint32_t areaIndex : adminBucket->second) {
                if (areaIndex >= data_.adminAreas.size()) {
                    continue;
                }
                const AdminAreaRecord& area = data_.adminAreas[areaIndex];
                if (area.bbox.contains(query) && pointInRing(query, data_.adminGeometry, area.geometryOffset, area.geometrySize)) {
                    adminMatches.push_back(areaIndex);
                }
            }
        }
        std::sort(adminMatches.begin(), adminMatches.end(), [this](uint32_t left, uint32_t right) {
            const AdminAreaRecord& leftArea = data_.adminAreas[left];
            const AdminAreaRecord& rightArea = data_.adminAreas[right];
            if (leftArea.adminLevel != rightArea.adminLevel) {
                return leftArea.adminLevel < rightArea.adminLevel;
            }
            return leftArea.osmId < rightArea.osmId;
        });
        adminMatches.erase(std::unique(adminMatches.begin(), adminMatches.end()), adminMatches.end());

        std::vector<uint32_t> containingAdminChain;
        NearestAdminResult adminResult;
        if (!adminMatches.empty()) {
            containingAdminChain = coherentAdminChainForRawMatches(data_, adminMatches);
            if (!containingAdminChain.empty()) {
                adminResult = {true, containingAdminChain.back(), 0.0, true};
            }
        }

        if (!adminResult.found) {
            std::unordered_set<uint32_t> seenAdmins;
            const double adminCellMeters = 0.25 * 111320.0 *
                std::max(0.01, std::abs(std::cos(lat * kPi / 180.0)));
            for (int radius = 0; radius <= 24; ++radius) {
                for (int32_t latCell = adminLatCell - radius; latCell <= adminLatCell + radius; ++latCell) {
                    for (int32_t lonCell = adminLonCell - radius; lonCell <= adminLonCell + radius; ++lonCell) {
                        if (radius > 0 && latCell != adminLatCell - radius && latCell != adminLatCell + radius &&
                            lonCell != adminLonCell - radius && lonCell != adminLonCell + radius) {
                            continue;
                        }
                        const auto bucket = adminIndex_.find(gridKey(latCell, lonCell));
                        if (bucket == adminIndex_.end()) {
                            continue;
                        }
                        for (uint32_t areaIndex : bucket->second) {
                            if (areaIndex >= data_.adminAreas.size() || !seenAdmins.insert(areaIndex).second) {
                                continue;
                            }
                            const AdminAreaRecord& area = data_.adminAreas[areaIndex];
                            const bool inside = area.bbox.contains(query) &&
                                pointInRing(query, data_.adminGeometry, area.geometryOffset, area.geometrySize);
                            const double currentDistance = inside ? 0.0 : distanceToAdminAreaMeters(query, data_, area);
                            if (!std::isfinite(currentDistance)) {
                                continue;
                            }
                            if (!adminResult.found ||
                                currentDistance < adminResult.distanceMeters ||
                                (std::abs(currentDistance - adminResult.distanceMeters) < 0.01 &&
                                 area.adminLevel > data_.adminAreas[adminResult.index].adminLevel)) {
                                adminResult = {true, areaIndex, currentDistance, inside};
                            }
                        }
                    }
                }
                if (adminResult.found && radius >= 2 &&
                    static_cast<double>(radius - 1) * adminCellMeters > adminResult.distanceMeters) {
                    break;
                }
            }
        }

        constexpr double kCloseHouseMeters = 100.0;
        constexpr double kCloseStreetMeters = 50.0;
        constexpr double kDirectHouseMeters = 3.0;
        constexpr double kDirectStreetMeters = 15.0;
        constexpr double kFallbackHouseMeters = 300.0;
        constexpr double kLooseObjectMeters = 1000.0;

        std::string resultType = "none";
        std::string label;
        double resultDistance = -1.0;
        if (houseResult.found && houseResult.distanceMeters <= kDirectHouseMeters) {
            resultType = "house";
            label = houseAddressLine(data_, data_.houses[houseResult.index]);
            resultDistance = houseResult.distanceMeters;
        } else if (streetResult.found && streetResult.distanceMeters <= kDirectStreetMeters) {
            resultType = "street";
            label = streetDisplayLabel(data_, data_.streets[streetResult.index]);
            resultDistance = streetResult.distanceMeters;
        } else if (streetResult.found && streetResult.distanceMeters <= kCloseStreetMeters &&
                   (!houseResult.found || streetResult.distanceMeters <= houseResult.distanceMeters)) {
            resultType = "street";
            label = streetDisplayLabel(data_, data_.streets[streetResult.index]);
            resultDistance = streetResult.distanceMeters;
        } else if (houseResult.found && houseResult.distanceMeters <= kCloseHouseMeters) {
            resultType = "house";
            label = houseAddressLine(data_, data_.houses[houseResult.index]);
            resultDistance = houseResult.distanceMeters;
        } else if (streetResult.found && streetResult.distanceMeters <= kCloseStreetMeters) {
            resultType = "street";
            label = streetDisplayLabel(data_, data_.streets[streetResult.index]);
            resultDistance = streetResult.distanceMeters;
        } else if (houseResult.found && houseResult.distanceMeters <= kFallbackHouseMeters) {
            resultType = "house";
            label = houseAddressLine(data_, data_.houses[houseResult.index]);
            resultDistance = houseResult.distanceMeters;
        } else if (adminResult.found) {
            resultType = "admin_area";
            label = data_.resolve(data_.adminAreas[adminResult.index].name);
            resultDistance = adminResult.distanceMeters;
        } else if (streetResult.found && streetResult.distanceMeters <= kLooseObjectMeters) {
            resultType = "street";
            label = streetDisplayLabel(data_, data_.streets[streetResult.index]);
            resultDistance = streetResult.distanceMeters;
        } else if (houseResult.found && houseResult.distanceMeters <= kLooseObjectMeters) {
            resultType = "house";
            label = houseAddressLine(data_, data_.houses[houseResult.index]);
            resultDistance = houseResult.distanceMeters;
        }

        if (resultType == "house" && houseResult.found) {
            const HouseRecord& house = data_.houses[houseResult.index];
            adminMatches = sortedUniqueAdminRefs(data_, adminRefsFromLinks(data_.houseAdminAreaIndexes,
                                                                           house.adminAreaOffset,
                                                                           house.adminAreaSize,
                                                                           data_.adminAreas.size()));
        } else if (resultType == "street" && streetResult.found) {
            const StreetRecord& street = data_.streets[streetResult.index];
            std::vector<uint32_t> streetAreas = sortedUniqueAdminRefs(data_,
                adminRefsFromLinks(data_.streetAdminAreaIndexes,
                                   street.adminAreaOffset,
                                   street.adminAreaSize,
                                   data_.adminAreas.size()));
            if (houseResult.found && houseResult.distanceMeters <= 1000.0) {
                const HouseRecord& house = data_.houses[houseResult.index];
                std::vector<uint32_t> houseAreas = sortedUniqueAdminRefs(data_,
                    adminRefsFromLinks(data_.houseAdminAreaIndexes,
                                       house.adminAreaOffset,
                                       house.adminAreaSize,
                                       data_.adminAreas.size()));
                std::vector<bool> hasLevel(13, false);
                for (uint32_t areaIndex : houseAreas) {
                    if (areaIndex < data_.adminAreas.size() && data_.adminAreas[areaIndex].adminLevel < hasLevel.size()) {
                        hasLevel[data_.adminAreas[areaIndex].adminLevel] = true;
                    }
                }
                for (uint32_t areaIndex : streetAreas) {
                    if (areaIndex < data_.adminAreas.size()) {
                        const uint8_t level = data_.adminAreas[areaIndex].adminLevel;
                        if (level >= hasLevel.size() || !hasLevel[level]) {
                            houseAreas.push_back(areaIndex);
                        }
                    }
                }
                adminMatches = sortedUniqueAdminRefs(data_, std::move(houseAreas));
            } else if (!containingAdminChain.empty()) {
                adminMatches = containingAdminChain;
            } else if (!streetAreas.empty()) {
                adminMatches = coherentAdminChainForRawMatches(data_, streetAreas);
            } else if (adminResult.found) {
                adminMatches = adminChainForArea(data_, adminResult.index);
            } else {
                adminMatches.clear();
            }
        } else if (resultType == "admin_area" && adminResult.found) {
            adminMatches = !containingAdminChain.empty()
                ? containingAdminChain
                : adminChainForArea(data_, adminResult.index);
        } else {
            adminMatches.clear();
        }

        const auto reverseFinished = std::chrono::steady_clock::now();
        const double reverseElapsedMs =
            std::chrono::duration<double, std::milli>(reverseFinished - reverseStarted).count();

        std::ostringstream json;
        json << "{";
        json << "\"found\":" << (resultType == "none" ? "false" : "true") << ",";
        json << "\"queryTimeMs\":" << std::fixed << std::setprecision(3) << reverseElapsedMs << ",";
        json << "\"query\":{\"lat\":" << std::setprecision(10) << lat << ",\"lon\":" << lon << "},";
        json << "\"result\":{\"type\":\"" << resultType << "\",\"label\":\"" << jsonEscape(label) << "\"";
        if (resultDistance >= 0.0) {
            json << ",\"distanceMeters\":" << std::fixed << std::setprecision(2) << resultDistance;
        }
        json << "},";
        json << "\"distanceMeters\":" << (resultDistance >= 0.0 ? resultDistance : -1.0) << ",";
        json << "\"house\":";
        if (houseResult.found) {
            writeHouseJson(json, data_, houseResult.index, houseResult.distanceMeters);
        } else {
            json << "null";
        }
        json << ",\"street\":";
        if (streetResult.found) {
            writeStreetJson(json, data_, streetResult.index, resultType == "street", streetResult.distanceMeters);
        } else {
            json << "null";
        }
        json << ",\"adminArea\":";
        if (!adminMatches.empty()) {
            writeAdminAreaJson(json, data_, adminMatches.back(), false);
        } else {
            json << "null";
        }
        json << ",\"adminAreas\":[";
        for (size_t i = 0; i < adminMatches.size(); ++i) {
            if (i > 0) {
                json << ",";
            }
            writeAdminAreaJson(json, data_, adminMatches[i], false);
        }
        json << "]}";
        return httpResponse(200, kJsonContentType, json.str());
    }

    if (path == "/api/admin-area") {
        uint32_t index = 0;
        if (!tryGetUint32(params, "index", index)) {
            uint64_t id = 0;
            if (!tryGetUint64(params, "id", id)) {
                return httpResponse(400, kJsonContentType, "{\"error\":\"missing id or index parameter\"}");
            }
            bool found = false;
            for (size_t i = 0; i < data_.adminAreas.size(); ++i) {
                if (data_.adminAreas[i].osmId == id) {
                    index = static_cast<uint32_t>(i);
                    found = true;
                    break;
                }
            }
            if (!found) {
                return httpResponse(404, kJsonContentType, "{\"error\":\"admin area not found\"}");
            }
        }
        if (index >= data_.adminAreas.size()) {
            return httpResponse(404, kJsonContentType, "{\"error\":\"admin area not found\"}");
        }
        const auto detail = params.find("detail");
        const bool fullDetail = detail != params.end() && detail->second == "full";
        const int maxGeometryPoints = fullDetail
            ? 0
            : getIntOrDefault(params, "maxPoints", 2000, 20, 200000);
        std::ostringstream json;
        writeAdminAreaJson(json, data_, index, true, static_cast<uint32_t>(maxGeometryPoints));
        return httpResponse(200, kJsonContentType, json.str());
    }

    if (path == "/api/houses") {
        double minLat = 0.0, maxLat = 0.0, minLon = 0.0, maxLon = 0.0;
        if (!tryGetDouble(params, "minLat", minLat) || !tryGetDouble(params, "maxLat", maxLat) ||
            !tryGetDouble(params, "minLon", minLon) || !tryGetDouble(params, "maxLon", maxLon)) {
            return httpResponse(400, kJsonContentType, "{\"error\":\"missing viewport parameters\"}");
        }
        const BoundingBox viewport = makeBoundingBox(minLat, maxLat, minLon, maxLon);
        const int limit = getIntOrDefault(params, "limit", defaultHouseLimit_, 1, 100000);

        int32_t minLatCell = 0;
        int32_t maxLatCell = 0;
        int32_t minLonCell = 0;
        int32_t maxLonCell = 0;
        cellRangeForBox(viewport, houseIndexCellSizeE7_, minLatCell, maxLatCell, minLonCell, maxLonCell);
        const uint64_t latCellCount = static_cast<uint64_t>(std::max<int32_t>(0, maxLatCell - minLatCell + 1));
        const uint64_t lonCellCount = static_cast<uint64_t>(std::max<int32_t>(0, maxLonCell - minLonCell + 1));
        const uint64_t cellCount = latCellCount * lonCellCount;

        std::vector<uint32_t> selectedHouses;
        bool truncated = false;
        if (cellCount <= static_cast<uint64_t>(limit) * 8) {
            const std::vector<uint32_t> candidates = queryBoxIndex(viewport, houseIndexCellSizeE7_, houseIndex_);
            std::vector<uint32_t> visibleHouses;
            visibleHouses.reserve(std::min<size_t>(candidates.size(), static_cast<size_t>(limit) * 4));
            for (uint32_t houseIndex : candidates) {
                if (houseIndex >= data_.houses.size()) {
                    continue;
                }
                if (viewport.contains(data_.houses[houseIndex].point)) {
                    visibleHouses.push_back(houseIndex);
                }
            }

            truncated = visibleHouses.size() > static_cast<size_t>(limit);
            if (!truncated) {
                selectedHouses = std::move(visibleHouses);
            } else {
                selectedHouses.reserve(static_cast<size_t>(limit));
                const double step = static_cast<double>(visibleHouses.size()) / static_cast<double>(limit);
                for (int i = 0; i < limit; ++i) {
                    const size_t sourceIndex = std::min(
                        visibleHouses.size() - 1,
                        static_cast<size_t>(std::floor((static_cast<double>(i) + 0.5) * step)));
                    selectedHouses.push_back(visibleHouses[sourceIndex]);
                }
            }
        } else {
            selectedHouses.reserve(static_cast<size_t>(limit));

            uint64_t nonEmptyCells = 0;
            for (int32_t latCell = minLatCell; latCell <= maxLatCell; ++latCell) {
                for (int32_t lonCell = minLonCell; lonCell <= maxLonCell; ++lonCell) {
                    const auto bucket = houseIndex_.find(gridKey(latCell, lonCell));
                    if (bucket != houseIndex_.end() && !bucket->second.empty()) {
                        ++nonEmptyCells;
                    }
                }
            }

            if (nonEmptyCells > 0) {
                truncated = nonEmptyCells > static_cast<uint64_t>(limit);
                const double step = static_cast<double>(nonEmptyCells) /
                    static_cast<double>(std::min<uint64_t>(nonEmptyCells, static_cast<uint64_t>(limit)));
                uint64_t nonEmptyOrdinal = 0;
                size_t wanted = 0;
                uint64_t nextWanted = static_cast<uint64_t>(std::floor((static_cast<double>(wanted) + 0.5) * step));

                for (int32_t latCell = minLatCell; latCell <= maxLatCell && selectedHouses.size() < static_cast<size_t>(limit); ++latCell) {
                    for (int32_t lonCell = minLonCell; lonCell <= maxLonCell && selectedHouses.size() < static_cast<size_t>(limit); ++lonCell) {
                        const auto bucket = houseIndex_.find(gridKey(latCell, lonCell));
                        if (bucket == houseIndex_.end() || bucket->second.empty()) {
                            continue;
                        }
                        if (nonEmptyOrdinal++ < nextWanted) {
                            continue;
                        }

                        const std::vector<uint32_t>& houses = bucket->second;
                        const uint64_t hash = (static_cast<uint64_t>(static_cast<uint32_t>(latCell)) * 73856093ULL) ^
                                              (static_cast<uint64_t>(static_cast<uint32_t>(lonCell)) * 19349663ULL);
                        const size_t start = static_cast<size_t>(hash % houses.size());
                        for (size_t offset = 0; offset < houses.size(); ++offset) {
                            const uint32_t houseIndex = houses[(start + offset) % houses.size()];
                            if (houseIndex < data_.houses.size() && viewport.contains(data_.houses[houseIndex].point)) {
                                selectedHouses.push_back(houseIndex);
                                break;
                            }
                        }

                        ++wanted;
                        nextWanted = static_cast<uint64_t>(std::floor((static_cast<double>(wanted) + 0.5) * step));
                    }
                }
            }
        }

        std::ostringstream json;
        json << "{\"houses\":[";
        bool first = true;
        for (uint32_t houseIndex : selectedHouses) {
            const HouseRecord& house = data_.houses[houseIndex];
            if (!first) {
                json << ",";
            }
            first = false;
            json << "{";
            json << "\"id\":" << house.osmId << ",";
            json << "\"lat\":" << std::setprecision(10) << latitudeOf(house.point) << ",";
            json << "\"lon\":" << longitudeOf(house.point) << ",";
            json << "\"street\":\"" << jsonEscape(data_.resolve(house.streetName)) << "\",";
            json << "\"number\":\"" << jsonEscape(data_.resolve(house.houseNumber)) << "\",";
            json << "\"city\":\"" << jsonEscape(data_.resolve(house.city)) << "\",";
            json << "\"postcode\":\"" << jsonEscape(data_.resolve(house.postcode)) << "\"";
            json << "}";
        }
        json << "],\"count\":" << selectedHouses.size() << ",\"truncated\":" << (truncated ? "true" : "false") << "}";
        return httpResponse(200, kJsonContentType, json.str());
    }

    if (path == "/api/streets") {
        double minLat = 0.0, maxLat = 0.0, minLon = 0.0, maxLon = 0.0;
        if (!tryGetDouble(params, "minLat", minLat) || !tryGetDouble(params, "maxLat", maxLat) ||
            !tryGetDouble(params, "minLon", minLon) || !tryGetDouble(params, "maxLon", maxLon)) {
            return httpResponse(400, kJsonContentType, "{\"error\":\"missing viewport parameters\"}");
        }
        const BoundingBox viewport = makeBoundingBox(minLat, maxLat, minLon, maxLon);
        const int limit = getIntOrDefault(params, "limit", 2000, 1, 20000);

        int32_t minLatCell = 0;
        int32_t maxLatCell = 0;
        int32_t minLonCell = 0;
        int32_t maxLonCell = 0;
        cellRangeForBox(viewport, streetIndexCellSizeE7_, minLatCell, maxLatCell, minLonCell, maxLonCell);
        const uint64_t latCellCount = static_cast<uint64_t>(std::max<int32_t>(0, maxLatCell - minLatCell + 1));
        const uint64_t lonCellCount = static_cast<uint64_t>(std::max<int32_t>(0, maxLonCell - minLonCell + 1));
        const uint64_t cellCount = latCellCount * lonCellCount;

        std::vector<uint32_t> selectedStreets;
        bool truncated = false;
        if (cellCount <= static_cast<uint64_t>(limit) * 8) {
            const std::vector<uint32_t> candidates = queryBoxIndex(viewport, streetIndexCellSizeE7_, streetIndex_);
            std::vector<uint32_t> visibleStreets;
            visibleStreets.reserve(std::min<size_t>(candidates.size(), static_cast<size_t>(limit) * 4));
            for (uint32_t streetIndex : candidates) {
                if (streetIndex < data_.streets.size() &&
                    data_.streets[streetIndex].bbox.intersects(viewport) &&
                    streetHasPointInViewport(data_, data_.streets[streetIndex], viewport)) {
                    visibleStreets.push_back(streetIndex);
                }
            }

            truncated = visibleStreets.size() > static_cast<size_t>(limit);
            if (!truncated) {
                selectedStreets = std::move(visibleStreets);
            } else {
                selectedStreets.reserve(static_cast<size_t>(limit));
                const double step = static_cast<double>(visibleStreets.size()) / static_cast<double>(limit);
                for (int i = 0; i < limit; ++i) {
                    const size_t sourceIndex = std::min(
                        visibleStreets.size() - 1,
                        static_cast<size_t>(std::floor((static_cast<double>(i) + 0.5) * step)));
                    selectedStreets.push_back(visibleStreets[sourceIndex]);
                }
            }
        } else {
            selectedStreets.reserve(static_cast<size_t>(limit));
            std::unordered_set<uint32_t> seenStreets;

            uint64_t nonEmptyCells = 0;
            for (int32_t latCell = minLatCell; latCell <= maxLatCell; ++latCell) {
                for (int32_t lonCell = minLonCell; lonCell <= maxLonCell; ++lonCell) {
                    const auto bucket = streetIndex_.find(gridKey(latCell, lonCell));
                    if (bucket != streetIndex_.end() && !bucket->second.empty()) {
                        ++nonEmptyCells;
                    }
                }
            }

            if (nonEmptyCells > 0) {
                truncated = nonEmptyCells > static_cast<uint64_t>(limit);
                const double step = static_cast<double>(nonEmptyCells) /
                    static_cast<double>(std::min<uint64_t>(nonEmptyCells, static_cast<uint64_t>(limit)));
                uint64_t nonEmptyOrdinal = 0;
                size_t wanted = 0;
                uint64_t nextWanted = static_cast<uint64_t>(std::floor((static_cast<double>(wanted) + 0.5) * step));

                for (int32_t latCell = minLatCell; latCell <= maxLatCell && selectedStreets.size() < static_cast<size_t>(limit); ++latCell) {
                    for (int32_t lonCell = minLonCell; lonCell <= maxLonCell && selectedStreets.size() < static_cast<size_t>(limit); ++lonCell) {
                        const auto bucket = streetIndex_.find(gridKey(latCell, lonCell));
                        if (bucket == streetIndex_.end() || bucket->second.empty()) {
                            continue;
                        }
                        if (nonEmptyOrdinal++ < nextWanted) {
                            continue;
                        }

                        const std::vector<uint32_t>& streets = bucket->second;
                        const uint64_t hash = (static_cast<uint64_t>(static_cast<uint32_t>(latCell)) * 73856093ULL) ^
                                              (static_cast<uint64_t>(static_cast<uint32_t>(lonCell)) * 19349663ULL);
                        const size_t start = static_cast<size_t>(hash % streets.size());
                        for (size_t offset = 0; offset < streets.size(); ++offset) {
                            const uint32_t streetIndex = streets[(start + offset) % streets.size()];
                            if (streetIndex < data_.streets.size() &&
                                seenStreets.insert(streetIndex).second &&
                                data_.streets[streetIndex].bbox.intersects(viewport) &&
                                streetHasPointInViewport(data_, data_.streets[streetIndex], viewport)) {
                                selectedStreets.push_back(streetIndex);
                                break;
                            }
                        }

                        ++wanted;
                        nextWanted = static_cast<uint64_t>(std::floor((static_cast<double>(wanted) + 0.5) * step));
                    }
                }
            }
        }

        std::ostringstream json;
        json << "{\"streets\":[";
        bool first = true;
        for (uint32_t streetIndex : selectedStreets) {
            if (!first) {
                json << ",";
            }
            first = false;
            writeStreetViewportJson(json, data_, streetIndex, viewport);
        }
        json << "],\"count\":" << selectedStreets.size() << ",\"truncated\":" << (truncated ? "true" : "false") << "}";
        return httpResponse(200, kJsonContentType, json.str());
    }

    if (path == "/api/admin") {
        double minLat = 0.0, maxLat = 0.0, minLon = 0.0, maxLon = 0.0;
        if (!tryGetDouble(params, "minLat", minLat) || !tryGetDouble(params, "maxLat", maxLat) ||
            !tryGetDouble(params, "minLon", minLon) || !tryGetDouble(params, "maxLon", maxLon)) {
            return httpResponse(400, kJsonContentType, "{\"error\":\"missing viewport parameters\"}");
        }
        const int limit = getIntOrDefault(params, "limit", 300, 1, 5000);
        const int minLevel = getIntOrDefault(params, "minLevel", 2, 2, 12);
        const int maxLevel = getIntOrDefault(params, "maxLevel", 10, 2, 12);
        const auto detail = params.find("detail");
        const bool fullDetail = detail != params.end() && detail->second == "full";
        const int maxGeometryPoints = fullDetail
            ? 0
            : getIntOrDefault(params, "maxPoints", 500, 20, 200000);
        const BoundingBox viewport = makeBoundingBox(minLat, maxLat, minLon, maxLon);
        const std::vector<uint32_t> candidates = queryBoxIndex(viewport, adminIndexCellSizeE7_, adminIndex_);
        std::vector<uint32_t> visibleAreas;
        visibleAreas.reserve(std::min<size_t>(candidates.size(), static_cast<size_t>(limit) * 4));
        for (uint32_t areaIndex : candidates) {
            if (areaIndex >= data_.adminAreas.size()) {
                continue;
            }
            const AdminAreaRecord& area = data_.adminAreas[areaIndex];
            if (area.adminLevel >= minLevel && area.adminLevel <= maxLevel && area.bbox.intersects(viewport)) {
                visibleAreas.push_back(areaIndex);
            }
        }

        const bool truncated = visibleAreas.size() > static_cast<size_t>(limit);
        std::vector<uint32_t> selectedAreas;
        if (!truncated) {
            selectedAreas = std::move(visibleAreas);
        } else {
            selectedAreas.reserve(static_cast<size_t>(limit));
            const double step = static_cast<double>(visibleAreas.size()) / static_cast<double>(limit);
            for (int i = 0; i < limit; ++i) {
                const size_t sourceIndex = std::min(
                    visibleAreas.size() - 1,
                    static_cast<size_t>(std::floor((static_cast<double>(i) + 0.5) * step)));
                selectedAreas.push_back(visibleAreas[sourceIndex]);
            }
        }

        std::ostringstream json;
        json << "{\"areas\":[";
        bool first = true;
        for (uint32_t areaIndex : selectedAreas) {
            if (!first) {
                json << ",";
            }
            first = false;
            writeAdminAreaJson(json, data_, areaIndex, true, static_cast<uint32_t>(maxGeometryPoints));
        }
        json << "],\"count\":" << selectedAreas.size() << ",\"truncated\":" << (truncated ? "true" : "false") << "}";
        return httpResponse(200, kJsonContentType, json.str());
    }

    return httpResponse(404, kJsonContentType, "{\"error\":\"endpoint not found\"}");
}

Server::ServerResponse Server::httpResponse(int statusCode,
                                            const std::string& contentType,
                                            const std::string& body) {
    return {statusCode, contentType, body};
}
