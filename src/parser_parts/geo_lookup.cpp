Coordinate quantizeCoordinate(double lat, double lon) {
    return Coordinate{
        static_cast<int32_t>(std::llround(lat * static_cast<double>(kCoordinateScale))),
        static_cast<int32_t>(std::llround(lon * static_cast<double>(kCoordinateScale)))
    };
}

Coordinate quantizeCoordinate(const osmium::Location& location) {
    return quantizeCoordinate(location.lat(), location.lon());
}

bool coordinatesEqual(const Coordinate& left, const Coordinate& right) {
    return left.latE7 == right.latE7 && left.lonE7 == right.lonE7;
}

BoundingBox computeBoundingBox(const std::vector<Coordinate>& geometry) {
    BoundingBox bbox;
    for (const Coordinate& point : geometry) {
        bbox.expand(point);
    }
    return bbox;
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

bool pointOnProjectedSegment(double pointX,
                             double pointY,
                             double startX,
                             double startY,
                             double endX,
                             double endY) {
    const double dx = endX - startX;
    const double dy = endY - startY;
    const double cross = (pointX - startX) * dy - (pointY - startY) * dx;
    if (std::abs(cross) > 1.0) {
        return false;
    }

    const double dot = (pointX - startX) * (endX - startX) +
                       (pointY - startY) * (endY - startY);
    if (dot < 0.0) {
        return false;
    }

    const double lengthSquared = dx * dx + dy * dy;
    return dot <= lengthSquared;
}

bool pointInPolygon(const Coordinate& point,
                    const std::vector<Coordinate>& geometry,
                    uint32_t offset,
                    uint32_t size,
                    const std::vector<uint32_t>& edgeIndexes) {
    if (size < 4 || edgeIndexes.empty() || offset >= geometry.size() ||
        static_cast<size_t>(size) > geometry.size() - offset) {
        return false;
    }

    const double queryLatRadians = latitudeOf(point) * 3.14159265358979323846 / 180.0;
    const double lonScale = std::cos(queryLatRadians);
    const double pointX = static_cast<double>(point.lonE7) * lonScale;
    const double pointY = static_cast<double>(point.latE7);

    bool inside = false;
    for (uint32_t current : edgeIndexes) {
        if (current >= size) {
            continue;
        }
        const uint32_t previous = current == 0 ? size - 1 : current - 1;
        const Coordinate& a = geometry[offset + current];
        const Coordinate& b = geometry[offset + previous];

        const double ax = static_cast<double>(a.lonE7) * lonScale;
        const double ay = static_cast<double>(a.latE7);
        const double bx = static_cast<double>(b.lonE7) * lonScale;
        const double by = static_cast<double>(b.latE7);

        if (pointOnProjectedSegment(pointX, pointY, ax, ay, bx, by)) {
            return true;
        }

        const bool crossesLatitude = (ay > pointY) != (by > pointY);
        if (crossesLatitude) {
            const double edgeX = ax + (pointY - ay) * (bx - ax) / (by - ay);
            if (edgeX >= pointX) {
                inside = !inside;
            }
        }
    }

    return inside;
}

class AdminAreaEdgeIndex {
public:
    explicit AdminAreaEdgeIndex(int32_t cellSizeE7 = kCoordinateScale / 20)
        : cellSizeE7_(cellSizeE7) {}

    void build(const OSMDataset& data) {
        for (uint32_t ringIndex = 0; ringIndex < data.adminRings.size(); ++ringIndex) {
            const AdminRingRecord& ring = data.adminRings[ringIndex];
            if (ring.geometrySize < 4 || ring.geometryOffset >= data.adminGeometry.size() ||
                static_cast<size_t>(ring.geometrySize) > data.adminGeometry.size() - ring.geometryOffset) {
                continue;
            }

            for (uint32_t current = 0; current < ring.geometrySize; ++current) {
                const uint32_t previous = current == 0 ? ring.geometrySize - 1 : current - 1;
                const Coordinate& a = data.adminGeometry[ring.geometryOffset + current];
                const Coordinate& b = data.adminGeometry[ring.geometryOffset + previous];

                const int32_t minLat = std::min(a.latE7, b.latE7);
                const int32_t maxLat = std::max(a.latE7, b.latE7);
                const int32_t minCell = floorDiv(minLat, cellSizeE7_);
                const int32_t maxCell = floorDiv(maxLat, cellSizeE7_);

                for (int32_t latCell = minCell; latCell <= maxCell; ++latCell) {
                    cells_[ringLatKey(ringIndex, latCell)].push_back(current);
                }
            }
        }
    }

    const std::vector<uint32_t>* edgesForRing(uint32_t ringIndex, const Coordinate& point) const {
        const int32_t latCell = floorDiv(point.latE7, cellSizeE7_);
        const auto found = cells_.find(ringLatKey(ringIndex, latCell));
        if (found == cells_.end()) {
            return nullptr;
        }
        return &found->second;
    }

private:
    static uint64_t ringLatKey(uint32_t ringIndex, int32_t latCell) {
        return (static_cast<uint64_t>(ringIndex) << 32) | static_cast<uint32_t>(latCell);
    }

    int32_t cellSizeE7_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells_;
};

class AdminAreaGridIndex {
public:
    explicit AdminAreaGridIndex(int32_t cellSizeE7 = kCoordinateScale / 4)
        : cellSizeE7_(cellSizeE7) {}

    void build(const OSMDataset& data) {
        for (uint32_t index = 0; index < data.adminAreas.size(); ++index) {
            const AdminAreaRecord& area = data.adminAreas[index];
            if (!area.bbox.valid || area.geometrySize < 4) {
                continue;
            }

            const int32_t minLatCell = floorDiv(area.bbox.minLatE7, cellSizeE7_);
            const int32_t maxLatCell = floorDiv(area.bbox.maxLatE7, cellSizeE7_);
            const int32_t minLonCell = floorDiv(area.bbox.minLonE7, cellSizeE7_);
            const int32_t maxLonCell = floorDiv(area.bbox.maxLonE7, cellSizeE7_);

            for (int32_t latCell = minLatCell; latCell <= maxLatCell; ++latCell) {
                for (int32_t lonCell = minLonCell; lonCell <= maxLonCell; ++lonCell) {
                    cells_[gridKey(latCell, lonCell)].push_back(index);
                }
            }
        }
    }

    const std::vector<uint32_t>* candidates(const Coordinate& point) const {
        const int32_t latCell = floorDiv(point.latE7, cellSizeE7_);
        const int32_t lonCell = floorDiv(point.lonE7, cellSizeE7_);
        const auto found = cells_.find(gridKey(latCell, lonCell));
        if (found == cells_.end()) {
            return nullptr;
        }
        return &found->second;
    }

private:
    int32_t cellSizeE7_;
    std::unordered_map<int64_t, std::vector<uint32_t>> cells_;
};


void sortAndUniqueAdminMatches(std::vector<uint32_t>& matches, const OSMDataset& data) {
    std::sort(matches.begin(), matches.end(), [&data](uint32_t left, uint32_t right) {
        const AdminAreaRecord& leftArea = data.adminAreas[left];
        const AdminAreaRecord& rightArea = data.adminAreas[right];
        if (leftArea.adminLevel != rightArea.adminLevel) {
            return leftArea.adminLevel < rightArea.adminLevel;
        }
        return leftArea.osmId < rightArea.osmId;
    });
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
}

double bboxAreaScore(const BoundingBox& bbox) {
    if (!bbox.valid) {
        return std::numeric_limits<double>::max();
    }
    const double latSpan = static_cast<double>(std::max<int32_t>(1, bbox.maxLatE7 - bbox.minLatE7));
    const double lonSpan = static_cast<double>(std::max<int32_t>(1, bbox.maxLonE7 - bbox.minLonE7));
    return latSpan * lonSpan;
}

void compactAdminMatchesByLevel(std::vector<uint32_t>& matches, const OSMDataset& data) {
    if (matches.empty()) {
        return;
    }

    std::vector<uint32_t> compact;
    compact.reserve(matches.size());
    for (uint32_t areaIndex : matches) {
        if (areaIndex >= data.adminAreas.size()) {
            continue;
        }
        if (compact.empty() ||
            data.adminAreas[compact.back()].adminLevel != data.adminAreas[areaIndex].adminLevel) {
            compact.push_back(areaIndex);
            continue;
        }
        if (bboxAreaScore(data.adminAreas[areaIndex].bbox) < bboxAreaScore(data.adminAreas[compact.back()].bbox)) {
            compact.back() = areaIndex;
        }
    }
    matches.swap(compact);
}

bool adminAreaContainsPoint(uint32_t areaIndex,
                            const Coordinate& point,
                            const OSMDataset& data,
                            const AdminAreaEdgeIndex& edgeIndex);

void findAdminAreasForPoint(const Coordinate& point,
                            const OSMDataset& data,
                            const AdminAreaGridIndex& index,
                            const AdminAreaEdgeIndex& edgeIndex,
                            std::vector<uint32_t>& matches) {
    const std::vector<uint32_t>* candidates = index.candidates(point);
    if (candidates == nullptr) {
        return;
    }

    for (uint32_t areaIndex : *candidates) {
        if (areaIndex >= data.adminAreas.size()) {
            continue;
        }
        const AdminAreaRecord& area = data.adminAreas[areaIndex];
        if (!area.bbox.contains(point)) {
            continue;
        }
        if (!adminAreaContainsPoint(areaIndex, point, data, edgeIndex)) {
            continue;
        }
        matches.push_back(areaIndex);
    }
}

bool adminAreaContainsPoint(uint32_t areaIndex,
                            const Coordinate& point,
                            const OSMDataset& data,
                            const AdminAreaEdgeIndex& edgeIndex) {
    if (areaIndex >= data.adminAreas.size()) {
        return false;
    }
    const AdminAreaRecord& area = data.adminAreas[areaIndex];
    if (!area.bbox.contains(point)) {
        return false;
    }
    bool insideOuter = false;
    const uint32_t available = area.ringOffset < data.adminRings.size()
        ? std::min<uint32_t>(area.ringSize, checkedU32(data.adminRings.size() - area.ringOffset, "admin ring available size"))
        : 0;
    for (uint32_t i = 0; i < available; ++i) {
        const uint32_t ringIndex = area.ringOffset + i;
        const AdminRingRecord& ring = data.adminRings[ringIndex];
        if (ring.adminAreaIndex != areaIndex) {
            continue;
        }
        const std::vector<uint32_t>* edges = edgeIndex.edgesForRing(ringIndex, point);
        if (edges == nullptr) {
            continue;
        }
        const bool insideRing = pointInPolygon(point,
                                               data.adminGeometry,
                                               ring.geometryOffset,
                                               ring.geometrySize,
                                               *edges);
        if (!insideRing) {
            continue;
        }
        if (ring.role == 1) {
            return false;
        }
        insideOuter = true;
    }
    if (available == 0) {
        std::vector<uint32_t> allEdges;
        allEdges.reserve(area.geometrySize);
        for (uint32_t i = 0; i < area.geometrySize; ++i) {
            allEdges.push_back(i);
        }
        return pointInPolygon(point, data.adminGeometry, area.geometryOffset, area.geometrySize, allEdges);
    }
    return insideOuter;
}

Coordinate adminAreaCentroid(const AdminAreaRecord& area, const OSMDataset& data) {
    if (area.geometryOffset >= data.adminGeometry.size() || area.geometrySize == 0) {
        return Coordinate{0, 0};
    }

    const uint32_t available = std::min<uint32_t>(
        area.geometrySize,
        checkedU32(data.adminGeometry.size() - area.geometryOffset, "admin geometry available size"));
    long double twiceArea = 0.0L;
    long double centroidLon = 0.0L;
    long double centroidLat = 0.0L;
    for (uint32_t i = 0; i < available; ++i) {
        const uint32_t next = i + 1 < available ? i + 1 : 0;
        const Coordinate& a = data.adminGeometry[area.geometryOffset + i];
        const Coordinate& b = data.adminGeometry[area.geometryOffset + next];
        const long double cross = static_cast<long double>(a.lonE7) * b.latE7 -
                                  static_cast<long double>(b.lonE7) * a.latE7;
        twiceArea += cross;
        centroidLon += (static_cast<long double>(a.lonE7) + b.lonE7) * cross;
        centroidLat += (static_cast<long double>(a.latE7) + b.latE7) * cross;
    }

    if (std::abs(twiceArea) < 1.0L) {
        return Coordinate{
            static_cast<int32_t>((static_cast<int64_t>(area.bbox.minLatE7) + area.bbox.maxLatE7) / 2),
            static_cast<int32_t>((static_cast<int64_t>(area.bbox.minLonE7) + area.bbox.maxLonE7) / 2)
        };
    }

    return Coordinate{
        static_cast<int32_t>(std::llround(centroidLat / (3.0L * twiceArea))),
        static_cast<int32_t>(std::llround(centroidLon / (3.0L * twiceArea)))
    };
}

Coordinate representativePointForAdminArea(uint32_t areaIndex,
                                           const OSMDataset& data,
                                           const AdminAreaEdgeIndex& edgeIndex) {
    const AdminAreaRecord& area = data.adminAreas[areaIndex];
    const Coordinate centroid = adminAreaCentroid(area, data);
    if (adminAreaContainsPoint(areaIndex, centroid, data, edgeIndex)) {
        return centroid;
    }

    const Coordinate bboxCenter{
        static_cast<int32_t>((static_cast<int64_t>(area.bbox.minLatE7) + area.bbox.maxLatE7) / 2),
        static_cast<int32_t>((static_cast<int64_t>(area.bbox.minLonE7) + area.bbox.maxLonE7) / 2)
    };
    if (adminAreaContainsPoint(areaIndex, bboxCenter, data, edgeIndex)) {
        return bboxCenter;
    }

    if (area.geometryOffset < data.adminGeometry.size()) {
        const uint32_t available = std::min<uint32_t>(
            area.geometrySize,
            checkedU32(data.adminGeometry.size() - area.geometryOffset, "admin geometry available size"));
        const uint32_t step = std::max<uint32_t>(1, available / 64);
        for (uint32_t i = 0; i < available; i += step) {
            const Coordinate point = data.adminGeometry[area.geometryOffset + i];
            if (adminAreaContainsPoint(areaIndex, point, data, edgeIndex)) {
                return point;
            }
        }
    }

    return bboxCenter;
}

void assignAdministrativeAttributes(OSMDataset& data, bool showProgress = true) {
    AdminAreaGridIndex index;
    index.build(data);
    AdminAreaEdgeIndex edgeIndex;
    edgeIndex.build(data);

    data.houseAdminAreaIndexes.clear();
    data.streetAdminAreaIndexes.clear();
    data.poiAdminAreaIndexes.clear();
    data.adminParentAreaIndexes.clear();

    data.stats.housesWithAdminAreas = 0;
    data.stats.houseAdminAreaLinks = 0;
    data.stats.streetsWithAdminAreas = 0;
    data.stats.streetAdminAreaLinks = 0;
    data.stats.poisWithAdminAreas = 0;
    data.stats.poiAdminAreaLinks = 0;
    data.stats.adminAreasWithParents = 0;
    data.stats.adminParentAreaLinks = 0;

    std::vector<uint32_t> matches;
    matches.reserve(16);

    const uint64_t totalObjects = static_cast<uint64_t>(data.houses.size() + data.streets.size() +
                                                        data.pois.size() + data.adminAreas.size());
    uint64_t processedObjects = 0;
    std::unique_ptr<ProgressReporter> progress;
    if (showProgress) {
        progress = std::make_unique<ProgressReporter>("Administrative attribute lookup",
                                                      0,
                                                      0,
                                                      totalObjects,
                                                      "objects");
    }
    const auto updateProgress = [&]() {
        if (progress && (processedObjects % 4096 == 0 || processedObjects == totalObjects)) {
            progress->update(processedObjects);
        }
    };

    for (HouseRecord& house : data.houses) {
        matches.clear();
        house.adminAreaOffset = checkedU32(data.houseAdminAreaIndexes.size(), "house admin link offset");
        findAdminAreasForPoint(house.point, data, index, edgeIndex, matches);
        sortAndUniqueAdminMatches(matches, data);

        data.houseAdminAreaIndexes.insert(data.houseAdminAreaIndexes.end(), matches.begin(), matches.end());
        house.adminAreaSize = checkedU32(matches.size(), "house admin link size");

        if (!matches.empty()) {
            data.stats.housesWithAdminAreas += 1;
            data.stats.houseAdminAreaLinks += matches.size();
        }
        if (progress) {
            ++processedObjects;
            updateProgress();
        }
    }

    for (StreetRecord& street : data.streets) {
        matches.clear();
        street.adminAreaOffset = checkedU32(data.streetAdminAreaIndexes.size(), "street admin link offset");

        const uint32_t available = street.geometryOffset < data.streetGeometry.size()
            ? std::min<uint32_t>(street.geometrySize,
                  checkedU32(data.streetGeometry.size() - street.geometryOffset, "street geometry available size"))
            : 0;
        for (uint32_t i = 0; i < available; ++i) {
            findAdminAreasForPoint(data.streetGeometry[street.geometryOffset + i], data, index, edgeIndex, matches);
        }
        sortAndUniqueAdminMatches(matches, data);

        data.streetAdminAreaIndexes.insert(data.streetAdminAreaIndexes.end(), matches.begin(), matches.end());
        street.adminAreaSize = checkedU32(matches.size(), "street admin link size");

        if (!matches.empty()) {
            data.stats.streetsWithAdminAreas += 1;
            data.stats.streetAdminAreaLinks += matches.size();
        }
        if (progress) {
            ++processedObjects;
            updateProgress();
        }
    }

    for (PoiRecord& poi : data.pois) {
        matches.clear();
        poi.adminAreaOffset = checkedU32(data.poiAdminAreaIndexes.size(), "poi admin link offset");
        findAdminAreasForPoint(poi.point, data, index, edgeIndex, matches);
        sortAndUniqueAdminMatches(matches, data);

        data.poiAdminAreaIndexes.insert(data.poiAdminAreaIndexes.end(), matches.begin(), matches.end());
        poi.adminAreaSize = checkedU32(matches.size(), "poi admin link size");

        if (!matches.empty()) {
            data.stats.poisWithAdminAreas += 1;
            data.stats.poiAdminAreaLinks += matches.size();
        }
        if (progress) {
            ++processedObjects;
            updateProgress();
        }
    }

    for (uint32_t areaIndex = 0; areaIndex < data.adminAreas.size(); ++areaIndex) {
        AdminAreaRecord& area = data.adminAreas[areaIndex];
        matches.clear();
        area.parentAreaOffset = checkedU32(data.adminParentAreaIndexes.size(), "admin parent link offset");

        const Coordinate representativePoint = representativePointForAdminArea(areaIndex, data, edgeIndex);
        findAdminAreasForPoint(representativePoint, data, index, edgeIndex, matches);
        matches.erase(std::remove_if(matches.begin(), matches.end(), [&data, areaIndex, &area](uint32_t match) {
            return match == areaIndex || match >= data.adminAreas.size() ||
                   data.adminAreas[match].adminLevel >= area.adminLevel;
        }), matches.end());
        sortAndUniqueAdminMatches(matches, data);
        compactAdminMatchesByLevel(matches, data);

        data.adminParentAreaIndexes.insert(data.adminParentAreaIndexes.end(), matches.begin(), matches.end());
        area.parentAreaSize = checkedU32(matches.size(), "admin parent link size");

        if (!matches.empty()) {
            data.stats.adminAreasWithParents += 1;
            data.stats.adminParentAreaLinks += matches.size();
        }
        if (progress) {
            ++processedObjects;
            updateProgress();
        }
    }

    if (progress) {
        progress->finish(processedObjects);
    }
}
