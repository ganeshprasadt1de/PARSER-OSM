#ifndef OSM_GEOCODE_INDEX_H
#define OSM_GEOCODE_INDEX_H

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "data_model.h"

constexpr uint8_t kGeocodeHouse = 1;
constexpr uint8_t kGeocodeStreet = 2;
constexpr uint8_t kGeocodeAdmin = 3;
constexpr uint8_t kGeocodePoi = 4;

std::string normalizeSearchText(const std::string& input);
std::vector<std::string> tokenizeSearchText(const std::string& input);
void uniqueTokens(std::vector<std::string>& tokens);
uint64_t geocodeKey(uint8_t type, uint32_t index);
bool geocodeRefLess(const GeocodeRef& left, const GeocodeRef& right);

void buildForwardGeocodeIndex(const OSMDataset& data,
                              ForwardGeocodeIndex& index,
                              bool showProgress,
                              std::ostream* logStream = nullptr);

std::vector<std::string> substringTokenExpansions(const ForwardGeocodeIndex& index,
                                                  const std::string& token,
                                                  size_t maxExpansions);

std::vector<std::string> fuzzyTokenExpansions(const ForwardGeocodeIndex& index,
                                              const std::string& token,
                                              size_t maxExpansions);

#endif
