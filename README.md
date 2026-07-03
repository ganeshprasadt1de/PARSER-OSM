# OSM Extraction And Viewer

This project turns a large OpenStreetMap `.osm.pbf` file into a smaller dataset that is easier to inspect and use later. The parser extracts houses, streets, and administrative areas, stores them in compact memory structures, and shows them in a Leaflet map through a C++ server.

The `--connect-streets` mode can join street segments when they have the same street identity and touch each other.

## 1. What The Parser Extracts

The parser reads the three main OSM primitive types:

- `node`: one point with latitude and longitude.
- `way`: an ordered list of node IDs. A way can describe a street, building, or boundary.
- `relation`: a group of OSM objects. Administrative boundaries and some multipolygon buildings are often stored as relations.

The reduced dataset stores:

- houses as representative points
- streets as line strings
- administrative areas as polygons

Only one metadata language is selected at a time. The default language is `de`.

## 2. Houses

The parser stores houses as points. This is useful for later reverse geocoding because one point is much cheaper to test against polygons than a full building shape.

Address nodes already contain one coordinate, so the parser stores that coordinate directly. A stored address can come from an address node, an addressed building way, or an addressed building relation.

Addressed building ways are reduced to one point:

- closed building polygon: polygon centroid (the geometric center of the polygon)
- incomplete or non-closed geometry: average of the available vertices

Addressed building relations are kept too. For multipolygon buildings, the parser assembles the outer member ways and stores one representative point, so relation buildings with inner holes are not skipped.

A hole means an empty area inside a polygon. A simple example is a building shaped like a ring around a courtyard. The outside ring is the building boundary, and the courtyard is the inner hole. This project stores one house point for the addressed building relation. It does not draw the full building footprint or the courtyard shape, because the house layer stores points instead of building polygons.

Administrative area holes are handled during point-in-polygon lookup. If a house point is inside the outer boundary but also inside an inner hole, that house is not treated as being inside that administrative area.

Stored address fields:

- `addr:housenumber`
- `addr:street`
- `addr:postcode`
- `addr:city`
- `addr:country`

The parser keeps houses even when the street or house number is missing. They are weaker for exact address lookup, but they are still useful for reverse geocoding, checking where address data is incomplete, and later enrichment.

The map background may show many grey building footprints. Those come from the OpenStreetMap tile layer. A building is stored by this parser only when it has useful address tags.

## 3. Streets

The parser stores street ways as line strings.

An OSM street way stores node IDs, not coordinates. The parser resolves those node IDs to coordinates and writes the final street geometry as a coordinate sequence.

Street identity is taken from the first available value:

1. `name`
2. `official_name`
3. `ref`

The `ref` fallback matters for larger roads. Roads such as `A 5`, `B 35`, `L 558`, or `51` are often identified by `ref=*` instead of `name=*`.

Area-like highway objects are skipped because a plaza polygon is not a street centerline. The parser also skips highway types that are not useful for this dataset, such as `path`, `footway`, `cycleway`, `steps`, `platform`, and `corridor`.

Some unnamed road pieces are kept when they help visible street continuity. This includes short unnamed connector roads such as `motorway_link`, `trunk_link`, `primary_link`, `secondary_link`, `tertiary_link`, `residential`, `unclassified`, `living_street`, `road`, and unnamed roundabouts that touch named streets. These pieces are stored with an empty name because the OSM object itself has no safe street label.

The parser still does not keep every possible road-like object. It skips things such as foot paths, cycle paths, construction roads, proposed roads, and area polygons. Named `track` ways can still be stored, but unnamed tracks are not part of the recovery rule.

## 4. Street Connection

OSM often stores one real street as many separate ways. A street can be split at intersections, bridges, turn restrictions, surface changes, or administrative borders.

The connection step can be run with:

```bash
--connect-streets
```

The merge rule is conservative:

- same street label
- same highway type
- endpoints touch exactly
- branches are kept as separate chains instead of being forced into one artificial line

The connection step works on the reduced street dataset. It creates endpoint records, sorts them by street label, highway type, and coordinate, then uses connected components (groups of objects linked by shared endpoints) to merge street chains.

Unnamed recovered roads are displayed, but they are not merged by name because they do not have a safe identity.

Example while parsing:

