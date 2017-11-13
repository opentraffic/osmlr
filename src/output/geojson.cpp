#include "osmlr/output/geojson.hpp"
#include <valhalla/midgard/util.h>
#include "segment.pb.h"
#include "tile.pb.h"
#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/regex.hpp>
#include <stdexcept>
#include <iomanip>

namespace vm = valhalla::midgard;
namespace vb = valhalla::baldr;
namespace bpt = boost::property_tree;
namespace bfs = boost::filesystem;
namespace bal = boost::algorithm;
namespace pbf = opentraffic::osmlr;

namespace {

// Minimum length for an OSMLR segment
constexpr uint32_t kMinimumLength = 5;

// Maximum length for an OSMLR segment
constexpr uint32_t kMaximumLength = 1000;

// Check if oneway. Assumes forward access is allowed. Edge is oneway if
// no reverse vehicular access is allowed
bool is_oneway(const vb::DirectedEdge *e) {
  return (e->reverseaccess() & vb::kVehicularAccess) == 0;
}

// writes out numbers without quotes.
// ptree writes everything out with quotes and we don't want
// this for json.
std::string fix_json_numbers(const std::string &json_str) {
  boost::regex re("\"(-?[0-9]+\\.{0,1}[0-9]*)\"");
  return  boost::regex_replace(json_str, re, "$1");
}

} // anonymous namespace

