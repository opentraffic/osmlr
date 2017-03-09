#include "osmlr/output/geojson.hpp"
#include <stdexcept>

namespace vm = valhalla::midgard;
namespace vb = valhalla::baldr;

namespace {

// Minimum length for an OSMLR segment
constexpr uint32_t kMinimumLength = 5;

// Maximum length for an OSMLR segment
constexpr uint32_t kMaximumLength = 1000;

const std::string k_geojson_header = "{\"type\":\"FeatureCollection\",\"features\":[";
const std::string k_geojson_footer = "]}";

// Check if oneway. Assumes forward access is allowed. Edge is oneway if
// no reverse vehicular access is allowed
bool is_oneway(const vb::DirectedEdge *e) {
  return (e->reverseaccess() & vb::kVehicularAccess) == 0;
}

vm::PointLL interp(vm::PointLL a, vm::PointLL b, double frac) {
  return vm::PointLL(a.AffineCombination(1.0 - frac, frac, b));
}

// chop the first "dist" length off seg, returning it as the result. this will
// modify seg!
std::vector<vm::PointLL> chop_subsegment(std::vector<vm::PointLL> &seg, uint32_t dist) {
  const size_t len = seg.size();
  assert(len > 1);
  std::vector<vm::PointLL> result;
  result.push_back(seg[0]);
  double d = 0.0;
  size_t i = 1;
  for (; i < len; ++i) {
    auto segdist = seg[i-1].Distance(seg[i]);
    if ((d + segdist) >= dist) {
      double frac = (static_cast<double>(dist) - d) / segdist;
      auto midpoint = interp(seg[i-1], seg[i], frac);
      result.push_back(midpoint);
      // remove used part of seg.
      seg.erase(seg.begin(), (seg.begin() + (i - 1)));
      seg[0] = midpoint;
      break;

    } else {
      d += segdist;
      result.push_back(seg[i]);
    }
  }

  // used all of seg, and exited the loop by iteration rather than breaking out.
  if (i == len) {
    seg.clear();
  }
  return result;
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
  // Get the length of the path
  uint32_t total_length = 0;
  for (auto edge_id : p.m_edges) {
    const auto *tile = m_reader.GetGraphTile(edge_id);
    const auto *edge = tile->directededge(edge_id);
    total_length += edge->length();
  }

  // Skip very short segments that are only 1 edge
  if (total_length < kMinimumLength && p.m_edges.size() == 1) {
    return;
  }

  // Split longer segments
  if (total_length < kMaximumLength) {
    output_segment(p);
  } else {
    split_path(p, total_length);
  }
}

void geojson::split_path(const vb::merge::path& p, const uint32_t total_length) {
  // Walk the merged path and split where needed
  uint32_t accumulated_length = 0;
  vb::merge::path split_path(p.m_start);
  for (auto edge_id : p.m_edges) {
    // TODO - do we need to check if we enter a new tile or does the writer
    // handle this?

    const auto* tile = m_reader.GetGraphTile(edge_id);
    const auto* edge = tile->directededge(edge_id);
    uint32_t edge_len = edge->length();

    if (edge_len >= kMaximumLength) {
      // Output prior segment
      if (split_path.m_edges.size() > 0) {
        output_segment(split_path);
      }

      // TODO - split this edge - for now just add the entire edge
      uint32_t edgeinfo_offset = edge->edgeinfo_offset();
      auto edgeinfo = tile->edgeinfo(edgeinfo_offset);
      std::vector<PointLL> shape = tile->edgeinfo(edge->edgeinfo_offset()).shape();
      if (!edge->forward()) {
        std::reverse(shape.begin(), shape.end());
      }

      float shape_length = length(shape);

      // Split the edge
      int n = (edge_len / kMaximumLength);
      float dist = static_cast<float>(edge_len) / static_cast<float>(n+1);
      uint32_t d = std::ceil(dist);
      for (int i = 0; i < n; i++) {
       // if (shape.size() == 0) break;
        auto sub_shape = chop_subsegment(shape, d);
        output_segment(sub_shape, edge, edge_id);
      }
      if (shape.size() > 0) {
        output_segment(shape, edge, edge_id);
      }

      // Start a new path at the end of this edge
      split_path.m_start = edge->endnode();
      split_path.m_edges.clear();
      accumulated_length = 0;
    } else if (accumulated_length + edge_len >= kMaximumLength) {
      // TODO - optimize the split to avoid short segments

      // Output the current split path and start a new split path
      output_segment(split_path);
      split_path.m_start = split_path.m_end;
      split_path.m_edges.clear();
      split_path.m_edges.push_back(edge_id);
      split_path.m_end = edge->endnode();
      accumulated_length = edge_len;
    } else {
      // Add this edge to the new path
      split_path.m_edges.push_back(edge_id);
      split_path.m_end = edge->endnode();
      accumulated_length += edge_len;
    }
  }

  // Output the last
  if (split_path.m_edges.size() > 0) {
    output_segment(split_path);
  }
}

void geojson::output_segment(const vb::merge::path &p) {
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

// Output a segment that is part of an edge.
void geojson::output_segment(const std::vector<vm::PointLL>& shape,
                             const vb::DirectedEdge* edge,
                             const vb::GraphId& edgeid) {
  std::ostringstream out;
  out.precision(17);

  auto tile_id = edgeid.Tile_Base();

  auto tile_path_itr = m_tile_path_ids.find(tile_id);
  if (tile_path_itr == m_tile_path_ids.end()) {
    out << k_geojson_header;
    std::tie(tile_path_itr, std::ignore) = m_tile_path_ids.emplace(tile_id, 0);
  } else {
    out << ",";
  }

  out << "{\"type\":\"Feature\",\"geometry\":";
  out << "{\"type\":\"MultiLineString\",\"coordinates\":[";
  out << "[";
  bool first_pt = true;
  for (const auto pt : shape) {
    if (first_pt) { first_pt = false; } else { out << ","; }
    out << "[" << pt.lng() << "," << pt.lat() << "]";
  }
  out << "]";

  vb::GraphId osmlr_id(tile_id.tileid(), tile_id.level(), tile_path_itr->second);
  bool first = true;
  bool oneway = is_oneway(edge);
  bool drive_on_right = edge->drive_on_right();
  vb::RoadClass best_frc = edge->classification();
  out << "]},\"properties\":{"
      << "\"tile_id\":" << tile_id.tileid() << ","
      << "\"level\":" << tile_id.level() << ","
      << "\"id\":" << tile_path_itr->second << ","
      << "\"osmlr_id\":" << osmlr_id.value << ","
      << "\"best_frc\":\"" << vb::to_string(best_frc) << "\","
      << "\"oneway\":" << oneway << ","
      << "\"drive_on_right\":" << drive_on_right << ","
      << "\"original_edges\":\"";

  out << edgeid;
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