```bash
./build-linux/osm_parser \
  --parse europe-latest.osm.pbf \
  --low-memory \
  --connect-streets \
  --save-binary europe_connected.bin \
  --pbf-threads 24
```

Example using an existing binary snapshot:

```bash
./build-linux/osm_parser \
  --load-binary europe_reduced.bin \
  --connect-streets \
  --save-binary europe_connected.bin
```

The parse command can extract the data, connect streets, and write the final connected snapshot in one run. The binary route is useful when an unconnected snapshot already exists and only the street connection step needs to be repeated.

## 5. Incomplete Address Records

OpenStreetMap address data is not always complete. Some houses have a street name but no house number. Other houses have a house number but no street name. The parser keeps both cases instead of throwing them away.

A node, building way, or addressed building relation is stored as a house when it has at least one of these tags:

- `addr:housenumber`
- `addr:street`

This means these records are still saved:

- a house with only `addr:street`
- a house with only `addr:housenumber`

The parser counts the missing parts separately:

- `Houses missing street`: stored houses without `addr:street`
- `Houses missing number`: stored houses without `addr:housenumber`

Keeping these incomplete houses is useful because the point on the map is still valuable:

- If a user clicks near that house, reverse geocoding can still use the house point to improve the result.
- The missing-count numbers show how much address data is incomplete. For example, a high `Houses missing street` value means many stored houses still need street names.
- A later step can try to fill missing values. For example, a house with a number but no street name can be matched to the nearest suitable street line.

If these records were skipped during extraction, that information would be lost and could not be improved later.

## 6. Administrative Areas

The parser stores administrative boundaries as polygons. These polygons are later used to link stored houses, streets, and smaller administrative areas to the larger areas around them.

Supported sources:

- closed administrative boundary ways
- administrative boundary relations made from outer member ways

Admin levels `2` to `12` are kept.

Approximate meaning of common levels:

- `2`: country
- `4`: state or large region
- `6`: district or county
- `8`: municipality, city, or town
- `9` to `12`: local areas such as suburbs, quarters, or neighborhoods

After these polygons are built, the parser can attach administrative area references to stored houses, streets, and administrative areas.

## 7. Administrative Area Lookup

Houses, streets, and administrative areas store references to the administrative areas around them. This makes the data more useful later because the backend can return an address, the street it belongs to, and the surrounding city, district, state, or country.

The lookup runs after administrative boundary assembly:

1. the parser builds a grid over administrative area bounding boxes
2. each house point checks only the admin areas that overlap its grid cell
3. each street checks the admin areas for its stored geometry points, so a street crossing a border can keep more than one area
4. each administrative area uses one representative interior point to find larger parent areas with lower admin level numbers
5. a point-in-polygon test confirms whether each checked point is really inside the polygon
6. matching admin area indexes are stored as compact integer links

The representative interior point in step 4 is an important speed improvement. To find the parent of an area, the parser does not need to test many points around that area's boundary. For example, to know that Stuttgart belongs to Baden-Wuerttemberg, one reliable point inside Stuttgart is enough. Testing one interior point gives the needed parent link and avoids repeating the same point-in-polygon work for many boundary points.

This is not machine learning. It is a geometry shortcut: use one good point that represents the smaller polygon when the question is only "which larger polygon contains this smaller polygon?" Houses are already points, so they are checked directly. Streets are still checked by their geometry points because a street can cross from one area into another.

The point-in-polygon test does not treat latitude and longitude as plain flat `x/y` coordinates. For each checked point, longitude is scaled by `cos(latitude)` before the ray-crossing test. This is a local equirectangular projection. It is still simple, but it avoids the worst mistake of using raw longitude as if the Earth were flat everywhere.

The polygon test also uses a latitude edge index. For a checked point, the parser checks only polygon edges near that latitude instead of scanning every edge of every candidate boundary. Inner holes are tested too, so a point inside a hole is excluded from that administrative area.

## 8. Reverse Geocoder

The reverse geocoder answers this question: "What useful map object is closest to this clicked map point?"

When the server starts, it builds grids over stored house points, street bounding boxes, and administrative area bounding boxes. A map click sends the clicked latitude and longitude to:

```text
/api/reverse?lat=<latitude>&lon=<longitude>
```

The backend searches nearby grid cells. It does not scan the full arrays for every click.

The best result is selected in this order:

