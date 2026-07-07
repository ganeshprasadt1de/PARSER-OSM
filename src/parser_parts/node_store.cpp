struct WayRelevance {
    bool neededForRelation = false;
    bool houseWay = false;
    bool streetWay = false;
    bool adminWay = false;
    bool poiWay = false;
    uint8_t adminLevel = 0;

    bool any() const {
        return neededForRelation || houseWay || streetWay || adminWay || poiWay;
    }
};

enum class ExtractionPass {
    All,
    NonStreet,
    Streets
};

WayRelevance classifyWay(const osmium::Way& way,
                         const std::unordered_map<uint64_t, size_t>& relationIndexByWayId,
                         const std::vector<uint64_t>& recoveredUnnamedStreetWayIds = {},
                         ExtractionPass pass = ExtractionPass::All) {
    WayRelevance relevance;
    const uint64_t wayId = static_cast<uint64_t>(way.id());

    if (pass == ExtractionPass::All || pass == ExtractionPass::NonStreet) {
        relevance.neededForRelation = relationIndexByWayId.find(wayId) != relationIndexByWayId.end();
        relevance.houseWay = isHouseWay(way.tags());
        relevance.adminWay = isAdministrativeBoundary(way.tags(), relevance.adminLevel);
        relevance.poiWay = isPoiObject(way.tags());
    }

    if (pass == ExtractionPass::All || pass == ExtractionPass::Streets) {
        relevance.streetWay = isStreetWay(way.tags()) ||
                              (!recoveredUnnamedStreetWayIds.empty() &&
                               containsSortedId(recoveredUnnamedStreetWayIds, wayId) &&
                               isUnnamedStreetRecoveryCandidate(way.tags()));
    }

    return relevance;
}

class NeededNodeStore {
public:
    void add(uint64_t nodeId) {
        ids_.push_back(nodeId);
    }

    void finalize() {
        std::sort(ids_.begin(), ids_.end());
        ids_.erase(std::unique(ids_.begin(), ids_.end()), ids_.end());
        coordinates_.assign(ids_.size(), Coordinate{});
        foundBits_.assign((ids_.size() + 63) / 64, 0);
        buildBucketIndex();
    }

    size_t size() const {
        return ids_.size();
    }

    bool empty() const {
        return ids_.empty();
    }

    bool setCoordinateBySearch(uint64_t nodeId, Coordinate coordinate) {
        size_t index = 0;
        if (!findIndex(nodeId, index)) {
            return false;
        }

        coordinates_[index] = coordinate;
        markFound(index);
        return true;
    }

    bool getCoordinate(uint64_t nodeId, Coordinate& coordinateOut) const {
        size_t index = 0;
        if (!findIndex(nodeId, index)) {
            return false;
        }

        if (!isFound(index)) {
            return false;
        }

        coordinateOut = coordinates_[index];
        return true;
    }

    const std::vector<uint64_t>& ids() const {
        return ids_;
    }

    std::vector<Coordinate>& coordinates() {
        return coordinates_;
    }

    void markFound(size_t index) {
        if (index >= ids_.size()) {
            return;
        }
        foundBits_[index / 64] |= (uint64_t{1} << (index % 64));
    }

    size_t foundCount() const {
        size_t total = 0;
        for (uint64_t bits : foundBits_) {
            while (bits != 0) {
                bits &= bits - 1;
                ++total;
            }
        }
        return total;
    }

    size_t bucketIndexBytes() const {
        return bucketStarts_.capacity() * sizeof(uint32_t);
    }

private:
    void buildBucketIndex() {
        bucketStarts_.clear();
        if (ids_.empty()) {
            return;
        }

        const uint64_t maxBucket = ids_.back() >> kBucketShift;
        if (maxBucket > kMaxBucketCountForIndex) {
            return;
        }

        const uint32_t sentinel = checkedU32(ids_.size(), "needed node bucket sentinel");
        bucketStarts_.assign(static_cast<size_t>(maxBucket) + 2, sentinel);

        size_t firstUnfilledBucket = 0;
        for (size_t index = 0; index < ids_.size(); ++index) {
            const size_t bucket = static_cast<size_t>(ids_[index] >> kBucketShift);
            while (firstUnfilledBucket <= bucket) {
                bucketStarts_[firstUnfilledBucket] = checkedU32(index, "needed node bucket start");
                ++firstUnfilledBucket;
            }
        }

        while (firstUnfilledBucket < bucketStarts_.size()) {
            bucketStarts_[firstUnfilledBucket] = sentinel;
            ++firstUnfilledBucket;
        }
    }

