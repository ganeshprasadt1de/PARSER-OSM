#include "geocode_index.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <iostream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr size_t kMinSubstringTokenLength = 3;
constexpr size_t kMaxFuzzyVocabularyScan = 250000;

uint32_t checkedU32(size_t value, const char* field) {
    if (value > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        throw std::runtime_error(std::string(field) + " exceeds uint32_t range");
    }
    return static_cast<uint32_t>(value);
}

bool stdoutIsTerminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

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

class ConsoleProgress {
public:
    ConsoleProgress(std::string label, uint64_t total, std::string unit, bool enabled)
        : label_(std::move(label)),
          total_(total),
          unit_(std::move(unit)),
          enabled_(enabled),
          interactive_(stdoutIsTerminal()),
          start_(std::chrono::steady_clock::now()),
          lastPrint_(start_) {
        update(0, true);
    }

    void update(uint64_t completed) {
        update(completed, false);
    }

    void finish(uint64_t completed = std::numeric_limits<uint64_t>::max()) {
        if (!enabled_) {
            return;
        }
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
        if (!enabled_) {
            return;
        }
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
    }

    std::string label_;
    uint64_t total_ = 0;
    std::string unit_;
    bool enabled_ = false;
    bool interactive_ = false;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point lastPrint_;
    uint64_t lastCompleted_ = std::numeric_limits<uint64_t>::max();
    size_t lastLength_ = 0;
    bool lineOpen_ = false;
};

bool appendUtf8Replacement(const std::string& input, size_t& index, std::string& output) {
    const unsigned char current = static_cast<unsigned char>(input[index]);
    if (index + 1 < input.size()) {
        const unsigned char next = static_cast<unsigned char>(input[index + 1]);
        if (current == 0xC3) {
            switch (next) {
                case 0x84: case 0xA4: output += "ae"; index += 2; return true;
                case 0x96: case 0xB6: output += "oe"; index += 2; return true;
                case 0x9C: case 0xBC: output += "ue"; index += 2; return true;
                case 0x9F: output += "ss"; index += 2; return true;
                default: break;
            }
        }
    }
    if (index + 1 < input.size() && current == 0xCC) {
        const unsigned char next = static_cast<unsigned char>(input[index + 1]);
        if (next >= 0x80 && next <= 0xBF) {
            index += 2;
            return true;
        }
    }
    return false;
}

void appendTokensFromText(std::vector<std::string>& tokens, const std::string& text) {
    std::vector<std::string> next = tokenizeSearchText(text);
    tokens.insert(tokens.end(), next.begin(), next.end());
}