1. a direct house click, when an address point is within about 3 m
2. a direct street click, when a street line is within about 15 m
3. a close street, when it is within about 50 m and closer than the nearest house
4. a close house, when an address point is within about 100 m
5. a fallback house, when an address point is still reasonably near, up to about 300 m
6. the containing administrative area, when there is no close house or street
7. the nearest administrative boundary, when the click is outside all stored areas but still near the dataset

The distance to a street or administrative boundary is approximated by measuring from the clicked point to the nearest line segment of the stored geometry. This is simple, but it is much better than comparing only to one endpoint or one center point.

The response includes:

- the clicked coordinate
- a result object with type `house`, `street`, or `admin_area`
- the closest stored house point, when one exists
- the nearest stored street line, when one exists nearby
- saved address fields such as street, house number, postcode, city, and country
- administrative areas for the matched house or street, or for the clicked point when no nearby object is selected

When map click lookup is enabled in the GUI, the frontend shows the clicked point, the best result, the closest house address, nearest street details, and the administrative polygons used for the result.

## 9. Forward Geocoder

The forward geocoder answers text queries such as:

```text
Hauptstrasse 10 Aalen
```

The backend builds an in-memory inverted index when the server starts. It does not change the parser output or the binary snapshot format. The index is built from the reduced dataset that already exists:

- address records from stored houses
- named streets
- administrative areas

Administrative area names are also attached to houses and streets in the search index. This allows a query with both an object name and a place name, for example a street and city, to be answered without scanning every object at query time.

The server builds two forward-search indexes:

- a context index, where houses and streets also receive surrounding administrative names for full address queries
- a primary-field index, where single plain-name queries use the object's own address/name fields first

The primary-field index prevents a one-word city query such as `Stuttgart` from first collecting every house located in Stuttgart. Full address queries still use the context index, because city and district tokens are useful there.

String preprocessing is intentionally simple and deterministic:

- case is ignored
- punctuation is treated as spacing
- `Straße`, `Strasse`, and `Str.` are normalized to the same token
- common German umlauts are normalized, for example `ä` to `ae`
- combining accent marks are ignored

The query endpoint is:

```text
/api/geocode?q=<query>&limit=<n>
```

The response contains a result list, `queryTimeMs`, `indexMode`, and candidate counters. Results can be houses, streets, administrative areas, or selected POIs. Street results include their line geometry, house results include the address point, POI results include category/tag metadata, and administrative area results include enough information for the frontend to fetch the polygon if needed.

The ranking is heuristic:

- all query tokens must match
- house results are preferred when the query contains a house number
- street results are preferred for street-name queries without a house number
- administrative areas are returned when the area name itself matches
- for single-token queries, area or street matches must come from the object's own name, not only from a surrounding region

Substring search and typo-tolerant fuzzy fallback are implemented for normalized search tokens. Selected Task 5 POI/natural queries are implemented through `/api/natural-geocode`: named POI in place, such as `Stuttgart Burger King`, nearest category to address, such as `Closest Park to Koenigstrasse 1 Stuttgart`, and product/service concept queries, such as `where can I buy nail polish remover near Koenigstrasse 1 Stuttgart`. The frontend exposes an optional checkbox for local Ollama intent parsing. When the checkbox is enabled, Ollama is tried first and the deterministic parser is still available if no valid intent is produced. Final results still come only from the PBF-derived indexes.

Product/service queries use a small local concept graph plus product families. Ollama first drafts strict intent JSON, then verifies and corrects that JSON in a second local call. The backend validates the final concept and product family, applies deterministic product-family corrections for known terms, and maps the family to weighted OSM tags such as `amenity=pharmacy`, `shop=chemist`, `shop=bicycle`, or `shop=copyshop`. This returns likely real OSM places, not guaranteed product inventory. Result clustering, external geocoding services, and route planning are not used.

Natural queries can use the current map view as their frame of reference. If a query contains an explicit place or address, the backend uses that address as the search origin. If the query has no usable place, the frontend sends the current map center as `lat` and `lon`, and the backend uses that point as the origin. The response includes `originSource` so this is visible: `address` means the typed place was used, `viewport` means the map center was used, and `none` means no valid origin was available. If Ollama invents an address that is not present in the user's text, the backend ignores that invented address and uses the supplied viewport center instead.

