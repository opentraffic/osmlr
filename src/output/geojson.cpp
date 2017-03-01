#include "osmlr/output/geojson.hpp"
#include <stdexcept>

namespace vb = valhalla::baldr;

namespace {

const std::string k_geojson_header = "{\"type\":\"FeatureCollection\",\"features\":[";
const std::string k_geojson_footer = "]}";

// Check if oneway. Assumes forward access is allowed. Edge is oneway if
// no reverse vehicular access is allowed
bool is_oneway(const vb::DirectedEdge *e) {
  return (e->reverseaccess() & vb::kVehicularAccess) == 0;
}

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
  bool oneway = false;
  bool drive_on_right = false;
  vb::RoadClass best_frc = vb::RoadClass::kServiceOther;
  for (auto edge_id : p.m_edges) {
    if (first) { first = false; } else { out << ","; };

    const auto *tile = m_reader.GetGraphTile(edge_id);
    const auto* directededge = tile->directededge(edge_id);
    oneway = is_oneway(directededge);
    drive_on_right = directededge->drive_on_right();
    if (directededge->classification() < best_frc) {
      best_frc = directededge->classification();
    }
    auto edgeinfo_offset = directededge->edgeinfo_offset();
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

  vb::GraphId osmlr_id(tile_id.tileid(), tile_id.level(), tile_path_itr->second);
  out << "]},\"properties\":{"
      << "\"tile_id\":" << tile_id.tileid() << ","
      << "\"level\":" << tile_id.level() << ","
      << "\"id\":" << tile_path_itr->second << ","
      << "\"osmlr_id\":" << osmlr_id.value << ","
      << "\"best_frc\":\"" << vb::to_string(best_frc) << "\","
      << "\"oneway\":" << oneway << ","
      << "\"drive_on_right\":" << drive_on_right << ","
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
