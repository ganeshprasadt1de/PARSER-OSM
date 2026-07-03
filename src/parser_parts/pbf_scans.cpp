class RelationScanHandler : public osmium::handler::Handler {
public:
    RelationScanHandler(std::vector<AdminRelationDefinition>& relationDefinitions,
                        std::vector<BuildingRelationDefinition>& buildingRelationDefinitions,
                        std::unordered_map<uint64_t, size_t>& relationIndexByWayId,
                        uint64_t& scannedRelations,
                        const std::string& language)
        : relationDefinitions_(relationDefinitions),
          buildingRelationDefinitions_(buildingRelationDefinitions),
          relationIndexByWayId_(relationIndexByWayId),
          scannedRelations_(scannedRelations),
          language_(language) {}

    void relation(const osmium::Relation& relation) {
        scannedRelations_ += 1;

        uint8_t adminLevel = 0;
        if (isAdministrativeBoundary(relation.tags(), adminLevel)) {
            AdminRelationDefinition definition;
            definition.relationId = static_cast<uint64_t>(relation.id());
            definition.adminLevel = adminLevel;
            definition.name = getLocalizedTag(relation.tags(), "name", language_);
            collectOuterWayIds(relation, definition.outerWayIds);
            if (!definition.outerWayIds.empty()) {
                const size_t relationIndex = relationDefinitions_.size();
                relationDefinitions_.push_back(std::move(definition));
                for (uint64_t wayId : relationDefinitions_.back().outerWayIds) {
                    relationIndexByWayId_.emplace(wayId, relationIndex);
                }
            }
        }

        if (!isHouseRelation(relation.tags())) {
            return;
        }

        BuildingRelationDefinition definition;
        definition.relationId = static_cast<uint64_t>(relation.id());
        definition.houseNumber = getTagValue(relation.tags(), "addr:housenumber");
        definition.streetName = getTagValue(relation.tags(), "addr:street");
        definition.postcode = getTagValue(relation.tags(), "addr:postcode");
        definition.city = getLocalizedTag(relation.tags(), "addr:city", language_);
        definition.country = getLocalizedTag(relation.tags(), "addr:country", language_);
        collectOuterWayIds(relation, definition.outerWayIds);
        if (definition.outerWayIds.empty()) {
            return;
        }

        const size_t relationIndex = relationDefinitions_.size() + buildingRelationDefinitions_.size();
        buildingRelationDefinitions_.push_back(std::move(definition));
        for (uint64_t wayId : buildingRelationDefinitions_.back().outerWayIds) {
            relationIndexByWayId_.emplace(wayId, relationIndex);
        }
    }

private:
    static void collectOuterWayIds(const osmium::Relation& relation, std::vector<uint64_t>& outerWayIds) {
        for (const auto& member : relation.members()) {
            if (member.type() != osmium::item_type::way) {
                continue;
            }

            const char* role = member.role();
            if (role == nullptr || std::string(role) != "outer") {
                continue;
            }

            outerWayIds.push_back(static_cast<uint64_t>(member.ref()));
        }
    }

    std::vector<AdminRelationDefinition>& relationDefinitions_;
    std::vector<BuildingRelationDefinition>& buildingRelationDefinitions_;
    std::unordered_map<uint64_t, size_t>& relationIndexByWayId_;
    uint64_t& scannedRelations_;
    const std::string& language_;
};

class NamedStreetEndpointScanHandler : public osmium::handler::Handler {
public:
    explicit NamedStreetEndpointScanHandler(std::vector<uint64_t>& endpointNodeIds)
        : endpointNodeIds_(endpointNodeIds) {}

    void way(const osmium::Way& way) {
        if (!isNamedStreetWay(way.tags())) {
            return;
        }

        uint64_t first = 0;
        uint64_t last = 0;
        if (!endpointNodeRefs(way, first, last)) {
            return;
        }

        endpointNodeIds_.push_back(first);
        endpointNodeIds_.push_back(last);
    }

private:
    std::vector<uint64_t>& endpointNodeIds_;
};

