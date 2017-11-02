#ifndef OSMLR_OUTPUT_GEOJSON_HPP
#define OSMLR_OUTPUT_GEOJSON_HPP

#include <osmlr/output/output.hpp>
#include <osmlr/util/tile_writer.hpp>
#include <string>
#include <unordered_map>
#include <ctime>

namespace osmlr {
namespace output {

struct geojson : public output {
  geojson(valhalla::baldr::GraphReader &reader, std::string base_dir, size_t max_fds,
          time_t creation_date, const uint64_t osm_changeset_id,
          const std::unordered_map<valhalla::baldr::GraphId, uint32_t> tile_index);
  virtual ~geojson();

  void add_path(const valhalla::baldr::merge::path &);
  void output_segment(const valhalla::baldr::merge::path &p);
  void output_segment(const std::vector<valhalla::midgard::PointLL>& shape,
                      const valhalla::baldr::DirectedEdge* edge,
                      const valhalla::baldr::GraphId& edgeid);
  void split_path(const valhalla::baldr::merge::path& p, const uint32_t total_length);
  std::unordered_map<valhalla::baldr::GraphId, uint32_t> update_tiles(
      const std::vector<std::string>& tiles);
  void finish();

private:
  time_t m_creation_date;
  std::string m_date_str;
  std::unordered_map<valhalla::baldr::GraphId, uint32_t> m_tile_index;
  uint64_t m_osm_changeset_id;
  valhalla::baldr::GraphReader &m_reader;
  util::tile_writer m_writer;
  std::unordered_map<valhalla::baldr::GraphId, uint32_t> m_tile_path_ids;
};

} // namespace output
} // namespace osmlr

#endif /* OSMLR_OUTPUT_GEOJSON_HPP */
