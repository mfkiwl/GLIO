﻿#include "utils/common.h"
#include "utils/timer.h"
#include "utils/math_tools.h"

#define  PI  3.1415926535

class Preprocessing {
private:
    float cloudCurvature[400000];
    int cloudSortInd[400000];
    int cloudNeighborPicked[400000];
    int cloudLabel[400000];
    int ds_rate = 2;
    double ds_v = 0.4;

    bool comp (int i,int j) { return (cloudCurvature[i]<cloudCurvature[j]);}

    ros::NodeHandle nh;

    ros::Subscriber sub_Lidar_cloud;
    ros::Subscriber sub_imu;

    ros::Publisher pub_surf;
    ros::Publisher pub_edge;
    ros::Publisher pub_cutted_cloud;

    int pre_num = 0;

    pcl::PointCloud<PointType> lidar_cloud_in;
    std_msgs::Header cloud_header;

    vector<sensor_msgs::ImuConstPtr> imu_buf;
    int idx_imu = 0;
    double current_time_imu = -1;

    Eigen::Vector3d gyr_0;
    Eigen::Quaterniond qIMU = Eigen::Quaterniond::Identity();
    Eigen::Vector3d rIMU = Eigen::Vector3d::Zero();
    bool first_imu = false;
    string imu_topic;

    std::deque<sensor_msgs::PointCloud2> cloud_queue;
    sensor_msgs::PointCloud2 current_cloud_msg;
    double time_scan_next;

    int N_SCANS = 64;

    double qlb0, qlb1, qlb2, qlb3;
    Eigen::Quaterniond q_lb;

    string frame_id = "GLIO";

    // "/points_raw", "/velodyne_pcl_gen/cloud", "/data_raw/lidar"
    string lidar_topic = "/velodyne_points";

    double runtime = 0;

    double edgeThreshold = 0;
    double surfThreshold = 0;
    Eigen::Matrix4f lidar_extrinsic;


public:
    Preprocessing():
        nh("~"){

        // lidar_extrinsic << 0.999945, 0.0104382,0.00142715, 0.00634028,
        //                     -0.00005845,0.845067, -0.534599, -0.354308,
        //                     -0.0067067, 0.534558, 0.845105, 0.163907,
        //                     0         , 0       , 0       ,  1;
        lidar_extrinsic << 0.999945, 0.0104382,0.00142715, 0.00634028,                            
                          -0.00805845,0.845067, -0.534599, -0.354308,                           
                          -0.00678627, 0.534558, 0.845105, 0.163907,                           
                          0         , 0       , 0       ,  1;
        if (!getParameter("/lidar_odometry/lidar_topic", lidar_topic)) {
            ROS_WARN("lidar_topic not set, use default value: /velodyne_points");
            lidar_topic = "/velodyne_points";
        }

        if (!getParameter("/lidar_odometry/line_num", N_SCANS)) {
            ROS_WARN("line_num not set, use default value: 64");
            N_SCANS = 64;
        }

        if (!getParameter("/lidar_odometry/ds_rate", ds_rate)) {
            ROS_WARN("ds_rate not set, use default value: 1");
            ds_rate = 1;
        }

        if (!getParameter("/common/frame_id", frame_id)) {
            ROS_WARN("frame_id not set, use default value: lili_odom");
            frame_id = "lili_odom";
        }

        if (!getParameter("/IMU/imu_topic", imu_topic)) {
            ROS_WARN("imu_topic not set, use default value: /imu/data");
            imu_topic = "/imu/data";
        }

        //extrinsic parameters
        if (!getParameter("/Estimator/ql2b_w", qlb0)) {
            ROS_WARN("ql2b_w not set, use default value: 1");
            qlb0 = 1;
        }

        if (!getParameter("/Estimator/ql2b_x", qlb1)) {
            ROS_WARN("ql2b_x not set, use default value: 0");
            qlb1 = 0;
        }

        if (!getParameter("/Estimator/ql2b_y", qlb2)) {
            ROS_WARN("ql2b_y not set, use default value: 0");
            qlb2 = 0;
        }

        if (!getParameter("/Estimator/ql2b_z", qlb3)) {
            ROS_WARN("ql2b_z not set, use default value: 0");
            qlb3 = 0;
        }

        if (!getParameter("/lidar_odometry/edgeThreshold", edgeThreshold)) {
            ROS_WARN("edgeThreshold not set, use default value: 1.0");
            edgeThreshold = 1.0;
        }

        if (!getParameter("/lidar_odometry/surfThreshold", surfThreshold)) {
            ROS_WARN("surfThreshold not set, use default value: 0.1");
            surfThreshold = 0.1;
        }

        q_lb = Eigen::Quaterniond(qlb0, qlb1, qlb2, qlb3);

        sub_Lidar_cloud = nh.subscribe<sensor_msgs::PointCloud2>(lidar_topic, 100, &Preprocessing::cloudHandler, this);
        sub_imu = nh.subscribe<sensor_msgs::Imu>(imu_topic, 200, &Preprocessing::imuHandler, this);

        pub_surf = nh.advertise<sensor_msgs::PointCloud2>("/surf_features", 100);
        pub_edge = nh.advertise<sensor_msgs::PointCloud2>("/edge_features", 100);
        pub_cutted_cloud = nh.advertise<sensor_msgs::PointCloud2>("/lidar_cloud_cutted", 100);
    }