class UnnamedStreetRecoveryScanHandler : public osmium::handler::Handler {
public:
    UnnamedStreetRecoveryScanHandler(const std::vector<uint64_t>& namedStreetEndpointNodeIds,
                                     std::vector<uint64_t>& recoveredWayIds,
                                     uint64_t& candidateWays,
                                     uint64_t& recoveredWays)
        : namedStreetEndpointNodeIds_(namedStreetEndpointNodeIds),
          recoveredWayIds_(recoveredWayIds),
          candidateWays_(candidateWays),
          recoveredWays_(recoveredWays) {}

    void way(const osmium::Way& way) {
        if (!isUnnamedStreetRecoveryCandidate(way.tags())) {
            return;
        }

        candidateWays_ += 1;

        const bool isRoundabout = getTagValue(way.tags(), "junction") == "roundabout";
        const size_t maxNodes = isRoundabout ? kMaxRoundaboutNodes : kMaxConnectorNodes;
        if (way.nodes().size() < 2 || way.nodes().size() > maxNodes) {
            return;
        }

        if (isRoundabout) {
            for (const auto& nodeRef : way.nodes()) {
                if (nodeRef.ref() > 0 &&
                    containsSortedId(namedStreetEndpointNodeIds_, static_cast<uint64_t>(nodeRef.ref()))) {
                    recoveredWayIds_.push_back(static_cast<uint64_t>(way.id()));
                    recoveredWays_ += 1;
                    return;
                }
            }
            return;
        }

        uint64_t first = 0;
        uint64_t last = 0;
        if (!endpointNodeRefs(way, first, last)) {
            return;
        }

        if (!containsSortedId(namedStreetEndpointNodeIds_, first) &&
            !containsSortedId(namedStreetEndpointNodeIds_, last)) {
            return;
        }

        recoveredWayIds_.push_back(static_cast<uint64_t>(way.id()));
        recoveredWays_ += 1;
    }

private:
    static constexpr size_t kMaxConnectorNodes = 24;
    static constexpr size_t kMaxRoundaboutNodes = 128;

    const std::vector<uint64_t>& namedStreetEndpointNodeIds_;
    std::vector<uint64_t>& recoveredWayIds_;
    uint64_t& candidateWays_;
    uint64_t& recoveredWays_;
};