When the server starts on Ubuntu/Linux, it can also start local Ollama automatically. If `OSM_AUTO_OLLAMA` is not disabled and no Ollama server is already listening on the configured local host and port, the backend starts `ollama serve`, warms the selected model, and stops only that managed Ollama process when the OSM server exits. If Ollama was already running before the OSM server started, the backend uses it but does not terminate it.

The `/api/stats` endpoint also exposes server-side index build metrics, including spatial-index build time, forward-index build time, posting-list counts, grid-cell counts, and an explicitly estimated forward-index memory value. The estimate is not reported as exact heap use; it is a coherent size estimate based on stored postings, token strings, and vector metadata.

## 10. Compact Storage

The Europe PBF used for testing is about 32 GB. The program does not keep the full OSM file in RAM. It keeps only the reduced objects needed for this project.

Memory is reduced by:

- storing coordinates as `int32` E7 values, for example `48.7758` as `487758000`
- pooling repeated strings so each street name is stored once
- storing street and admin geometry in shared coordinate arrays
- storing only offsets and lengths in street and admin records
- storing house-to-admin matches as compact integer indexes
- reducing building polygons to one house point
- skipping unused OSM metadata such as timestamp, user ID, version, and changeset

This gives the program a compact in-memory dataset that can also be written to a binary snapshot.

## 11. Needed-Node Architecture

The important scalability idea is the needed-node lookup.

A simple parser would store every OSM node coordinate first and then use that table to build ways. That is easy, but Europe has billions of nodes, so the full node table can fill RAM and force the operating system to swap to disk.

The compact approach:

1. scan administrative boundary relations and addressed building relations, and remember their outer member ways
2. scan named streets and collect endpoint nodes
3. scan unnamed road candidates and recover short connectors and roundabouts that touch named streets
4. scan useful ways and collect node IDs only from those ways
5. sort and deduplicate those node IDs
6. scan nodes and store coordinates only for needed node IDs
7. scan ways again and build house points, street lines, and admin polygons
8. assemble administrative relation polygons from their member ways
9. attach administrative area references to stored houses, streets, and administrative areas

With `--low-memory`, the useful-way extraction is split into two named phases:

- `Address And Boundary Extraction`: houses and administrative boundaries
- `Street Line Extraction`: named streets plus recovered unnamed connectors

This reads the PBF more times, but the scans are sequential. Sequential scans are much cheaper than overflowing RAM. A small bucket index over the sorted node IDs keeps coordinate lookup fast.

During parsing, the program prints phase-aware progress with elapsed time and ETA:

- PBF scan phases use the compressed file byte offset reported by libosmium.
- Administrative attribution uses the number of houses, streets, and admin areas processed.
- Binary snapshot writing uses the current output-file position against the estimated snapshot size.

On a real terminal, progress updates in place on the same line. When output is redirected to a file, progress is written as normal log lines so the log remains readable.

The ETA is an estimate. It is reliable for long sequential scans, but CPU-heavy phases can speed up or slow down depending on geometry complexity.

When starting the GUI/server from a binary snapshot, the server also shows progress while building spatial indexes and forward-geocoder indexes before it prints the localhost URL.

## 12. Binary Snapshots

Large PBF parsing takes minutes, so the reduced dataset can be saved:

```bash
./build-linux/osm_parser \
  --parse europe-latest.osm.pbf \
  --low-memory \
  --save-binary europe_reduced.bin \
  --pbf-threads 24
```

The snapshot can later be loaded without reparsing:

```bash
./build-linux/osm_parser \
  --load-binary europe_reduced.bin \
  --server 8080
```

The binary snapshot format is meant for the same platform and build style that created it. A snapshot written by the Ubuntu build should be loaded by the Ubuntu build.

## 13. GUI

The GUI uses a C++ backend server and a Leaflet frontend. The backend uses the vendored single-header `cpp-httplib` server.

When the map moves or zooms, the frontend asks the backend for houses, streets, and administrative areas inside the current viewport. The backend uses grid indexes for these requests, so it checks only nearby grid cells instead of scanning every stored object. This keeps viewport loading practical on large datasets.

Administrative geometry endpoints accept `maxPoints=<n>` and return `geometryMeta` with `sourcePoints`, `writtenPoints`, and `simplified`. The frontend requests bounded geometry by default to avoid very large polygon payloads. For debugging, `/api/admin-area?...&detail=full` and `/api/admin?...&detail=full` return full geometry.

