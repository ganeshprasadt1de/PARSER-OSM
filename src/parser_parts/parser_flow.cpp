void addBuildingRelationHouses(const std::vector<BuildingRelationDefinition>& relationDefinitions,
                               const std::vector<RelationWaySpan>& relationWaySpans,
                               const std::vector<Coordinate>& relationWayCoords,
                               OSMDataset& data,
                               StringInterner& interner) {
    for (const BuildingRelationDefinition& relation : relationDefinitions) {
        const std::vector<std::vector<Coordinate>> rings =
            buildClosedRings(relation.outerWayIds, relationWaySpans, relationWayCoords);
        if (rings.empty()) {
            continue;
        }

        const std::vector<Coordinate>& outerRing = rings.front();
        if (outerRing.size() < 4) {
            continue;
        }

        HouseRecord house;
        house.osmId = relation.relationId;
        house.point = representativePointForHouseWay(outerRing);
        house.houseNumber = interner.intern(relation.houseNumber);
        house.streetName = interner.intern(relation.streetName);
        house.postcode = interner.intern(relation.postcode);
        house.city = interner.intern(relation.city);
        house.country = interner.intern(relation.country);
        house.source = FeatureSource::Relation;

        data.houses.push_back(house);
        data.stats.housesFromRelations += 1;
        if (house.streetName == kEmptyStringRef) {
            data.stats.housesMissingStreet += 1;
        }
        if (house.houseNumber == kEmptyStringRef) {
            data.stats.housesMissingHouseNumber += 1;
        }
    }
}

bool pointInRingForAssembly(const Coordinate& point, const std::vector<Coordinate>& ring) {
    if (ring.size() < 4) {
        return false;
    }
    std::vector<uint32_t> edges;
    edges.reserve(ring.size());
    for (uint32_t i = 0; i < ring.size(); ++i) {
        edges.push_back(i);
    }
    return pointInPolygon(point, ring, 0, checkedU32(ring.size(), "assembly ring size"), edges);
}

Coordinate firstUsableRingPoint(const std::vector<Coordinate>& ring) {
    if (ring.empty()) {
        return Coordinate{};
    }
    return ring[ring.size() / 2];
}

void appendAdminRing(OSMDataset& data,
                     uint32_t areaIndex,
                     const std::vector<Coordinate>& ring,
                     uint8_t role) {
    const uint32_t offset = checkedU32(data.adminGeometry.size(), "admin ring geometry offset");
    data.adminGeometry.insert(data.adminGeometry.end(), ring.begin(), ring.end());

    AdminRingRecord record;
    record.geometryOffset = offset;
    record.geometrySize = checkedU32(ring.size(), "admin ring geometry size");
    record.adminAreaIndex = areaIndex;
    record.role = role;
    data.adminRings.push_back(record);
}

Parser::Parser() : Parser(Options{}) {}

Parser::Parser(Options options) : options_(std::move(options)) {}

namespace {

constexpr char kForwardIndexMagic[8] = {'F', 'G', 'I', 'D', 'X', '0', '0', '2'};

void writePostingLists(std::ofstream& out, const std::vector<GeocodePostingList>& lists) {
    const uint64_t count = static_cast<uint64_t>(lists.size());
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const GeocodePostingList& list : lists) {
        writeString(out, list.token);
        writePlainVector(out, list.refs);
    }
}

bool readPostingLists(std::ifstream& in, std::vector<GeocodePostingList>& lists) {
    uint64_t count = 0;
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!in.good()) {
        return false;
    }
    lists.resize(static_cast<size_t>(count));
    for (GeocodePostingList& list : lists) {
        readString(in, list.token);
        if (!readPlainVector(in, list.refs)) {
            return false;
        }
    }
    return true;
}

void writeForwardGeocodeIndex(std::ofstream& out, const ForwardGeocodeIndex& index) {
    out.write(kForwardIndexMagic, sizeof(kForwardIndexMagic));
    const uint8_t available = index.available ? 1 : 0;
    out.write(reinterpret_cast<const char*>(&available), sizeof(available));
    writePostingLists(out, index.context);
    writePostingLists(out, index.primary);

    const uint64_t vocabularyCount = static_cast<uint64_t>(index.vocabulary.size());
    out.write(reinterpret_cast<const char*>(&vocabularyCount), sizeof(vocabularyCount));
    for (const std::string& token : index.vocabulary) {
        writeString(out, token);
    }

    writePlainVector(out, index.suffixArray);
    out.write(reinterpret_cast<const char*>(&index.postingRefs), sizeof(index.postingRefs));
    out.write(reinterpret_cast<const char*>(&index.primaryPostingRefs), sizeof(index.primaryPostingRefs));
    out.write(reinterpret_cast<const char*>(&index.largestPostingList), sizeof(index.largestPostingList));
    out.write(reinterpret_cast<const char*>(&index.largestPrimaryPostingList), sizeof(index.largestPrimaryPostingList));
    out.write(reinterpret_cast<const char*>(&index.estimatedBytes), sizeof(index.estimatedBytes));
}

