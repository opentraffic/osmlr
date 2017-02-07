OSMLR
-----

Applications for generating linear references (traffic segments) given OSM data in the form of Valhalla routing tiles.

Build Status
------------

[![Build Status](https://travis-ci.org/opentraffic/osmlr.svg?branch=master)](https://travis-ci.org/opentraffic/osmlr)

Building and Running
--------------------

To build, install and run on Ubuntu (or other Debian based systems) try the following bash commands:

```bash
#get dependencies
sudo add-apt-repository ppa:valhalla-routing/valhalla
sudo apt-get update
sudo apt-get install libvalhalla-dev valhalla-bin

#download some data and make tiles out of it
wget YOUR_FAV_PLANET_MIRROR_HERE -O planet.pbf

#get the config and setup for it
wget https://raw.githubusercontent.com/valhalla/conf/master/valhalla.json
sudo mkdir -p /data/valhalla
sudo chown `whoami` /data/valhalla
rm -rf /data/valhalla/*

#build routing tiles
#TODO: run valhalla_build_addmins?
LD_LIBRARY_PATH=/usr/lib:/usr/local/lib valhalla_build_tiles -c valhalla.json planet.pbf

#tar it up for running the server
find /data/valhalla/* | sort -n | tar cf /data/valhalla/tiles.tar --no-recursion -T -

#make some osmlr segments
LD_LIBRARY_PATH=/usr/lib:/usr/local/lib osmlr /data/valhalla/tiles.tar

#HAVE FUN!
```

Documentation
-------------

You can find an [introduction](docs/intro.md) to OSMLR and the concepts behind it in the `docs/` directory. If there's something that you've found tricky to understand in OSMLR, then please open an [issue](https://github.com/opentraffic/osmlr/issues/new) and provide as much detail as possible. Or, even better, submit a [pull request with your improvements](https://github.com/opentraffic/osmlr/compare)!
