std::string jsonEscape(const std::string& input) {
    std::ostringstream out;
    for (char c : input) {
        switch (c) {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                out << c;
                break;
        }
    }
    return out.str();
}

std::string fallbackName(const std::string& prefix, uint64_t osmId) {
    std::ostringstream stream;
    stream << prefix << osmId;
    return stream.str();
}

double bytesToMB(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

uint64_t tryGetFileSizeBytes(const std::string& path) {
    try {
        return std::filesystem::file_size(path);
    } catch (...) {
        return 0;
    }
}

ProcessMemorySnapshot getProcessMemorySnapshot() {
    ProcessMemorySnapshot snapshot;

#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        snapshot.rssMB = bytesToMB(static_cast<uint64_t>(counters.WorkingSetSize));
        snapshot.peakRssMB = bytesToMB(static_cast<uint64_t>(counters.PeakWorkingSetSize));
    }
#else
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmRSS:") {
            uint64_t kb = 0;
            std::string unit;
            status >> kb >> unit;
            snapshot.rssMB = static_cast<double>(kb) / 1024.0;
        } else if (key == "VmHWM:") {
            uint64_t kb = 0;
            std::string unit;
            status >> kb >> unit;
            snapshot.peakRssMB = static_cast<double>(kb) / 1024.0;
        } else {
            std::string rest;
            std::getline(status, rest);
        }
    }
    if (snapshot.peakRssMB < snapshot.rssMB) {
        snapshot.peakRssMB = snapshot.rssMB;
    }
#endif

    return snapshot;
}

void updatePeakRss(const ProcessMemorySnapshot& snapshot, double& peakRssMB) {
    if (snapshot.peakRssMB > peakRssMB) {
        peakRssMB = snapshot.peakRssMB;
    }
    if (snapshot.rssMB > peakRssMB) {
        peakRssMB = snapshot.rssMB;
    }
}

void setProcessEnvironmentVariable(const std::string& name, const std::string& value) {
#ifdef _WIN32
    if (_putenv_s(name.c_str(), value.c_str()) != 0) {
        throw std::runtime_error("failed to configure environment variable: " + name);
    }
#else
    if (setenv(name.c_str(), value.c_str(), 1) != 0) {
        throw std::runtime_error("failed to configure environment variable: " + name);
    }
#endif
}

void configurePbfBlobThreads(int pbfThreads) {
    if (pbfThreads == 0) {
        return;
    }

    if (pbfThreads == 1) {
        setProcessEnvironmentVariable("OSMIUM_USE_POOL_THREADS_FOR_PBF_PARSING", "0");
        setProcessEnvironmentVariable("OSMIUM_POOL_THREADS", "1");
        return;
    }

    setProcessEnvironmentVariable("OSMIUM_USE_POOL_THREADS_FOR_PBF_PARSING", "1");
    setProcessEnvironmentVariable("OSMIUM_POOL_THREADS", std::to_string(pbfThreads));
}

bool appendAtBackIfConnected(std::vector<Coordinate>& chain, const std::vector<Coordinate>& candidate) {
    if (chain.empty() || candidate.empty()) {
        return false;
    }

    if (coordinatesEqual(chain.back(), candidate.front())) {
        chain.insert(chain.end(), candidate.begin() + 1, candidate.end());
        return true;
    }

    if (coordinatesEqual(chain.back(), candidate.back())) {
        for (auto it = candidate.rbegin() + 1; it != candidate.rend(); ++it) {
            chain.push_back(*it);
        }
        return true;
    }

    return false;
}

bool appendAtFrontIfConnected(std::vector<Coordinate>& chain, const std::vector<Coordinate>& candidate) {
    if (chain.empty() || candidate.empty()) {
        return false;
    }

    if (coordinatesEqual(chain.front(), candidate.back())) {
        chain.insert(chain.begin(), candidate.begin(), candidate.end() - 1);
        return true;
    }

    if (coordinatesEqual(chain.front(), candidate.front())) {
        std::vector<Coordinate> reversed(candidate.rbegin(), candidate.rend());
        chain.insert(chain.begin(), reversed.begin(), reversed.end() - 1);
        return true;
    }

    return false;
}

std::vector<std::vector<Coordinate>> buildClosedRings(
    const std::vector<uint64_t>& outerWayIds,
    const std::unordered_map<uint64_t, std::vector<Coordinate>>& outerWayGeometry) {
    std::vector<std::vector<Coordinate>> parts;
    parts.reserve(outerWayIds.size());

    for (uint64_t wayId : outerWayIds) {
        const auto found = outerWayGeometry.find(wayId);
        if (found == outerWayGeometry.end() || found->second.size() < 2) {
            continue;
        }
        parts.push_back(found->second);
    }

    std::vector<std::vector<Coordinate>> rings;
    std::vector<bool> used(parts.size(), false);

    for (size_t start = 0; start < parts.size(); ++start) {
        if (used[start]) {
            continue;
        }

        std::vector<Coordinate> chain = parts[start];
        used[start] = true;

        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t i = 0; i < parts.size(); ++i) {
                if (used[i]) {
                    continue;
                }
                if (appendAtBackIfConnected(chain, parts[i]) || appendAtFrontIfConnected(chain, parts[i])) {
                    used[i] = true;
                    changed = true;
                }
            }
        }

        if (chain.size() >= 4 && coordinatesEqual(chain.front(), chain.back())) {
            rings.push_back(std::move(chain));
        }
    }

    std::sort(rings.begin(), rings.end(), [](const auto& left, const auto& right) {
        return left.size() > right.size();
    });
    return rings;
}

std::vector<std::vector<Coordinate>> buildClosedRings(
    const AdminRelationDefinition& relation,
    const std::unordered_map<uint64_t, std::vector<Coordinate>>& outerWayGeometry) {
    return buildClosedRings(relation.outerWayIds, outerWayGeometry);
}

void writeString(std::ofstream& out, const std::string& value) {
    const uint32_t length = static_cast<uint32_t>(value.size());
    out.write(reinterpret_cast<const char*>(&length), sizeof(length));
    if (length > 0) {
        out.write(value.data(), static_cast<std::streamsize>(length));
    }
}

void readString(std::ifstream& in, std::string& value) {
    uint32_t length = 0;
    in.read(reinterpret_cast<char*>(&length), sizeof(length));
    value.resize(length);
    if (length > 0) {
        in.read(&value[0], static_cast<std::streamsize>(length));
    }
}

template <typename T>
void writePlainVector(std::ofstream& out, const std::vector<T>& values) {
    const uint64_t count = static_cast<uint64_t>(values.size());
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));
    if (count > 0) {
        out.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(sizeof(T) * values.size()));
    }
}

template <typename T>
bool readPlainVector(std::ifstream& in, std::vector<T>& values) {
    uint64_t count = 0;
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!in.good()) {
        return false;
    }
    if (count > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return false;
    }
    if (sizeof(T) > 0 &&
        count > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()) / sizeof(T)) {
        return false;
    }
    values.resize(static_cast<size_t>(count));
    if (count > 0) {
        in.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(sizeof(T) * values.size()));
    }
    return in.good();
}
