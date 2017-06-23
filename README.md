OSMLR
-----

Applications for generating linear references (traffic segments) given OSM data in the form of Valhalla routing tiles.

Related:
- [Scripts to download prebuilt OSMLR segment tiles from S3](py/README.md)
- [OSMLR Tile Specification](https://github.com/opentraffic/osmlr-tile-spec): Protocol Buffer definition files used by this generator application, as well as consumers of OSMLR segments

Build Status
------------

[![Build Status](https://travis-ci.org/opentraffic/osmlr.svg?branch=master)](https://travis-ci.org/opentraffic/osmlr)

Building and Running
--------------------

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

# rebuild tar with traffic segement associated tiles
find valhalla_tiles | sort -n | tar rf tiles.tar --no-recursion -T -

#HAVE FUN!
```

Documentation
-------------

You can find an [introduction](docs/intro.md) to OSMLR and the concepts behind it in the `docs/` directory. If there's something that you've found tricky to understand in OSMLR, then please open an [issue](https://github.com/opentraffic/osmlr/issues/new) and provide as much detail as possible. Or, even better, submit a [pull request with your improvements](https://github.com/opentraffic/osmlr/compare)!
