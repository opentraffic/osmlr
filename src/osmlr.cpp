#include <valhalla/midgard/logging.h>
#include <valhalla/baldr/graphreader.h>
#include <valhalla/baldr/tilehierarchy.h>
#include <valhalla/baldr/merge.h>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/algorithm/string.hpp>
#include <time.h>

#include "config.h"
#include "osmlr/output/output.hpp"
#include "osmlr/output/geojson.hpp"
#include "osmlr/output/tiles.hpp"

namespace vm = valhalla::midgard;
namespace vb = valhalla::baldr;

namespace bpo = boost::program_options;
namespace bpt = boost::property_tree;
namespace bra = boost::adaptors;
namespace bfs = boost::filesystem;

// Use this method when determining whether edge-merging can occur at a node.
// Do not allow merging at nodes where a ferry exists or where transitions
// exist (except to local level). Also do not allow where a roundabout or
// internal intersection edge exists.
bool allow_merge_pred(const vb::DirectedEdge *edge) {
  return (!edge->trans_up() && edge->use() != vb::Use::kFerry &&
          !edge->roundabout() && !edge->internal() &&
          !(edge->trans_down() && edge->endnode().level() != 2));
}


// Use this method to determine whether an edge should be allowed along the
// merged path. Only allow road and ramp use (exclude turn channels,
// cul-de-sacs, driveways, parking, etc.) Must have vehicular access. Also
// exclude service/other classification, shortcuts, and transition edges.
bool allow_edge_pred(const vb::DirectedEdge *edge) {
  return (!edge->trans_up() && !edge->trans_down() && !edge->is_shortcut() &&
           edge->classification() != vb::RoadClass::kServiceOther &&
          (edge->use() == vb::Use::kRoad || edge->use() == vb::Use::kRamp) &&
          !edge->roundabout() && !edge->internal() &&
        !((edge->forwardaccess() & vb::kVehicularAccess) == 0 &&
          (edge->reverseaccess() & vb::kVehicularAccess) == 0));
}

struct tiles_max_level {
  typedef std::vector<vb::TileLevel> levels_t;
  levels_t m_levels;

  struct const_iterator {
    levels_t::const_iterator m_level_itr, m_levels_end;
    uint64_t m_tile_idx, m_tile_count;

    const_iterator(levels_t::const_iterator begin_,
                   levels_t::const_iterator end_)
      : m_level_itr(begin_)
      , m_levels_end(end_)
      , m_tile_idx(0) {
      if (m_level_itr != m_levels_end) {
        m_tile_count = m_level_itr->tiles.TileCount();
      }
    }

    const_iterator(const const_iterator &other)
      : m_level_itr(other.m_level_itr)
      , m_levels_end(other.m_levels_end)
      , m_tile_idx(other.m_tile_idx)
      , m_tile_count(other.m_tile_count) {
    }

    bool operator==(const const_iterator &other) const {
      return (m_level_itr == other.m_level_itr &&
              m_tile_idx == other.m_tile_idx);
    }

    inline bool operator!=(const const_iterator &other) const {
      return !operator==(other);
    }

    const_iterator &operator++() {
      m_tile_idx++;
      if (m_tile_idx >= m_tile_count && m_level_itr != m_levels_end) {
        m_level_itr++;
        m_tile_idx = 0;
        if (m_level_itr != m_levels_end) {
          m_tile_count = m_level_itr->tiles.TileCount();
        } else {
          m_tile_count = 0;
        }
      }
      return *this;
    }

    const_iterator operator++(int) {
      const_iterator ret = *this;
      ++(*this);
      return ret;
    }

    vb::GraphId operator*() const {
      return vb::GraphId(m_tile_idx, m_level_itr->level, 0);
    }
  };

  tiles_max_level(unsigned int max_level) {
    for (auto level : vb::TileHierarchy::levels() | bra::map_values) {
      if (level.level <= max_level) {
        m_levels.push_back(level);
      }
    }
  }

  const_iterator begin() const { return const_iterator(m_levels.begin(), m_levels.end()); }
  const_iterator end() const { return const_iterator(m_levels.end(), m_levels.end()); }
};

template <typename Container>
struct tile_exists_filter {
  Container m_container;
  vb::GraphReader &m_reader;

  struct const_iterator {
    typename Container::const_iterator m_itr, m_end;
    vb::GraphReader &m_reader;

    const_iterator(typename Container::const_iterator itr,
                   typename Container::const_iterator end,
                   vb::GraphReader &reader)
      : m_itr(itr)
      , m_end(end)
      , m_reader(reader) {
    }

