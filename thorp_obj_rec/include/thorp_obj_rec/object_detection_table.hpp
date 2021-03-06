/*
 * Author: Jorge Santos
 */

#include <tf/tf.h>
#include <ros/ros.h>

// auxiliary libraries
#include <mag_common_cpp_libs/common.hpp>
#include <mag_common_cpp_libs/geometry.hpp>
namespace mcl = mag_common_libs;


// action client: ORK's tabletop object recognition
#include <object_recognition_msgs/TableArray.h>

namespace thorp_obj_rec
{

class ObjectDetectionTable
{
public:

  ObjectDetectionTable()
  {
    ros::NodeHandle nh;

    // Subscribe to detected tables array
    table_sub_ = nh.subscribe("tabletop/table_array", 2, &ObjectDetectionTable::tableCb, this);
  }

  void setOutputFrame(const std::string& output_frame)
  {
    output_frame_ = output_frame;
  }

  void tableCb(const object_recognition_msgs::TableArray& msg)
  {
    if (output_frame_.empty())
    {
      // Output frame is received as part of the goal, so it must be reinitialized every time
      ROS_WARN("[object detection] No output frame provided; cannot handle table observations");
      return;
    }

    if (msg.tables.size() == 0)
    {
      ROS_WARN("[object detection] Table array message is empty");
      return;
    }

    // Accumulate table poses while detecting objects so the resulting pose (centroid) is more accurate
    // We only take the first table in the message, assuming it is always the best match and so the same
    // table... very risky, to say the least. TODO: assure we always accumulate data over the same table
    object_recognition_msgs::Table table = msg.tables[0];
    geometry_msgs::Pose table_pose = table.pose;
    // Tables often have orientations with all-nan values, but assertQuaternionValid lets them go!
    if (std::isnan(table.pose.orientation.x) ||
        std::isnan(table.pose.orientation.y) ||
        std::isnan(table.pose.orientation.z) ||
        std::isnan(table.pose.orientation.w))
    {
      ROS_WARN("[object detection] Table discarded as its orientation has nan values");
      return;
    }

    if (! mcl::transformPose(msg.header.frame_id, output_frame_, table.pose, table_pose))
    {
      ROS_WARN("[object detection] Table with pose [%s] discarded: unable to transform to output frame [%s]",
               mcl::pose2cstr3D(table_pose), output_frame_.c_str());
    }
    else if ((std::abs(mcl::roll(table_pose)) >= M_PI/10.0) || (std::abs(mcl::pitch(table_pose)) >= M_PI/10.0))
    {
      // Only consider tables within +/-18 degrees away from the horizontal plane
      ROS_WARN("[object detection] Table with pose [%s] discarded: %.2f radians away from the horizontal",
               mcl::pose2cstr3D(table_pose),
               std::max(std::abs(mcl::roll(table_pose)), std::abs(mcl::pitch(table_pose))));
    }
    else
    {
      table_obs_.push_back(getTableParams(table_pose, table.convex_hull));
    }
  }

  /**
   * Get the average table as a collision object that can be added into the world,
   * so it gets excluded from the collision map.
   */
  bool getDetectedTable(moveit_msgs::CollisionObject& table_co)
  {
    if (table_obs_.empty())
      return false;

    table_co.header.stamp = ros::Time::now();
    table_co.header.frame_id = output_frame_;
    table_co.id = "table";
    table_co.operation = moveit_msgs::CollisionObject::ADD;

    // We calculate table size, centroid and orientation as the median of the accumulated convex hulls
    std::vector<double> table_center_x;
    std::vector<double> table_center_y;
    std::vector<double> table_size_x;
    std::vector<double> table_size_y;
    std::vector<double> table_yaw;

    // Calculate table's pose as the centroid of all accumulated poses; as yaw
    // we use the one estimated for the convex hull, ignoring the pose's yaw
    double roll_acc = 0.0, pitch_acc = 0.0;//, yaw_acc = 0.0;
    table_co.primitive_poses.resize(1);
    for (const TableDescriptor& table: table_obs_)
    {
      table_co.primitive_poses[0].position.x += table.pose.position.x;
      table_co.primitive_poses[0].position.y += table.pose.position.y;
      table_co.primitive_poses[0].position.z += table.pose.position.z;
      roll_acc                               += mcl::roll(table.pose);
      pitch_acc                              += mcl::pitch (table.pose);

      // Reuse the table descriptors to store the other parameters in vectors for easily calculate median values
      table_center_x.push_back(table.center_x);
      table_center_y.push_back(table.center_y);
      table_size_x.push_back(table.size_x);
      table_size_y.push_back(table.size_y);
      table_yaw.push_back(table.yaw);
    }

    table_co.primitives.resize(1);
    table_co.primitives[0].type = shape_msgs::SolidPrimitive::BOX;
    table_co.primitives[0].dimensions.resize(3);
    table_co.primitives[0].dimensions[shape_msgs::SolidPrimitive::BOX_X] = mcl::median(table_size_x);
    table_co.primitives[0].dimensions[shape_msgs::SolidPrimitive::BOX_Y] = mcl::median(table_size_y);
    table_co.primitives[0].dimensions[shape_msgs::SolidPrimitive::BOX_Z] = 0.01;  // arbitrarily set to 1 cm
    table_co.primitive_poses.resize(1);

    table_co.primitive_poses[0].position.x = table_co.primitive_poses[0].position.x / (double)table_obs_.size();
    table_co.primitive_poses[0].position.y = table_co.primitive_poses[0].position.y / (double)table_obs_.size();
    table_co.primitive_poses[0].position.z = table_co.primitive_poses[0].position.z / (double)table_obs_.size();
    table_co.primitive_poses[0].orientation =
        tf::createQuaternionMsgFromRollPitchYaw(roll_acc/(double)table_obs_.size(),
                                                pitch_acc/(double)table_obs_.size(),
                                                mcl::median(table_yaw));
    // Displace the table center according to the centroid of its convex hull
    table_co.primitive_poses[0].position.x += mcl::median(table_center_x);
    table_co.primitive_poses[0].position.y += mcl::median(table_center_y);
    table_co.primitive_poses[0].position.z -= table_co.primitives[0].dimensions[shape_msgs::SolidPrimitive::BOX_Z]/2.0;

    ROS_DEBUG("[object detection] Table estimated at %s, size %.2fx%.2fm, based on %lu observations",
              mcl::point2cstr3D(table_co.primitive_poses[0].position),
              table_co.primitives[0].dimensions[shape_msgs::SolidPrimitive::BOX_X],
              table_co.primitives[0].dimensions[shape_msgs::SolidPrimitive::BOX_Y], table_obs_.size());

    return true;
  }

