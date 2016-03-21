#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/point_types.h>
#include <pcl/point_cloud.h>
#include <cmath>
#include <Eigen/Eigenvalues>
#include "icp.h"
#include "dualquat.h"
#include <algorithm>
#include <stdlib.h>
#include <omp.h>
#ifdef PROFILE
#include <chrono>
#include <iostream>
#endif
#define MAX_ITER 40

using namespace pcl;

float choose_xi(std::vector<std::vector<float>> nearest_d,
    std::vector<int> matched) {
  unsigned num_bins = 25;

  // get estimate of max distance
  float max = 0.f;
  for (int n = 0; n<100; n++) {
    int r = rand()%matched.size();
    if (nearest_d[r][0] > max) {
      max = nearest_d[r][0];
    }
  }
  max *= 1.05;

  // build histogram
  std::vector<unsigned int> counts(num_bins+1, 0);
  for (size_t i=0; i<matched.size(); i++) {
    if (nearest_d[i][0] > max) {
      counts[num_bins]++;
    } else {
      counts[(int)(num_bins*nearest_d[i][0]/max)]++;
    }
  }

  // find biggest peak
  unsigned peak = 0;
  unsigned elevation = 0;
  for (size_t i=0; i<num_bins+1; i++) {
    if (counts[i] > elevation) {
      peak = i;
      elevation = counts[i];
    }
  }

  // find first valley after peak (lower than 60% of peak height)
  unsigned valley;
  for (valley=peak+1; valley<num_bins; valley++) {
    if (counts[valley] > elevation*0.6) continue;
    if (counts[valley+1] > counts[valley]) break;
  }

  return ((float) valley)/((float) num_bins) * max;
}

DualQuat<float> localize(PointCloud<PointXYZ>::Ptr reference,
    PointCloud<PointXYZ>::Ptr source, std::vector<int> matched) {
  // Compute matrices C1, C2, C3
  Eigen::Matrix<float, 4, 4> C1 = Eigen::Matrix<float, 4, 4>::Zero();
  Eigen::Matrix<float, 4, 4> C2 = Eigen::Matrix<float, 4, 4>::Zero();

  float W = 0.;
  for (size_t i=0; i<matched.size(); i++) {
    if (matched[i]<0) continue;
    C1 += Quat<float>(reference->points[matched[i]]).Q().transpose() *
      Quat<float>(source->points[i]).W();
    C2 += Quat<float>(source->points[i]).W() -
      Quat<float>(reference->points[matched[i]]).Q();
    W += 1.;
  }

  C1 *= -2;
  C2 *= 2;

  Eigen::Matrix<float, 4, 4> A = .5 * (0.5/W * C2.transpose() * C2 - C1 - C1.transpose());

  Eigen::EigenSolver<Eigen::Matrix<float, 4, 4>> solver;
  solver.compute(A, true);
  float max_eigenvalue = -INFINITY;
  Eigen::Matrix<float, 4, 1> max_eigenvector;
  for (long i = 0; i < solver.eigenvalues().size(); i++) {
    if (solver.eigenvalues()[i].real() > max_eigenvalue) {
      max_eigenvalue = solver.eigenvalues()[i].real();
      max_eigenvector = solver.eigenvectors().col(i).real();
    }
  }

  Quat<float> real(max_eigenvector);
  Quat<float> dual(-0.5/W * C2*max_eigenvector);
  return DualQuat<float>(real, dual);
}

