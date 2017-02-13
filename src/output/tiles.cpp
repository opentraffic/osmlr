#include "osmlr/output/tiles.hpp"
#include "segment.pb.h"
#include "tile.pb.h"
#include <stdexcept>

namespace vm = valhalla::midgard;
namespace vb = valhalla::baldr;
namespace pbf = opentraffic::osmlr;

namespace {

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

bool is_oneway(const vb::DirectedEdge *e) {
  // TODO: make this configurable?
  uint32_t vehicular = vb::kAutoAccess | vb::kTruckAccess |
    vb::kTaxiAccess | vb::kBusAccess | vb::kHOVAccess;
  // TODO: don't need to find opposite edge, as this info alread in the
  // reverseaccess mask?
  return (e->reverseaccess() & vehicular) == 0;
}

enum class FormOfWay {
  kUndefined = 0,
  kMotorway = 1,
  kMultipleCarriageway = 2,
  kSingleCarriageway = 3,
  kRoundabout = 4,
  kTrafficSquare = 5,
  kSlipRoad = 6,
  kOther = 7
};

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

struct lrp {
  const vm::PointLL coord;
  const uint16_t bear;
  const vb::RoadClass start_frc;
  const FormOfWay start_fow;
  const vb::RoadClass least_frc;
  const uint32_t length;

  lrp(const vm::PointLL &coord_,
      uint16_t bear_,
      vb::RoadClass start_frc_,
      FormOfWay start_fow_,
      vb::RoadClass least_frc_,
      uint32_t length_)
    : coord(coord_)
    , bear(bear_)
    , start_frc(start_frc_)
    , start_fow(start_fow_)
    , least_frc(least_frc_)
    , length(length_)
    {}
};

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
      double frac = (dist - d) / segdist;
      auto midpoint = interp(seg[i-1], seg[i], frac);
      result.push_back(midpoint);
      // remove used part of seg.
      seg.erase(seg.begin(), seg.begin() + (i - 1));
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

std::vector<lrp> build_segment_descriptor(vb::GraphReader &reader, const vb::merge::path &p, uint32_t max_length) {
  assert(p.m_edges.size() > 0);

  std::vector<lrp> seg;

  uint32_t accumulated_length = 0;
  vb::GraphId last_node = p.m_start;
  vb::GraphId start_node = p.m_start;
  std::vector<vm::PointLL> shape;
  vm::PointLL last_point_seen;
  vb::RoadClass start_frc, least_frc;
  FormOfWay start_fow;

  for (auto edge_id : p.m_edges) {
    const auto *tile = reader.GetGraphTile(edge_id);
    const auto *edge = tile->directededge(edge_id);
    uint32_t edgeinfo_offset = edge->edgeinfo_offset();
    auto edgeinfo = tile->edgeinfo(edgeinfo_offset);
    uint32_t edge_len = edge->length();

    if (edge_len + accumulated_length >= max_length && start_node != last_node) {
      assert(shape.size() > 0);
      uint16_t bear = bearing(shape);
      seg.emplace_back(shape[0], bear, start_frc, start_fow, least_frc, accumulated_length);
      accumulated_length = 0;
      start_node = last_node;
      last_point_seen = shape.back();
      shape.clear();
      start_frc = edge->classification();
      least_frc = start_frc;
      start_fow = form_of_way(edge);
    }

    std::vector<vm::PointLL> full_shape = edgeinfo.shape();
    if (!edge->forward()) {
      std::reverse(full_shape.begin(), full_shape.end());
    }

    if (edge_len >= max_length) {
      // check we've already flushed the previous edge
      assert(accumulated_length == 0);

      const auto frc = edge->classification();
      const auto fow = form_of_way(edge);

      const uint32_t num_segs = (edge_len / max_length) + 1;
      for (uint32_t i = 0; i < num_segs; ++i) {
        uint32_t start_dist = (i * edge_len) / num_segs;
        uint32_t end_dist = ((i+1) * edge_len) / num_segs;
        assert(end_dist - start_dist < max_length);

        // Since full_shape is chopped in this method make sure we use
        // end_dist - start_dist as distance along the shape to chop
        shape = chop_subsegment(full_shape, end_dist - start_dist);
        assert(!shape.empty());
        uint16_t bear = bearing(shape);

        seg.emplace_back(shape[0], bear, frc, fow, frc, end_dist - start_dist);
      }

      accumulated_length = 0;
      start_node = edge->endnode();
      last_point_seen = shape.back();
      shape.clear();

    } else {
      if (accumulated_length == 0) {
        start_frc = edge->classification();
        least_frc = start_frc;
        start_fow = form_of_way(edge);
      }
      if (edge->classification() < least_frc) {
        least_frc = edge->classification();
      }
      accumulated_length += edge_len;
      shape.insert(shape.end(), full_shape.begin(), full_shape.end());
    }

    last_node = edge->endnode();
  }

  if (accumulated_length > 0) {
    assert(shape.size() > 0);
    seg.emplace_back(shape[0], bearing(shape), start_frc, start_fow, least_frc, accumulated_length);
    last_point_seen = shape.back();
  }

  // output last LRP
  assert(last_point_seen.IsValid());
  assert(last_point_seen.lat() != 0.0);
  seg.emplace_back(last_point_seen, 0, start_frc, start_fow, least_frc, 0);

  return seg;
}

pbf::Segment_RoadClass convert_frc(vb::RoadClass rc) {
  assert(pbf::Segment_RoadClass_IsValid(int(rc)));
  return pbf::Segment_RoadClass(int(rc));
}

pbf::Segment_FormOfWay convert_fow(FormOfWay fow) {
  assert(pbf::Segment_FormOfWay_IsValid(int(fow)));
  return pbf::Segment_FormOfWay(int(fow));
}

} // anonymous namespace

