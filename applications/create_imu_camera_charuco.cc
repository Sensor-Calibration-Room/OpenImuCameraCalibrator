#include <dirent.h>
#include <gflags/gflags.h>
#include <time.h>
#include <algorithm>
#include <chrono>  // NOLINT
#include <string>
#include <vector>
#include <iostream>

#include <opencv2/aruco.hpp>
#include <opencv2/aruco/charuco.hpp>
#include <opencv2/aruco/dictionary.hpp>
#include <opencv2/opencv.hpp>

#include "OpenCameraCalibrator/imu/read_gopro_imu_json.h"
#include "OpenCameraCalibrator/utils/types.h"
#include "OpenCameraCalibrator/utils/utils.h"

#include "OpenCameraCalibrator/spline/trajectory_estimator.h"
#include "OpenCameraCalibrator/spline/trajectories/spline_base.h"
#include "OpenCameraCalibrator/spline/trajectories/split_trajectory.h"
#include "OpenCameraCalibrator/spline/trajectories/uniform_se3_spline_trajectory.h"
#include "OpenCameraCalibrator/spline/trajectories/uniform_r3_spline_trajectory.h"
#include "OpenCameraCalibrator/spline/trajectories/uniform_so3_spline_trajectory.h"
#include "OpenCameraCalibrator/spline/measurements/static_rscamera_measurement_xyz.h"
#include "OpenCameraCalibrator/spline/measurements/position_measurement.h"
#include "OpenCameraCalibrator/spline/measurements/orientation_measurement.h"
#include "OpenCameraCalibrator/spline/measurements/gyroscope_measurement.h"
#include "OpenCameraCalibrator/spline/measurements/accelerometer_measurement.h"
#include "OpenCameraCalibrator/spline/sensors/pinhole_camera.h"
#include "OpenCameraCalibrator/spline/sensors/division_undistortion_camera.h"
#include "OpenCameraCalibrator/spline/sensors/atan_camera.h"
#include "OpenCameraCalibrator/spline/sensors/camera.h"
#include "OpenCameraCalibrator/spline/sensors/sensors.h"
#include "OpenCameraCalibrator/spline/sensors/imu.h"
#include "OpenCameraCalibrator/spline/sensors/basic_imu.h"
#include "OpenCameraCalibrator/spline/sensors/constant_bias_imu.h"
#include "OpenCameraCalibrator/spline/sfm/sfm.h"

#include "theia/sfm/reconstruction.h"
#include "theia/io/reconstruction_reader.h"

// Input/output files.
DEFINE_string(
    gopro_telemetry_json, "",
    "Path to gopro telemetry json extracted with Sparsnet extractor.");
DEFINE_string(input_video, "", "Path to corresponding video file.");
DEFINE_string(detector_params, "", "Path detector yaml.");
DEFINE_double(downsample_factor, 2.5, "Downsample factor for images.");
DEFINE_string(input_calibration_dataset, "",
              "Path to input calibration dataset.");

namespace TT = kontiki::trajectories;
namespace M = kontiki::measurements;
namespace S = kontiki::sensors;
namespace SFM = kontiki::sfm;

using TrajClass = TT::UniformSE3SplineTrajectory;
using SO3TrajClass = TT::UniformSO3SplineTrajectory;
using R3TrajClass = TT::UniformR3SplineTrajectory;
using SplitTrajClass = TT::SplitTrajectory;
using AtanCameraClass = S::AtanCamera;
using DivisionUndistortionCameraClass = S::DivisionUndistortionCamera;
using PinholeCameraClass = S::PinholeCamera;

using IMUClass = S::ConstantBiasImu;
using Landmark = SFM::LandmarkXYZ;
using ViewKontiki = SFM::ViewXYZ;
using Observation = SFM::ObservationXYZ;
using CamMeasurementPinhole =
    M::StaticRsCameraMeasurementXYZ<PinholeCameraClass>;
using CamMeasurementAtan = M::StaticRsCameraMeasurementXYZ<AtanCameraClass>;
using CamMeasurementDivUndist =
    M::StaticRsCameraMeasurementXYZ<DivisionUndistortionCameraClass>;

using namespace cv;
using namespace theia;