  void clear()
  {
    table_obs_.clear();
    output_frame_.clear();  // Output frame is received as part of the goal, so it must be reinitialized every time
  }

private:

  typedef struct
  {
    double size_x;
    double size_y;
    double center_x;
    double center_y;
    double yaw;
    geometry_msgs::Pose pose;
    std_msgs::ColorRGBA color;
  } TableDescriptor;

  std::vector<TableDescriptor> table_obs_;

  std::string output_frame_;

  // Publishers and subscribers
  ros::Subscriber table_sub_;


  TableDescriptor getTableParams(geometry_msgs::Pose table_pose, std::vector<geometry_msgs::Point> convex_hull)
  {
    TableDescriptor table;
    table.pose = table_pose;

    // Calculate centroid, size and orientation for the given 2D convex hull
    // Algorithm adapted from here: http://www.geometrictools.com/Documentation/MinimumAreaRectangle.pdf
    double min_area = std::numeric_limits<double>::max();

    for (size_t i0 = convex_hull.size() - 1, i1 = 0; i1 < convex_hull.size(); i0 = i1++)
    {
      convex_hull[i0].z = convex_hull[i1].z = 0; // z-coordinate is ignored

      tf::Point origin, U0, U1, D;
      tf::pointMsgToTF(convex_hull[i0], origin);
      tf::pointMsgToTF(convex_hull[i1], U0); U0 -= origin;
      U0.normalize(); // length of U0 is 1
      U1 = tf::Point(-U0.y(), U0.x(), 0.0); // - perpendicular vector of U0
      U1.normalize(); // length of U1 is 1
      double min0 = 0, max0 = 0; // projection onto U0 − axis is [min0, max0]
      double min1 = 0, max1 = 0; // projection onto U1 − axis is [min1, max1], min1 = 0 is guaranteed  TODO si?????
      for (size_t j = 0; j < convex_hull.size(); ++j)
      {
        convex_hull[j].z = 0; // z-coordinate is ignored

        tf::pointMsgToTF(convex_hull[j], D); D -= origin;
        double dot = U0.dot(D);
        if (dot < min0)
          min0 = dot;
        else if (dot > max0)
          max0 = dot;
        dot = U1.dot(D);
        if (dot < min1)
          min1 = dot;
        else if (dot > max1)
          max1 = dot;
      }
      double area = (max0 - min0) * (max1 - min1);

      if (area < min_area)
      {
        tf::Point center(origin + ((min0 + max0)/2.0) * U0 + ((min1 + max1)/2.0) * U1);
        table.center_x =   center.y();
        table.center_y = - center.x();
        table.size_x = max0 - min0;
        table.size_y = max1 - min1;
        table.yaw = std::atan2(U1.y(), U1.x());
        if (table.yaw > 0.0)
          table.yaw -= M_PI;
        min_area = area;
      }
    }

    ROS_DEBUG("Table parameters: pose [%f, %f, %f], size [%f, %f]",
              table.center_x, table.center_y, table.yaw, table.size_x, table.size_y);

    return table;
  }
};

};  // namespace thorp_obj_rec
