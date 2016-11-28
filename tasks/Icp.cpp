/* Generated from orogen/lib/orogen/templates/tasks/Task.cpp */

#include "Icp.hpp"
#include <pcl/conversions.h>
#include <pcl/features/normal_3d.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/registration/icp.h>
#include <pcl/registration/icp_nl.h>
#include <pcl/registration/transforms.h>

using namespace icp;

#define DEBUG_PRINTS 1

Icp::Icp(std::string const& name)
    : IcpBase(name)
{
    source_cloud.reset(new PCLPointCloud);
    target_cloud.reset(new PCLPointCloud);
}

Icp::Icp(std::string const& name, RTT::ExecutionEngine* engine)
    : IcpBase(name, engine)
{
}

Icp::~Icp()
{
    source_cloud.reset();
    target_cloud.reset();
}

/// The following lines are template definitions for the various state machine
// hooks defined by Orocos::RTT. See Icp.hpp for more detailed
// documentation about them.

bool Icp::configureHook()
{
    if (! IcpBase::configureHook())
        return false;

    /** Initial cumulative pose **/
    pose.invalidate();
    pose.sourceFrame = _icp_odometry_source_frame.value();
    pose.targetFrame = _icp_odometry_target_frame.value();
    pose.position.setZero();
    pose.velocity.setZero();

    /** Assume well known starting position **/
    pose.cov_position = Eigen::Matrix3d::Zero();
    pose.cov_velocity = Eigen::Matrix3d::Zero();

    /** Orientation with respect to the relative navigation frame**/
    pose.orientation = Eigen::Quaterniond::Identity();
    pose.angular_velocity.setZero();

    /** Assume very well known initial attitude **/
    pose.cov_orientation = Eigen::Matrix3d::Zero();
    pose.cov_angular_velocity = Eigen::Matrix3d::Zero();

    tf_sensor_world.setIdentity();

    return true;
}

bool Icp::startHook()
{
    if (! IcpBase::startHook())
        return false;

    return true;
}

