# Third-Party Headers

This folder contains the third-party headers needed by the OSM parser.

Included dependencies:

- `cpp-httplib`: single-header HTTP server for the Leaflet backend
- `libosmium`: reads OSM objects and PBF input
- `protozero`: used by libosmium while decoding PBF data

These dependencies are used as header-only code here. The executable still links against normal Ubuntu system libraries for compression, XML support, and threads:

- zlib
- bzip2
- expat
- pthreads or the system thread library

Only the required header subset is copied. A separate parser folder is not needed to build this project.

The license files stay next to the copied headers because they are third-party legal text and should not be rewritten.
