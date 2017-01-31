# Introduction to OSMLR

## What's the problem we're trying to solve?

As part of the OpenTraffic project, we are trying to build an open and accessible traffic framework for global use which allows individuals and fleet operators to get the benefits of access to traffic data while participating in a transparent and privacy-respecting system.

At its core, traffic data is all about current or average speeds along a section of road. Specifying speed is easy, once a choice of units has been made, but specifying a section of road is more complex. There are many reasons for this:

* We are using data from OpenStreetMap, which provides a detailed, global road network with the same position on openness and accessibility that OpenTraffic aspires to. However, the OpenStreetMap (abbreviated to OSM) data is constantly updated as myriad people around the world contribute to it every minute of the day. While it would be possible for OpenTraffic to follow the same approach, we felt that there were benefits to having a slower, more controlled update process.
* Although we are using data from OSM, many individuals and fleet operators who wish to contribute to the project may want to use their own local datasets, and we want to make it possible for them to be part of OpenTraffic. This means that, although we construct our references to road sections from OSM data, it should be possible to find the section of road on a different map.
* Different datasets have different methods of splitting up roads into smaller parts, often to satisfy different requirements about their data model. For specifying traffic data, there is a balance between making road segments as long as possible to decrease the space that traffic data takes up, and making road segments shorter to increase the level of detail in the data. Getting this right for OpenTraffic will require a different method of splitting up roads than any other dataset has chosen.
* To keep data usage to a minimum, which may be important for sending traffic data to mobile devices, we want OSMLR descriptors and identifiers to be compact.

## How does OSMLR solve the problem?

To specify a section of road, OSMLR describes it as a series of "Location Reference Points" (LRPs) plus a final coordinate. Each LRP contains information about the road at that point and along to the next LRP, which help to uniquely identify the road section. The LRPs can be quite widely spaced as long as the route between two LRPs is uniquely identified by the endpoints and the properties attached to the LRP.

Those properties are:

* Bearing along the road from the LRP. Specifically, this is taken as the bearing from the LRP to the point 20m along the road from the LRP in the direction of the next coordinate. This helps to find the right section of road at junctions where many sections may start.
* Road class at the LRP. This helps to identify which of several roads meeting at a point, or flying over or under each other, is intended by the descriptor.
* "Form of Way" at the LRP. This also helps to identify the particular road which is referred to from the potentially many which might be found at a given point.
* Least road class between the LRP and the next coordinate. This tries to reduce ambiguity if multiple paths exist between the LRP and the next coordinate. For example, if the shortest path between the LRP and the next coordinate takes a detour away from the highway, but the least road class is set to highway, then we know that we must adjust the path.
* Length of the road between the LRP and the next coordinate. This avoids matching long detours if roads are missing in the dataset being matched to.

## Relationship to OpenLR

These problems have already been faced and a solution found by the OpenLR project. Although OpenLR deals with many kinds of location references, OpenTraffic only needs to specify linear references - i.e: sections of roads.

Therefore, the scheme described above is based on OpenLR with changes where appropriate to make it easier to work with OSM data.

## Creating OSMLR descriptors

This software, `osmlr`, creates OSMLR descriptors from Valhalla road data tiles. It starts by merging across adjacent Valhalla graph edges which distinguish properties which are important to Valhalla's routing algorithms, but not to the collection of traffic data. For example, Valhalla keeps separate edges for bridges and tunnels as these can have important restrictions for traffic. However, since the start of a bridge or tunnel does not represent a point at which the driver of a vehicle has choice - if there is no junction - then for traffic purposes it can be counted as the same section of road.

For each merged sequence of edges, OSMLR will calculate the descriptor and output it in a tile. Calculating the descriptor involves splitting up the merged sequence into a set of LRPs such that the distance between neighbouring LRPs is at most 15km. The properties are then calculated at each LRP, and stored in the descriptor.

Making sure that descriptors match only a single road is important, but currently unimplemented. This would mean matching the descriptor back to the road network and checking that it matched the original edges. Further, it is important to be _more_ specific than necessary for the input dataset, as there may be differences between the input dataset from OSM and the same area at a later date, or a different source of data.

## OSMLR descriptor packaging and distribution

OSMLR descriptors are packaged into separate levels of tiles, just like Valhalla road data tiles, which means that users can download only the subset of the data that they need. The descriptors and tiles are described in a Google Protocol Buffers description. This makes the output files relatively compact, gives a clear path for upgrades to the format and makes it easier to construct tools and libraries to access that data from many different programming languages.

Keeping OSMLR IDs compact is important, as these IDs are used in many places to look up traffic data or to keep correspondences between datasets. To ensure that OSMLR IDs are compact while allowing for them to change over time, OSMLR IDs are assigned by a central process. When an updated set of OSMLR descriptors is wanted, the central process will match the previous set of descriptors against the new data and retain any which still match. This means that new IDs are only assigned to new roads, or ones which have undergone major changes. For major changes, the old IDs are marked as "deleted" so that the association between a road section and an OSMLR descriptor is kept unique for any given data release.

## Matching OSMLR descriptors to a map dataset

OSMLR descriptors can be matched to a map dataset by taking the LRPs in each descriptor and making a shortest-distance route from each to the next coordinate, biasing in favour of routes which head in the right bearing. If several routes are candidates, then choose the one which most closesly matches the descriptor.

This can be somewhat fuzzy, and there may be more than one match. In these cases, it may require some tie-breaking to figure out which is the best match. Descriptors may not overlap, so it's possible to discard any part of the road network which was already matched to a descriptor. Indeed, descriptors which do not have a good candidate match - or have several - might be best postponed until all other good matches have been made to reduce the number of candidates. In general, a more major road such as a highway would make a better match than a residential road, but the circumstances may vary with the characteristics of the individual dataset.

Many of the descriptors will not be fuzzy, and will closely match a single segment of road. In these cases, optimisations are possible so as to not need to run a full route for each match. Running a route can sometimes be expensive in computer time, so avoiding the it whenever possible is recommended.

The first case for optimisation is when the target dataset has an edge in its routing network which corresponds exactly to the descriptor. These can be found easily when the source and destination points are the start and end nodes of an edge in the graph. While this may lead to spurious matches where there is also a shorter route, we rely on the extra attributes in the OSMLR descriptor - the bearing, length, road class, and so forth - to filter these out and ensure that only valid matches will be found.

This case can be extended for a sequence of edges. The edges leaving the start node can be "walked" to the next edges until either the end node is found, or a junction is encountered. Again, we rely on the properties of the descriptor to find candidate edges and filter out spurious matches.

It is possible that several edges will match one descriptor, or that a single descriptor might only match a part of an edge. We call such collections of edges and decriptors "chunks". Each chunk consists of multiple edges, each of which has several correspondences to ranges along some descriptors. A single edge might match the first 50% of its length to one descriptor and the remaining 50% to a different one, for example.

After the matching process is complete, this association between edges and OSMLR descriptors is saved to a data structure which handles both the common 1:1 match in a compact and efficient manner, but also allows for the more complex chunks. This consists of an array of OSMLR IDs indexed by the edge ID within a tile. There are some flag bits stored along with the OSMLR IDs which determine whether it is treated as a single ID for the common 1:1 case, or as an index into a second array of objects which describe the details of the chunk.
