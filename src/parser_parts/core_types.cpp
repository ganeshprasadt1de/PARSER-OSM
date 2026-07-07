constexpr char kBinaryMagic[8] = {'O', 'S', 'M', 'D', 'B', '0', '0', '7'};

uint32_t checkedU32(size_t value, const char* field) {
    if (value > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        throw std::runtime_error(std::string(field) + " exceeds uint32_t range");
    }
    return static_cast<uint32_t>(value);
}

struct AdminRelationDefinition {
    uint64_t relationId = 0;
    std::string name;
    uint8_t adminLevel = 0;
    std::vector<uint64_t> outerWayIds;
    std::vector<uint64_t> innerWayIds;
};

struct BuildingRelationDefinition {
    uint64_t relationId = 0;
    std::string houseNumber;
    std::string streetName;
    std::string postcode;
    std::string city;
    std::string country;
    std::vector<uint64_t> outerWayIds;
};

struct RelationWaySpan {
    uint64_t wayId = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
};

struct ProcessMemorySnapshot {
    double rssMB = 0.0;
    double peakRssMB = 0.0;
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

std::string formatProgressBytes(uint64_t bytes) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(1);
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        stream << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) << " GB";
    } else if (bytes >= 1024ULL * 1024ULL) {
        stream << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MB";
    } else if (bytes >= 1024ULL) {
        stream << (static_cast<double>(bytes) / 1024.0) << " KB";
    } else {
        stream << bytes << " B";
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

class ProgressReporter {
public:
    ProgressReporter(std::string phaseName, size_t phaseIndex, size_t phaseCount, uint64_t totalUnits, std::string unitName)
        : phaseName_(std::move(phaseName)),
          phaseIndex_(phaseIndex),
          phaseCount_(phaseCount),
          totalUnits_(totalUnits),
          unitName_(std::move(unitName)),
          interactive_(stdoutIsTerminal()),
          start_(std::chrono::steady_clock::now()),
          lastPrint_(start_) {
        print(0, true);
    }

    void update(uint64_t completedUnits) {
        if (totalUnits_ > 0) {
            completedUnits = std::min(completedUnits, totalUnits_);
        }
        if (completedUnits == lastCompletedUnits_) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (completedUnits < totalUnits_ &&
            std::chrono::duration<double>(now - lastPrint_).count() < kPrintIntervalSeconds) {
            return;
        }
        print(completedUnits, false);
    }

    void finish(uint64_t completedUnits = std::numeric_limits<uint64_t>::max()) {
        if (completedUnits == std::numeric_limits<uint64_t>::max()) {
            completedUnits = totalUnits_;
        }
        if (completedUnits == lastCompletedUnits_) {
            if (lineOpen_) {
                std::cout << "\n";
                lineOpen_ = false;
            }
            return;
        }
        print(completedUnits, true);
        if (lineOpen_) {
            std::cout << "\n";
            lineOpen_ = false;
        }
    }

private:
    static constexpr double kPrintIntervalSeconds = 1.0;
    static constexpr int kBarWidth = 24;

    void print(uint64_t completedUnits, bool force) {
        if (totalUnits_ > 0) {
            completedUnits = std::min(completedUnits, totalUnits_);
        }

        const auto now = std::chrono::steady_clock::now();
        const double elapsedSeconds = std::chrono::duration<double>(now - start_).count();
        const double fraction = totalUnits_ > 0
            ? static_cast<double>(completedUnits) / static_cast<double>(totalUnits_)
            : 0.0;
        const int filled = static_cast<int>(std::round(fraction * kBarWidth));
        const double rate = elapsedSeconds > 0.0
            ? static_cast<double>(completedUnits) / elapsedSeconds
            : 0.0;
        const bool hasEta = totalUnits_ > 0 && rate > 0.0 && completedUnits > 0 && completedUnits < totalUnits_;
        const double etaSeconds = hasEta
            ? static_cast<double>(totalUnits_ - completedUnits) / rate
            : 0.0;

        std::ostringstream line;
        if (interactive_) {
            line << "\r\033[2K";
        }
        if (phaseCount_ > 0) {
            line << "[" << phaseIndex_ << "/" << phaseCount_ << "] ";
        }
        line << phaseName_ << " [";
        for (int i = 0; i < kBarWidth; ++i) {
            line << (i < filled ? '#' : '-');
        }
        line << "] ";
        if (totalUnits_ > 0) {
            line << std::fixed << std::setprecision(1) << (fraction * 100.0) << "% | ";
            if (unitName_ == "bytes") {
                line << formatProgressBytes(completedUnits) << " / " << formatProgressBytes(totalUnits_);
                if (rate > 0.0) {
                    line << " | " << formatProgressBytes(static_cast<uint64_t>(rate)) << "/s";
                }
            } else {
                line << completedUnits << " / " << totalUnits_ << " " << unitName_;
                if (rate > 0.0) {
                    line << " | " << std::fixed << std::setprecision(0) << rate << " " << unitName_ << "/s";
                }
            }
            line << " | elapsed " << formatProgressDuration(elapsedSeconds);
            if (completedUnits >= totalUnits_) {
                line << " | ETA 0s";
            } else if (hasEta) {
                line << " | ETA " << formatProgressDuration(etaSeconds);
            } else {
                line << " | ETA unknown";
            }
        } else {
            line << "working | elapsed " << formatProgressDuration(elapsedSeconds);
        }

        const std::string text = line.str();
        std::cout << text;
        if (interactive_ && lastLineLength_ > text.size()) {
            std::cout << std::string(lastLineLength_ - text.size(), ' ');
        }
        if (!interactive_) {
            std::cout << "\n";
        }
        std::cout << std::flush;

        lastLineLength_ = text.size();
        lastCompletedUnits_ = completedUnits;
        lastPrint_ = now;
        lineOpen_ = true;
        if (force && totalUnits_ > 0 && completedUnits >= totalUnits_) {
            if (interactive_) {
                std::cout << "\n";
            }
            lineOpen_ = false;
            lastLineLength_ = 0;
        }
    }

    std::string phaseName_;
    size_t phaseIndex_ = 0;
    size_t phaseCount_ = 0;
    uint64_t totalUnits_ = 0;
    std::string unitName_;
    bool interactive_ = false;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point lastPrint_;
    uint64_t lastCompletedUnits_ = std::numeric_limits<uint64_t>::max();
    size_t lastLineLength_ = 0;
    bool lineOpen_ = false;
};

class StringInterner {
public:
    StringInterner() {
        values_.push_back("");
        index_.emplace("", 0);
    }

    StringRef intern(const std::string& value) {
        const auto found = index_.find(value);
        if (found != index_.end()) {
            return found->second;
        }

        const StringRef id = static_cast<StringRef>(values_.size());
        values_.push_back(value);
        index_.emplace(values_.back(), id);
        return id;
    }

    std::vector<std::string> releaseValues() {
        return std::move(values_);
    }

    void clearIndex() {
        std::unordered_map<std::string, StringRef>().swap(index_);
    }

private:
    std::vector<std::string> values_;
    std::unordered_map<std::string, StringRef> index_;
};
