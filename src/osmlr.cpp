#include <valhalla/midgard/logging.h>
#include <valhalla/baldr/graphreader.h>
#include <valhalla/baldr/merge.h>

#include <boost/program_options.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/range/adaptor/map.hpp>

#include "config.h"
#include "osmlr/output/output.hpp"
#include "osmlr/output/geojson.hpp"
#include "osmlr/output/tiles.hpp"

namespace vm = valhalla::midgard;
namespace vb = valhalla::baldr;

namespace bpo = boost::program_options;
namespace bpt = boost::property_tree;
namespace bra = boost::adaptors;

bool edge_pred(const vb::DirectedEdge *edge) {
  return (edge->use() != vb::Use::kFerry &&
          edge->use() != vb::Use::kTransitConnection &&
          !edge->trans_up() &&
          !edge->trans_down());
}

struct tiles_max_level {
  typedef std::vector<vb::TileHierarchy::TileLevel> levels_t;
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

  tiles_max_level(vb::GraphReader &reader, unsigned int max_level) {
    for (auto level : reader.GetTileHierarchy().levels() | bra::map_values) {
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
  uint32_t access = vb::kAllAccess;
  for (auto edge_id : p.m_edges) {
    auto edge = reader.GetGraphTile(edge_id)->directededge(edge_id);
    access &= edge->forwardaccess();

    // if any edge is a shortcut, then drop the whole path
    if (edge->shortcut()) {
      return false;
    }
  }

  // be permissive here, as we do want to collect traffic on most vehicular
  // routes.
  uint32_t vehicular = vb::kAutoAccess | vb::kTruckAccess |
    vb::kTaxiAccess | vb::kBusAccess | vb::kHOVAccess;
  return access & vehicular;
}

int main(int argc, char** argv) {
  unsigned int max_level, max_fds;
  std::string config;
  std::shared_ptr<osmlr::output::output> output_tiles, output_geojson;

  bpo::options_description options("valhalla_run_route " VERSION "\n"
                                     "\n"
                                     " Usage: osmlr [options]\n"
                                     "\n"
                                     "osmlr generates traffic segment descriptors. "
                                     "\n"
                                   "\n");

  options.add_options()
    ("help,h", "Print this help message.")
    ("version,v", "Print the version of this software.")
    ("max-level,m", bpo::value<unsigned int>(&max_level)->default_value(255), "Maximum level to evaluate")
    ("max-fds", bpo::value<unsigned int>(&max_fds)->default_value(512), "Maximum number of files to have open in each output.")
    ("output-tiles,T", bpo::value<std::string>(), "Optional. If set, the base path to use when outputting OSMLR tiles.")
    ("output-geojson,J", bpo::value<std::string>(), "Optional. If set, the base path to use when outputting GeoJSON tiles.")
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

  //parse the config
  bpt::ptree pt;
  bpt::read_json(config.c_str(), pt);

  //configure logging
  vm::logging::Configure({{"type","std_err"},{"color","true"}});

  //get something we can use to fetch tiles
  vb::GraphReader reader(pt.get_child("mjolnir"));

  if (vm.count("output-tiles")) {
    std::string dir = vm["output-tiles"].as<std::string>();
    output_tiles = std::make_shared<osmlr::output::tiles>(dir, reader);
  }

  if (vm.count("output-geojson")) {
    std::string dir = vm["output-geojson"].as<std::string>();
    output_geojson = std::make_shared<osmlr::output::geojson>(reader, dir, max_fds);
  }

  assert(max_level <= std::numeric_limits<uint8_t>::max());

  auto filtered_tiles = tile_exists_filter<tiles_max_level>(
    tiles_max_level(reader, max_level), reader);

  vb::merge::merge(
    filtered_tiles, reader, edge_pred,
    [&](const vb::merge::path &p) {
      if (check_access(reader, p)) {
        if (output_tiles) {
          output_tiles->add_path(p);
        }
        if (output_geojson) {
          output_geojson->add_path(p);
        }
      }
    });

  if (output_tiles) {
    output_tiles->finish();
  }
  if (output_geojson) {
    output_geojson->finish();
  }

  return EXIT_SUCCESS;
}
