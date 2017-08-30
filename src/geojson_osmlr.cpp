#include <future>
#include <thread>
#include <mutex>
#include <vector>
#include <queue>
#include <sys/stat.h>
#include <fstream>
#include <unistd.h>

#include <valhalla/midgard/logging.h>
#include <valhalla/baldr/graphreader.h>
#include <valhalla/baldr/tilehierarchy.h>
#include <osmlr/util/tile_writer.hpp>

#include <boost/program_options.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "segment.pb.h"
#include "tile.pb.h"
#include "config.h"

namespace vm = valhalla::midgard;
namespace vb = valhalla::baldr;
namespace util = osmlr::util;
namespace pbf  = opentraffic::osmlr;

namespace bpo = boost::program_options;
namespace bpt = boost::property_tree;

inline bool exists(const std::string& name) {
  struct stat buffer;
  return (stat(name.c_str(), &buffer) == 0);
}

std::string get_osmlr_tilename(const std::string& osmlr_dir,
                               const vb::GraphId& tile_id) {
  std::string osmlr_file = osmlr_dir + "/" + vb::GraphTile::FileSuffix(tile_id);
  osmlr_file.erase(osmlr_file.length()-3);
  osmlr_file += "osmlr";
  return osmlr_file;
}

// Output a segment that is part of an edge.
void output_segment(std::ostringstream& out,
                    const vb::GraphId& osmlr_id,
                    const vb::DirectedEdge* edge,
                    const std::vector<vm::PointLL>& shape) {
  out << "{\"type\":\"Feature\",\"geometry\":";
  out << "{\"type\":\"MultiLineString\",\"coordinates\":[";
  out << "[";
  bool first_pt = true;
  for (const auto pt : shape) {
    if (first_pt) { first_pt = false; } else { out << ","; }
    out << "[" << pt.lng() << "," << pt.lat() << "]";
  }
  out << "]";

  bool first = true;
  bool oneway = (edge->reverseaccess() & vb::kVehicularAccess) == 0;
  out << "]},\"properties\":{"
      << "\"tile_id\":" << osmlr_id.tileid() << ","
      << "\"level\":" << osmlr_id.level() << ","
      << "\"id\":" << osmlr_id.id() << ","
      << "\"osmlr_id\":" << osmlr_id.value << ","
      << "\"best_frc\":\"" << vb::to_string(edge->classification()) << "\","
      << "\"oneway\":" << oneway << ","
      << "\"drive_on_right\":" << edge->drive_on_right();
  out << "}}";
}

/**
 * Create GeoJSON for an OSMLR tile.
 */
void create_geojson(std::queue<vb::GraphId>& tilequeue,
                    util::tile_writer& writer,
                    const boost::property_tree::ptree& hierarchy_properties,
                    const std::string& osmlr_dir, std::mutex& lock) {
printf("Create GeoJSON\n");
  // Local Graphreader
  vb::GraphReader reader(hierarchy_properties);

  // Iterate through the tiles in the queue and perform enhancements
  while (true) {
    // Get the next tile Id from the queue and get writeable and readable
    // tile. Lock while we access the tile queue and get the tile.
    lock.lock();
    if (tilequeue.empty()) {
      lock.unlock();
      break;
    }
    vb::GraphId tile_id = tilequeue.front();
    tilequeue.pop();
    lock.unlock();

    // Get a Valhalla tile. If the tile is empty, skip it.
    const vb::GraphTile* tile = reader.GetGraphTile(tile_id);
    if (tile->header()->directededgecount() == 0) {
      continue;
    }

    // Read the OSMLR pbf tile
    pbf::Tile pbf_tile;
    std::string file_name = get_osmlr_tilename(osmlr_dir, tile_id);
    std::ifstream in(file_name);
    if (!pbf_tile.ParseFromIstream(&in)) {
      throw std::runtime_error("Unable to parse traffic segment file.");
    }
    uint32_t creation_date = pbf_tile.creation_date();
    const time_t t(creation_date);
    std::tm tm = *std::gmtime(&t);
    std::locale::global(std::locale("C"));
    char mbstr[100];
    std::strftime(mbstr, sizeof(mbstr), "%c %Z", &tm);
    std::string date_str = std::string(mbstr);
    uint64_t osm_changeset_id = pbf_tile.changeset_id();

    // Create the GeoJSON output stream
    std::ostringstream out;
    out.precision(17);
    out << "{\"type\":\"FeatureCollection\",\"properties\":{"
        << "\"creation_time\":" << creation_date << ","
        << "\"creation_date\":\"" << date_str << "\","
        << "\"changeset_id\":" << osm_changeset_id << "},";
    out << "\"features\":[";

    // Iterate through the Valhalla directed edges. Find edges that start an
    // OSMLR segment or that include "chunks".
    bool first = true;
    for (uint32_t n = 0; n < tile->header()->directededgecount(); ++n) {
      auto segments = tile->GetTrafficSegments(n);
      if (segments.empty()) {
        continue;
      }

      // Get the directed edge and the shape
      const vb::DirectedEdge* edge = tile->directededge(n);
      auto edgeinfo_offset = edge->edgeinfo_offset();
      std::vector<vm::PointLL> shape = tile->edgeinfo(edgeinfo_offset).shape();
      if (!edge->forward()) {
        std::reverse(shape.begin(), shape.end());
      }

      // Simple case - one segments that begins and ends on this edge
      if (segments.size() == 1 && segments.front().starts_segment_ &&
          segments.front().ends_segment_) {
        vb::GraphId segment_id = segments.front().segment_id_;
        if (!first) {
          out << ",";
        }
        output_segment(out, segment_id, edge, shape);
        first = false;
      } else {
        // Need to handle case where the segment continues onto next edge
        // and also chunk cases...
      }
    }

    // Output to file
    out << "]}";
    writer.write_to(tile_id, out.str());
  }
}