void storeWayRecords(const osmium::Way& way,
                     const std::vector<Coordinate>& geometry,
                     const WayRelevance& relevance,
                     OSMDataset& data,
                     StringInterner& interner,
                     const Parser::Options& options,
                     std::unordered_map<uint64_t, std::vector<Coordinate>>& relationWayGeometry) {
    if (geometry.empty()) {
        return;
    }

    const uint64_t wayId = static_cast<uint64_t>(way.id());

    if (relevance.neededForRelation) {
        relationWayGeometry[wayId] = geometry;
    }

    if (relevance.houseWay) {
        HouseRecord house;
        house.osmId = wayId;
        house.point = representativePointForHouseWay(geometry);
        house.houseNumber = interner.intern(getTagValue(way.tags(), "addr:housenumber"));
        house.streetName = interner.intern(getTagValue(way.tags(), "addr:street"));
        house.postcode = interner.intern(getTagValue(way.tags(), "addr:postcode"));
        house.city = interner.intern(getLocalizedTag(way.tags(), "addr:city", options.nameLanguage));
        house.country = interner.intern(getLocalizedTag(way.tags(), "addr:country", options.nameLanguage));
        house.source = FeatureSource::Way;

        data.houses.push_back(house);
        data.stats.housesFromWays += 1;
        if (house.streetName == kEmptyStringRef) {
            data.stats.housesMissingStreet += 1;
        }
        if (house.houseNumber == kEmptyStringRef) {
            data.stats.housesMissingHouseNumber += 1;
        }
    }

    if (relevance.streetWay && geometry.size() >= 2) {
        const uint32_t offset = static_cast<uint32_t>(data.streetGeometry.size());
        data.streetGeometry.insert(data.streetGeometry.end(), geometry.begin(), geometry.end());

        StreetRecord street;
        street.osmId = wayId;
        street.name = interner.intern(getStreetDisplayName(way.tags(), options.nameLanguage));
        street.highwayType = interner.intern(getTagValue(way.tags(), "highway"));
        street.geometryOffset = offset;
        street.geometrySize = static_cast<uint32_t>(geometry.size());
        street.bbox = computeBoundingBox(geometry);

        data.streets.push_back(street);
    }

    if (relevance.adminWay &&
        geometry.size() >= 4 &&
        coordinatesEqual(geometry.front(), geometry.back())) {
        const uint32_t offset = static_cast<uint32_t>(data.adminGeometry.size());
        data.adminGeometry.insert(data.adminGeometry.end(), geometry.begin(), geometry.end());

        AdminAreaRecord area;
        area.osmId = wayId;
        const std::string name = getLocalizedTag(way.tags(), "name", options.nameLanguage);
        area.name = interner.intern(name.empty() ? fallbackName("admin_way_", wayId) : name);
        area.adminLevel = relevance.adminLevel;
        area.source = FeatureSource::Way;
        area.geometryOffset = offset;
        area.geometrySize = static_cast<uint32_t>(geometry.size());
        area.bbox = computeBoundingBox(geometry);

        data.adminAreas.push_back(area);
        data.stats.adminAreasFromWays += 1;
    }

    if (relevance.poiWay) {
        std::string tagKey;
        std::string tagValue;
        const std::string category = poiCategoryFromTags(way.tags(), tagKey, tagValue);
        if (!category.empty()) {
            PoiRecord poi;
            poi.osmId = wayId;
            poi.point = representativePointForHouseWay(geometry);
            poi.bbox = computeBoundingBox(geometry);
            poi.name = interner.intern(getPoiDisplayName(way.tags(), options.nameLanguage));
            poi.category = interner.intern(category);
            poi.tagKey = interner.intern(tagKey);
            poi.tagValue = interner.intern(tagValue);
            poi.brand = interner.intern(getLocalizedTag(way.tags(), "brand", options.nameLanguage));
            poi.source = FeatureSource::Way;
            data.pois.push_back(poi);
            data.stats.poisFromWays += 1;
        }
    }
}

class NeededWayScanHandler : public osmium::handler::Handler {
public:
    NeededWayScanHandler(OSMDataset& data,
                         const Parser::Options& options,
                         const std::unordered_map<uint64_t, size_t>& relationIndexByWayId,
                         const std::vector<uint64_t>& recoveredUnnamedStreetWayIds,
                         ExtractionPass pass,
                         bool countScannedWays,
                         NeededNodeStore& neededNodes,
                         uint64_t& relevantWays,
                         uint64_t& collectedNodeRefs)
        : data_(data),
          options_(options),
          relationIndexByWayId_(relationIndexByWayId),
          recoveredUnnamedStreetWayIds_(recoveredUnnamedStreetWayIds),
          pass_(pass),
          countScannedWays_(countScannedWays),
          neededNodes_(neededNodes),
          relevantWays_(relevantWays),
          collectedNodeRefs_(collectedNodeRefs) {}

    void way(const osmium::Way& way) {
        if (countScannedWays_) {
            data_.stats.scannedWays += 1;
        }

        const WayRelevance relevance = classifyWay(way,
                                                   relationIndexByWayId_,
                                                   recoveredUnnamedStreetWayIds_,
                                                   pass_);
        if (!relevance.any()) {
            return;
        }

        relevantWays_ += 1;
        for (const auto& nodeRef : way.nodes()) {
            if (nodeRef.ref() <= 0) {
                continue;
            }
            neededNodes_.add(static_cast<uint64_t>(nodeRef.ref()));
            collectedNodeRefs_ += 1;
        }
    }

private:
    OSMDataset& data_;
    const Parser::Options& options_;
    const std::unordered_map<uint64_t, size_t>& relationIndexByWayId_;
    const std::vector<uint64_t>& recoveredUnnamedStreetWayIds_;
    ExtractionPass pass_;
    bool countScannedWays_ = false;
    NeededNodeStore& neededNodes_;
    uint64_t& relevantWays_;
    uint64_t& collectedNodeRefs_;
};