void appendAdminTokens(std::vector<std::string>& tokens,
                       const OSMDataset& data,
                       const std::vector<uint32_t>& links,
                       uint32_t offset,
                       uint32_t size,
                       uint32_t maxNames = std::numeric_limits<uint32_t>::max()) {
    const uint32_t start = size > maxNames ? size - maxNames : 0;
    for (uint32_t i = start; i < size; ++i) {
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

void appendSearchContextAdminTokens(std::vector<std::string>& tokens,
                                    const OSMDataset& data,
                                    const std::vector<uint32_t>& links,
                                    uint32_t offset,
                                    uint32_t size) {
    constexpr uint32_t kMaxIndexedAdminNames = 3;
    appendAdminTokens(tokens, data, links, offset, size, kMaxIndexedAdminNames);
}

void addTokensToIndex(std::unordered_map<std::string, std::vector<GeocodeRef>>& target,
                      const GeocodeRef& ref,
                      std::vector<std::string> tokens) {
    uniqueTokens(tokens);
    for (const std::string& token : tokens) {
        target[token].push_back(ref);
    }
}

void finalizePostingMap(std::unordered_map<std::string, std::vector<GeocodeRef>>& source,
                        std::vector<GeocodePostingList>& target,
                        uint64_t& postingRefs,
                        uint64_t& largestPostingList) {
    target.clear();
    target.reserve(source.size());
    postingRefs = 0;
    largestPostingList = 0;

    for (auto& entry : source) {
        GeocodePostingList list;
        list.token = std::move(entry.first);
        list.refs = std::move(entry.second);
        std::sort(list.refs.begin(), list.refs.end(), geocodeRefLess);
        list.refs.erase(std::unique(list.refs.begin(), list.refs.end(), [](const GeocodeRef& left, const GeocodeRef& right) {
            return left.type == right.type && left.index == right.index;
        }), list.refs.end());
        list.refs.shrink_to_fit();
        postingRefs += list.refs.size();
        largestPostingList = std::max<uint64_t>(largestPostingList, list.refs.size());
        target.push_back(std::move(list));
    }

    std::sort(target.begin(), target.end(), [](const GeocodePostingList& left, const GeocodePostingList& right) {
        return left.token < right.token;
    });
    std::unordered_map<std::string, std::vector<GeocodeRef>>().swap(source);
}

void mergeUniqueTokens(const std::vector<GeocodePostingList>& left,
                       const std::vector<GeocodePostingList>& right,
                       std::vector<std::string>& vocabulary) {
    vocabulary.clear();
    vocabulary.reserve(left.size() + right.size());
    size_t leftIndex = 0;
    size_t rightIndex = 0;
    while (leftIndex < left.size() || rightIndex < right.size()) {
        const std::string* token = nullptr;
        if (rightIndex >= right.size() ||
            (leftIndex < left.size() && left[leftIndex].token <= right[rightIndex].token)) {
            token = &left[leftIndex].token;
            ++leftIndex;
        } else {
            token = &right[rightIndex].token;
            ++rightIndex;
        }
        if (vocabulary.empty() || vocabulary.back() != *token) {
            vocabulary.push_back(*token);
        }
        while (leftIndex < left.size() && left[leftIndex].token == *token) {
            ++leftIndex;
        }
        while (rightIndex < right.size() && right[rightIndex].token == *token) {
            ++rightIndex;
        }
    }
}

void buildVocabularyAndSuffixArray(ForwardGeocodeIndex& index, bool showProgress) {
    mergeUniqueTokens(index.context, index.primary, index.vocabulary);

    uint64_t suffixCount = 0;
    for (const std::string& token : index.vocabulary) {
        if (token.size() >= kMinSubstringTokenLength) {
            suffixCount += token.size();
        }
    }

    index.suffixArray.clear();
    index.suffixArray.reserve(static_cast<size_t>(suffixCount));
    ConsoleProgress progress("Forward substring suffix build", suffixCount, "suffixes", showProgress);
    uint64_t processed = 0;
    for (size_t tokenIndex = 0; tokenIndex < index.vocabulary.size(); ++tokenIndex) {
        const std::string& token = index.vocabulary[tokenIndex];
        if (token.size() < kMinSubstringTokenLength) {
            continue;
        }
        for (size_t offset = 0; offset < token.size(); ++offset) {
            if (offset <= std::numeric_limits<uint16_t>::max()) {
                index.suffixArray.push_back(GeocodeSuffixEntry{
                    checkedU32(tokenIndex, "geocode suffix token index"),
                    static_cast<uint16_t>(offset)
                });
            }
            ++processed;
            if (processed % 65536 == 0 || processed == suffixCount) {
                progress.update(processed);
            }
        }
    }

    std::sort(index.suffixArray.begin(), index.suffixArray.end(), [&](const GeocodeSuffixEntry& left,
                                                                      const GeocodeSuffixEntry& right) {
        const std::string& leftToken = index.vocabulary[left.tokenIndex];
        const std::string& rightToken = index.vocabulary[right.tokenIndex];
        return leftToken.compare(left.offset, std::string::npos, rightToken, right.offset, std::string::npos) < 0;
    });
    progress.finish(processed);
}

uint64_t estimateIndexBytes(const ForwardGeocodeIndex& index) {
    uint64_t bytes = sizeof(ForwardGeocodeIndex);
    const auto addPostings = [&](const std::vector<GeocodePostingList>& lists) {
        uint64_t local = static_cast<uint64_t>(lists.capacity()) * sizeof(GeocodePostingList);
        for (const GeocodePostingList& list : lists) {
            local += static_cast<uint64_t>(list.token.capacity());
            local += static_cast<uint64_t>(list.refs.capacity()) * sizeof(GeocodeRef);
        }
        return local;
    };
    bytes += addPostings(index.context);
    bytes += addPostings(index.primary);
    bytes += static_cast<uint64_t>(index.vocabulary.capacity()) * sizeof(std::string);
    for (const std::string& token : index.vocabulary) {
        bytes += static_cast<uint64_t>(token.capacity());
    }
    bytes += static_cast<uint64_t>(index.suffixArray.capacity()) * sizeof(GeocodeSuffixEntry);
    return bytes;
}

int boundedEditDistance(const std::string& left, const std::string& right, int maxDistance) {
    if (std::abs(static_cast<int>(left.size()) - static_cast<int>(right.size())) > maxDistance) {
        return maxDistance + 1;
    }

    std::vector<int> previous(right.size() + 1);
    std::vector<int> current(right.size() + 1);
    for (size_t j = 0; j <= right.size(); ++j) {
        previous[j] = static_cast<int>(j);
    }

    for (size_t i = 1; i <= left.size(); ++i) {
        current[0] = static_cast<int>(i);
        int rowBest = current[0];
        for (size_t j = 1; j <= right.size(); ++j) {
            const int cost = left[i - 1] == right[j - 1] ? 0 : 1;
            current[j] = std::min({
                previous[j] + 1,
                current[j - 1] + 1,
                previous[j - 1] + cost
            });
            rowBest = std::min(rowBest, current[j]);
        }
        if (rowBest > maxDistance) {
            return maxDistance + 1;
        }
        std::swap(previous, current);
    }
    return previous[right.size()];
}

}  // namespace

std::string normalizeSearchText(const std::string& input) {
    std::string normalized;
    normalized.reserve(input.size());
    bool lastWasSpace = true;
    for (size_t i = 0; i < input.size();) {
        if (appendUtf8Replacement(input, i, normalized)) {
            lastWasSpace = false;
            continue;
        }

        const unsigned char c = static_cast<unsigned char>(input[i]);
        if (c < 128 && std::isalnum(c)) {
            normalized.push_back(static_cast<char>(std::tolower(c)));
            lastWasSpace = false;
        } else if (!lastWasSpace) {
            normalized.push_back(' ');
            lastWasSpace = true;
        }
        ++i;
    }

    while (!normalized.empty() && normalized.back() == ' ') {
        normalized.pop_back();
    }
    return normalized;
}

std::vector<std::string> tokenizeSearchText(const std::string& input) {
    std::istringstream stream(normalizeSearchText(input));
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token) {
        if (token == "str" || token == "strasse" || token == "strasze") {
            token = "strasse";
        }
        tokens.push_back(token);
    }
    uniqueTokens(tokens);
    return tokens;
}