    ~Preprocessing(){}

    template <typename PointT>
    void removeClosedPointCloud(const pcl::PointCloud<PointT> &cloud_in,
                                pcl::PointCloud<PointT> &cloud_out, float thres) {
        if (&cloud_in != &cloud_out) {
            cloud_out.header = cloud_in.header;
            cloud_out.points.resize(cloud_in.points.size());
        }

        size_t j = 0;

        for (size_t i = 0; i < cloud_in.points.size(); ++i) {
            if (cloud_in.points[i].x * cloud_in.points[i].x +
                    cloud_in.points[i].y * cloud_in.points[i].y +
                    cloud_in.points[i].z * cloud_in.points[i].z < thres * thres)
                continue;
            cloud_out.points[j] = cloud_in.points[i];
            j++;
        }
        if (j != cloud_in.points.size()) {
            cloud_out.points.resize(j);
        }

        cloud_out.height = 1;
        cloud_out.width = static_cast<uint32_t>(j);
        cloud_out.is_dense = true;
    }

    template <typename PointT>
    double getDepth(PointT pt) {
        return sqrt(pt.x*pt.x + pt.y*pt.y + pt.z*pt.z);
    }


    PointType undistortion(PointType pt, const Eigen::Quaterniond quat) {
        double dt = 0.1;
        int line = int(pt.intensity);
        double dt_i = pt.intensity - line;

        double ratio_i = dt_i / dt;
        if(ratio_i >= 1.0) {
            ratio_i = 1.0;
        }

        Eigen::Quaterniond q0 = Eigen::Quaterniond::Identity();
        Eigen::Quaterniond q_si = q0.slerp(ratio_i, qIMU);

        Eigen::Vector3d pt_i(pt.x, pt.y, pt.z);

        q_si = q_lb * q_si * q_lb.inverse();
        Eigen::Vector3d pt_s = q_si * pt_i;

        PointType p_out;
        p_out.x = pt_s.x();
        p_out.y = pt_s.y();
        p_out.z = pt_s.z();
        p_out.intensity = pt.intensity;
        return p_out;
    }

    void solveRotation(double dt, Eigen::Vector3d angular_velocity)
    {
        Eigen::Vector3d un_gyr = 0.5 * (gyr_0 + angular_velocity);
        qIMU *= deltaQ(un_gyr * dt);
        gyr_0 = angular_velocity;
    }

    PointXYZIRT TransfromPointToBody(const PointXYZIRT & pi){
        Eigen::Vector3f p(pi.x,pi.y,pi.z);
        p = lidar_extrinsic.block(0,0,3,3) * p;
        PointXYZIRT p0;
        p0.x = p.x();
        p0.y = p.y();
        p0.z = p.z();
        p0.intensity = pi.intensity;
        p0.ring = pi.ring;
        p0.timestamp = pi.intensity;
        return p0;
    }