class NeededNodeScanHandler : public osmium::handler::Handler {
public:
    NeededNodeScanHandler(OSMDataset& data,
                          StringInterner& interner,
                          const Parser::Options& options,
                          NeededNodeStore& neededNodes,
                          bool storeHouseNodes,
                          bool countScannedNodes)
        : data_(data),
          interner_(interner),
          options_(options),
          neededNodes_(neededNodes),
          storeHouseNodes_(storeHouseNodes),
          countScannedNodes_(countScannedNodes) {}

    void node(const osmium::Node& node) {
        if (countScannedNodes_) {
            data_.stats.scannedNodes += 1;
        }

        if (!node.location().valid()) {
            return;
        }

        const uint64_t nodeId = static_cast<uint64_t>(node.id());
        const Coordinate coordinate = quantizeCoordinate(node.location());
        storeNeededCoordinate(nodeId, coordinate);

        if (storeHouseNodes_ && isPoiObject(node.tags())) {
            std::string tagKey;
            std::string tagValue;
            const std::string category = poiCategoryFromTags(node.tags(), tagKey, tagValue);
            if (!category.empty()) {
                PoiRecord poi;
                poi.osmId = nodeId;
                poi.point = coordinate;
                poi.bbox.expand(coordinate);
                poi.name = interner_.intern(getPoiDisplayName(node.tags(), options_.nameLanguage));
                poi.category = interner_.intern(category);
                poi.tagKey = interner_.intern(tagKey);
                poi.tagValue = interner_.intern(tagValue);
                poi.brand = interner_.intern(getLocalizedTag(node.tags(), "brand", options_.nameLanguage));
                poi.source = FeatureSource::Node;
                data_.pois.push_back(poi);
                data_.stats.poisFromNodes += 1;
            }
        }

        if (!storeHouseNodes_ || !isHouseNode(node.tags())) {
            return;
        }

        HouseRecord house;
        house.osmId = nodeId;
        house.point = coordinate;
        house.houseNumber = interner_.intern(getTagValue(node.tags(), "addr:housenumber"));
        house.streetName = interner_.intern(getTagValue(node.tags(), "addr:street"));
        house.postcode = interner_.intern(getTagValue(node.tags(), "addr:postcode"));
        house.city = interner_.intern(getLocalizedTag(node.tags(), "addr:city", options_.nameLanguage));
        house.country = interner_.intern(getLocalizedTag(node.tags(), "addr:country", options_.nameLanguage));
        house.source = FeatureSource::Node;

        data_.houses.push_back(house);
        data_.stats.housesFromNodes += 1;
        if (house.streetName == kEmptyStringRef) {
            data_.stats.housesMissingStreet += 1;
        }
        if (house.houseNumber == kEmptyStringRef) {
            data_.stats.housesMissingHouseNumber += 1;
        }
    }

private:
    void storeNeededCoordinate(uint64_t nodeId, Coordinate coordinate) {
        if (neededNodes_.empty()) {
            return;
        }

        if (seenNode_ && nodeId < lastNodeId_) {
            nodesSorted_ = false;
        }
        seenNode_ = true;
        lastNodeId_ = nodeId;

        if (!nodesSorted_) {
            neededNodes_.setCoordinateBySearch(nodeId, coordinate);
            return;
        }

        const std::vector<uint64_t>& ids = neededNodes_.ids();
        while (neededCursor_ < ids.size() && ids[neededCursor_] < nodeId) {
            ++neededCursor_;
        }

        if (neededCursor_ < ids.size() && ids[neededCursor_] == nodeId) {
            neededNodes_.coordinates()[neededCursor_] = coordinate;
            neededNodes_.foundFlags()[neededCursor_] = 1;
        }
    }

