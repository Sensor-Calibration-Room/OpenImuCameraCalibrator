#include <algorithm>
#include <chrono> // NOLINT
#include <dirent.h>
#include <fstream>
#include <gflags/gflags.h>
#include <iostream>
#include <ostream>
#include <string>
#include <time.h>
#include <vector>

#include <opencv2/aruco.hpp>
#include <opencv2/aruco/charuco.hpp>
#include <opencv2/aruco/dictionary.hpp>
#include <opencv2/opencv.hpp>

#include "OpenCameraCalibrator/imu/read_gopro_imu_json.h"
#include "OpenCameraCalibrator/utils/types.h"
#include "OpenCameraCalibrator/utils/utils.h"

#include "OpenCameraCalibrator/basalt_spline/calib_helpers.h"
#include "OpenCameraCalibrator/basalt_spline/ceres_calib_spline_split.h"

#include "OpenCameraCalibrator/utils/json.h"
#include "theia/io/reconstruction_reader.h"
#include "theia/io/reconstruction_writer.h"
#include "theia/io/write_ply_file.h"
#include "theia/sfm/reconstruction.h"
#include <theia/sfm/camera/division_undistortion_camera_model.h>
#include <theia/sfm/camera/pinhole_camera_model.h>

// Input/output files.
DEFINE_string(
    gopro_telemetry_json, "",
    "Path to gopro telemetry json extracted with Sparsnet extractor.");
DEFINE_string(input_calibration_dataset, "",
              "Path to input calibration dataset.");
DEFINE_string(gyro_to_cam_initial_calibration, "",
              "Initial gyro to camera calibration json.");
DEFINE_string(imu_bias_file, "", "IMU bias json");
DEFINE_string(spline_error_weighting_json, "",
              "Path to spline error weighting data");
DEFINE_string(output_path, "", "Output path for results");
DEFINE_double(max_t, 1000., "Maximum nr of seconds to take");

using json = nlohmann::json;

using namespace cv;
using namespace theia;