int main(int argc, char* argv[]) {
  GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);

  // Load camera calibration reconstuction.
  theia::Reconstruction cam_calib_recon;
  CHECK(theia::ReadReconstruction(FLAGS_input_calibration_dataset,
                                  &cam_calib_recon))
      << "Could not read Reconstruction file.";

  int squaresX = 10;
  int squaresY = 8;
  int squareLength = 2;
  int markerLength = 1;
  int dictionaryId = cv::aruco::DICT_ARUCO_ORIGINAL;
  const int min_number_detected_corners = 30;

  // read gopro telemetry
  OpenCamCalib::CameraTelemetryData telemetry_data;
  if (!OpenCamCalib::ReadGoProTelemetry(FLAGS_gopro_telemetry_json,
                                        telemetry_data)) {
    std::cout << "Could not read: " << FLAGS_gopro_telemetry_json << std::endl;
  }

  // set charuco detector parameters
  Ptr<aruco::DetectorParameters> detectorParams =
      aruco::DetectorParameters::create();

  if (!OpenCamCalib::ReadDetectorParameters(FLAGS_detector_params, detectorParams)) {
    std::cerr << "Invalid detector parameters file\n";
    return 0;
  }

  Ptr<aruco::Dictionary> dictionary = aruco::getPredefinedDictionary(
      aruco::PREDEFINED_DICTIONARY_NAME(dictionaryId));
  // create charuco board object
  Ptr<aruco::CharucoBoard> charucoboard = aruco::CharucoBoard::create(
      squaresX, squaresY, squareLength, markerLength, dictionary);
  Ptr<aruco::Board> board = charucoboard.staticCast<aruco::Board>();

  theia::Reconstruction recon_calib_dataset;

  std::vector<cv::Point3f> chessoard3d = charucoboard->chessboardCorners;
  std::map<int, theia::TrackId> charuco_id_to_theia_track_id;
  for (int i = 0; i < chessoard3d.size(); ++i) {
    theia::TrackId track_id = recon_calib_dataset.AddTrack();
    theia::Track* track = recon_calib_dataset.MutableTrack(track_id);
    track->SetEstimated(true);
    Eigen::Vector4d* point = track->MutablePoint();
    (*point)[0] = static_cast<double>(chessoard3d[i].x);
    (*point)[1] = static_cast<double>(chessoard3d[i].y);
    (*point)[2] = static_cast<double>(chessoard3d[i].z);
    (*point)[3] = 1.0;
    charuco_id_to_theia_track_id[i] = track_id;
  }

  // run video and extract charuco board
  VideoCapture inputVideo;
  inputVideo.open(FLAGS_input_video);
  bool showRejected = false;
  int cnt_wrong = 0;
  const int skip_frames = 1;
  int frame_cnt = 0;
  while (true) {
    Mat image, imageCopy;
    if (!inputVideo.read(image)) {
      cnt_wrong++;
      if (cnt_wrong > 200) break;
      continue;
    }
    std::string timestamp_s = std::to_string(inputVideo.get(cv::CAP_PROP_POS_MSEC) / 1000.0);
    ++frame_cnt;

    std::vector<int> markerIds, charucoIds;
    std::vector<std::vector<Point2f>> markerCorners, rejectedMarkers;
    std::vector<Point2f> charucoCorners;

    // detect markers
    aruco::detectMarkers(image, dictionary, markerCorners, markerIds,
                         detectorParams, rejectedMarkers);

    // refind strategy to detect more markers
    aruco::refineDetectedMarkers(image, board, markerCorners, markerIds,
                                 rejectedMarkers);

    // interpolate charuco corners
    int interpolatedCorners = 0;
    if (markerIds.size() > 0)
      interpolatedCorners = aruco::interpolateCornersCharuco(
          markerCorners, markerIds, image, charucoboard, charucoCorners,
          charucoIds);

    if (charucoIds.size() < min_number_detected_corners) continue;
    // draw results
    image.copyTo(imageCopy);
    if (markerIds.size() > 0) {
      aruco::drawDetectedMarkers(imageCopy, markerCorners);
    }

    if (showRejected && rejectedMarkers.size() > 0)
      aruco::drawDetectedMarkers(imageCopy, rejectedMarkers, noArray(),
                                 Scalar(100, 0, 255));

    if (interpolatedCorners > 0) {
      Scalar color;
      color = Scalar(255, 0, 0);
      aruco::drawDetectedCornersCharuco(imageCopy, charucoCorners, charucoIds,
                                        color);
    }
    // fill charucoCorners to theia reconstruction
    theia::ViewId view_id =
        recon_calib_dataset.AddView(timestamp_s, 0);
    theia::View* view = recon_calib_dataset.MutableView(view_id);
    view->SetEstimated(true);

    std::cout<<"Found: "<<charucoIds.size()<<" marker."<<std::endl;
    for (int i = 0; i < charucoIds.size(); ++i) {
        theia::TrackId track_id =
            charuco_id_to_theia_track_id.find(charucoIds[i])->second;
        theia::Feature feature;
        feature << static_cast<double>(charucoCorners[i].x),
            static_cast<double>(charucoCorners[i].y);
        recon_calib_dataset.AddObservation(view_id, track_id, feature);
      }
  }


  // Number of cameras.
  const auto& view_ids = recon_calib_dataset.ViewIds();
  std::unordered_map<ViewId, int> view_id_to_index;
  std::unordered_map<ViewId, std::shared_ptr<ViewKontiki>> kontiki_views;
  std::vector<double> timestamps;
  // get all timestamps and find smallest one
  // Output each camera.
  for (const ViewId view_id : view_ids) {
    const View& view = *recon_calib_dataset.View(view_id);
    double timestamp = std::stod(view.Name());
    timestamps.push_back(timestamp);
  }

  const double dt_r3 = 0.1;
  const double dt_so3 = 0.1;

  // find smallest timestamp
  auto result = std::minmax_element(timestamps.begin(), timestamps.end());
  double t0 = timestamps[result.first - timestamps.begin()];
  double tend = timestamps[result.second - timestamps.begin()];

  for (const ViewId view_id : view_ids) {
    const int current_index = view_id_to_index.size();
    view_id_to_index[view_id] = current_index;

    const View& view = *recon_calib_dataset.View(view_id);
    double timestamp = std::stod(view.Name());
    if (timestamp >= tend-std::max(dt_so3,dt_r3) || timestamp < t0+std::max(dt_so3,dt_r3)) continue;
    kontiki_views[view_id] = std::make_shared<ViewKontiki>(view_id, timestamp);
  }

  // Number of points.
  const auto& track_ids = recon_calib_dataset.TrackIds();
  // Output each point.
  std::vector<std::shared_ptr<Landmark>> kontiki_landmarks;
  Eigen::Matrix3d K;
  int img_width, img_height;
  int lauf = 0;
  for (const TrackId track_id : track_ids) {
    const Track* track = recon_calib_dataset.Track(track_id);

    // Output the observations of this 3D point.
    const auto& views_observing_track = track->ViewIds();
    int nr_views = 0;
    for (const ViewId& view_id : views_observing_track) {
      if (kontiki_views.find(view_id) == kontiki_views.end()) continue;
      nr_views++;
    }
    if (nr_views >= 2) {
      // we know our landmarks for calibration and lock them!
      kontiki_landmarks.push_back(std::make_shared<Landmark>());
      kontiki_landmarks[kontiki_landmarks.size()-1]->set_point(track->Point());
      kontiki_landmarks[kontiki_landmarks.size()-1]->Lock(true);

      nr_views = 0;
      for (const ViewId& view_id : views_observing_track) {
        const View* view = recon_calib_dataset.View(view_id);

        // Get the feature location normalized by the principal point.
        const Camera& camera = view->Camera();

        const Feature feature = (*view->GetFeature(track_id));

        auto cur_view_it = kontiki_views.find(view_id);
        if (cur_view_it == kontiki_views.end()) {
            continue;
        }
        cur_view_it->second->CreateObservation(
            kontiki_landmarks[kontiki_landmarks.size() - 1], feature);

        img_width = camera.ImageWidth();
        img_height = camera.ImageHeight();

        ++nr_views;
      }
    }
    ++lauf;
  }

  // set landmark references to first observation
  for (auto l : kontiki_landmarks)
      l->set_reference(l->observations()[0]);

  const theia::View* view_1 = cam_calib_recon.View(0);
  view_1->Camera().GetCalibrationMatrix(&K);
  std::shared_ptr<PinholeCameraClass> cam_kontiki =
      std::make_shared<PinholeCameraClass>(img_width,
                                           img_height,
                                           0.00, K);



  cam_kontiki->LockRelativeOrientation(true);
  cam_kontiki->LockRelativePosition(true);

  std::cout << "Trajectory start time: " << t0 << " tend: " << tend
            << std::endl;
  std::shared_ptr<TrajClass> traj_spline =
      std::make_shared<TrajClass>(dt_r3, 0.0);
  // split trajectory
  std::shared_ptr<SO3TrajClass> so3_traj_spline =
      std::make_shared<SO3TrajClass>(dt_so3, 0.0);
  std::shared_ptr<R3TrajClass> r3_traj_spline =
      std::make_shared<R3TrajClass>(dt_r3, 0.0);

  r3_traj_spline->ExtendTo(tend, Eigen::Vector3d(0.0, 0.0, 0.0));
  so3_traj_spline->ExtendTo(tend, Eigen::Quaterniond(1.0, 0.0, 0.0, 0.0));

  std::shared_ptr<SplitTrajClass> split_traj_spline =
      std::make_shared<SplitTrajClass>(r3_traj_spline, so3_traj_spline);

  kontiki::TrajectoryEstimator<SplitTrajClass> traj_spline_estimator(
      split_traj_spline);

  // iterate all observations and create measurements
  std::vector<std::shared_ptr<CamMeasurementPinhole>> measurements;
  for (auto it : kontiki_views) {
    auto kon_view = it.second;
    std::vector<std::shared_ptr<Observation>> observations =
        kon_view->observations();
    for (int i = 0; i < observations.size(); ++i) {
      measurements.push_back(std::make_shared<CamMeasurementPinhole>(
          cam_kontiki, observations[i]));
      traj_spline_estimator.AddMeasurement(
          measurements[measurements.size() - 1]);
    }
  }

  r3_traj_spline->ExtendTo(std::max(t0, tend+0.1),
                           Eigen::Vector3d(0.0, 0.0, 0.0));
  so3_traj_spline->ExtendTo(std::max(t0, tend+0.1),
                            Eigen::Quaterniond(1.0, 0.0, 0.0, 0.0));

  traj_spline_estimator.Solve(100);
}