Map colors:

- orange dots: houses or address points
- blue lines: street ways
- green polygons: administrative boundaries
- black highlighted point, line, or polygon: forward geocoder result
- black marker and line: reverse geocoder click result
- dark green outlines: administrative polygons matched by the point-in-polygon lookup

Click behavior:

- geocoder text field: sends text queries to `/api/geocode`, displays the timed result list, and highlights returned objects on the map
- LLM checkbox: sends natural-language geocoder queries to `/api/natural-geocode` and includes the current map center as a fallback origin
- map click lookup checkbox: enables or disables reverse geocoder requests from map clicks
- map click while lookup is enabled: best result, closest stored house, nearest street, address fields, and admin areas
- map click while lookup is disabled: normal inspect mode for visible houses, streets, and administrative areas
- clear result button: removes the reverse geocoder marker, line, and matched admin polygons
- house popup: street, house number, place, OSM ID
- street popup: street label, highway type, OSM ID
- admin popup: boundary name, admin level, OSM ID

The reverse geocoder result uses a separate highlight layer, so matched administrative polygons are shown even when the normal viewport layer omits larger boundaries at the current zoom level. While map click lookup is enabled, visible map objects do not catch the click first. The clicked coordinate is sent to the reverse geocoder, and the backend decides whether the best result is a house, street, or administrative area.

When map click lookup is disabled, the frontend uses the visible viewport data for normal inspect popups. It checks for a nearby house first, then a nearby street, and then the most detailed visible administrative area containing the click. If streets are visible and the click is near a street inside a large polygon, the street popup is shown because the street is the more specific visible object.

## 14. Requirements On Ubuntu 22.04

Install the compiler, CMake, Ninja, and the system libraries needed by libosmium:

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build zlib1g-dev libbz2-dev libexpat1-dev
```

What these packages provide:

- `build-essential`: C++ compiler and basic build tools
- `cmake`: generates the build files
- `ninja-build`: runs the build quickly
- `zlib1g-dev`: zlib compression library used while reading compressed OSM data
- `libbz2-dev`: bzip2 compression library used by libosmium
- `libexpat1-dev`: XML parser library used by libosmium

The `--pbf-threads` value should match the machine. More threads can make PBF decoding faster, but they can also increase CPU and memory pressure. On a smaller laptop, use a lower value such as `4` or `8`. On a stronger machine with enough RAM, a higher value such as `16` or `24` can be used.

The required header-only dependencies are included here:

```text
third_party/cpp-httplib/httplib.h
third_party/libosmium/include
third_party/protozero/include
```

So the project normally does not need `libosmium`, `protozero`, or `cpp-httplib` installed globally on Linux. The `PATH` variable does not need to be edited for them.

`PATH` is the shell variable Linux uses to find executable programs. Header-only libraries are not executable programs, so they do not belong in `PATH`.

CMake finds the bundled headers through `CMakeLists.txt`. If CMake asks for the Protozero or Libosmium path, first check that the folders are really present:

```bash
ls third_party
ls third_party/protozero/include/protozero
ls third_party/libosmium/include/osmium
```

If those commands show files, the dependencies are inside the project. Delete the build folder and run CMake again from the project root:

```bash
rm -rf build-linux
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j
```

If CMake still asks for the paths, give the include folders manually:

```bash
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLIBOSMIUM_INCLUDE_DIR="$PWD/third_party/libosmium/include" \
  -DPROTOZERO_INCLUDE_DIR="$PWD/third_party/protozero/include"
```

## 15. Build

From the project folder:

```bash
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j
```

Executable:

```text
build-linux/osm_parser
```

## 16. Basic Commands

Parse a PBF:

```bash
./build-linux/osm_parser --parse /path/to/region.osm.pbf
```

Parse with low-memory mode and save a binary snapshot:

```bash
./build-linux/osm_parser \
  --parse europe-latest.osm.pbf \
  --low-memory \
  --save-binary europe_reduced.bin \
  --pbf-threads 24