int main(int argc, char *argv[]) {
  GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);

  // IMU Bias
  Eigen::Vector3d accl_bias, gyro_bias;
  CHECK(OpenCamCalib::ReadIMUBias(FLAGS_imu_bias_file, gyro_bias, accl_bias))
      << "Could not open " << FLAGS_imu_bias_file;

  // Load camera calibration reconstuction.
  theia::Reconstruction calib_dataset;
  CHECK(theia::ReadReconstruction(FLAGS_input_calibration_dataset,
                                  &calib_dataset))
      << "Could not read Reconstruction file.";
  // read gopro telemetry
  OpenCamCalib::CameraTelemetryData telemetry_data;
  CHECK(OpenCamCalib::ReadGoProTelemetry(FLAGS_gopro_telemetry_json,
                                         telemetry_data))
      << "Could not read: " << FLAGS_gopro_telemetry_json;

  // read a gyro to cam calibration json to initialize rotation between imu and
  // camera
  Eigen::Quaterniond imu2cam;
  double time_offset_imu_to_cam;
  CHECK(OpenCamCalib::ReadIMU2CamInit(FLAGS_gyro_to_cam_initial_calibration,
                                      imu2cam, time_offset_imu_to_cam))
      << "Could not read: " << FLAGS_gyro_to_cam_initial_calibration;
  Sophus::SE3<double> T_i_c_init(imu2cam.conjugate(), Eigen::Vector3d(0, 0, 0));

  CHECK(FLAGS_spline_error_weighting_json != "")
      << "You need to provide spline error weighting factors. Create with "
         "get_sew_for_dataset.py.";
  OpenCamCalib::SplineWeightingData weight_data;
  CHECK(OpenCamCalib::ReadSplineErrorWeighting(
      FLAGS_spline_error_weighting_json, weight_data))
      << "Could not open " << FLAGS_spline_error_weighting_json;
  // initialize spline parameters
  const double dt_r3 = weight_data.dt_r3;
  const double dt_so3 = weight_data.dt_so3;
  const double imu_update_rate_hz =
      1. / (telemetry_data.accelerometer.timestamp_ms[1] -
            telemetry_data.accelerometer.timestamp_ms[0]);

  // Start
  // Number of cameras.
  const auto &view_ids = calib_dataset.ViewIds();
  std::vector<double> timestamps;
  // get all timestamps and find smallest one
  // Output each camera.
  for (const ViewId view_id : view_ids) {
    const View &view = *calib_dataset.View(view_id);
    double timestamp = std::stod(view.Name());
    timestamps.push_back(timestamp);
  }

  // find smallest timestamp
  auto result = std::minmax_element(timestamps.begin(), timestamps.end());
  double t0 = timestamps[result.first - timestamps.begin()];
  double tend = timestamps[result.second - timestamps.begin()];
  const int64_t start_t_ns = t0 * 1e9;
  const int64_t end_t_ns = tend * 1e9;
  const int64_t dt_so3_ns = dt_so3 * 1e9;
  const int64_t dt_r3_ns = dt_r3 * 1e9;

  const int N = 5;
  CeresCalibrationSplineSplit<N> calib_spline(dt_so3_ns, dt_r3_ns, start_t_ns);
  calib_spline.setCalib(calib_dataset);
  calib_spline.setT_i_c(T_i_c_init);
  std::unordered_map<TimeCamId, CalibCornerData> calib_corners;
  std::unordered_map<TimeCamId, CalibInitPoseData> calib_init_poses;
  for (const ViewId view_id : view_ids) {
    const View &view = *calib_dataset.View(view_id);
    double timestamp = std::stod(view.Name());
    if (timestamp >= tend || timestamp < t0)
      continue;
    TimeCamId t_c_id(timestamp * 1e9, 0);
    CalibCornerData corner_data;
    const std::vector<theia::TrackId> trackIds = view.TrackIds();
    for (size_t t = 0; t < trackIds.size(); ++t) {
      corner_data.corners.push_back(*view.GetFeature(trackIds[t]));
    }
    corner_data.track_ids = trackIds;
    calib_corners[t_c_id] = corner_data;
    CalibInitPoseData pose_data;
    pose_data.T_a_c = Sophus::SE3<double>(
        view.Camera().GetOrientationAsRotationMatrix().transpose(),
        view.Camera().GetPosition());
    calib_init_poses[t_c_id] = pose_data;
  }

  // Initialize gravity
  bool g_initialized = false;
  Eigen::Vector3d g_a_init;
  for (size_t j = 0; j < timestamps.size(); ++j) {
    int64_t timestamp_ns = timestamps[j] * 1e9;

    TimeCamId tcid(timestamp_ns, 0);
    const auto cp_it = calib_init_poses.find(tcid);

    if (cp_it != calib_init_poses.end()) {
      Sophus::SE3d T_a_i = cp_it->second.T_a_c * T_i_c_init.inverse();

      if (!g_initialized) {
        for (size_t i = 0;
             i < telemetry_data.accelerometer.acc_masurement.size() &&
             !g_initialized;
             i++) {
          const Eigen::Vector3d ad =
              telemetry_data.accelerometer.acc_masurement[i] + accl_bias;
          const int64_t accl_t_ns =
              telemetry_data.accelerometer.timestamp_ms[i] * 1e9;
          if (std::abs(accl_t_ns - timestamp_ns) < 3000000) {
            g_a_init = T_a_i.so3() * ad;
            g_initialized = true;
            std::cout << "g_a initialized with " << g_a_init.transpose()
                      << std::endl;
          }
        }
      }
    }
  }
  calib_spline.setG(g_a_init);

  // init spline from first camera
  TimeCamId tcid_init(timestamps[result.first - timestamps.begin()], 0);
  Sophus::SE3d T_w_i_init =
      calib_init_poses.at(tcid_init).T_a_c * T_i_c_init.inverse();
  const int num_knots_so3 = (end_t_ns - start_t_ns) / dt_so3_ns + N;
  const int num_knots_r3 = (end_t_ns - start_t_ns) / dt_r3_ns + N;
  const double cam_readout = 1.0 / 30.0;
  std::cout << "Initializing " << num_knots_so3 << " SO3 knots.\n";
  std::cout << "Initializing " << num_knots_r3 << " R3 knots.\n";
  calib_spline.init(T_w_i_init, num_knots_so3, num_knots_r3);

  int num_gyro = 0;
  int num_accel = 0;
  int num_corner = 0;
  int num_frames = 0;

  const int sub_sample_imu = 1;
  std::cout << "Trajectory start time: " << t0 << " tend: " << tend
            << std::endl;
  std::cout << "Knot spacing SO3 / R3: " << dt_so3 << "/" << dt_r3 << "\n";
  std::cout << "Error weighting SO3 / R3: " << weight_data.var_so3 << "/"
            << weight_data.var_r3 << "\n";

  // add corners
  for (const auto &kv : calib_corners) {
    if (kv.first.frame_id >= start_t_ns && kv.first.frame_id < end_t_ns) {
//      if (cam_readout > 0.0) {

//        calib_spline.addRSCornersMeasurement(
//            &kv.second, &calib_dataset, &calib_dataset.View(0)->Camera(),
//            cam_readout, kv.first.cam_id, kv.first.frame_id);
//      } else {
        calib_spline.addCornersMeasurement(&kv.second, &calib_dataset,
                                           &calib_dataset.View(0)->Camera(),
                                           kv.first.cam_id, kv.first.frame_id);
      //}
      num_corner += kv.second.track_ids.size();
      num_frames++;
    }
  }

  // Add Accelerometer
  for (size_t i = 0; i < telemetry_data.accelerometer.acc_masurement.size();
       ++i) {
    if (i % sub_sample_imu == 0) {
      const double t = (telemetry_data.accelerometer.timestamp_ms[i] / 1000.0 +
                        time_offset_imu_to_cam) * 1e9;
      if (t < start_t_ns || t >= end_t_ns)
        continue;
      ++num_accel;
      calib_spline.addAccelMeasurement(
          telemetry_data.accelerometer.acc_masurement[i] + accl_bias, t,
          weight_data.var_r3, false);
    }
  }
  // Add Gyroscope
  for (size_t i = 0; i < telemetry_data.gyroscope.gyro_measurement.size();
       ++i) {
    if (i % sub_sample_imu == 0) {
      const double t = (telemetry_data.gyroscope.timestamp_ms[i] / 1000.0 +
                        time_offset_imu_to_cam) * 1e9;
      if (t < start_t_ns || t >= end_t_ns)
        continue;
      ++num_gyro;
      calib_spline.addGyroMeasurement(
          telemetry_data.gyroscope.gyro_measurement[i] + gyro_bias, t,
          weight_data.var_so3, false);
    }
  }

  // calib_spline.meanReprojection(calib_corners);
  ceres::Solver::Summary summary = calib_spline.optimize();

  double mean_reproj = calib_spline.meanReprojection(calib_corners);

  std::cout << "num_gyro " << num_gyro << " num_accel " << num_accel
            << " num_corner " << num_corner << " num_frames " << num_frames
            << " duration " << (end_t_ns - start_t_ns) * 1e-9 << std::endl;

  std::cout << "g: " << calib_spline.getG().transpose() << std::endl;
  std::cout << "accel_bias: " << calib_spline.getAccelBias().transpose()
            << std::endl;
  std::cout << "gyro_bias: " << calib_spline.getGyroBias().transpose()
            << std::endl;
  std::cout << "T_i_c " << calib_spline.getT_i_c().matrix() << std::endl;
  std::cout << "mean_reproj: " << mean_reproj << std::endl;

  return 0;
}