void uniqueTokens(std::vector<std::string>& tokens) {
    tokens.erase(std::remove_if(tokens.begin(), tokens.end(), [](const std::string& token) {
        return token.empty();
    }), tokens.end());
    std::sort(tokens.begin(), tokens.end());
    tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
}

uint64_t geocodeKey(uint8_t type, uint32_t index) {
    return (static_cast<uint64_t>(type) << 32) | index;
}

bool geocodeRefLess(const GeocodeRef& left, const GeocodeRef& right) {
    return geocodeKey(left.type, left.index) < geocodeKey(right.type, right.index);
}

void buildForwardGeocodeIndex(const OSMDataset& data,
                              ForwardGeocodeIndex& index,
                              bool showProgress,
                              std::ostream* logStream) {
    index.clear();

    std::unordered_map<std::string, std::vector<GeocodeRef>> context;
    std::unordered_map<std::string, std::vector<GeocodeRef>> primary;
    context.reserve(std::max<size_t>(1024, data.streets.size() / 2 + data.adminAreas.size() * 2));
    primary.reserve(std::max<size_t>(1024, data.streets.size() / 2 + data.adminAreas.size() * 2));

    const uint64_t totalRecords = static_cast<uint64_t>(data.houses.size() + data.streets.size() +
                                                        data.adminAreas.size() + data.pois.size());
    ConsoleProgress progress("Forward geocode index build", totalRecords, "records", showProgress);
    uint64_t processed = 0;
    const auto updateProgress = [&]() {
        if (processed % 65536 == 0 || processed == totalRecords) {
            progress.update(processed);
        }
    };

    const auto addEntry = [&](uint8_t type,
                              uint32_t recordIndex,
                              std::vector<std::string> allTokens,
                              std::vector<std::string> primaryTokens) {
        const GeocodeRef ref{type, recordIndex};
        addTokensToIndex(context, ref, std::move(allTokens));
        addTokensToIndex(primary, ref, std::move(primaryTokens));
    };

    for (size_t i = 0; i < data.houses.size(); ++i) {
        const HouseRecord& house = data.houses[i];
        if (house.streetName != kEmptyStringRef || house.houseNumber != kEmptyStringRef) {
            std::vector<std::string> tokens;
            appendTokensFromText(tokens, data.resolve(house.streetName));
            appendTokensFromText(tokens, data.resolve(house.houseNumber));
            appendTokensFromText(tokens, data.resolve(house.postcode));
            appendTokensFromText(tokens, data.resolve(house.city));
            appendTokensFromText(tokens, data.resolve(house.country));
            appendSearchContextAdminTokens(tokens, data, data.houseAdminAreaIndexes, house.adminAreaOffset, house.adminAreaSize);

            std::vector<std::string> primaryTokens;
            appendTokensFromText(primaryTokens, data.resolve(house.streetName));
            appendTokensFromText(primaryTokens, data.resolve(house.houseNumber));
            appendTokensFromText(primaryTokens, data.resolve(house.postcode));
            addEntry(kGeocodeHouse, checkedU32(i, "house geocode record index"), std::move(tokens), std::move(primaryTokens));
        }
        ++processed;
        updateProgress();
    }

    for (size_t i = 0; i < data.streets.size(); ++i) {
        const StreetRecord& street = data.streets[i];
        if (street.name != kEmptyStringRef) {
            std::vector<std::string> tokens;
            appendTokensFromText(tokens, data.resolve(street.name));
            appendSearchContextAdminTokens(tokens, data, data.streetAdminAreaIndexes, street.adminAreaOffset, street.adminAreaSize);
            std::vector<std::string> primaryTokens;
            appendTokensFromText(primaryTokens, data.resolve(street.name));
            addEntry(kGeocodeStreet, checkedU32(i, "street geocode record index"), std::move(tokens), std::move(primaryTokens));
        }
        ++processed;
        updateProgress();
    }

    for (size_t i = 0; i < data.pois.size(); ++i) {
        const PoiRecord& poi = data.pois[i];
        std::vector<std::string> tokens;
        appendTokensFromText(tokens, data.resolve(poi.name));
        appendTokensFromText(tokens, data.resolve(poi.brand));
        appendTokensFromText(tokens, data.resolve(poi.category));
        appendTokensFromText(tokens, data.resolve(poi.tagValue));
        appendSearchContextAdminTokens(tokens, data, data.poiAdminAreaIndexes, poi.adminAreaOffset, poi.adminAreaSize);

        std::vector<std::string> primaryTokens;
        appendTokensFromText(primaryTokens, data.resolve(poi.name));
        appendTokensFromText(primaryTokens, data.resolve(poi.brand));
        appendTokensFromText(primaryTokens, data.resolve(poi.category));
        appendTokensFromText(primaryTokens, data.resolve(poi.tagValue));
        addEntry(kGeocodePoi, checkedU32(i, "poi geocode record index"), std::move(tokens), std::move(primaryTokens));

        ++processed;
        updateProgress();
    }

    for (size_t i = 0; i < data.adminAreas.size(); ++i) {
        const AdminAreaRecord& area = data.adminAreas[i];
        if (area.name != kEmptyStringRef) {
            std::vector<std::string> tokens;
            appendTokensFromText(tokens, data.resolve(area.name));
            appendSearchContextAdminTokens(tokens, data, data.adminParentAreaIndexes, area.parentAreaOffset, area.parentAreaSize);
            std::vector<std::string> primaryTokens;
            appendTokensFromText(primaryTokens, data.resolve(area.name));
            addEntry(kGeocodeAdmin, checkedU32(i, "admin geocode record index"), std::move(tokens), std::move(primaryTokens));
        }
        ++processed;
        updateProgress();
    }
    progress.finish(processed);

    finalizePostingMap(context, index.context, index.postingRefs, index.largestPostingList);
    finalizePostingMap(primary, index.primary, index.primaryPostingRefs, index.largestPrimaryPostingList);
    buildVocabularyAndSuffixArray(index, showProgress);

    index.estimatedBytes = estimateIndexBytes(index);
    index.available = true;

    if (logStream) {
        *logStream << "Forward geocode index built\n";
        *logStream << "  Context terms:               " << index.context.size() << "\n";
        *logStream << "  Context postings:            " << index.postingRefs << "\n";
        *logStream << "  Primary terms:               " << index.primary.size() << "\n";
        *logStream << "  Primary postings:            " << index.primaryPostingRefs << "\n";
        *logStream << "  Substring suffixes:          " << index.suffixArray.size() << "\n";
        *logStream << "  Estimated index RAM:         "
                   << (static_cast<double>(index.estimatedBytes) / (1024.0 * 1024.0)) << " MB\n";
    }
}