```

Load a binary snapshot:

```bash
./build-linux/osm_parser --load-binary europe_reduced.bin
```

Start the GUI from a binary snapshot:

```bash
./build-linux/osm_parser --load-binary europe_reduced.bin --server 8080
```

Open:

[http://localhost:8080](http://localhost:8080)

## 17. Command Line Options

```text
--parse <input.osm.pbf>       Parse a PBF file
--load-binary <file>          Load the reduced binary dataset
--save-binary <file>          Save the reduced binary dataset
--lang <code>                 Use one metadata language (default: de)
--pbf-threads <n>             PBF decoder threads: 0=default, 1=serial, 2-32=parallel
--low-memory                  Use extra PBF scans to lower peak RAM during extraction
--connect-streets             Merge connected street segments with same label and highway type
--geojson <file>              Export sampled GeoJSON
--geojson-houses <n>          House export limit
--geojson-streets <n>         Street export limit
--geojson-admin <n>           Admin area export limit
--server [port]               Start the backend and Leaflet frontend
--viewport-limit <n>          Maximum houses returned in one viewport request
--help                        Show all command line options
```

Ollama runtime environment variables:

```text
OSM_AUTO_OLLAMA=0             Disable automatic Ollama startup
OSM_OLLAMA_BIN=<path>         Ollama executable path, if it is not on PATH
OSM_OLLAMA_MODEL=<name>       Local model used for natural-language intent parsing
OSM_OLLAMA_HOST=<host>        Ollama host, default localhost
OSM_OLLAMA_PORT=<port>        Ollama port, default 11434
OSM_OLLAMA_TIMEOUT_SECONDS=n  Timeout for Ollama generation calls
OSM_OLLAMA_WARMUP=0           Disable model warmup at server startup
```

## 18. Console Metrics

The console prints detailed phase metrics while the parser runs. These help with performance checks because they show what each scan did and how long it took.

Important scan sections:

- `Administrative Boundary Relation Scan`: scans OSM relations, keeps administrative boundary relations and addressed building relations, and records the outer member ways needed later.
- `Address And Boundary Extraction`: builds house points and administrative boundary way geometry.
- `Street Line Extraction`: builds street line geometry.
- `Administrative Boundary Assembly`: builds administrative polygons from relation member ways.
- `Administrative Attribute Lookup`: stores admin links for houses, streets, and administrative parent areas.
- `Street connection`: merges connected street segments when `--connect-streets` is enabled.

`Administrative Attribute Lookup` is often the largest time cost on Europe-sized data because it performs many point-in-polygon checks. The admin-parent part uses one representative interior point per smaller administrative area instead of testing many boundary points. This keeps the parent-area result useful while reducing repeated geometry work.

Each extraction phase prints only the main scale, memory, and time numbers:

- `OSM ways used`: ways that matter for that phase
- `Lookup index memory`: memory used by the small coordinate lookup index
- `Time`: total time for that extraction phase

The `Address And Boundary Extraction` time includes house extraction. The separate `House Extraction` section prints the house counts, including how many address nodes, building ways, and building relations were stored.

Important count sections:

- `Houses extracted`: all stored house and address objects
- `Houses from nodes`: addresses that were already stored as OSM nodes
- `Houses from ways`: addressed building ways reduced to one point
- `Houses from relations`: addressed building relations reduced to one point
- `Houses missing street`: stored houses without `addr:street`
- `Houses missing number`: stored houses without `addr:housenumber`
- `Houses with admin areas`: stored houses that matched at least one administrative polygon
- `House-admin links`: total stored house-to-admin references
- `Streets with admin areas`: stored street records that matched at least one administrative polygon
- `Street-admin links`: total stored street-to-admin references
- `Admin areas with parents`: administrative polygons with at least one larger parent area
- `Admin-parent links`: total stored admin-to-parent references
- `Admin areas from relations`: administrative polygons assembled from relation member ways
- `Street records before`: street line records before connection
- `Street records after`: street line records after connection
- `Street records reduced`: how many records were removed by connecting segments

Final timing and memory lines:

- `Total time taken`: the sum of the printed phase times, shown in seconds and minutes
- `Total memory used`: peak RSS during the run
- `Timing breakdown`: shows how relation scan, extraction, boundary assembly, administrative attribute lookup, and street connection add up

`RSS` means resident set size, which is the part of the process that is actually in RAM.

The Leaflet frontend shows the final dataset stats, current viewport counts, and reverse geocoder result. Reverse geocoder lookup is disabled by default and can be enabled from the sidebar when an address lookup is needed. The per-scan extraction details stay in the console so the sidebar does not become noisy.

## 19. GeoJSON Export

Sample export:

```bash
./build-linux/osm_parser --parse /path/to/region.osm.pbf --geojson out.geojson
```

With explicit limits:

```bash
./build-linux/osm_parser \
  --parse /path/to/region.osm.pbf \
  --geojson out.geojson \
  --geojson-houses 10000 \
  --geojson-streets 2000 \
  --geojson-admin 300