    OSMDataset& data_;
    StringInterner& interner_;
    const Parser::Options& options_;
    NeededNodeStore& neededNodes_;
    bool storeHouseNodes_ = false;
    bool countScannedNodes_ = false;
    size_t neededCursor_ = 0;
    uint64_t lastNodeId_ = 0;
    bool seenNode_ = false;
    bool nodesSorted_ = true;
};

class CompactWayBuildHandler : public osmium::handler::Handler {
public:
    CompactWayBuildHandler(OSMDataset& data,
                           StringInterner& interner,
                           const Parser::Options& options,
                           const std::unordered_map<uint64_t, size_t>& relationIndexByWayId,
                           const std::vector<uint64_t>& recoveredUnnamedStreetWayIds,
                           ExtractionPass pass,
                           const NeededNodeStore& neededNodes,
                           std::unordered_map<uint64_t, std::vector<Coordinate>>& relationWayGeometry)
        : data_(data),
          interner_(interner),
          options_(options),
          relationIndexByWayId_(relationIndexByWayId),
          recoveredUnnamedStreetWayIds_(recoveredUnnamedStreetWayIds),
          pass_(pass),
          neededNodes_(neededNodes),
          relationWayGeometry_(relationWayGeometry) {}

    void way(const osmium::Way& way) {
        const WayRelevance relevance = classifyWay(way,
                                                   relationIndexByWayId_,
                                                   recoveredUnnamedStreetWayIds_,
                                                   pass_);
        if (!relevance.any()) {
            return;
        }

        const std::vector<Coordinate> geometry = extractWayGeometryFromNeededNodes(way, neededNodes_);
        storeWayRecords(way, geometry, relevance, data_, interner_, options_, relationWayGeometry_);
    }

private:
    OSMDataset& data_;
    StringInterner& interner_;
    const Parser::Options& options_;
    const std::unordered_map<uint64_t, size_t>& relationIndexByWayId_;
    const std::vector<uint64_t>& recoveredUnnamedStreetWayIds_;
    ExtractionPass pass_;
    const NeededNodeStore& neededNodes_;
    std::unordered_map<uint64_t, std::vector<Coordinate>>& relationWayGeometry_;
};

struct CompactExtractionPassStats {
    uint64_t relevantWays = 0;
    uint64_t collectedNodeRefs = 0;
    size_t uniqueNeededNodes = 0;
    size_t resolvedCoordinates = 0;
    double bucketIndexMB = 0.0;
    double wayScanSeconds = 0.0;
    double nodeScanSeconds = 0.0;
    double wayBuildSeconds = 0.0;
};

struct CompactExtractionReport {
    bool lowMemory = false;
    double recoverySeconds = 0.0;
    CompactExtractionPassStats addressAndBoundary;
    CompactExtractionPassStats streetLines;
};

struct StreetConnectionStats {
    size_t endpointRecords = 0;
    size_t connectedComponents = 0;
    size_t streetRecordsBefore = 0;
    size_t streetRecordsAfter = 0;
};

template <typename Handler>
void applyReaderWithProgress(osmium::io::Reader& reader, Handler& handler, ProgressReporter& progress) {
    while (auto buffer = reader.read()) {
        osmium::apply(buffer, handler);
        const uint64_t offset = static_cast<uint64_t>(reader.offset());
        progress.update(offset);
    }
    progress.finish(static_cast<uint64_t>(reader.file_size()));
}

double roundedSeconds(double seconds) {
    return std::round(seconds * 1000.0) / 1000.0;
}

double displayedPassSeconds(const CompactExtractionPassStats& stats) {
    return roundedSeconds(stats.wayScanSeconds) +
           roundedSeconds(stats.nodeScanSeconds) +
           roundedSeconds(stats.wayBuildSeconds);
}