std::vector<std::string> substringTokenExpansions(const ForwardGeocodeIndex& index,
                                                  const std::string& token,
                                                  size_t maxExpansions) {
    std::vector<std::string> result;
    if (!index.available || token.size() < kMinSubstringTokenLength || maxExpansions == 0) {
        return result;
    }

    std::unordered_set<uint32_t> seen;
    const auto lower = std::lower_bound(index.suffixArray.begin(), index.suffixArray.end(), token,
        [&](const GeocodeSuffixEntry& entry, const std::string& value) {
            if (entry.tokenIndex >= index.vocabulary.size()) {
                return true;
            }
            const std::string& candidate = index.vocabulary[entry.tokenIndex];
            return candidate.compare(entry.offset, std::string::npos, value) < 0;
        });

    for (auto it = lower; it != index.suffixArray.end(); ++it) {
        const GeocodeSuffixEntry& entry = *it;
        if (entry.tokenIndex >= index.vocabulary.size()) {
            continue;
        }
        const std::string& candidate = index.vocabulary[entry.tokenIndex];
        if (entry.offset + token.size() > candidate.size()) {
            break;
        }
        const int comparison = candidate.compare(entry.offset, token.size(), token);
        if (comparison != 0) {
            break;
        }
        if (seen.insert(entry.tokenIndex).second) {
            result.push_back(candidate);
            if (result.size() >= maxExpansions) {
                break;
            }
        }
    }
    return result;
}

std::vector<std::string> fuzzyTokenExpansions(const ForwardGeocodeIndex& index,
                                              const std::string& token,
                                              size_t maxExpansions) {
    std::vector<std::string> result;
    if (!index.available || token.size() < 4 || maxExpansions == 0 ||
        index.vocabulary.size() > kMaxFuzzyVocabularyScan) {
        return result;
    }

    const int maxDistance = token.size() <= 5 ? 1 : 2;
    std::vector<std::pair<int, std::string>> candidates;
    for (const std::string& candidate : index.vocabulary) {
        if (candidate.empty() || std::abs(static_cast<int>(candidate.size()) - static_cast<int>(token.size())) > maxDistance) {
            continue;
        }
        const int distance = boundedEditDistance(token, candidate, maxDistance);
        if (distance <= maxDistance) {
            candidates.emplace_back(distance, candidate);
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        if (left.first != right.first) {
            return left.first < right.first;
        }
        return left.second < right.second;
    });

    for (const auto& candidate : candidates) {
        result.push_back(candidate.second);
        if (result.size() >= maxExpansions) {
            break;
        }
    }
    return result;
}