namespace osmlr {
namespace output {

geojson::geojson(vb::GraphReader &reader, std::string base_dir, size_t max_fds,
                 time_t creation_date, const uint64_t osm_changeset_id,
                 const std::unordered_map<valhalla::baldr::GraphId, uint32_t> tile_index)
  : m_osm_changeset_id(osm_changeset_id)
  , m_reader(reader)
  , m_writer(base_dir, "json", max_fds)
  , m_tile_index(tile_index){
  // Change cration date into string plus int
  m_creation_date = creation_date;
  std::tm tm = *std::gmtime(&creation_date);
  std::locale::global(std::locale("C"));
  char mbstr[100];
  std::strftime(mbstr, sizeof(mbstr), "%c %Z", &tm);
  m_date_str = std::string(mbstr);
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
        auto sub_shape = trim_front(shape, d);
        if (sub_shape.size() > 0) {
          output_segment(sub_shape, edge, edge_id);
        }
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

std::unordered_map<valhalla::baldr::GraphId, uint32_t> geojson::update_tiles(
    const std::vector<std::string>& tiles) {

  for (const auto& t : tiles) {

    auto base_id = vb::GraphTile::GetTileId(t);

    if (!m_reader.DoesTileExist(base_id)) {
      return m_tile_index;
    }

    std::unordered_set<vb::GraphId> traffic_seg;
    const auto *graph_tile = m_reader.GetGraphTile(base_id);
    const auto num_edges = graph_tile->header()->directededgecount();
    vb::GraphId edge_id(base_id.tileid(), base_id.level(), 0);
    for (uint32_t i = 0; i < num_edges; ++i, ++edge_id) {
      auto* edge = graph_tile->directededge(edge_id);

      if (edge->traffic_seg()) {
        std::vector<vb::TrafficSegment> segments = graph_tile->GetTrafficSegments(edge_id);
        for (const auto& seg : segments) {
          traffic_seg.emplace(seg.segment_id_);
        }
      }
    }

    bpt::ptree pt;
    bpt::read_json(t.c_str(), pt);
    bool is_updated = false;
    bpt::ptree features = pt.get_child("features");

    for(bpt::ptree::iterator iter = features.begin(); iter != features.end();)
    {
      vb::GraphId seg_id = vb::GraphId(iter->second.get<uint64_t>("properties.osmlr_id"));
      if (traffic_seg.find(seg_id) == traffic_seg.end()) {
        iter = features.erase(iter);
        is_updated = true;
      } else iter++;
    }

    if (is_updated) {

      pt.put_child("features", features);
      auto tile_index_itr = m_tile_index.find(base_id);
      if (tile_index_itr != m_tile_index.end()) {

        bfs::remove(t);
        //add the tileid and index to the map
        m_tile_path_ids.emplace(base_id, tile_index_itr->second);
        std::ostringstream oss;
        oss.precision(9);
        write_json(oss, pt, false);
        std::string json = oss.str();
        // remove the last chars so that we can add to this feature collection.
        json.erase(json.size()-3, 2);
        m_writer.write_to(base_id, fix_json_numbers(json));
      }
    }
  }

  return m_tile_index;
}

void geojson::output_segment(const vb::merge::path &p) {
  std::ostringstream out;
  out.precision(9);

  auto tile_id = p.m_start.Tile_Base();
  auto tile_path_itr = m_tile_path_ids.find(tile_id);
  if (tile_path_itr == m_tile_path_ids.end()) {
    if (m_tile_index.size()) { // is update?
      auto tile_index_itr = m_tile_index.find(tile_id);
      if (tile_index_itr != m_tile_index.end()) {
        std::string file_name = m_writer.get_name_for_tile(tile_id);

        if (bfs::exists(file_name) && bfs::is_regular_file(file_name)) { //existing file

          //add the tileid and index to the map
          std::tie(tile_path_itr, std::ignore) = m_tile_path_ids.emplace(tile_id, tile_index_itr->second);
          bpt::ptree pt;
          bpt::read_json(file_name.c_str(), pt);
          std::ostringstream oss;

          write_json(oss, pt, false);
          std::string json = oss.str();
          // remove the last chars so that we can add to this feature collection.
          json.erase(json.size()-3, 2);
          out << fix_json_numbers(json);
          out << ",";
          bfs::remove(file_name);

        } else throw std::runtime_error("Unable to open traffic geojson file. " + file_name); // should never happen
      } else { // new file
        out << "{\"type\":\"FeatureCollection\",\"properties\":{"
            << "\"creation_time\":" << m_creation_date << ","
            << "\"creation_date\":\"" << m_date_str << "\","
            << "\"description\":\"" << tile_id << "\","
            << "\"changeset_id\":" << m_osm_changeset_id << "},";
        out << "\"features\":[";
        std::tie(tile_path_itr, std::ignore) = m_tile_path_ids.emplace(tile_id, 0);
      }
    } else { // new file
      out << "{\"type\":\"FeatureCollection\",\"properties\":{"
          << "\"creation_time\":" << m_creation_date << ","
          << "\"creation_date\":\"" << m_date_str << "\","
          << "\"description\":\"" << tile_id << "\","
          << "\"changeset_id\":" << m_osm_changeset_id << "},";
      out << "\"features\":[";
      std::tie(tile_path_itr, std::ignore) = m_tile_path_ids.emplace(tile_id, 0);
    }
  } else out << ",";//already in the map

  out << "{\"type\":\"Feature\",\"geometry\":";
  out << "{\"type\":\"LineString\",\"coordinates\":[";

  bool first_pt = true;
  bool oneway = false;
  bool drive_on_right = false;
  vb::RoadClass best_frc = vb::RoadClass::kServiceOther;
  vm::PointLL prev_pt;
  for (auto edge_id : p.m_edges) {
    const auto *tile = m_reader.GetGraphTile(edge_id);
    const auto* directededge = tile->directededge(edge_id);
    oneway = is_oneway(directededge);
    drive_on_right = directededge->drive_on_right();
    if (directededge->classification() < best_frc) {
      best_frc = directededge->classification();
    }

    // Get the edge shape. reverse the order if needed
    auto edgeinfo_offset = directededge->edgeinfo_offset();
    std::vector<PointLL> shape = tile->edgeinfo(edgeinfo_offset).shape();
    if (!directededge->forward()) {
      std::reverse(shape.begin(), shape.end());
    }

    // Serialize the shape
    for (const auto& pt : shape) {
      if (pt == prev_pt) {
        continue;
      }
      if (first_pt) { first_pt = false; } else { out << ","; }
      out << "[" << pt.lng() << "," << pt.lat() << "]";
      prev_pt = pt;
    }
  }

  // Add properties for this OSMLR segment
  vb::GraphId osmlr_id(tile_id.tileid(), tile_id.level(), tile_path_itr->second);
  out << "]},\"properties\":{"
      << "\"id\":" << tile_path_itr->second << ","
      << "\"osmlr_id\":" << osmlr_id.value << ","
      << "\"best_frc\":\"" << vb::to_string(best_frc) << "\","
      << "\"oneway\":" << oneway << ","
      << "\"drive_on_right\":" << drive_on_right;
  out << "}}";

  m_writer.write_to(tile_id, out.str());
  tile_path_itr->second += 1;
}

// Output a segment that is part of an edge.
void geojson::output_segment(const std::vector<vm::PointLL>& shape,
                             const vb::DirectedEdge* edge,
                             const vb::GraphId& edgeid) {
  std::ostringstream out;
  out.precision(9);

  auto tile_id = edgeid.Tile_Base();
  auto tile_path_itr = m_tile_path_ids.find(tile_id);
  if (tile_path_itr == m_tile_path_ids.end()) {
    if (m_tile_index.size()) { // is update?
      auto tile_index_itr = m_tile_index.find(tile_id);
      if (tile_index_itr != m_tile_index.end()) {
        std::string file_name = m_writer.get_name_for_tile(tile_id);

        if (bfs::exists(file_name) && bfs::is_regular_file(file_name)) { //existing file

          //add the tileid and index to the map
          std::tie(tile_path_itr, std::ignore) = m_tile_path_ids.emplace(tile_id, tile_index_itr->second);
          bpt::ptree pt;
          bpt::read_json(file_name.c_str(), pt);

          std::ostringstream oss;
          write_json(oss, pt, false);
          std::string json = oss.str();
          // remove the last chars so that we can add to this feature collection.
          json.erase(json.size()-3, 2);
          out << fix_json_numbers(json);
          out << ",";
          bfs::remove(file_name);

        } else throw std::runtime_error("Unable to open traffic geojson file." + file_name); // should never happen
      } else { // new file
        out << "{\"type\":\"FeatureCollection\",\"properties\":{"
            << "\"creation_time\":" << m_creation_date << ","
            << "\"creation_date\":\"" << m_date_str << "\","
            << "\"description\":\"" << tile_id << "\","
            << "\"changeset_id\":" << m_osm_changeset_id << "},";
        out << "\"features\":[";
        std::tie(tile_path_itr, std::ignore) = m_tile_path_ids.emplace(tile_id, 0);
      }
    } else { // new file
      out << "{\"type\":\"FeatureCollection\",\"properties\":{"
          << "\"creation_time\":" << m_creation_date << ","
          << "\"creation_date\":\"" << m_date_str << "\","
          << "\"description\":\"" << tile_id << "\","
          << "\"changeset_id\":" << m_osm_changeset_id << "},";
      out << "\"features\":[";
      std::tie(tile_path_itr, std::ignore) = m_tile_path_ids.emplace(tile_id, 0);
    }
  } else out << ",";//already in the map

  out << "{\"type\":\"Feature\",\"geometry\":";
  out << "{\"type\":\"LineString\",\"coordinates\":[";

  bool first_pt = true;
  for (const auto pt : shape) {
    if (first_pt) { first_pt = false; } else { out << ","; }
    out << "[" << pt.lng() << "," << pt.lat() << "]";
  }

  vb::GraphId osmlr_id(tile_id.tileid(), tile_id.level(), tile_path_itr->second);
  bool first = true;
  out << "]},\"properties\":{"
      << "\"id\":" << tile_path_itr->second << ","
      << "\"osmlr_id\":" << osmlr_id.value << ","
      << "\"best_frc\":\"" << vb::to_string(edge->classification()) << "\","
      << "\"oneway\":" << is_oneway(edge) << ","
      << "\"drive_on_right\":" << edge->drive_on_right();
  out << "}}";

  m_writer.write_to(tile_id, out.str());
  tile_path_itr->second += 1;
}

void geojson::finish() {
  for (auto entry : m_tile_path_ids) {
    m_writer.write_to(entry.first, "]}");
  }
  m_writer.close_all();
}

} // namespace output
} // namespace osmlr