    bool isFound(size_t index) const {
        if (index >= ids_.size()) {
            return false;
        }
        return (foundBits_[index / 64] & (uint64_t{1} << (index % 64))) != 0;
    }

    bool findIndex(uint64_t nodeId, size_t& indexOut) const {
        if (!bucketStarts_.empty()) {
            const size_t bucket = static_cast<size_t>(nodeId >> kBucketShift);
            if (bucket + 1 >= bucketStarts_.size()) {
                return false;
            }

            const size_t begin = bucketStarts_[bucket];
            const size_t end = bucketStarts_[bucket + 1];
            if (begin == end) {
                return false;
            }

            const auto found = std::lower_bound(ids_.begin() + static_cast<std::ptrdiff_t>(begin),
                                                ids_.begin() + static_cast<std::ptrdiff_t>(end),
                                                nodeId);
            if (found == ids_.begin() + static_cast<std::ptrdiff_t>(end) || *found != nodeId) {
                return false;
            }

            indexOut = static_cast<size_t>(found - ids_.begin());
            return true;
        }

        const auto found = std::lower_bound(ids_.begin(), ids_.end(), nodeId);
        if (found == ids_.end() || *found != nodeId) {
            return false;
        }

        indexOut = static_cast<size_t>(found - ids_.begin());
        return true;
    }

    static constexpr uint64_t kBucketShift = 16;
    static constexpr uint64_t kMaxBucketCountForIndex = 20000000;

    std::vector<uint64_t> ids_;
    std::vector<Coordinate> coordinates_;
    std::vector<uint64_t> foundBits_;
    std::vector<uint32_t> bucketStarts_;
};

std::vector<Coordinate> extractWayGeometryFromNeededNodes(const osmium::Way& way, const NeededNodeStore& neededNodes) {
    std::vector<Coordinate> geometry;
    geometry.reserve(way.nodes().size());
    for (const auto& nodeRef : way.nodes()) {
        Coordinate coordinate;
        if (neededNodes.getCoordinate(static_cast<uint64_t>(nodeRef.ref()), coordinate)) {
            geometry.push_back(coordinate);
        }
    }
    return geometry;
}

Coordinate averagePoint(const std::vector<Coordinate>& geometry) {
    if (geometry.empty()) {
        return {};
    }

    long double latSum = 0.0;
    long double lonSum = 0.0;
    for (const Coordinate& point : geometry) {
        latSum += static_cast<long double>(point.latE7);
        lonSum += static_cast<long double>(point.lonE7);
    }

    return Coordinate{
        static_cast<int32_t>(std::llround(latSum / static_cast<long double>(geometry.size()))),
        static_cast<int32_t>(std::llround(lonSum / static_cast<long double>(geometry.size())))
    };
}

Coordinate representativePointForHouseWay(const std::vector<Coordinate>& geometry) {
    if (geometry.empty()) {
        return {};
    }

    if (geometry.size() < 4 || !coordinatesEqual(geometry.front(), geometry.back())) {
        return averagePoint(geometry);
    }

    long double signedAreaTwice = 0.0;
    long double centroidLat = 0.0;
    long double centroidLon = 0.0;

    for (size_t i = 0; i + 1 < geometry.size(); ++i) {
        const long double x0 = static_cast<long double>(geometry[i].lonE7);
        const long double y0 = static_cast<long double>(geometry[i].latE7);
        const long double x1 = static_cast<long double>(geometry[i + 1].lonE7);
        const long double y1 = static_cast<long double>(geometry[i + 1].latE7);
        const long double cross = x0 * y1 - x1 * y0;
        signedAreaTwice += cross;
        centroidLon += (x0 + x1) * cross;
        centroidLat += (y0 + y1) * cross;
    }

    if (std::abs(static_cast<double>(signedAreaTwice)) < 1.0) {
        return averagePoint(geometry);
    }

    const long double factor = 1.0L / (3.0L * signedAreaTwice);
    return Coordinate{
        static_cast<int32_t>(std::llround(centroidLat * factor)),
        static_cast<int32_t>(std::llround(centroidLon * factor))
    };
}