bool readRequiredForwardGeocodeIndex(std::ifstream& in, ForwardGeocodeIndex& index) {
    index.clear();
    char magic[sizeof(kForwardIndexMagic)] = {};
    in.read(magic, sizeof(magic));
    if (in.eof() || in.gcount() == 0) {
        return false;
    }
    if (in.gcount() != static_cast<std::streamsize>(sizeof(magic)) ||
        !std::equal(std::begin(magic), std::end(magic), std::begin(kForwardIndexMagic))) {
        return false;
    }

    uint8_t available = 0;
    in.read(reinterpret_cast<char*>(&available), sizeof(available));
    if (!readPostingLists(in, index.context) ||
        !readPostingLists(in, index.primary)) {
        return false;
    }

    uint64_t vocabularyCount = 0;
    in.read(reinterpret_cast<char*>(&vocabularyCount), sizeof(vocabularyCount));
    if (!in.good()) {
        return false;
    }
    index.vocabulary.resize(static_cast<size_t>(vocabularyCount));
    for (std::string& token : index.vocabulary) {
        readString(in, token);
    }

    if (!readPlainVector(in, index.suffixArray)) {
        return false;
    }
    in.read(reinterpret_cast<char*>(&index.postingRefs), sizeof(index.postingRefs));
    in.read(reinterpret_cast<char*>(&index.primaryPostingRefs), sizeof(index.primaryPostingRefs));
    in.read(reinterpret_cast<char*>(&index.largestPostingList), sizeof(index.largestPostingList));
    in.read(reinterpret_cast<char*>(&index.largestPrimaryPostingList), sizeof(index.largestPrimaryPostingList));
    in.read(reinterpret_cast<char*>(&index.estimatedBytes), sizeof(index.estimatedBytes));
    if (!in.good()) {
        return false;
    }
    index.available = available != 0;
    return true;
}

}  // namespace