    void processIMU(double t_cur)
    {
        double rx = 0, ry = 0, rz = 0;
        int i = idx_imu;
        if(i >= imu_buf.size())
            i--;
        while(imu_buf[i]->header.stamp.toSec() < t_cur) {

            double t = imu_buf[i]->header.stamp.toSec();
            if (current_time_imu < 0)
                current_time_imu = t;
            double dt = t - current_time_imu;
            current_time_imu = imu_buf[i]->header.stamp.toSec();

            rx = imu_buf[i]->angular_velocity.x;
            ry = imu_buf[i]->angular_velocity.y;
            rz = imu_buf[i]->angular_velocity.z;
            solveRotation(dt, Eigen::Vector3d(rx, ry, rz));
            i++;
            if(i >= imu_buf.size())
                break;
        }

        if(i < imu_buf.size()) {
            double dt1 = t_cur - current_time_imu;
            double dt2 = imu_buf[i]->header.stamp.toSec() - t_cur;

            double w1 = dt2 / (dt1 + dt2);
            double w2 = dt1 / (dt1 + dt2);

            rx = w1 * rx + w2 * imu_buf[i]->angular_velocity.x;
            ry = w1 * ry + w2 * imu_buf[i]->angular_velocity.y;
            rz = w1 * rz + w2 * imu_buf[i]->angular_velocity.z;
            solveRotation(dt1, Eigen::Vector3d(rx, ry, rz));
        }
        current_time_imu = t_cur;
        idx_imu = i;
    }

    void imuHandler(const sensor_msgs::ImuConstPtr& ImuIn)
    {

        imu_buf.push_back(ImuIn);
//        sensor_msgs::ImuPtr raw_imu_msg (new sensor_msgs::Imu(*ImuIn));
//        //imu_in.angular_velocity.x/180*3.1415926, imu_in.angular_velocity.y/180*3.1415926, imu_in.angular_velocity.z/180*3.141592
//        raw_imu_msg->angular_velocity.x =  raw_imu_msg->angular_velocity.x/180*3.1415926;
//
//
//        raw_imu_msg->angular_velocity.x =  raw_imu_msg->angular_velocity.x/180*3.1415926;
//        raw_imu_msg->angular_velocity.y =  raw_imu_msg->angular_velocity.y/180*3.1415926;
//        raw_imu_msg->angular_velocity.z =  raw_imu_msg->angular_velocity.z/180*3.1415926;
//        double correct_stamp  = gps2utc(2153, ImuIn->header.stamp.toSec());
//        raw_imu_msg->header.stamp = ros::Time(correct_stamp);
//
//        imu_buf.push_back(raw_imu_msg);

        if(imu_buf.size() > 3000)
            imu_buf[imu_buf.size() - 3001] = nullptr;

        if (current_time_imu < 0)
            current_time_imu = ImuIn->header.stamp.toSec();

        if (!first_imu)
        {
            first_imu = true;
            double rx = 0, ry = 0, rz = 0;
            rx = ImuIn->angular_velocity.x;
            ry = ImuIn->angular_velocity.y;
            rz = ImuIn->angular_velocity.z;
            Eigen::Vector3d angular_velocity(rx, ry, rz);
            gyr_0 = angular_velocity;
        }
    }

    bool getScanIDHLD32(double angle, int& SCANID)
    {
        //
        std::vector<double> angle2ID;
        angle2ID.push_back(-30.67); // 1st ring
        angle2ID.push_back(-9.33);
        angle2ID.push_back(-29.33);
        angle2ID.push_back(-8.00);
        angle2ID.push_back(-28.00);
        angle2ID.push_back(-6.67);
        angle2ID.push_back(-26.67);
        angle2ID.push_back(-5.33); 

        angle2ID.push_back(-25.33);
        angle2ID.push_back(-4.00);
        angle2ID.push_back(-24.00);
        angle2ID.push_back(-2.67);
        angle2ID.push_back(-22.67);
        angle2ID.push_back(-1.33);
        angle2ID.push_back(-21.33);
        angle2ID.push_back(0.00); 

        angle2ID.push_back(-20.00);
        angle2ID.push_back(1.33);
        angle2ID.push_back(-18.67);
        angle2ID.push_back(2.67);
        angle2ID.push_back(-17.33);
        angle2ID.push_back(4.00);
        angle2ID.push_back(-16.00);
        angle2ID.push_back(5.33); 

        angle2ID.push_back(-14.67);
        angle2ID.push_back(6.67);
        angle2ID.push_back(-13.33);
        angle2ID.push_back(8.0);
        angle2ID.push_back(-12.00);
        angle2ID.push_back(9.33);
        angle2ID.push_back(-10.67);
        angle2ID.push_back(10.67);

        int length = angle2ID.size();
        int diff = 10000;
        for(int i = 0; i < length; i++)
        {
            double diffTmp = fabs(angle2ID[i] - angle);
            if(diffTmp < diff)
            {
                SCANID = i;
            }
        }
    }
    
