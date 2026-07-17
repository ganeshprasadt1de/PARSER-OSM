# OSM Geocoder

This project is a local OpenStreetMap parser, geocoder, and browser map.

It reads an `.osm.pbf` file, keeps the OSM objects that are useful for the map and geocoder, writes a compact binary snapshot, and serves a Leaflet GUI through a local C++ HTTP server.

The program does not use Google, Nominatim, Overpass, PostGIS, SQLite, or an online geocoding API. Geocoding results come from the loaded PBF-derived dataset. The browser map can still request normal OpenStreetMap background tiles from the internet, because the tile layer is only the visual background.

The illustrated project explanation is included here:

```text
docs/osm_program_manual.pdf
```

That PDF explains the parser, reduced data model, reverse geocoder, forward geocoder, local knowledge graph, Ollama mode, ranking, API, GUI, binary snapshot format, and memory layout with diagrams.

## Index

1. [What You Need To Download](#1-what-you-need-to-download)
2. [Recommended Folder Layout](#2-recommended-folder-layout)
3. [Install Build Requirements On Ubuntu 22.04](#3-install-build-requirements-on-ubuntu-2204)
4. [Install Ollama For Natural-Language Search](#4-install-ollama-for-natural-language-search)
5. [Build The Program](#5-build-the-program)
6. [Parse A PBF Into A Binary Snapshot](#6-parse-a-pbf-into-a-binary-snapshot)
7. [Start The Browser Server](#7-start-the-browser-server)
8. [Quick API Checks](#8-quick-api-checks)
9. [What The Program Extracts](#9-what-the-program-extracts)
10. [Houses And Address Points](#10-houses-and-address-points)
11. [Streets And Street Connection](#11-streets-and-street-connection)
12. [Administrative Areas And Holes](#12-administrative-areas-and-holes)
13. [POIs](#13-pois)
14. [Reverse Geocoder](#14-reverse-geocoder)
15. [Forward Geocoder](#15-forward-geocoder)
16. [Substring And Fuzzy Search](#16-substring-and-fuzzy-search)
17. [Natural-Language Search With Ollama](#17-natural-language-search-with-ollama)
18. [Local Knowledge Graph](#18-local-knowledge-graph)
19. [Ranking](#19-ranking)
20. [Binary Snapshot Format](#20-binary-snapshot-format)
21. [Memory And Performance Design](#21-memory-and-performance-design)
22. [GUI Features](#22-gui-features)
23. [HTTP API Endpoints](#23-http-api-endpoints)
24. [Command Line Options](#24-command-line-options)
25. [Ollama Environment Variables](#25-ollama-environment-variables)
26. [Console Progress And Metrics](#26-console-progress-and-metrics)
27. [GeoJSON Export](#27-geojson-export)
28. [Program Manual](#28-program-manual)
29. [Main Source Files](#29-main-source-files)
30. [Troubleshooting](#30-troubleshooting)
31. [Quick Command Template](#31-quick-command-template)

## 1. What You Need To Download

You need three things:

1. This repository.
2. One OpenStreetMap `.osm.pbf` extract.
3. Ollama plus one local model, only if you want natural-language LLM search.

### 1.1 Repository

Clone the repository:

```bash
git clone <repository-url>
cd osm_project
```

Replace `<repository-url>` with the GitHub URL shown by the green `Code` button on the repository page.

If you already have the repository:

```bash
cd /path/to/osm_project
git pull origin main
```

### 1.2 PBF File

Download an OpenStreetMap `.osm.pbf` extract. The normal source is the Geofabrik download server:

- Baden-Wuerttemberg page: `https://download.geofabrik.de/europe/germany/baden-wuerttemberg.html`
- Europe PBF path: `https://download.geofabrik.de/europe-latest.osm.pbf`
- Baden-Wuerttemberg PBF path: `https://download.geofabrik.de/europe/germany/baden-wuerttemberg-latest.osm.pbf`

For a first run, use Baden-Wuerttemberg. Europe is much larger and takes longer to parse.

Example:

```bash
cd /path/to/osm_project
wget https://download.geofabrik.de/europe/germany/baden-wuerttemberg-latest.osm.pbf
```

Europe example:

```bash
cd /path/to/osm_project
wget https://download.geofabrik.de/europe-latest.osm.pbf
```

The `.osm.pbf` files are input data. They are not stored in the Git repository because they are large.

### 1.3 Ollama Model

The natural-language mode is designed around a small local Ollama model. The tested model name is:

```text
qwen2.5:3b
```

The deterministic forward geocoder and reverse geocoder do not need Ollama. Ollama is only needed when the GUI checkbox `Use LLM for natural-language query` is enabled or when `/api/natural-geocode?useLlm=1` is used.

## 2. Recommended Folder Layout

On Ubuntu 22.04 or WSL Ubuntu 22.04, the examples below use this layout:

```text
/path/to/osm_project                 repository
/path/to/osm_project-build           CMake build folder
/path/to/output/baden-geocoder.bin        Baden-Wuerttemberg binary snapshot
/path/to/output/europe-geocoder.bin       Europe binary snapshot
```

You can use different paths. If you do, replace the paths in the commands.

If you keep a Windows copy, a generic path looks like this:

```text
C:\path\to\osm_project
```

Run parsing and server commands from Ubuntu/WSL for best performance. Avoid parsing through `/mnt/c/...` when possible, because WSL file access through the Windows mount can be slower. Keeping the repository and PBF inside the Linux filesystem, for example under `$HOME/...`, is usually faster.

## 3. Install Build Requirements On Ubuntu 22.04

Install the compiler, CMake, Ninja, and the native libraries used by libosmium:

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build zlib1g-dev libbz2-dev libexpat1-dev wget curl
```

What these packages do:

- `build-essential`: C++ compiler and basic build tools.
- `cmake`: creates the build files.
- `ninja-build`: runs the build.
- `zlib1g-dev`: zlib compression support for reading OSM files.
- `libbz2-dev`: bzip2 compression support used by libosmium.
- `libexpat1-dev`: XML parser library used by libosmium.
- `wget` and `curl`: download PBF files and install Ollama.

The repository already includes these header-only dependencies:

```text
third_party/cpp-httplib/httplib.h
third_party/libosmium/include
third_party/protozero/include
```

That means you normally do not install `cpp-httplib`, `libosmium`, or `protozero` globally.

## 4. Install Ollama For Natural-Language Search

Ollama is optional for normal deterministic search, but required for LLM natural-language search.

Install Ollama on Ubuntu:

```bash
curl -fsSL https://ollama.com/install.sh | sh
```

Install the tested local model:

```bash
ollama pull qwen2.5:3b
```

Check that Ollama is available:

```bash
ollama --version
```

Check that the model is installed:

```bash
ollama list
```

The OSM server can start Ollama automatically when the server starts. If another Ollama server is already running on the configured port, the OSM server uses that existing Ollama process and does not stop it on shutdown.

The frontend also has a `Start Ollama Service` button under the geocoder. It calls the backend endpoint `/api/ollama/start`, clears the configured local Ollama port, starts `ollama serve`, warms the model, and reports the status in the browser.

## 5. Build The Program

From the repository folder:

```bash
cd /path/to/osm_project
cmake -S . -B /path/to/osm_project-build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /path/to/osm_project-build
```

The executable will be:

```text
/path/to/osm_project-build/osm_parser
```

If CMake cannot find bundled libosmium or protozero, give the include paths explicitly:

```bash
cd /path/to/osm_project
rm -rf /path/to/osm_project-build
cmake -S . -B /path/to/osm_project-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLIBOSMIUM_INCLUDE_DIR="$PWD/third_party/libosmium/include" \
  -DPROTOZERO_INCLUDE_DIR="$PWD/third_party/protozero/include"
cmake --build /path/to/osm_project-build
```

## 6. Parse A PBF Into A Binary Snapshot

Parsing reads the `.osm.pbf`, extracts the reduced dataset, builds the forward-geocoder index, and writes one binary snapshot.

### 6.1 Baden-Wuerttemberg

Use this for correctness checks and normal development:

```bash
cd /path/to/osm_project
/path/to/osm_project-build/osm_parser \
  --parse /path/to/data/baden-wuerttemberg-latest.osm.pbf \
  --low-memory \
  --connect-streets \
  --save-binary /path/to/output/baden-geocoder.bin \
  --pbf-threads 8
```

### 6.2 Europe

Europe is much larger. Use `--low-memory` on normal laptop hardware:

```bash
cd /path/to/osm_project
/path/to/osm_project-build/osm_parser \
  --parse /path/to/data/europe-latest.osm.pbf \
  --low-memory \
  --connect-streets \
  --save-binary /path/to/output/europe-geocoder.bin \
  --pbf-threads 24
```

If your machine has less available RAM or starts swapping heavily, lower the thread count:

```bash
--pbf-threads 8
```

The parser prints phase progress, elapsed time, throughput, ETA where possible, and memory statistics. On a real terminal, progress updates in place on one line.

## 7. Start The Browser Server

Start the server from a binary snapshot:

```bash
cd /path/to/osm_project
OSM_OLLAMA_MODEL=qwen2.5:3b \
OSM_OLLAMA_HOST=127.0.0.1 \
OSM_OLLAMA_TIMEOUT_SECONDS=30 \
/path/to/osm_project-build/osm_parser \
  --load-binary /path/to/output/europe-geocoder.bin \
  --server 8080
```

Open:

```text
http://localhost:8080
```

To run without automatic Ollama startup:

```bash
cd /path/to/osm_project
OSM_AUTO_OLLAMA=0 \
/path/to/osm_project-build/osm_parser \
  --load-binary /path/to/output/europe-geocoder.bin \
  --server 8080
```

Stop the server with `Ctrl+C`. If the OSM server started Ollama itself, it stops that managed Ollama process during shutdown. If Ollama was already running before the OSM server started, the OSM server leaves it running.

## 8. Quick API Checks

Run these in another Ubuntu terminal while the server is running.

Server stats:

```bash
curl "http://127.0.0.1:8080/api/stats"
```

Deterministic forward geocoder:

```bash
curl "http://127.0.0.1:8080/api/geocode?q=Berlin&limit=3"
```

Reverse geocoder:

```bash
curl "http://127.0.0.1:8080/api/reverse?lat=48.7758&lon=9.1829"
```

LLM natural-language geocoder with viewport origin:

```bash
curl "http://127.0.0.1:8080/api/natural-geocode?q=Where%20can%20I%20buy%20milk%3F&useLlm=1&lat=48.7758&lon=9.1829&limit=3"
```

The last query has no explicit city or address, so the backend can use the supplied `lat` and `lon` as the viewport origin. The response reports this through `originSource`.

## 9. What The Program Extracts

The parser reads the three main OSM primitive types:

- `node`: one coordinate with tags.
- `way`: an ordered list of node IDs plus tags.
- `relation`: a group of nodes, ways, or other relations with member roles.

The reduced dataset stores:

- houses and address objects as representative points
- streets as line strings
- administrative areas as polygons with outer and inner rings
- selected POIs as searchable points
- compact string references
- geometry coordinate arrays
- admin links for houses, streets, POIs, and admin parents
- an embedded forward-geocoder index

The parser skips OSM metadata that is not needed for this program, such as version, user ID, timestamp, and changeset. It does not store the full OSM database.

Only one metadata language is selected at a time. The default language is `de`.

## 10. Houses And Address Points

In this program, a house means an address object. It can be a real building, an address node, or an addressed building relation.

Stored address fields:

- `addr:housenumber`
- `addr:street`
- `addr:postcode`
- `addr:city`
- `addr:country`

Address sources:

- address nodes: stored directly as one point
- addressed building ways: reduced to one representative point
- addressed building relations: assembled from member ways and reduced to one representative point

For closed building ways, the representative point is the polygon centroid. For incomplete geometry, the parser uses the average of available vertices.

The parser keeps incomplete address records when at least one useful address tag exists. A house with only `addr:street` or only `addr:housenumber` is still stored. These weaker records are useful for map display, reverse geocoding, and data quality statistics.

## 11. Streets And Street Connection

The parser stores street ways as coordinate line strings.

Street identity is taken from the first available value:

1. `name`
2. `official_name`
3. `ref`

The `ref` field matters for roads whose route reference is their usable label, such as motorway, federal-road, and state-road numbers.

The parser skips area-like highway objects because a plaza polygon is not a street centerline. It also skips highway types that are not useful for the displayed street layer, such as footways, cycleways, steps, platforms, corridors, proposed roads, and construction roads.

Some unnamed connector roads are kept when they help visible street continuity. This includes selected link roads, residential connectors, unclassified roads, living streets, roads, and unnamed roundabouts that touch named streets.

`--connect-streets` merges street segments only when the match is conservative:

- same street label
- same highway type
- endpoints touch exactly
- branches stay separate instead of being forced into one artificial line

The connection step builds endpoint records, sorts them by identity and coordinate, finds connected components, and writes merged street chains.

## 12. Administrative Areas And Holes

Administrative areas are stored from:

- closed administrative boundary ways
- administrative boundary relations

Admin levels `2` to `12` are kept.

Common meanings:

- `2`: country
- `4`: state or large region
- `6`: district or county
- `8`: municipality, city, or town
- `9` to `12`: suburb, quarter, neighborhood, or smaller local area

Relation geometry stores both outer and inner rings. Inner rings are holes. During point-in-polygon lookup, a point is inside an admin area only when it is inside at least one outer ring and not inside an inner ring for the same area.

Admin lookup links:

- houses to containing admin areas
- streets to admin areas touched by street geometry points
- POIs to containing admin areas
- smaller admin areas to larger parent admin areas

The polygon test uses bounding boxes, grid pruning, latitude edge indexing, and a local longitude scaling by `cos(latitude)`. This keeps checks faster and avoids treating longitude as a flat distance everywhere.

## 13. POIs

POIs are selected searchable map objects. They are used by deterministic place search and by natural-language product/service queries.

The POI index can use fields such as:

- `name`
- `brand`
- `operator`
- `shop`
- `amenity`
- `craft`
- `office`
- `tourism`
- `leisure`

Final POI results always come from the loaded PBF-derived dataset. The program does not create fake shops or fake coordinates.

## 14. Reverse Geocoder

The reverse geocoder answers:

```text
What useful stored object is closest to this clicked coordinate?
```

Endpoint:

```text
/api/reverse?lat=<latitude>&lon=<longitude>
```

The server builds runtime spatial grids over houses, streets, and administrative areas. A click checks nearby grid cells instead of scanning all objects.

Selection order:

1. direct house click near an address point
2. direct street click near a street line
3. close street
4. close house
5. wider nearby-house search
6. containing administrative area
7. nearest administrative boundary when outside all stored areas

The response can include:

- clicked coordinate
- best result type
- closest house
- nearest street
- address fields
- matched administrative areas
- distances
- query time

## 15. Forward Geocoder

The forward geocoder accepts four useful query shapes:

```text
<street name> <house number> <place or postcode>
<street name> <place>
<place name>
<POI or brand name> <place>
```

Endpoint:

```text
/api/geocode?q=<query>&limit=<n>
```

The forward-geocoder index is built during PBF parsing and saved inside the binary snapshot. Loading the snapshot does not rebuild the forward index from scratch.

The index contains:

- house address records
- street records
- administrative area records
- selected POI records

Two posting indexes are stored:

- context index: includes surrounding admin names for full address queries
- primary-field index: uses object-owned fields first for simple name queries

This prevents a single-token query such as `Stuttgart` from returning every house located inside Stuttgart before returning the city or direct POI/name matches.

### 15.1 Structured Address Planning

A numbered query is parsed as an address instead of being matched as one undivided phrase:

1. numeric tokens are separated as the house number or postcode
2. exact administrative-place tokens are resolved from the loaded admin index
3. the remaining ordered words become the street phrase
4. street candidates must match that phrase and the requested administrative context
5. house candidates must match the street, number, and administrative context
6. a house result is returned only when the stored OSM address fields agree

The program never invents a house coordinate and never substitutes a different house number. If the street exists but the exact house point is absent, the mixed `All` view and the `Streets` view may return the matching street with an explicit street-level status. The `Houses` view remains empty because it is restricted to exact stored house records.

A query containing a number and only a recognized place name is incomplete because no street phrase remains. The endpoint rejects that shape with `address query needs a street name` instead of treating the place name as a street.

### 15.2 Result-Type Controls

The GUI offers `All`, `Houses`, `Streets`, `Admin`, and `POIs` controls. `All` preserves the mixed ranking. A selected type restricts the final candidate set after the normal scoring rules have run, so each view keeps the same text and context checks while showing only the requested object class.

Text normalization:

- lowercase matching
- punctuation becomes spacing
- German street variants normalize toward `strasse`
- German umlaut spellings normalize toward ASCII forms such as `ae`, `oe`, and `ue`
- duplicate tokens are removed

## 16. Substring And Fuzzy Search

Substring search expands query fragments to indexed tokens. It uses suffix entries built from the vocabulary.

Fuzzy search is bounded:

- tokens shorter than 4 characters are not fuzzy-expanded
- medium tokens allow small edit distance
- longer tokens allow a slightly larger edit distance
- vocabulary scanning is capped

Both features only expand to tokens already present in the local index. They do not invent new places.

Direct, substring, and fuzzy posting references are merged and deduplicated before object scoring. The final ranker then verifies the match against the object's own fields. Exact matches rank above prefixes, prefixes above contained substrings, and valid fuzzy corrections below those stronger relationships. Important administrative places can therefore outrank incidental POI text when the corrected token is a stronger place-name match.

Expansion is bounded by token length, edit distance, vocabulary seeds, and posting-list size. These limits prevent a short or common fragment from creating an unbounded Europe-wide candidate set.

## 17. Natural-Language Search With Ollama

Natural-language search is exposed through:

```text
/api/natural-geocode
```

The GUI uses this endpoint when the checkbox `Use LLM for natural-language query` is enabled.

Ollama does not return final places. It only drafts structured intent JSON. The backend then validates the intent and searches the local PBF-derived indexes.

The Ollama flow:

1. user writes a natural sentence
2. backend sends it to the local Ollama model
3. Ollama drafts strict JSON intent
4. Ollama verifies that JSON in two local verification passes
5. backend validates the schema and allowed intent
6. backend checks the local knowledge graph
7. backend searches the PBF-derived index
8. backend ranks real stored OSM objects

If the query has an explicit place or address, that text is used as the origin. If the query has no explicit place, the frontend sends the current map center, and the backend can use it as a viewport origin.

Named brands are handled generically. Ollama may identify a brand or named POI, but the backend accepts results only when the loaded snapshot contains matching `name`, `brand`, or selected `operator` text. Generic category words are kept as categories rather than being mistaken for brand names.

Natural queries can also describe one category relative to another, such as a target product or service near a generic reference category. The reference category is resolved to real local POIs, bounded nearest anchors are selected, and target POIs are ranked by graph relevance, text relevance, distance to the closest anchor, explicit place context, and viewport context. The target remains the result; the reference POI only explains the spatial relationship.

The GUI shows the model's one-sentence interpretation, the final Ollama JSON after two verification passes, the backend-accepted JSON, and the number of completed checks. This is a compact intent explanation, not hidden chain-of-thought. The backend JSON is the authoritative record of the search that was executed.

## 18. Local Knowledge Graph

The knowledge graph is a local set of typed concepts and allowed connections. It is not a remote database and not an LLM memory.

Node types:

- product concept: medicine, bread, groceries, electronics, fuel
- service concept: bike repair, document printing, eating, charging, parking, banking
- brand concept: a chain or company name that may appear in `name`, `brand`, or `operator`
- category concept: restaurant, fast food, pharmacy, hotel, fuel, park, museum
- place/origin concept: typed address, typed city, map center, viewport
- OSM tag concept: `shop=*`, `amenity=*`, `tourism=*`, `leisure=*`, `name`, `brand`, `operator`

Edge meanings:

- product likely sold at category
- service provided by category
- brand matches POI text fields
- category maps to OSM tags
- place/origin limits or biases the search

The graph can map a product concept to likely store categories and then to OSM tags. It cannot prove live shelf inventory. The program returns likely real OSM places, not guaranteed product stock.

Brand handling is mostly generic. The backend searches indexed `name`, `brand`, and `operator` fields instead of maintaining a huge brand table. A small amount of deterministic cleanup is used for obvious variants such as `McDonalds`, `H&M`, and `C&A`.

## 19. Ranking

Ranking combines several signals:

- object type
- exact text match
- token match count
- house number match
- postcode match
- street-name match
- admin/place context
- POI name, brand, operator, and category relevance
- graph relevance for natural-language queries
- distance from resolved origin
- viewport/map-center relevance when no typed origin exists

For address queries, exact house numbers and street names are strong signals.

For place queries, admin areas and direct POI/name matches are stronger than houses that merely sit inside a place.

For natural product/service queries, the backend first chooses valid local POI candidates through the graph and then ranks by text, category, distance, and place context.

For reference-category queries, distance to the closest accepted reference POI is part of the score. A typed place or address has priority over the current map view. The viewport is used when no usable explicit origin is present; it is a search context, not proof that every valid result must lie inside the visible rectangle.

## 20. Binary Snapshot Format

The binary snapshot stores the reduced dataset and the embedded forward-geocoder index.

It includes:

- strings
- houses
- streets
- POIs
- administrative areas
- admin rings
- shared street geometry
- shared admin geometry
- house-admin links
- street-admin links
- POI-admin links
- admin-parent links
- forward-geocoder posting lists
- forward-geocoder suffix data

The snapshot is meant for the same platform and build style that created it. A snapshot written by the Ubuntu build should be loaded by the Ubuntu build.

The loader requires the current snapshot schema, including admin ring metadata and the embedded forward index. A snapshot that does not satisfy that schema is rejected and must be recreated from the PBF.

## 21. Memory And Performance Design

The program reduces memory pressure by:

- storing coordinates as compact integer E7 values
- pooling repeated strings
- storing street/admin geometry in shared arrays
- storing offsets and sizes instead of separate geometry vectors per record
- storing admin rings separately from admin area records
- using checked integer casts for offsets and sizes
- clearing temporary string lookup maps before expensive admin attribution
- using compact needed-node storage
- using flat relation-way geometry storage
- saving the forward-geocoder index in the binary snapshot

The important parser idea is the needed-node architecture. OSM ways store node IDs, not coordinates. A simple parser would keep every node coordinate first. Europe has billions of nodes, so that approach can fill RAM. This parser first learns which node IDs are needed, sorts and deduplicates them, then scans nodes and stores only those coordinates.

With `--low-memory`, the parser uses extra sequential PBF scans to lower peak RAM. Sequential scans take time, but they are usually better than swapping.

## 22. GUI Features

The GUI is served by the C++ backend and rendered with Leaflet.

Main visual layers:

- orange dots: houses/address points
- blue lines: street ways
- green polygons: administrative areas
- purple/black highlighted markers: forward geocoder results
- black marker and line: reverse geocoder result
- dark green outlines: matched admin polygons
- light-blue markers: reference POIs used by a natural relative search
- blue dotted lines: straight visual links from each target result to its selected reference POI; these are not road routes

Sidebar features:

- dataset statistics
- cursor coordinate display
- deterministic geocoder text box
- LLM checkbox for natural-language mode
- `Start Ollama Service` button
- query time and result count
- LLM interpretation, model JSON, backend JSON, and verification count in natural mode
- `All`, `Houses`, `Streets`, `Admin`, and `POIs` result controls
- ranked result list
- reverse-geocoder click lookup toggle
- clear reverse result button

Viewport loading is bounded. The frontend requests houses, streets, and admin areas for the current map view instead of downloading the whole dataset at once.

Admin geometry endpoints support `maxPoints=<n>` and simplified geometry responses, so large polygons do not freeze the browser.

Clicking a ranked result does not rerun the query. It highlights the selected marker or street, moves the map to that result, and leaves the other ranked results visible for comparison.

## 23. HTTP API Endpoints

Main endpoints:

```text
/api/stats
/api/geocode?q=<query>&limit=<n>
/api/natural-geocode?q=<query>&useLlm=1&lat=<lat>&lon=<lon>&limit=<n>
/api/reverse?lat=<lat>&lon=<lon>
/api/houses?bbox=<south>,<west>,<north>,<east>&limit=<n>
/api/streets?bbox=<south>,<west>,<north>,<east>
/api/admin?bbox=<south>,<west>,<north>,<east>&maxPoints=<n>
/api/admin-area?index=<n>&maxPoints=<n>
/api/ollama/start
```

`/api/stats` includes dataset counts, parse timings, spatial-index metrics, forward-index metrics, and estimated forward-index memory.

`/api/geocode` is deterministic and does not call Ollama.

`/api/natural-geocode` can call Ollama when `useLlm=1`.

`/api/ollama/start` starts or restarts the configured local Ollama service from the backend.

## 24. Command Line Options

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
--help                        Show command line help
```

## 25. Ollama Environment Variables

```text
OSM_AUTO_OLLAMA=0             Disable automatic Ollama startup
OSM_OLLAMA_BIN=<path>         Ollama executable path if it is not on PATH
OSM_OLLAMA_MODEL=<name>       Model name, recommended: qwen2.5:3b
OSM_OLLAMA_HOST=<host>        Ollama host, default localhost
OSM_OLLAMA_PORT=<port>        Ollama port, default 11434
OSM_OLLAMA_TIMEOUT_SECONDS=n  Timeout for Ollama generation calls
OSM_OLLAMA_WARMUP=0           Disable model warmup at server startup
```

Recommended demo values:

```bash
OSM_OLLAMA_MODEL=qwen2.5:3b
OSM_OLLAMA_HOST=127.0.0.1
OSM_OLLAMA_TIMEOUT_SECONDS=30
```

## 26. Console Progress And Metrics

The parser prints phase-aware progress and final metrics.

Important phases:

- administrative relation scan
- street endpoint recovery scan
- needed-node collection
- node coordinate scan
- address and boundary extraction
- street line extraction
- boundary assembly
- administrative attribute lookup
- street connection
- forward-geocoder index build
- binary snapshot writing

Important counters:

- scanned nodes, ways, and relations
- houses from nodes, ways, and relations
- houses missing street
- houses missing number
- stored streets
- stored POIs
- admin areas from ways and relations
- house-admin links
- street-admin links
- POI-admin links
- admin-parent links

Important timings:

- boundary relation scan
- dataset extraction
- boundary assembly
- admin attribute lookup
- street connection
- forward-index build
- binary writing
- PBF extraction total

Important memory lines:

- estimated dataset storage
- PBF extraction peak RSS
- estimated embedded forward-index storage

`RSS` means resident set size, the part of the process that is actually resident in RAM. The
stored PBF extraction peak does not describe a later server run. The storage estimates add the
capacities of the dataset and forward-index containers; they exclude allocator bookkeeping,
the runtime spatial hash indexes, HTTP state, the operating-system page cache, and Ollama.
Use the operating system's process RSS when measuring the complete live server.

## 27. GeoJSON Export

Export a bounded sample for inspection:

```bash
/path/to/osm_project-build/osm_parser \
  --parse /path/to/data/baden-wuerttemberg-latest.osm.pbf \
  --geojson /path/to/output/baden-sample.geojson
```

Export with explicit limits:

```bash
/path/to/osm_project-build/osm_parser \
  --parse /path/to/data/baden-wuerttemberg-latest.osm.pbf \
  --geojson /path/to/output/baden-sample.geojson \
  --geojson-houses 10000 \
  --geojson-streets 2000 \
  --geojson-admin 300
```

## 28. Program Manual

```text
docs/osm_program_manual.pdf
```

This manual explains how the final program works from PBF input to browser output. It covers houses, streets, administrative areas, POIs, reverse geocoding, structured address resolution, substring and fuzzy search, Ollama natural-language search, the local knowledge graph, ranking, binary snapshots, memory design, and the GUI.

Use it when you want a step-by-step explanation of the implementation.

## 29. Main Source Files

```text
CMakeLists.txt                         build configuration
include/data_model.h                   compact records and indexes
include/parser.h                       parser interface
include/geocode_index.h                forward-geocoder index interface
include/server.h                       server interface
src/main.cpp                           command line handling
src/parser.cpp                         parser entry file
src/geocode_index.cpp                  forward index, substring, fuzzy helpers
src/parser_parts/core_types.cpp        parser helper types
src/parser_parts/tag_rules.cpp         OSM tag rules
src/parser_parts/geo_lookup.cpp        geometry and admin lookup
src/parser_parts/node_store.cpp        compact needed-node store
src/parser_parts/runtime_io.cpp        memory and binary helpers
src/parser_parts/pbf_scans.cpp         PBF scan handlers
src/parser_parts/street_merge.cpp      street connection
src/parser_parts/parser_flow.cpp       parser command flow and snapshot IO
src/server.cpp                         HTTP API, reverse geocoder, natural search
frontend/index.html                    Leaflet GUI
docs/osm_program_manual.pdf            illustrated explanation
third_party/README.md                  bundled dependency notes
```

The parser part files are included by `src/parser.cpp`. This keeps one compiled parser translation unit while making the implementation easier to navigate.

## 30. Troubleshooting

### 30.1 `Binary snapshot has no embedded forward index`

The file does not contain the current embedded forward-index schema. Reparse the PBF and save a new `.bin` with the current executable.

### 30.2 `Binary snapshot not found`

Check the path:

```bash
ls -lh /path/to/output/europe-geocoder.bin
```

Then start the server with the same path.

### 30.3 Ollama command not found

Install Ollama:

```bash
curl -fsSL https://ollama.com/install.sh | sh
```

Then check:

```bash
ollama --version
```

If Ollama is installed but not on `PATH`, set:

```bash
OSM_OLLAMA_BIN=/path/to/ollama
```

### 30.4 Ollama port already in use

The server can use an existing Ollama process. If you want the frontend button to restart Ollama, use the `Start Ollama Service` button.

Manual check:

```bash
curl http://127.0.0.1:11434/api/tags
```

### 30.5 Server looks frozen after loading a binary

The server builds runtime spatial indexes before printing the localhost URL. For Europe, this can still take time, but the forward-geocoder index is loaded from the snapshot and should not be rebuilt from scratch.

### 30.6 Europe parse slows down during a phase

The progress speed is an average over the phase. CPU-heavy geometry work, disk cache changes, memory pressure, and WSL filesystem speed can make the displayed throughput drop. Keep the PBF and build under `/path/to`, use `--low-memory`, and reduce `--pbf-threads` if swap pressure starts.

### 30.7 Browser shows zero LLM results

Check these in order:

1. Ollama is installed.
2. `qwen2.5:3b` is installed.
3. The server was started with the Ollama environment variables.
4. The GUI checkbox `Use LLM for natural-language query` is enabled.
5. The query has enough context, or the map is centered near the area you want.

### 30.8 No product inventory guarantee

The local knowledge graph can map a product to likely OSM categories. It cannot know live store inventory. For example, it can search pharmacies or chemists for cough syrup, but it cannot prove that one specific shelf currently contains cough syrup.

## 31. Quick Command Template

These commands use placeholder paths. Replace them with your local paths:

```text
repository: /path/to/osm_project
build:      /path/to/osm_project-build
Europe PBF: /path/to/data/europe-latest.osm.pbf
Europe bin: /path/to/output/europe-geocoder.bin
model:      qwen2.5:3b
```

Build:

```bash
cd /path/to/osm_project
cmake -S . -B /path/to/osm_project-build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /path/to/osm_project-build
```

Install Ollama and model:

```bash
curl -fsSL https://ollama.com/install.sh | sh
ollama pull qwen2.5:3b
```

Parse Europe:

```bash
cd /path/to/osm_project
/path/to/osm_project-build/osm_parser \
  --parse /path/to/data/europe-latest.osm.pbf \
  --low-memory \
  --connect-streets \
  --save-binary /path/to/output/europe-geocoder.bin \
  --pbf-threads 24
```

Start the Europe GUI:

```bash
cd /path/to/osm_project
OSM_OLLAMA_MODEL=qwen2.5:3b \
OSM_OLLAMA_HOST=127.0.0.1 \
OSM_OLLAMA_TIMEOUT_SECONDS=30 \
/path/to/osm_project-build/osm_parser \
  --load-binary /path/to/output/europe-geocoder.bin \
  --server 8080
```

Open:

```text
http://localhost:8080
```