void printCompactExtractionPassStats(const std::string& title,
                                     const CompactExtractionPassStats& stats,
                                     double seconds) {
    std::cout << "\n" << title << "\n";
    std::cout << "  OSM ways used:              " << stats.relevantWays << "\n";
    std::cout << "  Lookup index memory:        " << std::fixed << std::setprecision(3)
              << stats.bucketIndexMB << " MB\n";
    std::cout << "  Time:                       " << seconds << " s\n";
}

CompactExtractionPassStats runCompactExtractionPass(
    const std::string& pbfPath,
    OSMDataset& data,
    StringInterner& interner,
    const Parser::Options& options,
    const std::unordered_map<uint64_t, size_t>& relationIndexByWayId,
    const std::vector<uint64_t>& recoveredUnnamedStreetWayIds,
    ExtractionPass pass,
    bool storeHouseNodes,
    bool countScannedPrimitives,
    std::unordered_map<uint64_t, std::vector<Coordinate>>& relationWayGeometry,
    size_t phaseIndex,
    size_t phaseCount,
    const std::string& phaseLabel) {
    NeededNodeStore neededNodes;
    CompactExtractionPassStats stats;

    const auto wayScanStart = std::chrono::steady_clock::now();
    {
        osmium::io::Reader reader(pbfPath,
                                  osmium::osm_entity_bits::way,
                                  osmium::io::read_meta::no);
        NeededWayScanHandler handler(data,
                                     options,
                                     relationIndexByWayId,
                                     recoveredUnnamedStreetWayIds,
                                     pass,
                                     countScannedPrimitives,
                                     neededNodes,
                                     stats.relevantWays,
                                     stats.collectedNodeRefs);
        ProgressReporter progress(phaseLabel + " way scan", phaseIndex, phaseCount,
                                  static_cast<uint64_t>(reader.file_size()), "bytes");
        applyReaderWithProgress(reader, handler, progress);
        reader.close();
    }
    neededNodes.finalize();
    const auto wayScanEnd = std::chrono::steady_clock::now();

    const auto nodeScanStart = std::chrono::steady_clock::now();
    {
        osmium::io::Reader reader(pbfPath,
                                  osmium::osm_entity_bits::node,
                                  osmium::io::read_meta::no);
        NeededNodeScanHandler handler(data,
                                      interner,
                                      options,
                                      neededNodes,
                                      storeHouseNodes,
                                      countScannedPrimitives);
        ProgressReporter progress(phaseLabel + " node scan", phaseIndex + 1, phaseCount,
                                  static_cast<uint64_t>(reader.file_size()), "bytes");
        applyReaderWithProgress(reader, handler, progress);
        reader.close();
    }
    const auto nodeScanEnd = std::chrono::steady_clock::now();

    const auto wayBuildStart = std::chrono::steady_clock::now();
    {
        osmium::io::Reader reader(pbfPath,
                                  osmium::osm_entity_bits::way,
                                  osmium::io::read_meta::no);
        CompactWayBuildHandler handler(data,
                                       interner,
                                       options,
                                       relationIndexByWayId,
                                       recoveredUnnamedStreetWayIds,
                                       pass,
                                       neededNodes,
                                       relationWayGeometry);
        ProgressReporter progress(phaseLabel + " way build", phaseIndex + 2, phaseCount,
                                  static_cast<uint64_t>(reader.file_size()), "bytes");
        applyReaderWithProgress(reader, handler, progress);
        reader.close();
    }
    const auto wayBuildEnd = std::chrono::steady_clock::now();

    stats.uniqueNeededNodes = neededNodes.size();
    stats.resolvedCoordinates = neededNodes.foundCount();
    stats.bucketIndexMB = bytesToMB(neededNodes.bucketIndexBytes());
    stats.wayScanSeconds = std::chrono::duration<double>(wayScanEnd - wayScanStart).count();
    stats.nodeScanSeconds = std::chrono::duration<double>(nodeScanEnd - nodeScanStart).count();
    stats.wayBuildSeconds = std::chrono::duration<double>(wayBuildEnd - wayBuildStart).count();
    return stats;
}

