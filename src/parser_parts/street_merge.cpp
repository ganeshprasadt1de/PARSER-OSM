std::string featureSourceToString(FeatureSource source) {
    switch (source) {
        case FeatureSource::Node:
            return "node";
        case FeatureSource::Way:
            return "way";
        case FeatureSource::Relation:
            return "relation";
    }
    return "unknown";
}

struct CoordinateKey {
    int32_t latE7 = 0;
    int32_t lonE7 = 0;

    bool operator==(const CoordinateKey& other) const {
        return latE7 == other.latE7 && lonE7 == other.lonE7;
    }

    bool operator<(const CoordinateKey& other) const {
        if (latE7 != other.latE7) {
            return latE7 < other.latE7;
        }
        return lonE7 < other.lonE7;
    }
};

CoordinateKey keyOf(const Coordinate& coordinate) {
    return CoordinateKey{coordinate.latE7, coordinate.lonE7};
}

uint64_t packedCoordinateKey(CoordinateKey key) {
    const uint64_t lat = static_cast<uint32_t>(key.latE7);
    const uint64_t lon = static_cast<uint32_t>(key.lonE7);
    return (lat << 32U) | lon;
}

struct EndpointRecord {
    StringRef name = kEmptyStringRef;
    StringRef highwayType = kEmptyStringRef;
    CoordinateKey point;
    uint32_t streetIndex = 0;
};

class DisjointSet {
public:
    explicit DisjointSet(size_t size) : parent_(size), rank_(size, 0) {
        std::iota(parent_.begin(), parent_.end(), 0);
    }

    uint32_t find(uint32_t value) {
        uint32_t root = value;
        while (parent_[root] != root) {
            root = parent_[root];
        }
        while (parent_[value] != value) {
            const uint32_t next = parent_[value];
            parent_[value] = root;
            value = next;
        }
        return root;
    }

    void unite(uint32_t left, uint32_t right) {
        uint32_t leftRoot = find(left);
        uint32_t rightRoot = find(right);
        if (leftRoot == rightRoot) {
            return;
        }
        if (rank_[leftRoot] < rank_[rightRoot]) {
            std::swap(leftRoot, rightRoot);
        }
        parent_[rightRoot] = leftRoot;
        if (rank_[leftRoot] == rank_[rightRoot]) {
            rank_[leftRoot] += 1;
        }
    }

private:
    std::vector<uint32_t> parent_;
    std::vector<uint8_t> rank_;
};

struct LocalStreetEdge {
    uint32_t originalStreetIndex = 0;
    size_t startNode = 0;
    size_t endNode = 0;
    bool used = false;
};

struct IncidentEdge {
    size_t edgeIndex = 0;
    bool atStart = true;
};

void appendStreetGeometry(const OSMDataset& data,
                          const StreetRecord& street,
                          bool forward,
                          bool skipFirstPoint,
                          std::vector<Coordinate>& output) {
    if (street.geometrySize == 0) {
        return;
    }

    if (forward) {
        for (uint32_t i = 0; i < street.geometrySize; ++i) {
            if (skipFirstPoint && i == 0) {
                continue;
            }
            output.push_back(data.streetGeometry[street.geometryOffset + i]);
        }
        return;
    }

    for (uint32_t i = street.geometrySize; i > 0; --i) {
        const uint32_t sourceIndex = i - 1;
        if (skipFirstPoint && sourceIndex == street.geometrySize - 1) {
            continue;
        }
        output.push_back(data.streetGeometry[street.geometryOffset + sourceIndex]);
    }
}

void storeMergedStreet(const OSMDataset& oldData,
                       const std::vector<Coordinate>& geometry,
                       const std::vector<uint32_t>& sourceStreetIndexes,
                       std::vector<StreetRecord>& newStreets,
                       std::vector<Coordinate>& newGeometry) {
    if (geometry.size() < 2 || sourceStreetIndexes.empty()) {
        return;
    }

    const StreetRecord& first = oldData.streets[sourceStreetIndexes.front()];
    uint64_t minOsmId = first.osmId;
    for (uint32_t index : sourceStreetIndexes) {
        minOsmId = std::min(minOsmId, oldData.streets[index].osmId);
    }

    StreetRecord merged;
    merged.osmId = minOsmId;
    merged.name = first.name;
    merged.highwayType = first.highwayType;
    merged.geometryOffset = checkedU32(newGeometry.size(), "merged street geometry offset");
    merged.geometrySize = checkedU32(geometry.size(), "merged street geometry size");
    merged.bbox = computeBoundingBox(geometry);

    newGeometry.insert(newGeometry.end(), geometry.begin(), geometry.end());
    newStreets.push_back(merged);
}

