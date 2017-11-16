# OSMLR segment generation application

OSMLR segments are used as part of the [OTv2 platform](https://github.com/opentraffic/otv2-platform) to associate traffic speeds and volumes with roadways through a stable set of identifiers. This application is used to generate and update OSMLR segments. It's run approximately once a quarter and the resulting tileset of OSMLR segments are posted to Amazon S3.

The code is open-source for contribution -- but the power of OSMLR comes from everyone using a single canonical tileset produced by this program. In other words, it's recommended that you download existing OSMLR segment tiles from S3, rather than run this application yourself to create your own set.

## Using OSMLR segments

- Related code:
  - [Scripts to download prebuilt OSMLR segment tiles from S3](py/README.md)
  - [OSMLR tile specification](https://github.com/opentraffic/osmlr-tile-spec): Protocol Buffer definition files used by this generator application, as well as consumers of OSMLR segments
- Documentation:
  - [Introduction to OSMLR](docs/intro.md)
  - [Update process](docs/osmlr_updates.md)
- Blog posts:
  - [OSMLR hits a "mile marker" (and joins AWS Public Datasets)](https://mapzen.com/blog/osmlr-released-as-public-dataset/)
  - [Open Traffic technical preview #1: OSMLR segments](https://mapzen.com/blog/open-traffic-osmlr-technical-preview/)
  - [OSMLR traffic segments for the entire planet](https://mapzen.com/blog/osmlr-2nd-technical-preview/)

## Development of this application

### Build Status

[![Build Status](https://travis-ci.org/opentraffic/osmlr.svg?branch=master)](https://travis-ci.org/opentraffic/osmlr)

### Building and Running

To build, install and run on Ubuntu (or other Debian based systems) try the following bash commands:

```bash
#get dependencies
sudo apt-add-repository ppa:kevinkreiser/prime-server
sudo add-apt-repository ppa:valhalla-routing/valhalla
sudo apt-get update
sudo apt-get install libvalhalla-dev valhalla-bin

#download some data and make tiles out of it
wget YOUR_FAV_PLANET_MIRROR_HERE -O planet.pbf

#get the config and setup for it
valhalla_build_config --mjolnir-tile-dir ${PWD}/valhalla_tiles --mjolnir-tile-extract ${PWD}/valhalla_tiles.tar --mjolnir-timezone ${PWD}/valhalla_tiles/timezones.sqlite --mjolnir-admin ${PWD}/valhalla_tiles/admins.sqlite > valhalla.json

#build routing tiles
#TODO: run valhalla_build_admins?
valhalla_build_tiles -c valhalla.json planet.pbf

#tar it up for running the server
find valhalla_tiles | sort -n | tar cf tiles.tar --no-recursion -T -

#make some osmlr segments
LD_LIBRARY_PATH=/usr/lib:/usr/local/lib osmlr -m 1 -T ${PWD}/osmlr_tiles valhalla.json

# -j 2 uses two threads for association process (use more or fewer as available cores permit)
valhalla_associate_segments -t ${PWD}/osmlr_tiles -j 2 --config valhalla.json

#rebuild tar with traffic segement associated tiles
find valhalla_tiles | sort -n | tar rf tiles.tar --no-recursion -T -

#Update OSMLR segments.  
This will copy your existing pbf and geojson tiles to their equivalent output directories and update the tiles as needed.  Features will be removed add added from the feature collection in the geojson tiles.  Moreover, segements that no longer exist in the valhalla tiles will be cleared and a deletion date will be set. 
./osmlr -u -m 2 -f 256 -P ./<old_tiles>/pbf -G ./<old_tiles>/geojson -J ./<new_tiles>/geojson -T ./<new_tiles>/pbf --config valhalla.json

#HAVE FUN!
```
