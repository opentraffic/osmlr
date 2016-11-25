#include "osmlr/output/geojson.hpp"
#include <stdexcept>

namespace vb = valhalla::baldr;

namespace {

const std::string k_geojson_header = "{\"type\":\"FeatureCollection\",\"features\":[";
const std::string k_geojson_footer = "]}";

} // anonymous namespace

namespace osmlr {
namespace output {

geojson::geojson(vb::GraphReader &reader, std::string base_dir, size_t max_fds)
  : m_reader(reader)
  , m_writer(base_dir, "json", max_fds) {
}

geojson::~geojson() {
}

void geojson::add_path(const vb::merge::path &p) {
  std::ostringstream out;
  out.precision(17);

  auto tile_id = p.m_start.Tile_Base();

  auto tile_path_itr = m_tile_path_ids.find(tile_id);
  if (tile_path_itr == m_tile_path_ids.end()) {
    out << k_geojson_header;
    std::tie(tile_path_itr, std::ignore) = m_tile_path_ids.emplace(tile_id, 0);
  } else {
    out << ",";
  }

  out << "{\"type\":\"Feature\",\"geometry\":";
  out << "{\"type\":\"MultiLineString\",\"coordinates\":[";

  bool first = true;
  for (auto edge_id : p.m_edges) {
    if (first) { first = false; } else { out << ","; };

    const auto *tile = m_reader.GetGraphTile(edge_id);
    auto edgeinfo_offset = tile->directededge(edge_id)->edgeinfo_offset();
    auto edgeinfo = tile->edgeinfo(edgeinfo_offset);
    auto decoder = edgeinfo.lazy_shape();

    out << "[";
    bool first_pt = true;
    while (!decoder.empty()) {
      if (first_pt) { first_pt = false; } else { out << ","; }

      auto pt = decoder.pop();
      out << "[" << pt.lng() << "," << pt.lat() << "]";
    }
    out << "]";
  }

  out << "]},\"properties\":{"
      << "\"tile_id\":" << tile_id.tileid() << ","
      << "\"level\":" << tile_id.level() << ","
      << "\"id\":" << tile_path_itr->second << ","
      << "\"original_edges\":\"";

  first = true;
  for (auto edge_id : p.m_edges) {
    if (first) { first = false; } else { out << ", "; }
    out << edge_id;
  }

  out << "\"}}";

  m_writer.write_to(tile_id, out.str());
  tile_path_itr->second += 1;
}

void geojson::finish() {
  for (auto entry : m_tile_path_ids) {
    m_writer.write_to(entry.first, k_geojson_footer);
  }
  m_writer.close_all();
}

} // namespace output
} // namespace osmlr