void Parser::parsePbf(const std::string& pbfPath) {
    configurePbfBlobThreads(options_.pbfThreads);

    data_ = OSMDataset{};
    data_.selectedNameLanguage = options_.nameLanguage;
    data_.stats.inputFileBytes = tryGetFileSizeBytes(pbfPath);
    data_.stats.inputFileMB = bytesToMB(data_.stats.inputFileBytes);

    const ProcessMemorySnapshot startMemory = getProcessMemorySnapshot();
    double peakRssMB = startMemory.peakRssMB;
    data_.stats.rssStartMB = startMemory.rssMB;

    std::vector<AdminRelationDefinition> relationDefinitions;
    std::vector<BuildingRelationDefinition> buildingRelationDefinitions;
    std::unordered_map<uint64_t, size_t> relationIndexByWayId;
    const size_t pbfPhaseCount = options_.lowMemory ? 9 : 6;

    const auto relationScanStart = std::chrono::steady_clock::now();
    {
        osmium::io::Reader reader(pbfPath,
                                  osmium::osm_entity_bits::relation,
                                  osmium::io::read_meta::no);
        RelationScanHandler handler(relationDefinitions,
                                    buildingRelationDefinitions,
                                    relationIndexByWayId,
                                    data_.stats.scannedRelations,
                                    options_.nameLanguage);
        ProgressReporter progress("Administrative relation scan", 1, pbfPhaseCount,
                                  static_cast<uint64_t>(reader.file_size()), "bytes");
        applyReaderWithProgress(reader, handler, progress);
        reader.close();
    }
    const auto relationScanEnd = std::chrono::steady_clock::now();
    data_.stats.relationScanSeconds =
        std::chrono::duration<double>(relationScanEnd - relationScanStart).count();
    updatePeakRss(getProcessMemorySnapshot(), peakRssMB);

    std::cout << "\nAdministrative Boundary Relation Scan\n";
    std::cout << "  Relations scanned:           " << data_.stats.scannedRelations << "\n";
    std::cout << "  Admin relations kept:        " << relationDefinitions.size() << "\n";
    std::cout << "  Building relations kept:     " << buildingRelationDefinitions.size() << "\n";
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "  Scan time:                   " << data_.stats.relationScanSeconds << " s\n";

    StringInterner interner;
    std::vector<RelationWaySpan> relationWaySpans;
    std::vector<Coordinate> relationWayCoords;
    relationWaySpans.reserve(relationIndexByWayId.size());

    const auto extractionStart = std::chrono::steady_clock::now();
    CompactExtractionReport extractionReport =
        extractNodesAndWaysWithCompactIndex(pbfPath,
                                            data_,
                                            interner,
                                            options_,
                                            relationIndexByWayId,
                                            relationWaySpans,
                                            relationWayCoords);
    std::sort(relationWaySpans.begin(), relationWaySpans.end(), [](const RelationWaySpan& left,
                                                                   const RelationWaySpan& right) {
        return left.wayId < right.wayId;
    });
    addBuildingRelationHouses(buildingRelationDefinitions, relationWaySpans, relationWayCoords, data_, interner);
    const auto extractionEnd = std::chrono::steady_clock::now();
    const double measuredExtractionSeconds =
        std::chrono::duration<double>(extractionEnd - extractionStart).count();
    updatePeakRss(getProcessMemorySnapshot(), peakRssMB);

    const double streetExtractionSeconds =
        options_.lowMemory ? displayedPassSeconds(extractionReport.streetLines) : 0.0;
    const double addressExtractionSeconds = options_.lowMemory
        ? std::max(0.0, measuredExtractionSeconds - streetExtractionSeconds)
        : measuredExtractionSeconds;
    data_.stats.extractionSeconds = addressExtractionSeconds + streetExtractionSeconds;
    data_.stats.housesTotal = data_.houses.size();
    data_.stats.poisTotal = data_.pois.size();

    printCompactExtractionPassStats(options_.lowMemory ? "Address And Boundary Extraction"
                                                       : "Full Dataset Extraction",
                                    extractionReport.addressAndBoundary,
                                    roundedSeconds(addressExtractionSeconds));

    std::cout << "\nHouse Extraction\n";
    std::cout << "  Houses extracted:           " << data_.stats.housesTotal << "\n";
    std::cout << "  Houses from address nodes:  " << data_.stats.housesFromNodes << "\n";
    std::cout << "  Houses from building ways:  " << data_.stats.housesFromWays << "\n";
    std::cout << "  Houses from relations:      " << data_.stats.housesFromRelations << "\n";
    std::cout << "  Houses missing street:      " << data_.stats.housesMissingStreet << "\n";
    std::cout << "  Houses missing number:      " << data_.stats.housesMissingHouseNumber << "\n";
    std::cout << "  POIs extracted:             " << data_.stats.poisTotal << "\n";
    if (options_.lowMemory) {
        std::cout << "  Note: Time and memory included in Address And Boundary Extraction phase\n";
    } else {
        std::cout << "  Note: Time and memory included in Full Dataset Extraction phase\n";
    }

    if (options_.lowMemory) {
        printCompactExtractionPassStats("Street Line Extraction",
                                        extractionReport.streetLines,
                                        roundedSeconds(streetExtractionSeconds));
    }

    const auto relationAssemblyStart = std::chrono::steady_clock::now();
    for (const AdminRelationDefinition& relation : relationDefinitions) {
        const std::vector<std::vector<Coordinate>> outerRings =
            buildClosedRings(relation.outerWayIds, relationWaySpans, relationWayCoords);
        const std::vector<std::vector<Coordinate>> innerRings =
            buildClosedRings(relation.innerWayIds, relationWaySpans, relationWayCoords);
        std::vector<BoundingBox> outerBboxes;
        outerBboxes.reserve(outerRings.size());
        for (const std::vector<Coordinate>& outerRing : outerRings) {
            outerBboxes.push_back(computeBoundingBox(outerRing));
        }

        std::vector<std::vector<size_t>> assignedInnerRings(outerRings.size());
        for (size_t innerIndex = 0; innerIndex < innerRings.size(); ++innerIndex) {
            const std::vector<Coordinate>& innerRing = innerRings[innerIndex];
            if (innerRing.size() < 4) {
                continue;
            }

            const Coordinate probe = firstUsableRingPoint(innerRing);
            size_t bestOuterIndex = outerRings.size();
            double bestAreaScore = std::numeric_limits<double>::max();
            for (size_t outerIndex = 0; outerIndex < outerRings.size(); ++outerIndex) {
                if (outerRings[outerIndex].size() < 4 || !outerBboxes[outerIndex].contains(probe)) {
                    continue;
                }
                if (!pointInRingForAssembly(probe, outerRings[outerIndex])) {
                    continue;
                }
                const double score = bboxAreaScore(outerBboxes[outerIndex]);
                if (score < bestAreaScore) {
                    bestAreaScore = score;
                    bestOuterIndex = outerIndex;
                }
            }
            if (bestOuterIndex < outerRings.size()) {
                assignedInnerRings[bestOuterIndex].push_back(innerIndex);
            }
        }

        for (size_t outerIndex = 0; outerIndex < outerRings.size(); ++outerIndex) {
            const std::vector<Coordinate>& outerRing = outerRings[outerIndex];
            if (outerRing.size() < 4) {
                continue;
            }

            const uint32_t offset = checkedU32(data_.adminGeometry.size(), "admin geometry offset");
            data_.adminGeometry.insert(data_.adminGeometry.end(), outerRing.begin(), outerRing.end());

            AdminAreaRecord area;
            area.osmId = relation.relationId;
            area.name = interner.intern(
                relation.name.empty() ? fallbackName("admin_relation_", relation.relationId) : relation.name);
            area.adminLevel = relation.adminLevel;
            area.source = FeatureSource::Relation;
            area.geometryOffset = offset;
            area.geometrySize = checkedU32(outerRing.size(), "admin geometry size");
            area.ringOffset = checkedU32(data_.adminRings.size(), "admin ring offset");
            area.ringSize = 0;
            area.bbox = outerBboxes[outerIndex];

            data_.adminAreas.push_back(area);
            const uint32_t areaIndex = checkedU32(data_.adminAreas.size() - 1, "admin area index");
            AdminRingRecord outerRecord;
            outerRecord.geometryOffset = offset;
            outerRecord.geometrySize = checkedU32(outerRing.size(), "admin ring geometry size");
            outerRecord.adminAreaIndex = areaIndex;
            outerRecord.role = 0;
            data_.adminRings.push_back(outerRecord);
            data_.adminAreas[areaIndex].ringSize += 1;

            for (size_t innerIndex : assignedInnerRings[outerIndex]) {
                appendAdminRing(data_, areaIndex, innerRings[innerIndex], 1);
                data_.adminAreas[areaIndex].ringSize += 1;
            }

            data_.stats.adminAreasFromRelations += 1;
        }
    }
    const auto relationAssemblyEnd = std::chrono::steady_clock::now();
    data_.stats.relationAssemblySeconds =
        std::chrono::duration<double>(relationAssemblyEnd - relationAssemblyStart).count();

    std::cout << "\nAdministrative Boundary Assembly\n";
    std::cout << "  Admin areas from relations:  " << data_.stats.adminAreasFromRelations << "\n";
    std::cout << "  Assembly time:               " << data_.stats.relationAssemblySeconds << " s\n";

    data_.stats.adminAreasTotal = data_.adminAreas.size();

    std::vector<RelationWaySpan>().swap(relationWaySpans);
    std::vector<Coordinate>().swap(relationWayCoords);
    std::vector<AdminRelationDefinition>().swap(relationDefinitions);
    std::vector<BuildingRelationDefinition>().swap(buildingRelationDefinitions);
    std::unordered_map<uint64_t, size_t>().swap(relationIndexByWayId);
    updatePeakRss(getProcessMemorySnapshot(), peakRssMB);

    data_.strings = interner.releaseValues();
    interner.clearIndex();

    StreetConnectionStats connectionStats;
    if (options_.connectStreets) {
        const auto connectionStart = std::chrono::steady_clock::now();
        connectionStats = connectStreetSegments(data_);
        const auto connectionEnd = std::chrono::steady_clock::now();
        data_.stats.connectionSeconds = std::chrono::duration<double>(connectionEnd - connectionStart).count();
        updatePeakRss(getProcessMemorySnapshot(), peakRssMB);

        std::cout << "\nStreet connection\n";
        std::cout << "  Endpoint records:           " << connectionStats.endpointRecords << "\n";
        std::cout << "  Connected components:       " << connectionStats.connectedComponents << "\n";
        std::cout << "  Street records before:      " << connectionStats.streetRecordsBefore << "\n";
        std::cout << "  Street records after:       " << connectionStats.streetRecordsAfter << "\n";
        std::cout << "  Street records reduced:     "
                  << (connectionStats.streetRecordsBefore - connectionStats.streetRecordsAfter) << "\n";
        std::cout << "  Connection time:            " << std::fixed << std::setprecision(3)
                  << roundedSeconds(data_.stats.connectionSeconds) << " s\n";
    }

    const auto adminAttributionStart = std::chrono::steady_clock::now();
    assignAdministrativeAttributes(data_);
    const auto adminAttributionEnd = std::chrono::steady_clock::now();
    data_.stats.adminAttributionSeconds =
        std::chrono::duration<double>(adminAttributionEnd - adminAttributionStart).count();
    updatePeakRss(getProcessMemorySnapshot(), peakRssMB);

    std::cout << "\nAdministrative Attribute Lookup\n";
    std::cout << "  Houses with admin areas:     " << data_.stats.housesWithAdminAreas << "\n";
    std::cout << "  House-admin links:           " << data_.stats.houseAdminAreaLinks << "\n";
    std::cout << "  Streets with admin areas:    " << data_.stats.streetsWithAdminAreas << "\n";
    std::cout << "  Street-admin links:          " << data_.stats.streetAdminAreaLinks << "\n";
    std::cout << "  POIs with admin areas:       " << data_.stats.poisWithAdminAreas << "\n";
    std::cout << "  POI-admin links:             " << data_.stats.poiAdminAreaLinks << "\n";
    std::cout << "  Admin areas with parents:    " << data_.stats.adminAreasWithParents << "\n";
    std::cout << "  Admin-parent links:          " << data_.stats.adminParentAreaLinks << "\n";
    std::cout << "  Lookup time:                 " << std::fixed << std::setprecision(3)
              << roundedSeconds(data_.stats.adminAttributionSeconds) << " s\n";

    data_.stats.housesTotal = data_.houses.size();
    data_.stats.streetsTotal = data_.streets.size();
    data_.stats.adminAreasTotal = data_.adminAreas.size();
    data_.stats.poisTotal = data_.pois.size();
    data_.stats.datasetBytes = estimateDatasetBytes();
    data_.stats.datasetMB = data_.stats.datasetBytes / (1024.0 * 1024.0);

    const double totalSeconds =
        roundedSeconds(data_.stats.relationScanSeconds) +
        roundedSeconds(data_.stats.relationAssemblySeconds) +
        roundedSeconds(data_.stats.adminAttributionSeconds) +
        roundedSeconds(addressExtractionSeconds) +
        roundedSeconds(streetExtractionSeconds) +
        roundedSeconds(data_.stats.connectionSeconds);
    data_.stats.totalSeconds = totalSeconds;
    if (data_.stats.totalSeconds > 0.0) {
        data_.stats.inputMegabytesPerSecond = data_.stats.inputFileMB / data_.stats.totalSeconds;
        data_.stats.objectsPerSecond =
            static_cast<double>(data_.stats.scannedNodes + data_.stats.scannedWays + data_.stats.scannedRelations) /
            data_.stats.totalSeconds;
    }

    const ProcessMemorySnapshot endMemory = getProcessMemorySnapshot();
    updatePeakRss(endMemory, peakRssMB);
    data_.stats.rssEndMB = endMemory.rssMB;
    data_.stats.rssPeakMB = peakRssMB;

    // Print final totals and summary
    const int totalMinutes = static_cast<int>(data_.stats.totalSeconds / 60.0);
    const int remainingSeconds = static_cast<int>(data_.stats.totalSeconds) % 60;
    std::cout << "\nTotal time taken:            " << std::fixed << std::setprecision(3)
              << data_.stats.totalSeconds << " s (" << totalMinutes << " min "
              << remainingSeconds << " s)\n";
    std::cout << "Total memory used:           " << data_.stats.rssPeakMB << " MB\n";

    // Timing breakdown explanation
    std::cout << "\nTiming breakdown:\n";
    std::cout << "  Relation scan + extraction + assembly + admin lookup + connection = total\n";
    std::cout << std::fixed << std::setprecision(3)
              << "  " << roundedSeconds(data_.stats.relationScanSeconds) << " + "
              << "(" << roundedSeconds(addressExtractionSeconds) << " + "
              << roundedSeconds(streetExtractionSeconds) << ") + "
              << roundedSeconds(data_.stats.relationAssemblySeconds) << " + "
              << roundedSeconds(data_.stats.adminAttributionSeconds) << " + "
              << roundedSeconds(data_.stats.connectionSeconds) << " = "
              << data_.stats.totalSeconds << " s\n";
}