int main(int argc, char** argv) {
  bpo::options_description options("geojson_osmlr " VERSION "\n"
                                   "\n"
                                   " Usage: geojson_osmlr [options]\n"
                                   "\n"
                                   "geojson_osmlr generates GeoJSON representations of OSMLR traffic segmentst."
                                   "\n"
                                   "\n");
  uint32_t concurrency;
  uint32_t default_concurrency = std::thread::hardware_concurrency();
  std::string config;
  std::string input_dir, output_dir;
  options.add_options()
    ("help,h", "Print this help message.")
    ("version,v", "Print the version of this software.")
    ("threads,t", bpo::value<unsigned int>(&concurrency)->default_value(default_concurrency), "Concurrency, number of threads.")
    ("input_dir,i", bpo::value<std::string>(&input_dir), "Base path of OSMLR pbf tiles [required]")
    ("output_dir,o", bpo::value<std::string>(&output_dir), "Base path to use when outputting GeoJSON tiles [required]")
    // positional arguments
    ("config,c", bpo::value<std::string>(&config), "Valhalla configuration file [required]");

  bpo::positional_options_description pos_options;
  pos_options.add("config", 1);
  bpo::variables_map vm;
  try {
    bpo::store(bpo::command_line_parser(argc, argv).options(options).positional(pos_options).run(), vm);
    bpo::notify(vm);
  }
  catch (std::exception &e) {
    std::cerr << "Unable to parse command line options because: " << e.what()
              << "\n" << "This is a bug, please report it at " PACKAGE_BUGREPORT
              << "\n";
    return EXIT_FAILURE;
  }

  if (vm.count("help") || !vm.count("config")) {
    std::cout << options << "\n";
    return EXIT_SUCCESS;
  }

  if (vm.count("version")) {
    std::cout << "osmlr " << VERSION << "\n";
    return EXIT_SUCCESS;
  }

  // Verify that input and output directories have been specified
  if (input_dir.empty()) {
    LOG_ERROR("Must specify an input directory (use -i)");
    return EXIT_FAILURE;
  }
  if (output_dir.empty()) {
    LOG_ERROR("Must specify an output directory (use -o)");
    return EXIT_FAILURE;
  }
  LOG_INFO("Input OSMLR directory: " + input_dir);
  LOG_INFO("Output OSMLR GeoJSON directory: " + output_dir);

  // Parse the config
  bpt::ptree pt;
  bpt::read_json(config.c_str(), pt);

  // Configure logging
  vm::logging::Configure({{"type","std_err"},{"color","true"}});

  // A place to hold worker threads and their results, exceptions or otherwise
  uint32_t nthreads = std::max(static_cast<unsigned int>(1), concurrency);
  std::vector<std::shared_ptr<std::thread> > threads(nthreads);

  // Create a randomized queue of tiles from OSMLR pbf
  std::deque<vb::GraphId> tempqueue;
  for (auto level : vb::TileHierarchy::levels()) {
    auto level_id = level.second.level;
    auto tiles = level.second.tiles;
    for (uint32_t id = 0; id < tiles.TileCount(); ++id) {
      // Get the location/filename for the OSMLR tile
      vb::GraphId tile_id(id, level_id, 0);
      std::string osmlr_tile = get_osmlr_tilename(input_dir, tile_id);

      // If OSMLR pbf tile exists add it to the queue
      if (exists(osmlr_tile)) {
        tempqueue.push_back(tile_id);
      }
    }
  }

  std::random_shuffle(tempqueue.begin(), tempqueue.end());
  std::queue<vb::GraphId> tilequeue(tempqueue);

  // An atomic object we can use to do the synchronization
  std::mutex lock;

  // Create tile writer support
  util::tile_writer writer(output_dir, "json", 256);

  // Start the threads
  LOG_INFO("Forming GeoJSON for " + std::to_string(tilequeue.size()) + " OSMLR tiles");
  boost::property_tree::ptree hierarchy_properties = pt.get_child("mjolnir");
  for (auto& thread : threads) {
    //results.emplace_back();
    thread.reset(new std::thread(create_geojson,
                    std::ref(tilequeue),
                    std::ref(writer),
                    std::cref(hierarchy_properties),
                    std::cref(input_dir),
                    std::ref(lock)));
          //          std::ref(results.back())));
  }

  // Wait for them to finish up their work
  for (auto& thread : threads) {
    thread->join();
  }
/*
  // Check all of the outcomes, to see about maximum density (km/km2)
  enhancer_stats stats{std::numeric_limits<float>::min(), 0};
  for (auto& result : results) {
    // If something bad went down this will rethrow it
    try {
      auto thread_stats = result.get_future().get();
      stats(thread_stats);
    }
    catch(std::exception& e) {
      //TODO: throw further up the chain?
    }
  }
*/
  LOG_INFO("Done");
  return EXIT_SUCCESS;
}