    double gps2utc(double gps_week, double gps_second){
        
        return (gps_week * 604800.0 + gps_second - 18.0) + 315964800.0;
    }

    void cloudHandler( const sensor_msgs::PointCloud2ConstPtr &laserCloudMsg)
    {
        // cache point cloud
        cloud_queue.push_back(*laserCloudMsg);
//        sensor_msgs::PointCloud2Ptr correctlaserCloudMsg (new sensor_msgs::PointCloud2(*laserCloudMsg));
//        // for date 2021/04/16 03:20:54.000
//        double correct_stamp  = gps2utc(2153, laserCloudMsg->header.stamp.toSec());
//        correctlaserCloudMsg->header.stamp = ros::Time(correct_stamp);
//        cloud_queue.push_back(*correctlaserCloudMsg);
        if (cloud_queue.size() <= 2)
            return;
        else {
            current_cloud_msg = cloud_queue.front();
            cloud_queue.pop_front();

            cloud_header = current_cloud_msg.header;
            cloud_header.frame_id = frame_id;
            time_scan_next = cloud_queue.front().header.stamp.toSec();
        }

        int tmpIdx = 0;
        if(idx_imu > 0)
            tmpIdx = idx_imu - 1;
        if (imu_buf.empty() || imu_buf[tmpIdx]->header.stamp.toSec() > time_scan_next) {
            ROS_WARN("Waiting for IMU data ...");
            return;
        }

//        Timer t_pre("LidarPreprocessing");
//        Timer t_pre_1("LidarPreprocessing_1");
//        Timer t_pre_2("LidarPreprocessing_2");
//        Timer t_pre_3("LidarPreprocessing_3");
//        Timer t_pre_4("LidarPreprocessing_4");

        std::vector<int> scanStartInd(N_SCANS, 0);
        std::vector<int> scanEndInd(N_SCANS, 0);

//        pcl::PointCloud<PointXYZIRT> lidar_cloud_in;
        pcl::PointCloud<PointType> lidar_cloud_in;
        pcl::fromROSMsg(current_cloud_msg, lidar_cloud_in);

        std::vector<int> indices;

        pcl::removeNaNFromPointCloud(lidar_cloud_in, lidar_cloud_in, indices);
        removeClosedPointCloud(lidar_cloud_in, lidar_cloud_in, 3.0);


        int cloudSize = lidar_cloud_in.points.size();
        float startOri = -atan2(lidar_cloud_in.points[0].y, lidar_cloud_in.points[0].x);
        float endOri = -atan2(lidar_cloud_in.points[cloudSize - 1].y,
                lidar_cloud_in.points[cloudSize - 1].x) +
                2 * M_PI;

        if (endOri - startOri > 3 * M_PI)
            endOri -= 2 * M_PI;

        else if (endOri - startOri < M_PI)
            endOri += 2 * M_PI;


        if(first_imu)
            processIMU(time_scan_next);
        if(isnan(qIMU.w()) || isnan(qIMU.x()) || isnan(qIMU.y()) || isnan(qIMU.z())) {
            qIMU = Eigen::Quaterniond::Identity();
        }
//        t_pre_1.tic_toc();
        bool halfPassed = false;
        int count = cloudSize;
        PointType point;
        PointType point_undis;
        std::vector<pcl::PointCloud<PointType>> laserCloudScans(N_SCANS);
        for (int i = 0; i < cloudSize; i++) {
//            PointXYZIRT point_transform;
//
//            point_transform = TransfromPointToBody(lidar_cloud_in.points[i]);
//            point.x = point_transform.x;
//            point.y = point_transform.y;
//            point.z = point_transform.z;
//            point.intensity = 0.1 * point_transform.intensity;
            point.x = lidar_cloud_in.points[i].x;
            point.y = lidar_cloud_in.points[i].y;
            point.z = lidar_cloud_in.points[i].z;
            point.intensity = 0.1 * lidar_cloud_in.points[i].intensity;


            float angle = atan(point.z / sqrt(point.x * point.x + point.y * point.y)) * 180 / M_PI;
            int scanID = 0;

            if (N_SCANS == 16) {
                scanID = int((angle + 15) / 2 + 0.5);
                if (scanID > (N_SCANS - 1) || scanID < 0) {
                    count--;
                    continue;
                }
            }
            else if (N_SCANS == 32) {
                scanID = int((angle + 92.0/3.0) * 3.0 / 4.0);
                // std::cout << "Scan ID: " << scanID << std::endl;
                // std::cout << "angle:   " << angle << std::endl;
                // getScanIDHLD32(angle, scanID);
                // std::cout << "Scan ID-M: " << scanID << std::endl;
                // std::cout << "angle-M:   " << angle << std::endl;
                if (scanID > (N_SCANS - 1) || scanID < 0) {
                    count--;
                    continue;
                }
            }
//            else if (N_SCANS == 40) {
//                scanID = lidar_cloud_in.points[i].ring;
//                // std::cout << "Scan ID: " << scanID << std::endl;
//                // std::cout << "angle:   " << angle << std::endl;
//                // getScanIDHLD32(angle, scanID);
//                // std::cout << "Scan ID-M: " << scanID << std::endl;
//                // std::cout << "angle-M:   " << angle << std::endl;
//                if (scanID > (N_SCANS - 1) || scanID < 0) {
//                    count--;
//                    continue;
//                }
//            }
            else if (N_SCANS == 64) {
                if (angle >= -8.83)
                    scanID = int((2 - angle) * 3.0 + 0.5);
                else
                    scanID = N_SCANS / 2 + int((-8.83 - angle) * 2.0 + 0.5);

                // use [0 50]  > 50 remove outlies
                if (angle > 2 || angle < -24.33 || scanID > 50 || scanID < 0) {
                    count--;
                    continue;
                }
            }
            else {
                printf("wrong scan number\n");
                ROS_BREAK();
            }

            float ori = -atan2(point.y, point.x);
            if (!halfPassed) {
                if (ori < startOri - M_PI / 2)
                    ori += 2 * M_PI;
                else if (ori > startOri + M_PI * 3 / 2)
                    ori -= 2 * M_PI;

                if (ori - startOri > M_PI)
                    halfPassed = true;
            }
            else {
                ori += 2 * M_PI;
                if (ori < endOri - M_PI * 3 / 2)
                    ori += 2 * M_PI;
                else if (ori > endOri + M_PI / 2)
                    ori -= 2 * M_PI;
            }

            float relTime = (ori - startOri) / (endOri - startOri);
//            point.intensity = scanID;
            point.intensity = scanID + 0.1 * relTime;


            point_undis = undistortion(point, qIMU);
//            laserCloudScans[scanID].push_back(point);
            laserCloudScans[scanID].push_back(point_undis);
        }
//        std::cout << "cloudSize: " << cloudSize << std::endl;
//        t_pre_2.tic_toc();
        cloudSize = count;
        // printf("points size %d \n", cloudSize);

        pcl::PointCloud<PointType>::Ptr laserCloud(new pcl::PointCloud<PointType>());
        for (int i = 0; i < N_SCANS; i++) {
            scanStartInd[i] = laserCloud->size() + 5;
            *laserCloud += laserCloudScans[i];
            scanEndInd[i] = laserCloud->size() - 6;
        }


        for (int i = 5; i < cloudSize - 5; i++) {
            float diffX = laserCloud->points[i - 5].x + laserCloud->points[i - 4].x + laserCloud->points[i - 3].x + laserCloud->points[i - 2].x + laserCloud->points[i - 1].x - 10 * laserCloud->points[i].x + laserCloud->points[i + 1].x + laserCloud->points[i + 2].x + laserCloud->points[i + 3].x + laserCloud->points[i + 4].x + laserCloud->points[i + 5].x;
            float diffY = laserCloud->points[i - 5].y + laserCloud->points[i - 4].y + laserCloud->points[i - 3].y + laserCloud->points[i - 2].y + laserCloud->points[i - 1].y - 10 * laserCloud->points[i].y + laserCloud->points[i + 1].y + laserCloud->points[i + 2].y + laserCloud->points[i + 3].y + laserCloud->points[i + 4].y + laserCloud->points[i + 5].y;
            float diffZ = laserCloud->points[i - 5].z + laserCloud->points[i - 4].z + laserCloud->points[i - 3].z + laserCloud->points[i - 2].z + laserCloud->points[i - 1].z - 10 * laserCloud->points[i].z + laserCloud->points[i + 1].z + laserCloud->points[i + 2].z + laserCloud->points[i + 3].z + laserCloud->points[i + 4].z + laserCloud->points[i + 5].z;

            cloudCurvature[i] = diffX * diffX + diffY * diffY + diffZ * diffZ;
            cloudSortInd[i] = i;
            cloudNeighborPicked[i] = 0;
            cloudLabel[i] = 0;
        }
//        t_pre_3.tic_toc();
        pcl::PointCloud<PointType> cornerPointsSharp;
        pcl::PointCloud<PointType> cornerPointsLessSharp;
        pcl::PointCloud<PointType> surfPointsFlat;
        pcl::PointCloud<PointType> surfPointsLessFlat;

        for (int i = 0; i < N_SCANS; i++) {
            if( scanEndInd[i] - scanStartInd[i] < 6 || i % ds_rate != 0)
                continue;
            pcl::PointCloud<PointType>::Ptr surfPointsLessFlatScan(new pcl::PointCloud<PointType>);
            for (int j = 0; j < 6; j++) {
                int sp = scanStartInd[i] + (scanEndInd[i] - scanStartInd[i]) * j / 6;
                int ep = scanStartInd[i] + (scanEndInd[i] - scanStartInd[i]) * (j + 1) / 6 - 1;

                auto bound_comp = bind(&Preprocessing::comp, this, _1, _2);
                std::sort(cloudSortInd + sp, cloudSortInd + ep + 1, bound_comp);

                int largestPickedNum = 0;
                for (int k = ep; k >= sp; k--) {
                    int ind = cloudSortInd[k];

                    /* select the edge features */
                    if (cloudNeighborPicked[ind] == 0 &&
                            cloudCurvature[ind] > edgeThreshold) { // original 2.0

                        largestPickedNum++;
                        if (largestPickedNum <= 2) {
                            cloudLabel[ind] = 2;
                            cornerPointsSharp.push_back(laserCloud->points[ind]);
                            cornerPointsLessSharp.push_back(laserCloud->points[ind]);
                        }
                        else if (largestPickedNum <= 10) {
                            cloudLabel[ind] = 1;
                            cornerPointsLessSharp.push_back(laserCloud->points[ind]);
                        }
                        else
                            break;

                        cloudNeighborPicked[ind] = 1;

                        for (int l = 1; l <= 5; l++) {
                            float diffX = laserCloud->points[ind + l].x - laserCloud->points[ind + l - 1].x;
                            float diffY = laserCloud->points[ind + l].y - laserCloud->points[ind + l - 1].y;
                            float diffZ = laserCloud->points[ind + l].z - laserCloud->points[ind + l - 1].z;
                            if (diffX * diffX + diffY * diffY + diffZ * diffZ > 0.05)
                                break;

                            cloudNeighborPicked[ind + l] = 1;
                        }
                        for (int l = -1; l >= -5; l--) {
                            float diffX = laserCloud->points[ind + l].x - laserCloud->points[ind + l + 1].x;
                            float diffY = laserCloud->points[ind + l].y - laserCloud->points[ind + l + 1].y;
                            float diffZ = laserCloud->points[ind + l].z - laserCloud->points[ind + l + 1].z;
                            if (diffX * diffX + diffY * diffY + diffZ * diffZ > 0.05)
                                break;

                            cloudNeighborPicked[ind + l] = 1;
                        }
                    }
                }

                int smallestPickedNum = 0;
                for (int k = sp; k <= ep; k++) {
                    int ind = cloudSortInd[k];

                    if(laserCloud->points[ind].x*laserCloud->points[ind].x+laserCloud->points[ind].y*laserCloud->points[ind].y+laserCloud->points[ind].z*laserCloud->points[ind].z < 0.25)
                        continue;

                    /* select the plannar features */
                    if (cloudNeighborPicked[ind] == 0 &&
                            cloudCurvature[ind] < surfThreshold) { // 0.1 original

                        cloudLabel[ind] = -1;
                        surfPointsFlat.push_back(laserCloud->points[ind]);

                        smallestPickedNum++;
                        if (smallestPickedNum >= 4)
                            break;

                        cloudNeighborPicked[ind] = 1;
                        for (int l = 1; l <= 5; l++) {
                            float diffX = laserCloud->points[ind + l].x - laserCloud->points[ind + l - 1].x;
                            float diffY = laserCloud->points[ind + l].y - laserCloud->points[ind + l - 1].y;
                            float diffZ = laserCloud->points[ind + l].z - laserCloud->points[ind + l - 1].z;
                            if (diffX * diffX + diffY * diffY + diffZ * diffZ > 0.05)
                                break;

                            cloudNeighborPicked[ind + l] = 1;
                        }
                        for (int l = -1; l >= -5; l--) {
                            float diffX = laserCloud->points[ind + l].x - laserCloud->points[ind + l + 1].x;
                            float diffY = laserCloud->points[ind + l].y - laserCloud->points[ind + l + 1].y;
                            float diffZ = laserCloud->points[ind + l].z - laserCloud->points[ind + l + 1].z;
                            if (diffX * diffX + diffY * diffY + diffZ * diffZ > 0.05)
                                break;

                            cloudNeighborPicked[ind + l] = 1;
                        }
                    }
                }

                for (int k = sp; k <= ep; k++) {
                    if(laserCloud->points[k].x*laserCloud->points[k].x+laserCloud->points[k].y*laserCloud->points[k].y+laserCloud->points[k].z*laserCloud->points[k].z < 0.25)
                        continue;
                    if (cloudLabel[k] <= 0)
                        surfPointsLessFlatScan->push_back(laserCloud->points[k]);
                }
            }

            pcl::PointCloud<PointType> surfPointsLessFlatScanDS;
            pcl::VoxelGrid<PointType> downSizeFilter;
            downSizeFilter.setInputCloud(surfPointsLessFlatScan);
            downSizeFilter.setLeafSize(ds_v, ds_v, ds_v);
            downSizeFilter.filter(surfPointsLessFlatScanDS);

            surfPointsLessFlat += surfPointsLessFlatScanDS;
        }
//        t_pre_4.tic_toc();
        sensor_msgs::PointCloud2 laserCloudOutMsg;
        pcl::toROSMsg(*laserCloud, laserCloudOutMsg);
        laserCloudOutMsg.header.stamp = current_cloud_msg.header.stamp;
        laserCloudOutMsg.header.frame_id = frame_id;
        pub_cutted_cloud.publish(laserCloudOutMsg);

        sensor_msgs::PointCloud2 cornerPointsLessSharpMsg;
        pcl::toROSMsg(cornerPointsLessSharp, cornerPointsLessSharpMsg);
        cornerPointsLessSharpMsg.header.stamp = current_cloud_msg.header.stamp;
        cornerPointsLessSharpMsg.header.frame_id = frame_id;
        pub_edge.publish(cornerPointsLessSharpMsg);

        sensor_msgs::PointCloud2 surfPointsLessFlat2;
        pcl::toROSMsg(surfPointsLessFlat, surfPointsLessFlat2);
        surfPointsLessFlat2.header.stamp = current_cloud_msg.header.stamp;
        surfPointsLessFlat2.header.frame_id = frame_id;
        pub_surf.publish(surfPointsLessFlat2);

        qIMU = Eigen::Quaterniond::Identity();
        rIMU = Eigen::Vector3d::Zero();
//        t_pre.tic_toc();
//        runtime += t_pre.toc();
        //cout<<"pre_num: "<<++pre_num<<endl;
        //cout<<"Preprocessing average run time: "<<runtime / pre_num<<endl;
    }
};

int main(int argc, char** argv) {
    ros::init(argc, argv, "GLIO");
    Preprocessing Pre;
    ROS_INFO("\033[1;32m---->\033[0m Preprocessing Started.");

    ros::spin();
    return 0;
}
