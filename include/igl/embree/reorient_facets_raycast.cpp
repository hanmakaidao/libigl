// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "reorient_facets_raycast.h"
#include "../per_face_normals.h"
#include "../doublearea.h"
#include "../random_dir.h"
#include "../boost/bfs_orient.h"
#include "orient_outward_ao.h"
#include "EmbreeIntersector.h"
#include <iostream>
#include <random>
#include <ctime>
#include <limits>

  template <
    typename DerivedV, 
    typename DerivedF, 
    typename DerivedI>
  IGL_INLINE void igl::reorient_facets_raycast(
    const Eigen::PlainObjectBase<DerivedV> & V,
    const Eigen::PlainObjectBase<DerivedF> & F,
    int rays_total,
    int rays_minimum,
    bool use_parity,
    bool is_verbose,
    Eigen::PlainObjectBase<DerivedI> & I)
{
  using namespace Eigen;
  using namespace std;
  assert(F.cols() == 3);
  assert(V.cols() == 3);
  
  // number of faces
  const int m = F.rows();
  
  if (is_verbose) cout << "extracting patches... ";
  VectorXi C;
  MatrixXi FF = F;
  bfs_orient(F,FF,C);
  if (is_verbose) cout << (C.maxCoeff() + 1)  << " components. ";
  
  // number of patches
  const int num_cc = C.maxCoeff()+1;
  
  // Init Embree
  EmbreeIntersector ei;
  ei.init(V.template cast<float>(),FF);
  
  // face normal
  MatrixXd N;
  per_face_normals(V,FF,N);
  
  // face area
  Matrix<typename DerivedV::Scalar,Dynamic,1> A;
  doublearea(V,FF,A);
  double area_min = numeric_limits<double>::max();
  for (int f = 0; f < m; ++f)
  {
    area_min = A(f) != 0 && A(f) < area_min ? A(f) : area_min;
  }
  double area_total = A.sum();
  
  // determine number of rays per component according to its area
  VectorXd area_per_component;
  area_per_component.setZero(num_cc);
  for (int f = 0; f < m; ++f)
  {
    area_per_component(C(f)) += A(f);
  }
  VectorXi num_rays_per_component(num_cc);
  for (int c = 0; c < num_cc; ++c)
  {
    num_rays_per_component(c) = max<int>(static_cast<int>(rays_total * area_per_component(c) / area_total), rays_minimum);
  }
  rays_total = num_rays_per_component.sum();
  
  // generate all the rays
  if (is_verbose) cout << "generating rays... ";
  uniform_real_distribution<float> rdist;
  mt19937 prng;
  prng.seed(time(nullptr));
  vector<int     > ray_face;
  vector<Vector3f> ray_ori;
  vector<Vector3f> ray_dir;
  ray_face.reserve(rays_total);
  ray_ori .reserve(rays_total);
  ray_dir .reserve(rays_total);
  for (int c = 0; c < num_cc; ++c)
  {
    if (area_per_component[c] == 0)
    {
      continue;
    }
    vector<int> CF;     // set of faces per component
    vector<unsigned long long> CF_area;
    for (int f = 0; f < m; ++f)
    {
      if (C(f)==c)
      {
        CF.push_back(f);
        CF_area.push_back(static_cast<unsigned long long>(100 * A(f) / area_min));
      }
    }
    // discrete distribution for random selection of faces with probability proportional to their areas
    auto ddist_func = [&] (double i) { return CF_area[static_cast<int>(i)]; };
    discrete_distribution<int> ddist(CF.size(), 0, CF.size(), ddist_func);      // simple ctor of (Iter, Iter) not provided by the stupid VC11 impl...
    for (int i = 0; i < num_rays_per_component[c]; ++i)
    {
      int f = CF[ddist(prng)];          // select face with probability proportional to face area
      float s = rdist(prng);            // random barycentric coordinate (reference: Generating Random Points in Triangles [Turk, Graphics Gems I 1990])
      float t = rdist(prng);
      float sqrt_t = sqrtf(t);
      float a = 1 - sqrt_t;
      float b = (1 - s) * sqrt_t;
      float c = s * sqrt_t;
      Vector3f p = a * V.row(FF(f,0)).template cast<float>().eval()       // be careful with the index!!!
                 + b * V.row(FF(f,1)).template cast<float>().eval()
                 + c * V.row(FF(f,2)).template cast<float>().eval();
      Vector3f n = N.row(f).cast<float>();
      if (n.isZero()) continue;
      // random direction in hemisphere around n (avoid too grazing angle)
      Vector3f d;
      while (true) {
        d = random_dir().cast<float>();
        float ndotd = n.dot(d);
        if (fabsf(ndotd) < 0.1f)
        {
          continue;
        }
        if (ndotd < 0)
        {
          d *= -1.0f;
        }
        break;
      }
      ray_face.push_back(f);
      ray_ori .push_back(p);
      ray_dir .push_back(d);

      if (is_verbose && ray_face.size() % (rays_total / 10) == 0) cout << ".";
    }
  }
  if (is_verbose) cout << ray_face.size()  << " rays. ";
  
  // per component voting: first=front, second=back
  vector<pair<float, float>> C_vote_distance(num_cc, make_pair(0, 0));      // sum of distance between ray origin and intersection
  vector<pair<int  , int  >> C_vote_infinity(num_cc, make_pair(0, 0));      // number of rays reaching infinity
  vector<pair<int  , int  >> C_vote_parity(num_cc, make_pair(0, 0));        // sum of parity count for each ray
  
  if (is_verbose) cout << "shooting rays... ";
#pragma omp parallel for
  for (int i = 0; i < (int)ray_face.size(); ++i)
  {
    int      f = ray_face[i];
    Vector3f o = ray_ori [i];
    Vector3f d = ray_dir [i];
    int c = C(f);
    
    // shoot ray toward front & back
    vector<Hit> hits_front;
    vector<Hit> hits_back;
    int num_rays_front;
    int num_rays_back;
    ei.intersectRay(o,  d, hits_front, num_rays_front);
    ei.intersectRay(o, -d, hits_back , num_rays_back );
    if (!hits_front.empty() && hits_front[0].id == f) hits_front.erase(hits_front.begin());
    if (!hits_back .empty() && hits_back [0].id == f) hits_back .erase(hits_back .begin());
    
    if (use_parity) {
#pragma omp atomic
      C_vote_parity[c].first  += hits_front.size() % 2;
#pragma omp atomic
      C_vote_parity[c].second += hits_back .size() % 2;
    
    } else {
      if (hits_front.empty())
      {
#pragma omp atomic
        C_vote_infinity[c].first++;
      } else {
#pragma omp atomic
        C_vote_distance[c].first += hits_front[0].t;
      }
    
      if (hits_back.empty())
      {
#pragma omp atomic
        C_vote_infinity[c].second++;
      } else {
#pragma omp atomic
        C_vote_distance[c].second += hits_back[0].t;
      }
    }
  }
  
  I.resize(m);
  for(int f = 0; f < m; ++f)
  {
    int c = C(f);
    if (use_parity) {
      I(f) = C_vote_parity[c].first > C_vote_parity[c].second ? 1 : 0;      // Ideally, parity for the front/back side should be 1/0 (i.e., parity sum for all rays should be smaller on the front side)
    
    } else {
      I(f) = (C_vote_infinity[c].first == C_vote_infinity[c].second && C_vote_distance[c].first <  C_vote_distance[c].second) ||
              C_vote_infinity[c].first <  C_vote_infinity[c].second
              ? 1 : 0;
    }
  }
  if (is_verbose) cout << "done!" << endl;
}

#ifndef IGL_HEADER_ONLY
// Explicit template specialization
#endif