void copyStreetRecord(const OSMDataset& oldData,
                      uint32_t streetIndex,
                      std::vector<StreetRecord>& newStreets,
                      std::vector<Coordinate>& newGeometry) {
    const StreetRecord& oldStreet = oldData.streets[streetIndex];
    if (oldStreet.geometrySize < 2) {
        return;
    }

    StreetRecord copy = oldStreet;
    copy.geometryOffset = checkedU32(newGeometry.size(), "copied street geometry offset");
    newGeometry.insert(newGeometry.end(),
                       oldData.streetGeometry.begin() + oldStreet.geometryOffset,
                       oldData.streetGeometry.begin() + oldStreet.geometryOffset + oldStreet.geometrySize);
    newStreets.push_back(copy);
}

size_t findOrAddLocalNode(CoordinateKey key,
                          std::unordered_map<uint64_t, size_t>& nodeIndexByCoordinate,
                          std::vector<std::vector<IncidentEdge>>& incidentEdges) {
    const uint64_t packedKey = packedCoordinateKey(key);
    const auto found = nodeIndexByCoordinate.find(packedKey);
    if (found != nodeIndexByCoordinate.end()) {
        return found->second;
    }

    const size_t index = incidentEdges.size();
    nodeIndexByCoordinate.emplace(packedKey, index);
    incidentEdges.emplace_back();
    return index;
}

void appendChainFromComponent(const OSMDataset& oldData,
                              const std::vector<uint32_t>& component,
                              std::vector<StreetRecord>& newStreets,
                              std::vector<Coordinate>& newGeometry) {
    if (component.size() == 1) {
        copyStreetRecord(oldData, component.front(), newStreets, newGeometry);
        return;
    }

    std::unordered_map<uint64_t, size_t> nodeIndexByCoordinate;
    std::vector<std::vector<IncidentEdge>> incidentEdges;
    std::vector<LocalStreetEdge> edges;
    nodeIndexByCoordinate.reserve(component.size() * 2);
    incidentEdges.reserve(component.size() * 2);
    edges.reserve(component.size());

    for (uint32_t streetIndex : component) {
        const StreetRecord& street = oldData.streets[streetIndex];
        if (street.geometrySize < 2) {
            continue;
        }

        const Coordinate& first = oldData.streetGeometry[street.geometryOffset];
        const Coordinate& last = oldData.streetGeometry[street.geometryOffset + street.geometrySize - 1];
        if (coordinatesEqual(first, last)) {
            copyStreetRecord(oldData, streetIndex, newStreets, newGeometry);
            continue;
        }

        const size_t startNode = findOrAddLocalNode(keyOf(first), nodeIndexByCoordinate, incidentEdges);
        const size_t endNode = findOrAddLocalNode(keyOf(last), nodeIndexByCoordinate, incidentEdges);
        const size_t edgeIndex = edges.size();
        edges.push_back(LocalStreetEdge{streetIndex, startNode, endNode, false});
        incidentEdges[startNode].push_back(IncidentEdge{edgeIndex, true});
        incidentEdges[endNode].push_back(IncidentEdge{edgeIndex, false});
    }

    const auto findUnusedIncident = [&](size_t nodeIndex) -> size_t {
        for (const IncidentEdge& incident : incidentEdges[nodeIndex]) {
            if (!edges[incident.edgeIndex].used) {
                return incident.edgeIndex;
            }
        }
        return std::numeric_limits<size_t>::max();
    };

    const auto hasUnusedIncident = [&](size_t nodeIndex) -> bool {
        return findUnusedIncident(nodeIndex) != std::numeric_limits<size_t>::max();
    };

    size_t remainingEdges = 0;
    for (const LocalStreetEdge& edge : edges) {
        if (!edge.used) {
            remainingEdges += 1;
        }
    }

    while (remainingEdges > 0) {
        size_t startNode = std::numeric_limits<size_t>::max();
        size_t startEdge = std::numeric_limits<size_t>::max();

        for (size_t nodeIndex = 0; nodeIndex < incidentEdges.size(); ++nodeIndex) {
            if (incidentEdges[nodeIndex].size() != 2 && hasUnusedIncident(nodeIndex)) {
                startNode = nodeIndex;
                startEdge = findUnusedIncident(nodeIndex);
                break;
            }
        }

        if (startEdge == std::numeric_limits<size_t>::max()) {
            for (size_t edgeIndex = 0; edgeIndex < edges.size(); ++edgeIndex) {
                if (!edges[edgeIndex].used) {
                    startEdge = edgeIndex;
                    startNode = edges[edgeIndex].startNode;
                    break;
                }
            }
        }

        if (startEdge == std::numeric_limits<size_t>::max()) {
            break;
        }

        std::vector<Coordinate> mergedGeometry;
        std::vector<uint32_t> sourceStreetIndexes;
        size_t currentNode = startNode;
        size_t currentEdge = startEdge;
        bool skipFirstPoint = false;

        while (currentEdge != std::numeric_limits<size_t>::max() && !edges[currentEdge].used) {
            LocalStreetEdge& edge = edges[currentEdge];
            const bool forward = currentNode == edge.startNode;
            appendStreetGeometry(oldData,
                                 oldData.streets[edge.originalStreetIndex],
                                 forward,
                                 skipFirstPoint,
                                 mergedGeometry);
            sourceStreetIndexes.push_back(edge.originalStreetIndex);
            edge.used = true;
            remainingEdges -= 1;
            skipFirstPoint = true;

            currentNode = forward ? edge.endNode : edge.startNode;
            if (incidentEdges[currentNode].size() != 2) {
                break;
            }

            currentEdge = std::numeric_limits<size_t>::max();
            for (const IncidentEdge& incident : incidentEdges[currentNode]) {
                if (!edges[incident.edgeIndex].used) {
                    currentEdge = incident.edgeIndex;
                    break;
                }
            }
        }

        storeMergedStreet(oldData, mergedGeometry, sourceStreetIndexes, newStreets, newGeometry);
    }
}