void Parser::connectStreets() {
    connectStreetSegments(data_);
    assignAdministrativeAttributes(data_);
    data_.forwardGeocodeIndex.clear();
    data_.stats.streetsTotal = data_.streets.size();
    data_.stats.poisTotal = data_.pois.size();
    data_.stats.datasetBytes = estimateDatasetBytes();
    data_.stats.datasetMB = data_.stats.datasetBytes / (1024.0 * 1024.0);
}

void Parser::ensureForwardGeocodeIndex(bool showProgress) {
    if (data_.forwardGeocodeIndex.available) {
        return;
    }

    buildForwardGeocodeIndex(data_, data_.forwardGeocodeIndex, showProgress, &std::cout);
    data_.stats.datasetBytes = estimateDatasetBytes();
    data_.stats.datasetMB = data_.stats.datasetBytes / (1024.0 * 1024.0);
}

bool Parser::writeBinarySnapshot(const std::string& outputPath) {
    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    ensureForwardGeocodeIndex(true);

    const uint64_t estimatedBytes = static_cast<uint64_t>(
        std::max(1.0, estimateDatasetBytes()));
    ProgressReporter progress("Binary snapshot write", 0, 0, estimatedBytes, "bytes");
    const auto updateProgress = [&]() {
        const std::streampos position = out.tellp();
        if (position >= std::streampos{0}) {
            progress.update(static_cast<uint64_t>(position));
        }
    };

    out.write(kBinaryMagic, sizeof(kBinaryMagic));
    writeString(out, data_.selectedNameLanguage);
    updateProgress();

    const uint64_t stringCount = static_cast<uint64_t>(data_.strings.size());
    out.write(reinterpret_cast<const char*>(&stringCount), sizeof(stringCount));
    for (size_t i = 0; i < data_.strings.size(); ++i) {
        const std::string& value = data_.strings[i];
        writeString(out, value);
        if ((i + 1) % 4096 == 0) {
            updateProgress();
        }
    }
    updateProgress();

    writePlainVector(out, data_.houses);
    updateProgress();
    writePlainVector(out, data_.streets);
    updateProgress();
    writePlainVector(out, data_.adminAreas);
    updateProgress();
    writePlainVector(out, data_.pois);
    updateProgress();
    writePlainVector(out, data_.houseAdminAreaIndexes);
    updateProgress();
    writePlainVector(out, data_.streetAdminAreaIndexes);
    updateProgress();
    writePlainVector(out, data_.poiAdminAreaIndexes);
    updateProgress();
    writePlainVector(out, data_.adminParentAreaIndexes);
    updateProgress();
    writePlainVector(out, data_.streetGeometry);
    updateProgress();
    writePlainVector(out, data_.adminGeometry);
    updateProgress();
    writePlainVector(out, data_.adminRings);
    updateProgress();
    out.write(reinterpret_cast<const char*>(&data_.stats), sizeof(data_.stats));
    updateProgress();
    writeForwardGeocodeIndex(out, data_.forwardGeocodeIndex);
    updateProgress();
    progress.finish();

    return out.good();
}

