#include "loam_mapper/loam_mapper.hpp"

#include "loam_mapper/Occtree.h"
#include "loam_mapper/point_types.hpp"
#include "loam_mapper/utils.hpp"
#include "pcapplusplus/PcapFileDevice.h"

#include <pcl/io/pcd_io.h>
#include <pcl_conversions/pcl_conversions.h>

#include <execution>
#include <iostream>
#include <numeric>

namespace loam_mapper
{
LoamMapper::LoamMapper() : Node("loam_mapper")
{
  // params
  this->declare_parameter("pcap_dir_path", "");
  this->declare_parameter("pose_txt_path", "");
  this->declare_parameter("pcd_export_directory", "");
  this->declare_parameter("map_origin_x", 0.0);
  this->declare_parameter("map_origin_y", 0.0);
  this->declare_parameter("map_origin_z", 0.0);
  this->declare_parameter("imu2lidar_roll", 0.0);
  this->declare_parameter("imu2lidar_pitch", 0.0);
  this->declare_parameter("imu2lidar_yaw", 0.0);
  this->declare_parameter("enable_ned2enu", true);
  this->declare_parameter("voxel_resolution", 0.4);
  this->declare_parameter("debug_mode", true);

  pcap_dir_path_ = this->get_parameter("pcap_dir_path").as_string();
  pose_txt_path_ = this->get_parameter("pose_txt_path").as_string();
  pcd_export_dir_ = this->get_parameter("pcd_export_directory").as_string();
  map_origin_x_ = this->get_parameter("map_origin_x").as_double();
  map_origin_y_ = this->get_parameter("map_origin_y").as_double();
  map_origin_z_ = this->get_parameter("map_origin_z").as_double();
  imu2lidar_roll_ = this->get_parameter("imu2lidar_roll").as_double();
  imu2lidar_pitch_ = this->get_parameter("imu2lidar_pitch").as_double();
  imu2lidar_yaw_ = this->get_parameter("imu2lidar_yaw").as_double();
  enable_ned2enu_ = this->get_parameter("enable_ned2enu").as_bool();
  voxel_resolution_ = this->get_parameter("voxel_resolution").as_double();
  debug_mode_ = this->get_parameter("debug_mode").as_bool();

  if (debug_mode_) {
    ros_cloud_pub = this->create_publisher<sensor_msgs::msg::PointCloud2>("parsed_cloud", 10);
    path_pub = this->create_publisher<nav_msgs::msg::Path>("ground_truth_path", 10);
  }

  transform_provider = std::make_shared<loam_mapper::TransformProvider>(
    pose_txt_path_, map_origin_x_, map_origin_y_, map_origin_z_);
  points_provider = std::make_shared<loam_mapper::PointsProvider>(pcap_dir_path_);

  for (int i = 0; i < points_provider->paths_pcaps_.size(); i++) {
    auto process_cloud_single = [&](const std::vector<PointsProvider::PointXYZIT> & cloud) {
      std::vector<PointsProvider::PointXYZIT> cloud_trans;
      cloud_trans.resize(cloud.size());

      std::transform(
        std::execution::par, cloud.cbegin(), cloud.cend(), cloud_trans.begin(),
        [this](const PointsProvider::PointXYZIT & point) {
          PointsProvider::PointXYZIT point_trans;

          // position from applanix data is taken into pose below according to the stamps .
          TransformProvider::Pose pose =
            transform_provider->get_pose_at(point.stamp_unix_seconds, point.stamp_nanoseconds);

          Eigen::Quaterniond quat_ins_to_map(
            pose.pose_with_covariance.pose.orientation.w,
            pose.pose_with_covariance.pose.orientation.x,
            pose.pose_with_covariance.pose.orientation.y,
            pose.pose_with_covariance.pose.orientation.z);
          Eigen::Affine3d affine_imu2lidar(Eigen::Affine3d::Identity());
          affine_imu2lidar.matrix().topLeftCorner<3, 3>() =
            Eigen::AngleAxisd(utils::Utils::deg_to_rad(imu2lidar_yaw_), Eigen::Vector3d::UnitZ())
              .toRotationMatrix() *
            Eigen::AngleAxisd(utils::Utils::deg_to_rad(imu2lidar_pitch_), Eigen::Vector3d::UnitY())
              .toRotationMatrix() *
            Eigen::AngleAxisd(utils::Utils::deg_to_rad(imu2lidar_roll_), Eigen::Vector3d::UnitX())
              .toRotationMatrix();

          Eigen::Affine3d affine_sensor2map(Eigen::Affine3d::Identity());

          if (enable_ned2enu_) {
            Eigen::Affine3d ned2enu(Eigen::Affine3d::Identity());
            ned2enu.matrix().topLeftCorner<3, 3>() =
              Eigen::AngleAxisd(utils::Utils::deg_to_rad(-90.0), Eigen::Vector3d::UnitZ())
                .toRotationMatrix() *
              Eigen::AngleAxisd(utils::Utils::deg_to_rad(0.0), Eigen::Vector3d::UnitY())
                .toRotationMatrix() *
              Eigen::AngleAxisd(utils::Utils::deg_to_rad(180.0), Eigen::Vector3d::UnitX())
                .toRotationMatrix();

            Eigen::Affine3d affine_imu2lidar_enu(Eigen::Affine3d::Identity());
            affine_imu2lidar_enu = affine_imu2lidar.matrix() * ned2enu.matrix();

            // sensor to map rotation is created to add translations and get the right rotation.
            affine_sensor2map.matrix().topLeftCorner<3, 3>() =
              quat_ins_to_map.toRotationMatrix() * affine_imu2lidar_enu.rotation();
          } else {
            // sensor to map rotation is created to add translations and get the right rotation.
            affine_sensor2map.matrix().topLeftCorner<3, 3>() =
              quat_ins_to_map.toRotationMatrix() * affine_imu2lidar.rotation();
          }

          // pose is added to the transformation matrix. - these were completed for every point in
          // the pointclouds.
          affine_sensor2map.matrix().topRightCorner<3, 1>()
            << pose.pose_with_covariance.pose.position.x,
            pose.pose_with_covariance.pose.position.y, pose.pose_with_covariance.pose.position.z;

          // get the point's position w.r.t. the point cloud origin.
          Eigen::Vector4d vec_point_in_first(point.x, point.y, point.z, 1.0);
          // create a 3D vector for transformed point to the map position and rotation.
          Eigen::Vector4d vec_point_trans = affine_sensor2map.matrix() * vec_point_in_first;

          point_trans.x = static_cast<double>(vec_point_trans(0));
          point_trans.y = static_cast<double>(vec_point_trans(1));
          point_trans.z = static_cast<double>(vec_point_trans(2));
          point_trans.intensity = point.intensity;

          return point_trans;
        });

      std::string point_cloud_name = pcd_export_dir_ + "ytu_campus_" + std::to_string(i) + ".pcd";

      Occtree occ_cloud(voxel_resolution_);
      for (auto & point : cloud_trans) {
        PointType pcl_point;
        pcl_point.x = point.x;
        pcl_point.y = point.y;
        pcl_point.z = point.z;
        pcl_point.intensity = point.intensity;
        occ_cloud.addPointIfVoxelEmpty(pcl_point);
      }

      CloudType new_cloud;
      for (auto & point : *occ_cloud.cloud) {
        new_cloud.push_back(point);
      }

      if (debug_mode_) {
        sensor_msgs::msg::PointCloud2 ros_cloud;
        pcl::toROSMsg(new_cloud, ros_cloud);
        ros_cloud_pub->publish(ros_cloud);
      } else {
        pcl::io::savePCDFileASCII(point_cloud_name, new_cloud);
      }
    };

//    points_provider->process_pcap(points_provider->paths_pcaps_.at(i));

    pcpp::IFileReaderDevice * reader = pcpp::IFileReaderDevice::getReader(points_provider->paths_pcaps_.at(i).string());
    if (reader == nullptr) {
      printf("Cannot determine reader for file type\n");
      exit(1);
    }
    if (!reader->open()) {
      printf("Cannot open input.pcap for reading\n");
      exit(1);
    }

    pcpp::RawPacket rawPacket;
    while (reader->getNextPacket(rawPacket)) {
      points_provider->instant_cloud_.clear();
      points_provider->process_packet(rawPacket);
      process_cloud_single(points_provider->instant_cloud_);
    }

    reader->close();
    points_provider->cloud_.clear();
  }
}

}  // namespace loam_mapper

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<loam_mapper::LoamMapper>());
  rclcpp::shutdown();

  return 0;
}