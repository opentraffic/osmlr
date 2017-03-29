#ifndef OSMLR_OUTPUT_TILES_HPP
#define OSMLR_OUTPUT_TILES_HPP

#include <unordered_map>
#include <osmlr/output/output.hpp>
#include <osmlr/util/tile_writer.hpp>

namespace osmlr {
namespace output {

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

struct lrp {
  bool at_node;
  const valhalla::midgard::PointLL coord;
  const uint16_t bear;
  const valhalla::baldr::RoadClass start_frc;
  const FormOfWay start_fow;
  const valhalla::baldr::RoadClass least_frc;
  const uint32_t length;

  lrp(const bool at_node_,
      const valhalla::midgard::PointLL &coord_,
      uint16_t bear_,
      valhalla::baldr::RoadClass start_frc_,
      FormOfWay start_fow_,
      valhalla::baldr::RoadClass least_frc_,
      uint32_t length_)
    : at_node(at_node_)
    , coord(coord_)
    , bear(bear_)
    , start_frc(start_frc_)
    , start_fow(start_fow_)
    , least_frc(least_frc_)
    , length(length_)
    {}
};

struct tiles : public output {
  tiles(valhalla::baldr::GraphReader &reader, std::string base_dir, size_t max_fds, uint32_t max_length = 15000);
  virtual ~tiles();

  void add_path(const valhalla::baldr::merge::path &p);
  void split_path(const valhalla::baldr::merge::path &p, const uint32_t total_length);
  void output_segment(const valhalla::baldr::merge::path &p);
  void output_segment(const std::vector<valhalla::midgard::PointLL>& shape,
                      const valhalla::baldr::DirectedEdge* edge,
                      const valhalla::baldr::GraphId& edgeid,
                      const bool start_at_node, const bool end_at_node);
  void output_segment(std::vector<lrp>& lrps, const valhalla::baldr::GraphId& tile_id);
  void finish();

private:
  valhalla::baldr::GraphReader &m_reader;
  util::tile_writer m_writer;
  uint32_t m_max_length;

  std::unordered_map<valhalla::baldr::GraphId, uint32_t> m_counts;

  std::vector<lrp> build_segment_descriptor(const valhalla::baldr::merge::path &p);
  std::vector<lrp> build_segment_descriptor(const std::vector<valhalla::midgard::PointLL>& shape,
                                            const valhalla::baldr::DirectedEdge* edge,
                                            const bool start_at_node,
                                            const bool end_at_node);
};

} // namespace output
} // namespace osmlr

#endif /* OSMLR_OUTPUT_TILES_HPP */