void Icp::updateHook()
{
    IcpBase::updateHook();

    ::pcl::PCLPointCloud2 point_cloud_source_sample;

    if(_point_cloud_source.readNewest(point_cloud_source_sample) == RTT::NewData)
    {

        #ifdef DEBUG_PRINTS
        std::cout<<"** [POINT_CLOUD_SOURCE] received point_cloud_source ("<<point_cloud_source_sample.height <<", "<<point_cloud_source_sample.width<< ")**\n";
        #endif

        ::base::samples::RigidBodyState delta_pose;

        Eigen::Affine3d tf_world_sensor; /** Transformer transformation **/
        /** Get the transformation Tworld_sensor **/
        if (_sensor_frame.value().compare(_world_frame.value()) == 0)
        {
            tf_world_sensor.setIdentity();
        }
        else if (!_sensor2world.get(base::Time::now(), tf_world_sensor, false))
        {
            RTT::log(RTT::Fatal)<<"[ICP_Icp_TASK] No transformation provided."<<RTT::endlog();
           return;
        }

        /** Convert to pcl point clouds **/
        pcl::fromPCLPointCloud2(point_cloud_source_sample, *source_cloud.get());

        /** First execution to check the Identity **/
        if (tf_sensor_world.rotation().isIdentity())
        {
            tf_sensor_world = tf_world_sensor.inverse();
        }

        /** Get the estimated  delta transformation from transformer **/
        Eigen::Affine3d tf_sensor_sensor_k = tf_sensor_world * tf_world_sensor;

        /** PRE ALIGNMENT: Transform the cloud **/
        this->transformPointCloud(*source_cloud.get(), tf_sensor_sensor_k);
        tf_sensor_world = tf_world_sensor.inverse();
        #ifdef DEBUG_PRINTS
        std::cout<<"TRANSFORMED\n";
        #endif

        /** First iteration when there is not point cloud in the other port **/
        if ((!_point_cloud_target.connected()) && (target_cloud->size() == 0))
        {
            target_cloud = boost::make_shared<PCLPointCloud>(*source_cloud);
        }

        #ifdef DEBUG_PRINTS
        std::cout<<"height: "<<source_cloud->height<<"\n";
        std::cout<<"width: "<<source_cloud->width<<"\n";
        std::cout<<"size(): "<<source_cloud->size()<<"\n";
        std::cout<<"PASSED TARGET\n";
        std::cout<<"height: "<<target_cloud->height<<"\n";
        std::cout<<"width: "<<target_cloud->width<<"\n";
        std::cout<<"size(): "<<target_cloud->size()<<"\n";
        #endif

        /** Perform align **/
        Eigen::Matrix4f pair_transform = tf_sensor_sensor_k.cast<float>().matrix();
        PCLPointCloudPtr cloud_source_registered;
        std::clock_t begin = std::clock();
        //this->pairAlign(source_cloud, target_cloud, cloud_source_registered, pair_transform);
        std::clock_t end = std::clock();
        double elapsed_secs = static_cast<double>(end - begin) / CLOCKS_PER_SEC;

        #ifdef DEBUG_PRINTS
        std::cout<<"Icp alignment in "<<elapsed_secs<<" [seconds]\n";
        #endif

        double fitness_score;// = icp.getFitnessScore();

        /** Get the transformation **/
        Eigen::Isometry3f transformation;
        transformation.matrix() = pair_transform;

        #ifdef DEBUG_PRINTS
        std::cout<<"** [POINT_CLOUD_SOURCE] Fitness_score: "<<fitness_score<<"\n";
        std::cout<<"** [POINT_CLOUD_SOURCE] Transformation alignment:\n"<<transformation.matrix()<<"\n";
        #endif

        /** Delta Pose Output port **/
        transformation.makeAffine();
        Eigen::Affine3d tf;
        tf.matrix() = transformation.cast<double>().matrix();
        delta_pose.invalidate();
        delta_pose.sourceFrame = _icp_delta_odometry_source_frame.value();
        delta_pose.targetFrame = _icp_delta_odometry_target_frame.value();
        delta_pose.time = base::Time::now();
        delta_pose.orientation = Eigen::Quaternion <double>(tf.rotation());
        delta_pose.position = tf.translation();
        _delta_pose_samples_out.write(delta_pose);

        /** Cumulative Pose **/
        Eigen::Affine3d poseTrans = pose.getTransform();
        poseTrans = poseTrans * delta_pose;
        pose.time = delta_pose.time;
        pose.setTransform(poseTrans);
        _pose_samples_out.write(pose);

        /** ICP info **/
        icp::ICPInfo icp_info;
        icp_info.time = delta_pose.time;
        icp_info.fitness_score = fitness_score;
        icp_info.compute_time = ::base::Time::fromSeconds(elapsed_secs);
        _icp_info.write(icp_info);

        /** Debug Output **/
        if (_output_debug.value())
        {
            /** Filtered point cloud **/
            pcl::PCLPointCloud2 output_pc;
            pcl::toPCLPointCloud2(*source_cloud.get(), output_pc);
            _point_cloud_samples_out.write(output_pc);
        }

        /** In case target port is not connected **/
        if (!_point_cloud_target.connected())
        {
            /** The source is now the target for the next iteration **/
            target_cloud = boost::make_shared<PCLPointCloud>(*source_cloud);
        }
        else
        {
            ::pcl::PCLPointCloud2 point_cloud_target_sample;

            if(_point_cloud_target.readNewest(point_cloud_target_sample) == RTT::NewData)
            {
                #ifdef DEBUG_PRINTS
                std::cout<<"** [POINT_CLOUD_TARGET] received point_cloud_target **\n";
                #endif

                /** Convert to pcl point clouds **/
                pcl::fromPCLPointCloud2(point_cloud_target_sample, *target_cloud.get());
            }
        }
    }
}

void Icp::errorHook()
{
    IcpBase::errorHook();

}

void Icp::stopHook()
{
    IcpBase::stopHook();
}

void Icp::cleanupHook()
{
    IcpBase::cleanupHook();
}

void Icp::transformPointCloud(pcl::PointCloud<pcl::PointXYZ> &pcl_pc, const Eigen::Affine3d& transformation)
{
    for(std::vector< pcl::PointXYZ, Eigen::aligned_allocator<pcl::PointXYZ> >::iterator it = pcl_pc.begin(); it != pcl_pc.end(); it++)
    {
        Eigen::Vector3d point (it->x, it->y, it->z);
        point = transformation * point;
        *it = pcl::PointXYZ (point[0], point[1], point[2]);
    }
}