// input: reference and source point clouds, prior SE3 transform guess
// output: SE3 transform from source to reference, total error (of some sort TODO)
float ICP(PointCloud<PointXYZ>::Ptr reference, PointCloud<PointXYZ>::Ptr source,
    Eigen::Matrix<float, 4, 4> &Trs) {

  // initialize ICP parameters
  float D = 10.0;
  float Dmax = 20*D;

  // transformations calculated at each iteration
  DualQuat<float> T;

#ifdef PROFILE
  std::chrono::duration<uint64_t, std::micro> kdtree_build_time(0);
  std::chrono::duration<uint64_t, std::micro> kdtree_search_time(0);
  std::chrono::duration<uint64_t, std::micro> match_time(0);
  std::chrono::duration<uint64_t, std::micro> localize_time(0);
  std::chrono::duration<uint64_t, std::micro> update_time(0);
  auto start = std::chrono::high_resolution_clock::now();
#endif
  // build k-d tree
  KdTreeFLANN<PointXYZ> kdtree;
  kdtree.setInputCloud(reference);
#ifdef PROFILE
  auto stop = std::chrono::high_resolution_clock::now();
  kdtree_build_time = std::chrono::duration_cast<std::chrono::microseconds>(stop - start);
  start = stop;
#endif

  // vectors to receive nearest neighbors
  std::vector<std::vector<int>> nearest_i(source->size(), std::vector<int>(1));
  std::vector<std::vector<float>> nearest_d(source->size(),
      std::vector<float>(1));

  std::vector<int> matched(source->size());

  // transform source points according to prior Trs
  for (size_t i=0; i<source->size(); i++) {
    source->points[i].getVector4fMap() = Trs*source->points[i].getVector4fMap();
  }

  for (int iter=0; iter < MAX_ITER; iter++) {

    // find closest points
#pragma omp parallel for
    for (size_t i=0; i<source->size(); i++) {
      nearest_i[i][0] = -1;
      nearest_d[i][0] = 0.;
      if (!isnan(source->points[i].x)) {
        kdtree.nearestKSearch(source->points[i], 1, nearest_i[i],
            nearest_d[i]);
      }
    }
#ifdef PROFILE
    stop = std::chrono::high_resolution_clock::now();
    kdtree_search_time += std::chrono::duration_cast<std::chrono::microseconds>(stop - start);
    start = stop;
#endif

    // choose which matches to use
    float mu=0.;
    float sigma=0.;
    int n=0;

    for (size_t i=0; i<source->size(); i++) {
      if (nearest_d[i][0] < Dmax) {
        matched[i] = nearest_i[i][0];
        mu += nearest_d[i][0];
        sigma += nearest_d[i][0]*nearest_d[i][0];
      } else {
        matched[i] = -1;
      }
      n++;
    }

    mu = mu/n;
    sigma = sqrt(sigma/n - mu*mu);

    if (mu < D) {
      Dmax = mu + 3*sigma;
    } else if (mu < 3*D) {
      Dmax = mu + 2*sigma;
    } else if (mu < 6*D) {
      Dmax = mu + sigma;
    } else {
      Dmax = choose_xi(nearest_d, matched);
    }

    for (size_t i=0; i<source->size(); i++) {
      if (nearest_d[i][0] < Dmax) {
        matched[i] = nearest_i[i][0];
      } else {
        matched[i] = -1;
      }
    }
#ifdef PROFILE
    stop = std::chrono::high_resolution_clock::now();
    match_time += std::chrono::duration_cast<std::chrono::microseconds>(stop - start);
    start = stop;
#endif

    // compute motion
    T = localize(reference, source, matched);

#ifdef PROFILE
    stop = std::chrono::high_resolution_clock::now();
    localize_time += std::chrono::duration_cast<std::chrono::microseconds>(stop - start);
    start = stop;
#endif
    // apply to all source points
    Eigen::Matrix<float, 4, 4> Tmat = T.Matrix();
    for (size_t i=0; i<source->points.size(); i++) {
      source->points[i].getVector4fMap() = Tmat*source->points[i].getVector4fMap();
    }

    // update Trs
    Trs = Tmat*Trs;

    // check stopping criteria
    float dt = T.getTranslation().norm();
    float dth = T.r.Angle();

#ifdef PROFILE
    std::cerr << "Iteration " << iter << " dt " << dt << ", dtheta " << dth <<
      std::endl;
    stop = std::chrono::high_resolution_clock::now();
    update_time += std::chrono::duration_cast<std::chrono::microseconds>(stop - start);
    start = stop;
#endif
    if (iter > 0 && dt < 0.01 && dth < 0.01) {
      break;
    }
  }

#ifdef PROFILE
  std::cerr << "kdtree_build_time  " << kdtree_build_time.count() << std::endl;
  std::cerr << "kdtree_search_time " << kdtree_search_time.count() << std::endl;
  std::cerr << "match_time         " << match_time.count() << std::endl;
  std::cerr << "localize_time      " << localize_time.count() << std::endl;
  std::cerr << "update_time        " << update_time.count() << std::endl;
#endif

  return 0.f;
}
