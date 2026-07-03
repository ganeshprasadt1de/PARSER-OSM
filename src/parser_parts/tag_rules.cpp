std::string trimCopy(const std::string& input) {
    const size_t first = input.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) {
        return "";
    }
    const size_t last = input.find_last_not_of(" \t\n\r");
    return input.substr(first, last - first + 1);
}

std::string getTagValue(const osmium::TagList& tags, const char* key) {
    const char* value = tags.get_value_by_key(key);
    return value == nullptr ? "" : trimCopy(value);
}

std::string getLocalizedTag(const osmium::TagList& tags,
                            const std::string& baseKey,
                            const std::string& language) {
    if (!language.empty()) {
        const std::string localizedKey = baseKey + ":" + language;
        const char* localized = tags.get_value_by_key(localizedKey.c_str());
        if (localized != nullptr && *localized != '\0') {
            return trimCopy(localized);
        }
    }

    const char* fallback = tags.get_value_by_key(baseKey.c_str());
    return fallback == nullptr ? "" : trimCopy(fallback);
}

std::string getStreetDisplayName(const osmium::TagList& tags, const std::string& language) {
    std::string name = getLocalizedTag(tags, "name", language);
    if (!name.empty()) {
        return name;
    }

    name = getLocalizedTag(tags, "official_name", language);
    if (!name.empty()) {
        return name;
    }

    // Many larger roads are identified by ref instead of name, for example A 5,
    // B 35, or L 558. The parser uses that as the display and merge identity so these
    // highways do not disappear from the reduced street layer.
    return getTagValue(tags, "ref");
}

bool parseAdminLevel(const std::string& value, uint8_t& adminLevelOut) {
    try {
        const int level = std::stoi(value);
        if (level < 2 || level > 12) {
            return false;
        }
        adminLevelOut = static_cast<uint8_t>(level);
        return true;
    } catch (...) {
        return false;
    }
}

bool isAdministrativeBoundary(const osmium::TagList& tags, uint8_t& adminLevelOut) {
    if (getTagValue(tags, "boundary") != "administrative") {
        return false;
    }
    return parseAdminLevel(getTagValue(tags, "admin_level"), adminLevelOut);
}

bool hasHouseAddress(const osmium::TagList& tags) {
    return !getTagValue(tags, "addr:housenumber").empty() ||
           !getTagValue(tags, "addr:street").empty();
}

bool isHouseNode(const osmium::TagList& tags) {
    return hasHouseAddress(tags);
}

bool isHouseWay(const osmium::TagList& tags) {
    return hasHouseAddress(tags) && !getTagValue(tags, "building").empty();
}

bool isHouseRelation(const osmium::TagList& tags) {
    return hasHouseAddress(tags) && !getTagValue(tags, "building").empty();
}

std::string poiCategoryFromTags(const osmium::TagList& tags, std::string& tagKeyOut, std::string& tagValueOut) {
    const std::string amenity = getTagValue(tags, "amenity");
    if (amenity == "fast_food" || amenity == "restaurant" || amenity == "cafe" ||
        amenity == "bar" || amenity == "pub" || amenity == "fuel" ||
        amenity == "hospital" || amenity == "school" || amenity == "university" ||
        amenity == "pharmacy" || amenity == "bank" || amenity == "parking") {
        tagKeyOut = "amenity";
        tagValueOut = amenity;
        return amenity;
    }

    const std::string shop = getTagValue(tags, "shop");
    if (!shop.empty() && shop != "no") {
        tagKeyOut = "shop";
        tagValueOut = shop;
        return "shop";
    }

    const std::string tourism = getTagValue(tags, "tourism");
    if (tourism == "hotel" || tourism == "museum" || tourism == "attraction") {
        tagKeyOut = "tourism";
        tagValueOut = tourism;
        return tourism;
    }

    const std::string leisure = getTagValue(tags, "leisure");
    if (leisure == "park" || leisure == "garden" || leisure == "playground" ||
        leisure == "sports_centre" || leisure == "swimming_pool") {
        tagKeyOut = "leisure";
        tagValueOut = leisure;
        return leisure == "garden" ? "park" : leisure;
    }

    const std::string landuse = getTagValue(tags, "landuse");
    if (landuse == "recreation_ground" || landuse == "village_green") {
        tagKeyOut = "landuse";
        tagValueOut = landuse;
        return "park";
    }

    return "";
}

bool isPoiObject(const osmium::TagList& tags) {
    std::string tagKey;
    std::string tagValue;
    return !poiCategoryFromTags(tags, tagKey, tagValue).empty();
}

std::string getPoiDisplayName(const osmium::TagList& tags, const std::string& language) {
    std::string name = getLocalizedTag(tags, "name", language);
    if (!name.empty()) {
        return name;
    }

    name = getLocalizedTag(tags, "brand", language);
    if (!name.empty()) {
        return name;
    }

    name = getLocalizedTag(tags, "operator", language);
    if (!name.empty()) {
        return name;
    }

    return "";
}

bool isBlockedStreetHighwayType(const std::string& highway) {
    static const std::vector<std::string> blocked = {
        "construction", "proposed", "path", "footway", "cycleway", "bridleway", "steps",
        "pedestrian", "platform", "corridor"
    };

    return std::find(blocked.begin(), blocked.end(), highway) != blocked.end();
}

bool hasStreetLikeHighway(const osmium::TagList& tags, std::string& highwayOut) {
    highwayOut = getTagValue(tags, "highway");
    if (highwayOut.empty()) {
        return false;
    }

    const std::string area = getTagValue(tags, "area");
    if (area == "yes" || area == "1" || area == "true") {
        return false;
    }

    return !isBlockedStreetHighwayType(highwayOut);
}

bool isUnnamedStreetRecoveryHighwayType(const std::string& highway) {
    static const std::vector<std::string> allowed = {
        "motorway_link", "trunk_link", "primary_link", "secondary_link", "tertiary_link",
        "unclassified", "residential", "living_street", "road"
    };

    return std::find(allowed.begin(), allowed.end(), highway) != allowed.end();
}

bool isNamedStreetWay(const osmium::TagList& tags) {
    std::string highway;
    if (!hasStreetLikeHighway(tags, highway)) {
        return false;
    }

    return !getStreetDisplayName(tags, "").empty();
}

bool isUnnamedStreetRecoveryCandidate(const osmium::TagList& tags) {
    std::string highway;
    if (!hasStreetLikeHighway(tags, highway)) {
        return false;
    }

    if (!getStreetDisplayName(tags, "").empty()) {
        return false;
    }

    if (getTagValue(tags, "junction") == "roundabout") {
        return true;
    }

    return isUnnamedStreetRecoveryHighwayType(highway);
}

bool isStreetWay(const osmium::TagList& tags) {
    return isNamedStreetWay(tags);
}

bool containsSortedId(const std::vector<uint64_t>& ids, uint64_t id) {
    return std::binary_search(ids.begin(), ids.end(), id);
}

bool endpointNodeRefs(const osmium::Way& way, uint64_t& firstOut, uint64_t& lastOut) {
    if (way.nodes().size() < 2) {
        return false;
    }

    const auto first = way.nodes().front().ref();
    const auto last = way.nodes().back().ref();
    if (first <= 0 || last <= 0) {
        return false;
    }

    firstOut = static_cast<uint64_t>(first);
    lastOut = static_cast<uint64_t>(last);
    return true;
}