bool Parser::loadBinarySnapshot(const std::string& inputPath) {
    std::ifstream in(inputPath, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    data_ = OSMDataset{};

    char magic[sizeof(kBinaryMagic)] = {};
    in.read(magic, sizeof(magic));
    if (!in.good() || !std::equal(std::begin(magic), std::end(magic), std::begin(kBinaryMagic))) {
        return false;
    }

    readString(in, data_.selectedNameLanguage);

    uint64_t stringCount = 0;
    in.read(reinterpret_cast<char*>(&stringCount), sizeof(stringCount));
    data_.strings.resize(static_cast<size_t>(stringCount));
    for (std::string& value : data_.strings) {
        readString(in, value);
    }

    if (!readPlainVector(in, data_.houses) ||
        !readPlainVector(in, data_.streets) ||
        !readPlainVector(in, data_.adminAreas) ||
        !readPlainVector(in, data_.pois) ||
        !readPlainVector(in, data_.houseAdminAreaIndexes) ||
        !readPlainVector(in, data_.streetAdminAreaIndexes) ||
        !readPlainVector(in, data_.poiAdminAreaIndexes) ||
        !readPlainVector(in, data_.adminParentAreaIndexes) ||
        !readPlainVector(in, data_.streetGeometry) ||
        !readPlainVector(in, data_.adminGeometry) ||
        !readPlainVector(in, data_.adminRings)) {
        return false;
    }

    in.read(reinterpret_cast<char*>(&data_.stats), sizeof(data_.stats));
    if (!in.good()) {
        return false;
    }

    if (!readRequiredForwardGeocodeIndex(in, data_.forwardGeocodeIndex) ||
        !data_.forwardGeocodeIndex.available) {
        return false;
    }

    data_.stats.datasetBytes = estimateDatasetBytes();
    data_.stats.datasetMB = data_.stats.datasetBytes / (1024.0 * 1024.0);
    return true;
}

bool Parser::exportGeoJson(const std::string& outputPath,
                                size_t maxHouses,
                                size_t maxStreets,
                                size_t maxAdminAreas) const {
    std::ofstream out(outputPath, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    out << "{\n  \"type\": \"FeatureCollection\",\n  \"features\": [";
    bool firstFeature = true;

    const auto writeSeparator = [&]() {
        if (!firstFeature) {
            out << ",";
        }
        out << "\n    ";
        firstFeature = false;
    };

    const size_t houseCount = std::min(maxHouses, data_.houses.size());
    for (size_t i = 0; i < houseCount; ++i) {
        const HouseRecord& house = data_.houses[i];
        writeSeparator();
        out << "{\"type\":\"Feature\",\"properties\":{";
        out << "\"feature\":\"house\",";
        out << "\"id\":" << house.osmId << ",";
        out << "\"street\":\"" << jsonEscape(data_.resolve(house.streetName)) << "\",";
        out << "\"number\":\"" << jsonEscape(data_.resolve(house.houseNumber)) << "\",";
        out << "\"source\":\"" << featureSourceToString(house.source) << "\"},";
        out << "\"geometry\":{\"type\":\"Point\",\"coordinates\":["
            << std::setprecision(10) << longitudeOf(house.point) << "," << latitudeOf(house.point)
            << "]}}";
    }

    const size_t streetCount = std::min(maxStreets, data_.streets.size());
    for (size_t i = 0; i < streetCount; ++i) {
        const StreetRecord& street = data_.streets[i];
        if (street.geometrySize < 2) {
            continue;
        }

        writeSeparator();
        out << "{\"type\":\"Feature\",\"properties\":{";
        out << "\"feature\":\"street\",";
        out << "\"id\":" << street.osmId << ",";
        out << "\"name\":\"" << jsonEscape(data_.resolve(street.name)) << "\",";
        out << "\"highway\":\"" << jsonEscape(data_.resolve(street.highwayType)) << "\"},";
        out << "\"geometry\":{\"type\":\"LineString\",\"coordinates\":[";

        for (uint32_t j = 0; j < street.geometrySize; ++j) {
            if (j > 0) {
                out << ",";
            }
            const Coordinate& point = data_.streetGeometry[street.geometryOffset + j];
            out << "[" << std::setprecision(10) << longitudeOf(point) << "," << latitudeOf(point) << "]";
        }

        out << "]}}";
    }

    const size_t adminCount = std::min(maxAdminAreas, data_.adminAreas.size());
    for (size_t i = 0; i < adminCount; ++i) {
        const AdminAreaRecord& area = data_.adminAreas[i];
        if (area.geometrySize < 4) {
            continue;
        }

        writeSeparator();
        out << "{\"type\":\"Feature\",\"properties\":{";
        out << "\"feature\":\"admin_area\",";
        out << "\"id\":" << area.osmId << ",";
        out << "\"name\":\"" << jsonEscape(data_.resolve(area.name)) << "\",";
        out << "\"admin_level\":" << static_cast<int>(area.adminLevel) << ",";
        out << "\"source\":\"" << featureSourceToString(area.source) << "\"},";
        out << "\"geometry\":{\"type\":\"Polygon\",\"coordinates\":[";

        const auto writeRing = [&](uint32_t offset, uint32_t size) {
            out << "[";
            const uint32_t available = offset < data_.adminGeometry.size()
                ? std::min<uint32_t>(size,
                      checkedU32(data_.adminGeometry.size() - offset, "admin geometry available size"))
                : 0;
            for (uint32_t j = 0; j < available; ++j) {
                if (j > 0) {
                    out << ",";
                }
                const Coordinate& point = data_.adminGeometry[offset + j];
                out << "[" << std::setprecision(10) << longitudeOf(point) << "," << latitudeOf(point) << "]";
            }
            out << "]";
        };

        writeRing(area.geometryOffset, area.geometrySize);
        const uint32_t ringCount = area.ringOffset < data_.adminRings.size()
            ? std::min<uint32_t>(area.ringSize,
                  checkedU32(data_.adminRings.size() - area.ringOffset, "admin ring available size"))
            : 0;
        for (uint32_t j = 0; j < ringCount; ++j) {
            const AdminRingRecord& ring = data_.adminRings[area.ringOffset + j];
            if (ring.adminAreaIndex != i || ring.role != 1) {
                continue;
            }
            out << ",";
            writeRing(ring.geometryOffset, ring.geometrySize);
        }

        out << "]}}";
    }

    out << "\n  ]\n}\n";
    return true;
}

double Parser::estimateDatasetBytes() const {
    uint64_t bytes = 0;
    bytes += static_cast<uint64_t>(sizeof(OSMDataset));
    bytes += static_cast<uint64_t>(data_.strings.capacity()) * sizeof(std::string);
    for (const std::string& value : data_.strings) {
        bytes += static_cast<uint64_t>(value.capacity());
    }
    bytes += static_cast<uint64_t>(data_.houses.capacity()) * sizeof(HouseRecord);
    bytes += static_cast<uint64_t>(data_.streets.capacity()) * sizeof(StreetRecord);
    bytes += static_cast<uint64_t>(data_.adminAreas.capacity()) * sizeof(AdminAreaRecord);
    bytes += static_cast<uint64_t>(data_.pois.capacity()) * sizeof(PoiRecord);
    bytes += static_cast<uint64_t>(data_.houseAdminAreaIndexes.capacity()) * sizeof(uint32_t);
    bytes += static_cast<uint64_t>(data_.streetAdminAreaIndexes.capacity()) * sizeof(uint32_t);
    bytes += static_cast<uint64_t>(data_.poiAdminAreaIndexes.capacity()) * sizeof(uint32_t);
    bytes += static_cast<uint64_t>(data_.adminParentAreaIndexes.capacity()) * sizeof(uint32_t);
    bytes += static_cast<uint64_t>(data_.streetGeometry.capacity()) * sizeof(Coordinate);
    bytes += static_cast<uint64_t>(data_.adminGeometry.capacity()) * sizeof(Coordinate);
    bytes += static_cast<uint64_t>(data_.adminRings.capacity()) * sizeof(AdminRingRecord);
    const auto addPostingBytes = [](const std::vector<GeocodePostingList>& lists) {
        uint64_t localBytes = static_cast<uint64_t>(lists.capacity()) * sizeof(GeocodePostingList);
        for (const GeocodePostingList& list : lists) {
            localBytes += static_cast<uint64_t>(list.token.capacity());
            localBytes += static_cast<uint64_t>(list.refs.capacity()) * sizeof(GeocodeRef);
        }
        return localBytes;
    };
    bytes += addPostingBytes(data_.forwardGeocodeIndex.context);
    bytes += addPostingBytes(data_.forwardGeocodeIndex.primary);
    bytes += static_cast<uint64_t>(data_.forwardGeocodeIndex.vocabulary.capacity()) * sizeof(std::string);
    for (const std::string& token : data_.forwardGeocodeIndex.vocabulary) {
        bytes += static_cast<uint64_t>(token.capacity());
    }
    bytes += static_cast<uint64_t>(data_.forwardGeocodeIndex.suffixArray.capacity()) * sizeof(GeocodeSuffixEntry);
    return static_cast<double>(bytes);
}

void Parser::printStats() const {
    const double totalMinutes = data_.stats.totalSeconds / 60.0;

    std::cout << "\n===== OSM Extraction Summary =====\n";
    std::cout << "Selected language:             " << data_.selectedNameLanguage << "\n";

    std::cout << "\nScanned OSM primitives\n";
    std::cout << "  Nodes:                       " << data_.stats.scannedNodes << "\n";
    std::cout << "  Ways:                        " << data_.stats.scannedWays << "\n";
    std::cout << "  Relations:                   " << data_.stats.scannedRelations << "\n";

    std::cout << "\nExtracted dataset\n";
    std::cout << "  Houses total:                " << data_.stats.housesTotal << "\n";
    std::cout << "  Houses from nodes:           " << data_.stats.housesFromNodes << "\n";
    std::cout << "  Houses from ways:            " << data_.stats.housesFromWays << "\n";
    std::cout << "  Houses from relations:       " << data_.stats.housesFromRelations << "\n";
    std::cout << "  Houses missing street:       " << data_.stats.housesMissingStreet << "\n";
    std::cout << "  Houses missing number:       " << data_.stats.housesMissingHouseNumber << "\n";
    std::cout << "  Houses with admin areas:     " << data_.stats.housesWithAdminAreas << "\n";
    std::cout << "  House-admin links:           " << data_.stats.houseAdminAreaLinks << "\n";
    std::cout << "  Streets with admin areas:    " << data_.stats.streetsWithAdminAreas << "\n";
    std::cout << "  Street-admin links:          " << data_.stats.streetAdminAreaLinks << "\n";
    std::cout << "  POIs total:                  " << data_.stats.poisTotal << "\n";
    std::cout << "  POIs from nodes:             " << data_.stats.poisFromNodes << "\n";
    std::cout << "  POIs from ways:              " << data_.stats.poisFromWays << "\n";
    std::cout << "  POIs with admin areas:       " << data_.stats.poisWithAdminAreas << "\n";
    std::cout << "  POI-admin links:             " << data_.stats.poiAdminAreaLinks << "\n";
    std::cout << "  Admin areas with parents:    " << data_.stats.adminAreasWithParents << "\n";
    std::cout << "  Admin-parent links:          " << data_.stats.adminParentAreaLinks << "\n";
    std::cout << "  Street ways stored:          " << data_.stats.streetsTotal << "\n";
    std::cout << "  Admin areas from ways:       " << data_.stats.adminAreasFromWays << "\n";
    std::cout << "  Admin areas from relations:  " << data_.stats.adminAreasFromRelations << "\n";
    std::cout << "  Admin areas total:           " << data_.stats.adminAreasTotal << "\n";

    std::cout << "\nFinal timings\n";
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "  Boundary relation scan:      " << std::setw(11)
              << data_.stats.relationScanSeconds << " s\n";
    std::cout << "  Dataset extraction:          " << data_.stats.extractionSeconds << " s\n";
    std::cout << "  Boundary assembly:           " << data_.stats.relationAssemblySeconds << " s\n";
    std::cout << "  Admin attribute lookup:      " << data_.stats.adminAttributionSeconds << " s\n";
    std::cout << "  Total time:                  " << data_.stats.totalSeconds
              << " s (" << totalMinutes << " min)\n";

    std::cout << "\nPerformance\n";
    std::cout << std::setprecision(2);
    std::cout << "  Input size:                  " << data_.stats.inputFileMB << " MB\n";
    std::cout << "  Input throughput:            " << data_.stats.inputMegabytesPerSecond << " MB/s\n";

    std::cout << "\nMemory\n";
    std::cout << "  Compact dataset size:        " << data_.stats.datasetMB << " MB\n";
    std::cout << "  Peak RSS:                    " << data_.stats.rssPeakMB << " MB\n";
    std::cout << "===============================\n";
}
