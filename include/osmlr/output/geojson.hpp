#ifndef OSMLR_OUTPUT_GEOJSON_HPP
#define OSMLR_OUTPUT_GEOJSON_HPP

#include <osmlr/output/output.hpp>
#include <osmlr/util/tile_writer.hpp>
#include <unordered_map>

namespace osmlr {
namespace output {

struct geojson : public output {
  geojson(valhalla::baldr::GraphReader &reader, std::string base_dir, size_t max_fds);
  virtual ~geojson();

  void add_path(const valhalla::baldr::merge::path &);
  void output_segment(const valhalla::baldr::merge::path &p);
  void output_segment(const std::vector<valhalla::midgard::PointLL>& shape,
                      const valhalla::baldr::DirectedEdge* edge,
                      const valhalla::baldr::GraphId& edgeid);
  void split_path(const valhalla::baldr::merge::path& p, const uint32_t total_length);
  void finish();

private:
  valhalla::baldr::GraphReader &m_reader;
  util::tile_writer m_writer;
  std::unordered_map<valhalla::baldr::GraphId, uint32_t> m_tile_path_ids;
};

} // namespace output
} // namespace osmlr

#endif /* OSMLR_OUTPUT_GEOJSON_HPP */