    const_iterator(const const_iterator &other)
      : m_itr(other.m_itr)
      , m_end(other.m_end)
      , m_reader(other.m_reader) {
    }

    bool operator==(const const_iterator &other) const {
      return m_itr == other.m_itr;
    }

    inline bool operator!=(const const_iterator &other) const {
      return !operator==(other);
    }

    const_iterator &operator++() {
      do {
        m_itr++;
      } while (m_itr != m_end && !exists());
      return *this;
    }

    const_iterator operator++(int) {
      const_iterator ret(*this);
      ++(*this);
      return ret;
    }

    vb::GraphId operator*() const {
      return *m_itr;
    }

    bool exists() const {
      assert(m_itr != m_end);
      return m_reader.DoesTileExist(*m_itr);
    }
  };

  tile_exists_filter(Container &&c, vb::GraphReader &reader)
    : m_container(std::move(c))
    , m_reader(reader) {
  }

  const_iterator begin() const {
    const_iterator itr(m_container.begin(), m_container.end(), m_reader);
    const const_iterator end_ = end();
    while (itr != end_ && !itr.exists()) {
      ++itr;
    }
    return itr;
  }

  const_iterator end() const {
    return const_iterator(m_container.end(), m_container.end(), m_reader);
  }
};

bool check_access(vb::GraphReader &reader, const vb::merge::path &p) {
  // TODO: make traffic mask configurable
  int i = 0;
  uint32_t access = vb::kAllAccess;
  for (auto edge_id : p.m_edges) {
    auto edge = reader.GetGraphTile(edge_id)->directededge(edge_id);
    access &= edge->forwardaccess();

    // If the allow edge predicate is false for any edge, then drop
    // the whole path.
    if (!allow_edge_pred(edge)) {
      // Output an error if we find a disallowed edge along a multi-edge path
      if (p.m_edges.size() > 1) {
        LOG_WARN("Disallow path due to non-allowed edge. " +  std::to_string(p.m_edges.size()) +
                 " edges: i = " + std::to_string(i));
      }
      return false;
    }
    i++;
  }
  return access & vb::kVehicularAccess;
}

bool recursive_copy(const bfs::path &src, const bfs::path &dst,
                    const std::string &extension) {
  try {
    if (bfs::exists(dst)){
      LOG_WARN("Destination directory " + dst.string() + " already exists.");

      std::string doit;
      std::cout << "Delete and recreate destination directory " << dst.string() << " [Y|N]?" << std::endl;
      std::getline(std::cin,doit);
      boost::algorithm::to_upper(doit);
      if (doit != "Y")
        return false;

      bfs::remove_all(dst);
      bfs::create_directory(dst);
    }

    if (bfs::is_directory(src)) {
      bfs::create_directories(dst);
      bfs::directory_iterator dir_itr(src), end_iter;
      for (; dir_itr != end_iter; ++dir_itr)
        recursive_copy(dir_itr->path(), dst/dir_itr->path().filename(), extension);
    }
    else if (bfs::is_regular_file(src)) {
      // only grab the files that we want.
      auto ext = src.extension();
      if (ext == extension)
        bfs::copy(src, dst);
    }
    else {
      LOG_ERROR(dst.generic_string() + " not a directory or file");
      return false;
    }
  } catch (boost::filesystem::filesystem_error const & e) {
    std::cerr << "Exception checking or creating directories or files " << e.what() << std::endl;
    return false;
  }
  return true;
}