namespace osmlr {
namespace output {

tiles::tiles(vb::GraphReader &reader, std::string base_dir, size_t max_fds, uint32_t max_length)
  : m_reader(reader)
  , m_writer(base_dir, "osmlr", max_fds)
  , m_max_length(max_length) {
}

tiles::~tiles() {
}

void tiles::add_path(const vb::merge::path &p) {
  auto tile_id = p.m_start.Tile_Base();

  std::vector<lrp> lrps = build_segment_descriptor(m_reader, p, m_max_length);

  // this appends a tile with only a single entry. each repeated message (not a
  // packed=true primitive) in the protocol buffers format is tagged with the
  // field number. this means the concatenation of two messages consisting only
  // of repeated fields is a valid message with those fields concatenated. since
  // there is no Tile header at this time, we can just concatenate individual
  // Tile messages to make the full Tile. this means we don't have to track any
  // additional state for each Tile being built.
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

    pb_lrp->set_bear(lrp.bear);
    pb_lrp->set_start_frc(convert_frc(lrp.start_frc));
    pb_lrp->set_start_fow(convert_fow(lrp.start_fow));
    pb_lrp->set_least_frc(convert_frc(lrp.least_frc));
    pb_lrp->set_length(lrp.length);
  }

  // final LRP with just a coord
  {
    auto *pb_lrp = segment->add_lrps();
    const auto &lrp = lrps[lrps.size() - 1];

        auto *coord = pb_lrp->mutable_coord();
    coord->set_lat(int32_t(lrp.coord.lat() * 1.0e7));
    coord->set_lng(int32_t(lrp.coord.lng() * 1.0e7));
  }

  std::string buf;
  if (!tile.SerializeToString(&buf)) {
    throw std::runtime_error("Unable to serialize Tile message.");
  }
  m_writer.write_to(tile_id, buf);
}

void tiles::finish() {
  // because protobuf Tile messages can be concatenated and there's no footer to
  // write, the only thing to ensure is that all the files are flushed to disk.
  m_writer.close_all();
}

} // namespace output
} // namespace osmlr