void Icp::pairAlign (const PCLPointCloudPtr cloud_src, const PCLPointCloudPtr cloud_tgt, PCLPointCloudPtr output, Eigen::Matrix4f &final_transform, bool downsample)
{
  //
  // Downsample for consistency and speed
  // \note enable this for large datasets
  PCLPointCloudPtr src (new PCLPointCloud);
  PCLPointCloudPtr tgt (new PCLPointCloud);
  pcl::VoxelGrid<PCLPointT> grid;
  if (downsample)
  {
    grid.setLeafSize (0.05, 0.05, 0.05);
    grid.setInputCloud (cloud_src);
    grid.filter (*src);

    grid.setInputCloud (cloud_tgt);
    grid.filter (*tgt);
  }
  else
  {
    src = cloud_src;
    tgt = cloud_tgt;
  }


  // Compute surface normals and curvature
  PCLPointCloudWithNormals::Ptr points_with_normals_src (new PCLPointCloudWithNormals);
  PCLPointCloudWithNormals::Ptr points_with_normals_tgt (new PCLPointCloudWithNormals);

    std::cout<<"IN PAIR ALIGN FUNCTION\n";
    std::cout<<"height: "<<src->height<<"\n";
    std::cout<<"width: "<<src->width<<"\n";
    std::cout<<"size(): "<<src->size()<<"\n";

  pcl::NormalEstimation<PCLPointT, PCLPointNormalT> norm_est;
  norm_est.setSearchMethod (pcl::search::KdTree<PCLPointT>::Ptr (new pcl::search::KdTree<PCLPointT>));
  norm_est.setRadiusSearch (0.1);

  norm_est.setInputCloud (src);
  norm_est.compute (*points_with_normals_src);
  pcl::copyPointCloud (*src, *points_with_normals_src);

  norm_est.setInputCloud (tgt);
  norm_est.compute (*points_with_normals_tgt);
  pcl::copyPointCloud (*tgt, *points_with_normals_tgt);

  // Instantiate our custom point representation (defined above) ...
  MyPointRepresentation point_representation;
  // ... and weight the 'curvature' dimension so that it is balanced against x, y, and z
  float alpha[4] = {1.0, 1.0, 1.0, 1.0};
  point_representation.setRescaleValues (alpha);

  // Align
  pcl::IterativeClosestPointNonLinear<PCLPointNormalT, PCLPointNormalT> reg;
  reg.setTransformationEpsilon (1e-6);
  // Set the maximum distance between two correspondences (src<->tgt) to 10cm
  // Note: adjust this based on the size of your datasets
  reg.setMaxCorrespondenceDistance (0.1);  
  // Set the point representation
  reg.setPointRepresentation (boost::make_shared<const MyPointRepresentation> (point_representation));

  reg.setInputSource (points_with_normals_src);
  reg.setInputTarget (points_with_normals_tgt);

  // Run the same optimization in a loop and visualize the results
  Eigen::Matrix4f Ti = Eigen::Matrix4f::Identity (), prev, targetToSource;
  PCLPointCloudWithNormals::Ptr reg_result = points_with_normals_src;
  reg.setMaximumIterations (2);
  for (int i = 0; i < 30; ++i)
  {
    PCL_INFO ("Iteration Nr. %d.\n", i);

    // save cloud for visualization purpose
    points_with_normals_src = reg_result;

    // Estimate
    reg.setInputSource (points_with_normals_src);
    reg.align (*reg_result);

    //accumulate transformation between each Iteration
    Ti = reg.getFinalTransformation () * Ti;

    //if the difference between this transformation and the previous one
    //is smaller than the threshold, refine the process by reducing
    //the maximal correspondence distance
    if (fabs ((reg.getLastIncrementalTransformation () - prev).sum ()) < reg.getTransformationEpsilon ())
      reg.setMaxCorrespondenceDistance (reg.getMaxCorrespondenceDistance () - 0.001);

    prev = reg.getLastIncrementalTransformation ();

  }

    // Get the transformation from target to source
    targetToSource = Ti.inverse();

    // Transform target back in source frame
    pcl::transformPointCloud (*cloud_tgt, *output, targetToSource);

    //add the source to the transformed target
    *output += *cloud_src;

    final_transform = targetToSource;
 }