StreetConnectionStats connectStreetSegments(OSMDataset& data) {
    StreetConnectionStats stats;
    const size_t originalStreetCount = data.streets.size();
    stats.streetRecordsBefore = originalStreetCount;
    if (originalStreetCount < 2) {
        stats.streetRecordsAfter = originalStreetCount;
        return stats;
    }

    std::vector<EndpointRecord> endpoints;
    endpoints.reserve(originalStreetCount * 2);
    for (size_t i = 0; i < data.streets.size(); ++i) {
        const StreetRecord& street = data.streets[i];
        if (street.name == kEmptyStringRef || street.geometrySize < 2) {
            continue;
        }

        const Coordinate& first = data.streetGeometry[street.geometryOffset];
        const Coordinate& last = data.streetGeometry[street.geometryOffset + street.geometrySize - 1];
        if (coordinatesEqual(first, last)) {
            continue;
        }

        const uint32_t streetIndex = checkedU32(i, "street record index");
        endpoints.push_back(EndpointRecord{street.name, street.highwayType, keyOf(first), streetIndex});
        endpoints.push_back(EndpointRecord{street.name, street.highwayType, keyOf(last), streetIndex});
    }

    if (endpoints.empty()) {
        stats.streetRecordsAfter = originalStreetCount;
        return stats;
    }
    stats.endpointRecords = endpoints.size();

    std::sort(endpoints.begin(), endpoints.end(), [](const EndpointRecord& left, const EndpointRecord& right) {
        if (left.name != right.name) {
            return left.name < right.name;
        }
        if (left.highwayType != right.highwayType) {
            return left.highwayType < right.highwayType;
        }
        if (left.point.latE7 != right.point.latE7) {
            return left.point.latE7 < right.point.latE7;
        }
        if (left.point.lonE7 != right.point.lonE7) {
            return left.point.lonE7 < right.point.lonE7;
        }
        return left.streetIndex < right.streetIndex;
    });

    DisjointSet components(originalStreetCount);
    for (size_t i = 0; i < endpoints.size();) {
        size_t j = i + 1;
        while (j < endpoints.size() &&
               endpoints[j].name == endpoints[i].name &&
               endpoints[j].highwayType == endpoints[i].highwayType &&
               endpoints[j].point == endpoints[i].point) {
            ++j;
        }

        if (j - i > 1) {
            const uint32_t firstStreet = endpoints[i].streetIndex;
            for (size_t k = i + 1; k < j; ++k) {
                components.unite(firstStreet, endpoints[k].streetIndex);
            }
        }

        i = j;
    }

    std::vector<uint32_t> order(originalStreetCount);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](uint32_t left, uint32_t right) {
        const uint32_t leftRoot = components.find(left);
        const uint32_t rightRoot = components.find(right);
        if (leftRoot != rightRoot) {
            return leftRoot < rightRoot;
        }
        return left < right;
    });

    std::vector<StreetRecord> newStreets;
    std::vector<Coordinate> newGeometry;
    newStreets.reserve(data.streets.size());
    newGeometry.reserve(data.streetGeometry.size());

    std::vector<uint32_t> component;
    size_t connectedComponents = 0;

    for (size_t i = 0; i < order.size();) {
        const uint32_t root = components.find(order[i]);
        size_t j = i + 1;
        while (j < order.size() && components.find(order[j]) == root) {
            ++j;
        }

        component.assign(order.begin() + static_cast<std::ptrdiff_t>(i),
                         order.begin() + static_cast<std::ptrdiff_t>(j));
        if (component.size() > 1) {
            connectedComponents += 1;
        }
        appendChainFromComponent(data, component, newStreets, newGeometry);
        i = j;
    }

    data.streets = std::move(newStreets);
    data.streetGeometry = std::move(newGeometry);
    data.stats.streetsTotal = data.streets.size();
    data.stats.datasetBytes = 0.0;
    data.stats.datasetMB = 0.0;

    stats.connectedComponents = connectedComponents;
    stats.streetRecordsAfter = data.streets.size();
    return stats;
}
