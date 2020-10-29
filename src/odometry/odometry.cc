#include "odometry.h"
#include "solver/solver.h"

namespace oh_my_loam {

namespace {
int kNearbyScanNum = 2;
double kDistSquareThresh = 25;
size_t kMinMatchNum = 10;
}  // namespace

bool Odometry::Init(const YAML::Node& config) {
  config_ = config;
  kdtree_surf_pts_.reset(new pcl::KdTreeFLANN<TPoint>);
  kdtree_corn_pts_.reset(new pcl::KdTreeFLANN<TPoint>);
  return true;
}

void Odometry::Process(const FeaturePoints& feature, Pose3D* const pose) {
  if (!is_initialized) {
    is_initialized = true;
    UpdatePre(feature);
    *pose = pose_curr2world_;
    return;
  }
  for (int i = 0; i < config_["icp_iter_num"].as<int>(); ++i) {
    std::vector<PointLinePair> pl_pairs;
    std::vector<PointPlanePair> pp_pairs;
    AssociateCornPoints(*feature.sharp_corner_pts, *corn_pts_pre_, &pl_pairs,
                        kDistSquareThresh);
    AssociateSurfPoints(*feature.flat_surf_pts, *surf_pts_pre_, &pp_pairs,
                        kDistSquareThresh);
    if (pl_pairs.size() + pp_pairs.size() < kMinMatchNum) {
      AWARN << "Too less correspondence";
    }
    double* q = pose_curr2last_.q().coeffs().data();
    double* p = pose_curr2last_.p().data();
    PoseSolver solver(q, p);
    for (const auto& pair : pl_pairs) {
      solver.AddPointLinePair(pair, GetTime(pair.pt));
    }
    for (const auto& pair : pp_pairs) {
      solver.AddPointPlanePair(pair, GetTime(pair.pt));
    }
    solver.Solve();
    pose_curr2last_ = Pose3D(q, p);
  }
  pose_curr2world_ = pose_curr2world_ * pose_curr2last_;
  *pose = pose_curr2world_;
  UpdatePre(feature);
}

void Odometry::AssociateCornPoints(const TPointCloud& src,
                                   const TPointCloud& tgt,
                                   std::vector<PointLinePair>* const pairs,
                                   double dist_thresh) const {
  kdtree_corn_pts_->setInputCloud(tgt.makeShared());
  for (const auto& query_pt : src) {
    std::vector<int> indices;
    std::vector<float> dists;
    kdtree_corn_pts_->nearestKSearch(query_pt, 1, indices, dists);
    if (dists[0] >= dist_thresh) continue;
    TPoint pt1 = tgt.points[indices[0]];
    int pt2_idx = -1;
    double min_dist_pt2_squre = dist_thresh * dist_thresh;
    int query_pt_scan_id = GetScanId(query_pt);
    for (int i = indices[0] + 1; i < static_cast<int>(tgt.size()); ++i) {
      const auto pt = tgt.points[i];
      int scan_id = GetScanId(pt);
      if (scan_id <= query_pt_scan_id) continue;
      if (scan_id > query_pt_scan_id + kNearbyScanNum) break;
      double dist_squre = DistanceSqure(query_pt, pt);
      if (dist_squre < min_dist_pt2_squre) {
        pt2_idx = i;
        min_dist_pt2_squre = dist_squre;
      }
    }
    for (int i = indices[0] - 1; i >= 0; --i) {
      const auto pt = tgt.points[i];
      int scan_id = GetScanId(pt);
      if (scan_id >= query_pt_scan_id) continue;
      if (scan_id < query_pt_scan_id - kNearbyScanNum) break;
      double dist_squre = DistanceSqure(query_pt, pt);
      if (dist_squre < min_dist_pt2_squre) {
        pt2_idx = i;
        min_dist_pt2_squre = dist_squre;
      }
    }
    if (pt2_idx >= 0) {
      TPoint pt2 = tgt.points[pt2_idx];
      pairs->emplace_back(query_pt, pt1, pt2);
    }
  }
}

void Odometry::AssociateSurfPoints(const TPointCloud& src,
                                   const TPointCloud& tgt,
                                   std::vector<PointPlanePair>* const pairs,
                                   double dist_thresh) const {
  kdtree_surf_pts_->setInputCloud(tgt.makeShared());
  for (const auto& query_pt : src) {
    std::vector<int> indices;
    std::vector<float> dists;
    kdtree_surf_pts_->nearestKSearch(query_pt, 1, indices, dists);
    if (dists[0] >= dist_thresh) continue;
    TPoint pt1 = tgt.points[indices[0]];
    int pt2_idx = -1, pt3_idx = -1;
    double min_dist_pt2_squre = dist_thresh * dist_thresh;
    double min_dist_pt3_squre = dist_thresh * dist_thresh;
    int query_pt_scan_id = GetScanId(query_pt);
    for (int i = indices[0] + 1; i < static_cast<int>(tgt.size()); ++i) {
      const auto pt = tgt.points[i];
      int scan_id = GetScanId(pt);
      if (scan_id > query_pt_scan_id + kNearbyScanNum) break;
      double dist_squre = DistanceSqure(query_pt, pt);
      if (scan_id == query_pt_scan_id && dist_squre < min_dist_pt2_squre) {
        pt2_idx = i;
        min_dist_pt2_squre = dist_squre;
      }
      if (scan_id > query_pt_scan_id && dist_squre < min_dist_pt3_squre) {
        pt3_idx = i;
        min_dist_pt3_squre = dist_squre;
      }
      if (pt2_idx == -1 && scan_id > query_pt_scan_id) break;
    }
    for (int i = indices[0] - 1; i >= 0; --i) {
      const auto pt = tgt.points[i];
      int scan_id = GetScanId(pt);
      if (scan_id >= query_pt_scan_id) continue;
      if (scan_id < query_pt_scan_id - kNearbyScanNum) break;
      double dist_squre = DistanceSqure(query_pt, pt);
      if (scan_id == query_pt_scan_id && dist_squre < min_dist_pt2_squre) {
        pt2_idx = i;
        min_dist_pt2_squre = dist_squre;
      }
      if (scan_id < query_pt_scan_id && dist_squre < min_dist_pt3_squre) {
        pt3_idx = i;
        min_dist_pt3_squre = dist_squre;
      }
      if (pt2_idx == -1 && scan_id < query_pt_scan_id) break;
    }
    if (pt2_idx >= 0 && pt3_idx >= 0) {
      TPoint pt2 = tgt.points[pt2_idx], pt3 = tgt.points[pt3_idx];
      pairs->emplace_back(query_pt, pt1, pt2, pt3);
    }
  }
}

void Odometry::UpdatePre(const FeaturePoints& feature) {
  surf_pts_pre_ = feature.less_flat_surf_pts;
  corn_pts_pre_ = feature.less_sharp_corner_pts;
}

}  // namespace oh_my_loam