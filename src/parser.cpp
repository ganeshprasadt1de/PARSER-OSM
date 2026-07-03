#include "parser.h"
#include "geocode_index.h"

#include <osmium/handler.hpp>
#include <osmium/io/any_input.hpp>
#include <osmium/io/reader.hpp>
#include <osmium/osm/node.hpp>
#include <osmium/osm/relation.hpp>
#include <osmium/osm/way.hpp>
#include <osmium/visitor.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <psapi.h>
#include <windows.h>
#else
#include <unistd.h>
#endif


namespace {

// Parser implementation parts are included in one translation unit.
// This keeps helper types private while making the code easier to read.
#include "parser_parts/core_types.cpp"
#include "parser_parts/tag_rules.cpp"
#include "parser_parts/geo_lookup.cpp"
#include "parser_parts/node_store.cpp"
#include "parser_parts/runtime_io.cpp"
#include "parser_parts/pbf_scans.cpp"
#include "parser_parts/street_merge.cpp"

}  // namespace

#include "parser_parts/parser_flow.cpp"