CompactExtractionReport extractNodesAndWaysWithCompactIndex(
    const std::string& pbfPath,
    OSMDataset& data,
    StringInterner& interner,
    const Parser::Options& options,
    const std::unordered_map<uint64_t, size_t>& relationIndexByWayId,
    std::unordered_map<uint64_t, std::vector<Coordinate>>& relationWayGeometry) {
    CompactExtractionReport report;
    report.lowMemory = options.lowMemory;
    std::vector<uint64_t> recoveredUnnamedStreetWayIds;
    uint64_t unnamedStreetRecoveryCandidates = 0;
    uint64_t recoveredUnnamedStreetWays = 0;
    const size_t phaseCount = options.lowMemory ? 9 : 6;

    const auto recoveryStart = std::chrono::steady_clock::now();
    {
        std::vector<uint64_t> namedStreetEndpointNodeIds;
        {
            osmium::io::Reader reader(pbfPath,
                                      osmium::osm_entity_bits::way,
                                      osmium::io::read_meta::no);
            NamedStreetEndpointScanHandler handler(namedStreetEndpointNodeIds);
            ProgressReporter progress("Street endpoint recovery scan", 2, phaseCount,
                                      static_cast<uint64_t>(reader.file_size()), "bytes");
            applyReaderWithProgress(reader, handler, progress);
            reader.close();
        }

        std::sort(namedStreetEndpointNodeIds.begin(), namedStreetEndpointNodeIds.end());
        namedStreetEndpointNodeIds.erase(std::unique(namedStreetEndpointNodeIds.begin(),
                                                     namedStreetEndpointNodeIds.end()),
                                         namedStreetEndpointNodeIds.end());

        {
            osmium::io::Reader reader(pbfPath,
                                      osmium::osm_entity_bits::way,
                                      osmium::io::read_meta::no);
            UnnamedStreetRecoveryScanHandler handler(namedStreetEndpointNodeIds,
                                                     recoveredUnnamedStreetWayIds,
                                                     unnamedStreetRecoveryCandidates,
                                                     recoveredUnnamedStreetWays);
            ProgressReporter progress("Unnamed street recovery scan", 3, phaseCount,
                                      static_cast<uint64_t>(reader.file_size()), "bytes");
            applyReaderWithProgress(reader, handler, progress);
            reader.close();
        }
    }

    std::sort(recoveredUnnamedStreetWayIds.begin(), recoveredUnnamedStreetWayIds.end());
    recoveredUnnamedStreetWayIds.erase(std::unique(recoveredUnnamedStreetWayIds.begin(),
                                                  recoveredUnnamedStreetWayIds.end()),
                                      recoveredUnnamedStreetWayIds.end());
    const auto recoveryEnd = std::chrono::steady_clock::now();
    report.recoverySeconds = std::chrono::duration<double>(recoveryEnd - recoveryStart).count();

    if (options.lowMemory) {

        report.addressAndBoundary =
            runCompactExtractionPass(pbfPath,
                                     data,
                                     interner,
                                     options,
                                     relationIndexByWayId,
                                     recoveredUnnamedStreetWayIds,
                                     ExtractionPass::NonStreet,
                                     true,
                                     true,
                                     relationWayGeometry,
                                     4,
                                     phaseCount,
                                     "Address and boundary extraction");

        report.streetLines =
            runCompactExtractionPass(pbfPath,
                                     data,
                                     interner,
                                     options,
                                     relationIndexByWayId,
                                     recoveredUnnamedStreetWayIds,
                                     ExtractionPass::Streets,
                                     false,
                                     false,
                                     relationWayGeometry,
                                     7,
                                     phaseCount,
                                     "Street line extraction");
    } else {
        report.addressAndBoundary =
            runCompactExtractionPass(pbfPath,
                                     data,
                                     interner,
                                     options,
                                     relationIndexByWayId,
                                     recoveredUnnamedStreetWayIds,
                                     ExtractionPass::All,
                                     true,
                                     true,
                                     relationWayGeometry,
                                     4,
                                     phaseCount,
                                     "Full dataset extraction");
    }
    return report;
}
