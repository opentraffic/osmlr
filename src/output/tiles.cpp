#include "osmlr/output/tiles.hpp"
#include "segment.pb.h"
#include "tile.pb.h"
#include <valhalla/midgard/logging.h>
#include <valhalla/midgard/util.h>
#include <stdexcept>

namespace vm = valhalla::midgard;
namespace vb = valhalla::baldr;
namespace pbf = opentraffic::osmlr;

namespace {

// Minimum length for an OSMLR segment
constexpr uint32_t kMinimumLength = 5;

// Maximum length for an OSMLR segment
constexpr uint32_t kMaximumLength = 1000;

// Local stats for testing
int count = 0;
int shortsegs = 0;
int longsegs = 0;
int chunks = 0;
float accum = 0.0f;

uint16_t bearing(const std::vector<vm::PointLL> &shape) {
  // OpenLR says to use 20m along the edge, but we could use the
  // GetOffsetForHeading function, which adapts it to the road class.
  float heading = vm::PointLL::HeadingAlongPolyline(shape, 20);
  assert(heading >= 0.0);
  assert(heading < 360.0);
  return uint16_t(std::round(heading));
}

vb::RoadClass lowest_frc(vb::GraphReader &m_reader, const vb::merge::path &p) {
  vb::RoadClass min = vb::RoadClass::kServiceOther;
  for (auto edge_id : p.m_edges) {
    vb::RoadClass c = m_reader.GetGraphTile(edge_id)->directededge(edge_id)->classification();
    if (c < min) {
      min = c;
    }
  }
  return min;
}

// Check if oneway. Assumes forward access is allowed. Edge is oneway if
// no reverse vehicular access is allowed
bool is_oneway(const vb::DirectedEdge *e) {
  return (e->reverseaccess() & vb::kVehicularAccess) == 0;
}

pbf::Segment_RoadClass convert_frc(vb::RoadClass rc) {
  assert(pbf::Segment_RoadClass_IsValid(int(rc)));
  return pbf::Segment_RoadClass(int(rc));
}

} // anonymous namespace