```

## 20. Main Files

- `CMakeLists.txt`: build configuration
- `include/data_model.h`: compact data structures
- `include/parser.h`: parser interface
- `include/server.h`: server interface
- `src/main.cpp`: command line handling
- `src/parser.cpp`: parser implementation entry file
- `src/parser_parts/core_types.cpp`: shared parser types
- `src/parser_parts/tag_rules.cpp`: OSM tag checks
- `src/parser_parts/geo_lookup.cpp`: geometry and area lookup
- `src/parser_parts/node_store.cpp`: compact node lookup
- `src/parser_parts/runtime_io.cpp`: memory and binary helpers
- `src/parser_parts/pbf_scans.cpp`: PBF scan handlers
- `src/parser_parts/street_merge.cpp`: street connection
- `src/parser_parts/parser_flow.cpp`: parser command flow
- `src/server.cpp`: `cpp-httplib` HTTP backend and API endpoints
- `frontend/index.html`: Leaflet GUI
- `third_party/README.md`: bundled third-party header notes

The parser part files are included by `src/parser.cpp`. This keeps the parser behavior the same, but the code is easier to read because each file has one clear job.

## 21. Installation Commands

Install Ubuntu build dependencies:

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build zlib1g-dev libbz2-dev libexpat1-dev
```

Go to the project folder:

```bash
cd /path/to/Parser-OSM-Project
```

Build the executable:

```bash
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j
```

Build with manual dependency paths if CMake asks for Protozero or Libosmium:

```bash
rm -rf build-linux
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLIBOSMIUM_INCLUDE_DIR="$PWD/third_party/libosmium/include" \
  -DPROTOZERO_INCLUDE_DIR="$PWD/third_party/protozero/include"
cmake --build build-linux -j
```

Parse a PBF, connect streets, and save a binary snapshot:

```bash
./build-linux/osm_parser \
  --parse input.osm.pbf \
  --low-memory \
  --connect-streets \
  --save-binary output.bin \
  --pbf-threads 8
```

Start the GUI from a binary snapshot:

```bash
./build-linux/osm_parser \
  --load-binary output.bin \
  --server 8080
```

Open the GUI:

```text
http://localhost:8080
```

## 22. Easy Run Commands

Use these commands on Ubuntu 22.04 when the project is in `/home/ganesh/osm-task1`.

Build the executable:

```bash
cd /home/ganesh/osm-task1
cmake -S . -B /home/ganesh/osm-task1-build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /home/ganesh/osm-task1-build
```

Check that the Europe binary snapshot exists:

```bash
ls -lh /home/ganesh/europe-geocoder.bin
```

Start the GUI from the Europe binary snapshot:

```bash
cd /home/ganesh/osm-task1
OSM_OLLAMA_MODEL=qwen2.5:3b \
OSM_OLLAMA_HOST=127.0.0.1 \
OSM_OLLAMA_TIMEOUT_SECONDS=30 \
/home/ganesh/osm-task1-build/osm_parser \
  --load-binary /home/ganesh/europe-geocoder.bin \
  --server 8080
```

Open the GUI:

```text
http://localhost:8080
```

The server starts Ollama automatically if it is not already running. Stop the server with `Ctrl+C`. If the server started Ollama itself, it also stops that managed Ollama process during shutdown.

Quick API checks from another Ubuntu terminal:

```bash
curl "http://127.0.0.1:8080/api/stats"
```

```bash
curl "http://127.0.0.1:8080/api/geocode?q=Berlin&limit=3"
```

```bash
curl "http://127.0.0.1:8080/api/natural-geocode?q=Where%20can%20I%20buy%20milk%3F&useLlm=1&lat=48.7758&lon=9.1829&limit=3"
```

The last command has no city in the query. The backend should therefore use the supplied `lat` and `lon` as the viewport origin and return `originSource:"viewport"`.
