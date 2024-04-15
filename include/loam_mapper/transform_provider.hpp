
#ifndef BUILD_TRANSFORM_PROVIDER_HPP
#define BUILD_TRANSFORM_PROVIDER_HPP

#include <boost/filesystem.hpp>
#include "loam_mapper/csv.hpp"
#include <string>
#include <memory>
#include <geometry_msgs/msg/pose_with_covariance.hpp>


namespace loam_mapper
{
class TransformProvider
{
public:
  using SharedPtr = std::shared_ptr<TransformProvider>;
  using ConstSharedPtr = const SharedPtr;

  explicit TransformProvider(const boost::filesystem::path & pose_txt);

  double origin_x = 658761.0;
  double origin_y = 4542599.0;
  double origin_z = 116.250886;

  struct Pose
  {
    uint32_t stamp_unix_seconds{0U};
    uint32_t stamp_nanoseconds{0U};
    geometry_msgs::msg::PoseWithCovariance pose_with_covariance;
  };

  std::vector<Pose> poses_;

  Pose get_pose_at(
    uint32_t stamp_unix_seconds,
    uint32_t stamp_nanoseconds);

private:
  std::string header_line_string;
  std::string time_string;
  int data_line_number;
  std::string mission_date;
};
}



#endif  // BUILD_TRANSFORM_PROVIDER_HPP