namespace osmlr {
namespace output {

pbf::Segment_FormOfWay convert_fow(FormOfWay fow) {
  assert(pbf::Segment_FormOfWay_IsValid(int(fow)));
  return pbf::Segment_FormOfWay(int(fow));
}

std::ostream &operator<<(std::ostream &out, FormOfWay fow) {
  switch (fow) {
  case FormOfWay::kUndefined:           out << "undefined";            break;
  case FormOfWay::kMotorway:            out << "motorway";             break;
  case FormOfWay::kMultipleCarriageway: out << "multiple_carriageway"; break;
  case FormOfWay::kSingleCarriageway:   out << "single_carriageway";   break;
  case FormOfWay::kRoundabout:          out << "roundabout";           break;
  case FormOfWay::kTrafficSquare:       out << "traffic_square";       break;
  case FormOfWay::kSlipRoad:            out << "sliproad";             break;
  default:
    out << "other";
  }
  return out;
}

FormOfWay form_of_way(const vb::DirectedEdge *e) {
  bool oneway = is_oneway(e);
  auto rclass = e->classification();

  // if it's a slip road, return that. TODO: am i doing this right?
  if (e->link()) {
    return FormOfWay::kSlipRoad;
  }
  // if it's a roundabout, return that
  else if (e->roundabout()) {
    return FormOfWay::kRoundabout;
  }
  // if it's a motorway and it's one-way, then it's likely to be grade separated
  else if (rclass == vb::RoadClass::kMotorway && oneway) {
    return FormOfWay::kMotorway;
  }
  // if it's a major road, and it's one-way then it might be a multiple
  // carriageway road.
  else if (rclass <= vb::RoadClass::kTertiary && oneway) {
    return FormOfWay::kMultipleCarriageway;
  }
  // not one-way, so perhaps it's a single carriageway
  else if (rclass <= vb::RoadClass::kTertiary) {
    return FormOfWay::kSingleCarriageway;
  }
  // everything else
  else {
    return FormOfWay::kOther;
  }
}

tiles::tiles(vb::GraphReader &reader, std::string base_dir, size_t max_fds, uint32_t max_length)
  : m_reader(reader)
  , m_writer(base_dir, "osmlr", max_fds)
  , m_max_length(max_length) {
}

tiles::~tiles() {
}

void tiles::split_path(const vb::merge::path& p, const uint32_t total_length) {
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

      // Split this edge
      uint32_t edgeinfo_offset = edge->edgeinfo_offset();
      auto edgeinfo = tile->edgeinfo(edgeinfo_offset);
      std::vector<PointLL> shape = tile->edgeinfo(edge->edgeinfo_offset()).shape();
      if (!edge->forward()) {
        std::reverse(shape.begin(), shape.end());
      }
      int n = (edge_len / kMaximumLength);
      float dist = static_cast<float>(edge_len) / static_cast<float>(n+1);
      for (int i = 0; i < n; i++) {
        auto sub_shape = trim_front(shape, std::ceil(dist));
        output_segment(sub_shape, edge, edge_id, (i==0), false);
        chunks++;
      }
      if (shape.size() > 0) {
        output_segment(shape, edge, edge_id, false, true);
        chunks++;
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

void tiles::add_path(const vb::merge::path &p) {
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

// Build a segment descriptor for a portion of an edge. This requires the
// portion of the edge shape and the directed edge.
std::vector<lrp> tiles::build_segment_descriptor(const std::vector<vm::PointLL>& shape,
                                                 const vb::DirectedEdge* edge,
                                                 const bool start_at_node,
                                                 const bool end_at_node) {
  assert(shape.size() > 0);

  std::vector<lrp> seg;
  vb::RoadClass frc = edge->classification();
  FormOfWay fow = form_of_way(edge);

  // Output first LRP. TODO - set at_node flag
  float accumulated_length = length(shape);
  seg.emplace_back(start_at_node, shape[0], bearing(shape), frc, fow, frc, accumulated_length);

  // Output last LRP
  seg.emplace_back(end_at_node, shape.back(), 0, frc, fow, frc, 0);

  // Update stats for total, short, and long segments. Add 10 to max segment
  // length to account for roundoff.
  count++;
  accum += accumulated_length;
  if (accumulated_length < 25) {
    LOG_INFO("accumulated length = " + std::to_string(accumulated_length));
    shortsegs++;
  } else if (accumulated_length > kMaximumLength+10) {
    LOG_INFO("accumulated length = " + std::to_string(accumulated_length));
    longsegs++;
  }
  return seg;
}


// Build segment LRPs for a single segment. The first LRP is at the beginning node of the path
// and the last LRP is at the end edge in the path
std::vector<lrp> tiles::build_segment_descriptor(const vb::merge::path &p) {
  assert(p.m_edges.size() > 0);

  std::vector<lrp> seg;
  uint32_t accumulated_length = 0;
  vb::GraphId last_node = p.m_start;
  std::vector<vm::PointLL> shape;
  vb::RoadClass start_frc, least_frc;
  FormOfWay start_fow;
  for (auto edge_id : p.m_edges) {
    const auto* tile = m_reader.GetGraphTile(edge_id);
    const auto* edge = tile->directededge(edge_id);
    uint32_t edge_len = edge->length();

    // First edge - get the shape so we can get bearing. Get FRC and FOW
    if (accumulated_length == 0) {
      shape = tile->edgeinfo(edge->edgeinfo_offset()).shape();
      if (!edge->forward()) {
        std::reverse(shape.begin(), shape.end());
      }
      start_frc = edge->classification();
      least_frc = start_frc;
      start_fow = form_of_way(edge);
    }
    if (edge->classification() < least_frc) {
      least_frc = edge->classification();
    }
    accumulated_length += edge_len;
    last_node = edge->endnode();
  }

  // Output the start LRP
  if (accumulated_length > 0) {
    assert(shape.size() > 0);
    seg.emplace_back(true, shape[0], bearing(shape), start_frc, start_fow,
                     least_frc, accumulated_length);
  }

  // output last LRP
  const auto* tile = m_reader.GetGraphTile(last_node);
  vm::PointLL endll = tile->node(last_node)->latlng();
  seg.emplace_back(true, endll, 0, start_frc, start_fow, least_frc, 0);

  // Update stats
  count++;
  accum += accumulated_length;
  if (accumulated_length < 25) {
    shortsegs++;
  } else if (accumulated_length > kMaximumLength) {
    LOG_INFO("path accumulated length = " + std::to_string(accumulated_length));
    longsegs++;
  }
  return seg;
}

// this appends a tile with only a single entry. each repeated message (not a
// packed=true primitive) in the protocol buffers format is tagged with the
// field number. this means the concatenation of two messages consisting only
// of repeated fields is a valid message with those fields concatenated. since
// there is no Tile header at this time, we can just concatenate individual
// Tile messages to make the full Tile. this means we don't have to track any
// additional state for each Tile being built.
void tiles::output_segment(const vb::merge::path &p) {
  auto lrps = build_segment_descriptor(p);
  output_segment(lrps, p.m_start.Tile_Base());
}

void tiles::output_segment(const std::vector<vm::PointLL>& shape,
                           const vb::DirectedEdge* edge,
                           const vb::GraphId& edgeid,
                           const bool start_at_node, const bool end_at_node) {
  auto lrps = build_segment_descriptor(shape, edge, start_at_node, end_at_node);
  output_segment(lrps, edgeid.Tile_Base());
}

void tiles::output_segment(std::vector<lrp>& lrps,
                           const vb::GraphId& tile_id) {
  pbf::Tile tile;
  auto *entry = tile.add_entries();
  // don't (yet) support deleted entries, so every entry is a Segment.
  auto *segment = entry->mutable_segment();

  // should be at least 2 LRPs - at least a start and an end.
  assert(lrps.size() >= 2);
  for (size_t i = 0; i < lrps.size() - 1; ++i) {
    auto *pb_lrp = segment->add_lrps();
    const auto &lrp = lrps[i];

    auto *coord = pb_lrp->mutable_coord();
    coord->set_lat(int32_t(lrp.coord.lat() * 1.0e7));
    coord->set_lng(int32_t(lrp.coord.lng() * 1.0e7));
    pb_lrp->set_at_node(lrp.at_node);
    pb_lrp->set_bear(lrp.bear);
    pb_lrp->set_start_frc(convert_frc(lrp.start_frc));
    pb_lrp->set_start_fow(convert_fow(lrp.start_fow));
    pb_lrp->set_least_frc(convert_frc(lrp.least_frc));
    pb_lrp->set_length(lrp.length);
  }

  // final LRP with just a coord
  auto *pb_lrp = segment->add_lrps();
  const auto &lrp = lrps[lrps.size() - 1];
  auto *coord = pb_lrp->mutable_coord();
  pb_lrp->set_at_node(lrp.at_node);
  coord->set_lat(int32_t(lrp.coord.lat() * 1.0e7));
  coord->set_lng(int32_t(lrp.coord.lng() * 1.0e7));

  std::string buf;
  if (!tile.SerializeToString(&buf)) {
    throw std::runtime_error("Unable to serialize Tile message.");
  }
  m_writer.write_to(tile_id, buf);
}


void tiles::finish() {
  // Output some simple stats
  float avg = accum / count;
  std::cout << "count = " << count << " shortsegs = " << shortsegs <<
          " longsegs " << longsegs << std::endl;
  std::cout << "chunks " << chunks << std::endl;
  std::cout << "average length = " << avg << std::endl;

  // because protobuf Tile messages can be concatenated and there's no footer to
  // write, the only thing to ensure is that all the files are flushed to disk.
  m_writer.close_all();
}

} // namespace output
} // namespace osmlr