int main(int argc, char** argv) {
  bpo::options_description options("osmlr " VERSION "\n"
                                     "\n"
                                     " Usage: osmlr [options]\n"
                                     "\n"
                                     "osmlr generates traffic segment descriptors. "
                                     "\n"
                                   "\n");

  // Parse options
  unsigned int max_level, max_fds;
  std::string config;
  std::string input_osmlr_dir, input_geojson_dir, output_osmlr_dir, output_geojson_dir;
  options.add_options()
    ("input-tiles,P", bpo::value<std::string>(&input_osmlr_dir), "Required for update. The base path to use when inputting OSMLR tiles.")
    ("input-geojson,G", bpo::value<std::string>(&input_geojson_dir), "Required for update. The base path to use when inputting GeoJSON tiles.")
    ("help,h", "Print this help message.")
    ("version,v", "Print the version of this software.")
    ("max-level,m", bpo::value<unsigned int>(&max_level)->default_value(255), "Maximum level to evaluate")
    ("max-fds,f", bpo::value<unsigned int>(&max_fds)->default_value(512), "Maximum number of files to have open in each output.")
    ("output-tiles,T", bpo::value<std::string>(&output_osmlr_dir), "Required. The base path to use when outputting OSMLR tiles.")
    ("output-geojson,J", bpo::value<std::string>(&output_geojson_dir), "Required. The base path to use when outputting GeoJSON tiles.")
    ("update,u", "Optional.  Do you want to update the OSMLR data?")
    // positional arguments
    ("config", bpo::value<std::string>(&config), "Valhalla configuration file [required]");

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

  if (vm.count("update")) {
    // Make sure both input directories are present
    if (input_osmlr_dir.empty() || input_osmlr_dir == "--config") {
      LOG_ERROR("Must specify an input directory for OSMLR tiles");
      return EXIT_FAILURE;
    }
    if (input_geojson_dir.empty() || input_geojson_dir == "--config") {
      LOG_ERROR("Must specify an input directory for GeoJSON tiles");
      return EXIT_FAILURE;
    }

  } else {
    std::string doit;
    std::cout << "Are you sure you want to create new OSMLR data [Y|N]?" << std::endl;
    std::getline(std::cin,doit);
    boost::algorithm::to_upper(doit);
    if (doit != "Y")
      EXIT_SUCCESS;
  }

  // Make sure both output directories are present
  if (output_osmlr_dir.empty() || output_osmlr_dir == "--config") {
    LOG_ERROR("Must specify an output directory for OSMLR tiles");
    return EXIT_FAILURE;
  }
  if (output_geojson_dir.empty() || output_geojson_dir == "--config") {
    LOG_ERROR("Must specify an output directory for GeoJSON tiles");
    return EXIT_FAILURE;
  }

  //parse the config
  bpt::ptree pt;
  bpt::read_json(config.c_str(), pt);

  //configure logging
  vm::logging::Configure({{"type","std_err"},{"color","true"}});

  //get something we can use to fetch tiles
  vb::GraphReader reader(pt.get_child("mjolnir"));

  assert(max_level <= std::numeric_limits<uint8_t>::max());
  auto filtered_tiles = tile_exists_filter<tiles_max_level>(
    tiles_max_level(max_level), reader);

  // Get the OSM changeset Id and current date. Do this here so common across
  // all tiles.
  uint64_t osm_changeset_id = 0;
  time_t creation_date = time(nullptr);
  for (vb::GraphId tile_id : filtered_tiles) {
    const auto *tile = reader.GetGraphTile(tile_id);
    if (tile != nullptr) {
      osm_changeset_id = tile->header()->dataset_id();
      break;
    }
  }

  // Create output for OSMLR (pbf) and GeoJSON tiles
  std::shared_ptr<osmlr::output::output> output_tiles, output_geojson;
  output_tiles = std::make_shared<osmlr::output::tiles>(reader, output_osmlr_dir, max_fds,
                             creation_date, osm_changeset_id);

  output_geojson = std::make_shared<osmlr::output::geojson>(reader, output_geojson_dir, max_fds,
                             creation_date, osm_changeset_id);
  if (!output_tiles || !output_geojson) {
    LOG_ERROR("Error creating output - exiting");
    return EXIT_FAILURE;
  }

  if (vm.count("update")) {

    if (!recursive_copy(input_osmlr_dir,output_osmlr_dir, ".osmlr") ||
        !recursive_copy(input_geojson_dir,output_geojson_dir, ".json")) {
      LOG_ERROR("Data copy failed.");
      return EXIT_FAILURE;
    }

    std::vector<std::string> osmlr_tiles;
    auto itr = bfs::recursive_directory_iterator(output_osmlr_dir);
    auto end = bfs::recursive_directory_iterator();
    for (; itr != end; ++itr) {
      auto dir_entry = *itr;
      if (bfs::is_regular_file(dir_entry)) {
        auto ext = dir_entry.path().extension();
        if (ext == ".osmlr") {
          osmlr_tiles.emplace_back(dir_entry.path().string());
        }
      }
    }
    output_tiles->update_tiles(osmlr_tiles);
  }

  // Merge edges to create OSMLR segments. Output to both pbf and GeoJSON
  vb::merge::merge(
    filtered_tiles, reader, allow_merge_pred, allow_edge_pred,
    [&](const vb::merge::path &p) {
      if (check_access(reader, p)) {
        output_tiles->add_path(p);
        output_geojson->add_path(p);
      }
    });

  output_tiles->finish();
  output_geojson->finish();
  LOG_INFO("Done");
  return EXIT_SUCCESS;
}
