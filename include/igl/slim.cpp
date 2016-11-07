// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Michael Rabinovich
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "slim.h"

#include <igl/boundary_loop.h>
#include <igl/cotmatrix.h>
#include <igl/edge_lengths.h>
#include <igl/grad.h>
#include <igl/local_basis.h>
#include <igl/readOBJ.h>
#include <igl/repdiag.h>
#include <igl/vector_area_matrix.h>
#include <iostream>

#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <map>
#include <set>
#include <vector>

#include "igl/arap.h"
#include "igl/cat.h"
#include "igl/doublearea.h"
#include "igl/grad.h"
#include "igl/local_basis.h"
#include "igl/per_face_normals.h"
#include "igl/slice_into.h"
#include "igl/volume.h"
#include "igl/polar_svd.h"

#include <Eigen/IterativeLinearSolvers>
#include <Eigen/Sparse>
#include <Eigen/SparseQR>
#include <Eigen/SparseCholesky>
#include <Eigen/IterativeLinearSolvers>

#include "flip_avoiding_line_search.h"

using namespace std;
using namespace Eigen;

///////// Helper functions to compute gradient matrices

void compute_surface_gradient_matrix(const Eigen::MatrixXd &V, const Eigen::MatrixXi &F,
                                     const Eigen::MatrixXd &F1, const Eigen::MatrixXd &F2,
                                     Eigen::SparseMatrix<double> &D1, Eigen::SparseMatrix<double> &D2)
{

  Eigen::SparseMatrix<double> G;
  igl::grad(V, F, G);
  Eigen::SparseMatrix<double> Dx = G.block(0, 0, F.rows(), V.rows());
  Eigen::SparseMatrix<double> Dy = G.block(F.rows(), 0, F.rows(), V.rows());
  Eigen::SparseMatrix<double> Dz = G.block(2 * F.rows(), 0, F.rows(), V.rows());

  D1 = F1.col(0).asDiagonal() * Dx + F1.col(1).asDiagonal() * Dy + F1.col(2).asDiagonal() * Dz;
  D2 = F2.col(0).asDiagonal() * Dx + F2.col(1).asDiagonal() * Dy + F2.col(2).asDiagonal() * Dz;
}

// Computes the weights and solve the linear system for the quadratic proxy specified in the paper
// The output of this is used to generate a search direction that will be fed to the Linesearch class
class WeightedGlobalLocal
{

public:
  WeightedGlobalLocal(igl::SLIMData &state);

  // Compute necessary information before solving the proxy quadratic
  void pre_calc();

  // Solve the weighted proxy global step
  // Output:
  //    V_new #V by dim list of mesh positions (will be fed to a linesearch algorithm)
  void solve_weighted_proxy(Eigen::MatrixXd &V_new);

  // Compute the energy specified in the SLIMData structure + the soft constraint energy (in case there are soft constraints)
  // Input:
  //    V_new #V by dim list of mesh positions
  virtual double compute_energy(Eigen::MatrixXd &V_new);

//private:

  void compute_jacobians(const Eigen::MatrixXd &V_o);

  double compute_energy_with_jacobians(const Eigen::MatrixXd &V, const Eigen::MatrixXi &F,
                                       const Eigen::MatrixXd &Ji, Eigen::MatrixXd &V_o, Eigen::VectorXd &areas);

  double compute_soft_const_energy(const Eigen::MatrixXd &V, const Eigen::MatrixXi &F,
                                   Eigen::MatrixXd &V_o);

  void update_weights_and_closest_rotations(const Eigen::MatrixXd &V, const Eigen::MatrixXi &F, Eigen::MatrixXd &uv);

  void solve_weighted_arap(const Eigen::MatrixXd &V, const Eigen::MatrixXi &F, Eigen::MatrixXd &uv, Eigen::VectorXi &b,
                           Eigen::MatrixXd &bc);

  void build_linear_system(Eigen::SparseMatrix<double> &L);

  void buildA(Eigen::SparseMatrix<double> &A);

  void buildRhs(const Eigen::SparseMatrix<double> &At);

  void add_soft_constraints(Eigen::SparseMatrix<double> &L);

  void add_proximal_penalty();

  igl::SLIMData &m_state;
};

//// Implementation

WeightedGlobalLocal::WeightedGlobalLocal(igl::SLIMData &state) :
    m_state(state)
{
}

void WeightedGlobalLocal::solve_weighted_proxy(Eigen::MatrixXd &V_new)
{

  update_weights_and_closest_rotations(m_state.V, m_state.F, V_new);
  solve_weighted_arap(m_state.V, m_state.F, V_new, m_state.b, m_state.bc);
}

void WeightedGlobalLocal::compute_jacobians(const Eigen::MatrixXd &uv)
{
  if (m_state.F.cols() == 3)
  {
    // Ji=[D1*u,D2*u,D1*v,D2*v];
    m_state.Ji.col(0) = m_state.Dx * uv.col(0);
    m_state.Ji.col(1) = m_state.Dy * uv.col(0);
    m_state.Ji.col(2) = m_state.Dx * uv.col(1);
    m_state.Ji.col(3) = m_state.Dy * uv.col(1);
  }
  else /*tet mesh*/{
    // Ji=[D1*u,D2*u,D3*u, D1*v,D2*v, D3*v, D1*w,D2*w,D3*w];
    m_state.Ji.col(0) = m_state.Dx * uv.col(0);
    m_state.Ji.col(1) = m_state.Dy * uv.col(0);
    m_state.Ji.col(2) = m_state.Dz * uv.col(0);
    m_state.Ji.col(3) = m_state.Dx * uv.col(1);
    m_state.Ji.col(4) = m_state.Dy * uv.col(1);
    m_state.Ji.col(5) = m_state.Dz * uv.col(1);
    m_state.Ji.col(6) = m_state.Dx * uv.col(2);
    m_state.Ji.col(7) = m_state.Dy * uv.col(2);
    m_state.Ji.col(8) = m_state.Dz * uv.col(2);
  }
}

void WeightedGlobalLocal::update_weights_and_closest_rotations(const Eigen::MatrixXd &V,
                                                               const Eigen::MatrixXi &F, Eigen::MatrixXd &uv)
{
  compute_jacobians(uv);

  const double eps = 1e-8;
  double exp_f = m_state.exp_factor;

  if (m_state.dim == 2)
  {
    for (int i = 0; i < m_state.Ji.rows(); ++i)
    {
      typedef Eigen::Matrix<double, 2, 2> Mat2;
      typedef Eigen::Matrix<double, 2, 1> Vec2;
      Mat2 ji, ri, ti, ui, vi;
      Vec2 sing;
      Vec2 closest_sing_vec;
      Mat2 mat_W;
      Vec2 m_sing_new;
      double s1, s2;

      ji(0, 0) = m_state.Ji(i, 0);
      ji(0, 1) = m_state.Ji(i, 1);
      ji(1, 0) = m_state.Ji(i, 2);
      ji(1, 1) = m_state.Ji(i, 3);

      igl::polar_svd(ji, ri, ti, ui, sing, vi);

      s1 = sing(0);
      s2 = sing(1);

      // Update Weights according to energy
      switch (m_state.slim_energy)
      {
        case igl::SLIMData::ARAP:
        {
          m_sing_new << 1, 1;
          break;
        }
        case igl::SLIMData::SYMMETRIC_DIRICHLET:
        {
          double s1_g = 2 * (s1 - pow(s1, -3));
          double s2_g = 2 * (s2 - pow(s2, -3));
          m_sing_new << sqrt(s1_g / (2 * (s1 - 1))), sqrt(s2_g / (2 * (s2 - 1)));
          break;
        }
        case igl::SLIMData::LOG_ARAP:
        {
          double s1_g = 2 * (log(s1) / s1);
          double s2_g = 2 * (log(s2) / s2);
          m_sing_new << sqrt(s1_g / (2 * (s1 - 1))), sqrt(s2_g / (2 * (s2 - 1)));
          break;
        }
        case igl::SLIMData::CONFORMAL:
        {
          double s1_g = 1 / (2 * s2) - s2 / (2 * pow(s1, 2));
          double s2_g = 1 / (2 * s1) - s1 / (2 * pow(s2, 2));

          double geo_avg = sqrt(s1 * s2);
          double s1_min = geo_avg;
          double s2_min = geo_avg;

          m_sing_new << sqrt(s1_g / (2 * (s1 - s1_min))), sqrt(s2_g / (2 * (s2 - s2_min)));

          // change local step
          closest_sing_vec << s1_min, s2_min;
          ri = ui * closest_sing_vec.asDiagonal() * vi.transpose();
          break;
        }
        case igl::SLIMData::EXP_CONFORMAL:
        {
          double s1_g = 2 * (s1 - pow(s1, -3));
          double s2_g = 2 * (s2 - pow(s2, -3));

          double geo_avg = sqrt(s1 * s2);
          double s1_min = geo_avg;
          double s2_min = geo_avg;

          double in_exp = exp_f * ((pow(s1, 2) + pow(s2, 2)) / (2 * s1 * s2));
          double exp_thing = exp(in_exp);

          s1_g *= exp_thing * exp_f;
          s2_g *= exp_thing * exp_f;

          m_sing_new << sqrt(s1_g / (2 * (s1 - 1))), sqrt(s2_g / (2 * (s2 - 1)));
          break;
        }
        case igl::SLIMData::EXP_SYMMETRIC_DIRICHLET:
        {
          double s1_g = 2 * (s1 - pow(s1, -3));
          double s2_g = 2 * (s2 - pow(s2, -3));

          double in_exp = exp_f * (pow(s1, 2) + pow(s1, -2) + pow(s2, 2) + pow(s2, -2));
          double exp_thing = exp(in_exp);

          s1_g *= exp_thing * exp_f;
          s2_g *= exp_thing * exp_f;

          m_sing_new << sqrt(s1_g / (2 * (s1 - 1))), sqrt(s2_g / (2 * (s2 - 1)));
          break;
        }
      }

      if (abs(s1 - 1) < eps) m_sing_new(0) = 1;
      if (abs(s2 - 1) < eps) m_sing_new(1) = 1;
      mat_W = ui * m_sing_new.asDiagonal() * ui.transpose();

      m_state.W_11(i) = mat_W(0, 0);
      m_state.W_12(i) = mat_W(0, 1);
      m_state.W_21(i) = mat_W(1, 0);
      m_state.W_22(i) = mat_W(1, 1);

      // 2) Update local step (doesn't have to be a rotation, for instance in case of conformal energy)
      m_state.Ri(i, 0) = ri(0, 0);
      m_state.Ri(i, 1) = ri(1, 0);
      m_state.Ri(i, 2) = ri(0, 1);
      m_state.Ri(i, 3) = ri(1, 1);
    }
  }
  else
  {
    typedef Eigen::Matrix<double, 3, 1> Vec3;
    typedef Eigen::Matrix<double, 3, 3> Mat3;
    Mat3 ji;
    Vec3 m_sing_new;
    Vec3 closest_sing_vec;
    const double sqrt_2 = sqrt(2);
    for (int i = 0; i < m_state.Ji.rows(); ++i)
    {
      ji(0, 0) = m_state.Ji(i, 0);
      ji(0, 1) = m_state.Ji(i, 1);
      ji(0, 2) = m_state.Ji(i, 2);
      ji(1, 0) = m_state.Ji(i, 3);
      ji(1, 1) = m_state.Ji(i, 4);
      ji(1, 2) = m_state.Ji(i, 5);
      ji(2, 0) = m_state.Ji(i, 6);
      ji(2, 1) = m_state.Ji(i, 7);
      ji(2, 2) = m_state.Ji(i, 8);

      Mat3 ri, ti, ui, vi;
      Vec3 sing;
      igl::polar_svd(ji, ri, ti, ui, sing, vi);

      double s1 = sing(0);
      double s2 = sing(1);
      double s3 = sing(2);

      // 1) Update Weights
      switch (m_state.slim_energy)
      {
        case igl::SLIMData::ARAP:
        {
          m_sing_new << 1, 1, 1;
          break;
        }
        case igl::SLIMData::LOG_ARAP:
        {
          double s1_g = 2 * (log(s1) / s1);
          double s2_g = 2 * (log(s2) / s2);
          double s3_g = 2 * (log(s3) / s3);
          m_sing_new << sqrt(s1_g / (2 * (s1 - 1))), sqrt(s2_g / (2 * (s2 - 1))), sqrt(s3_g / (2 * (s3 - 1)));
          break;
        }
        case igl::SLIMData::SYMMETRIC_DIRICHLET:
        {
          double s1_g = 2 * (s1 - pow(s1, -3));
          double s2_g = 2 * (s2 - pow(s2, -3));
          double s3_g = 2 * (s3 - pow(s3, -3));
          m_sing_new << sqrt(s1_g / (2 * (s1 - 1))), sqrt(s2_g / (2 * (s2 - 1))), sqrt(s3_g / (2 * (s3 - 1)));
          break;
        }
        case igl::SLIMData::EXP_SYMMETRIC_DIRICHLET:
        {
          double s1_g = 2 * (s1 - pow(s1, -3));
          double s2_g = 2 * (s2 - pow(s2, -3));
          double s3_g = 2 * (s3 - pow(s3, -3));
          m_sing_new << sqrt(s1_g / (2 * (s1 - 1))), sqrt(s2_g / (2 * (s2 - 1))), sqrt(s3_g / (2 * (s3 - 1)));

          double in_exp = exp_f * (pow(s1, 2) + pow(s1, -2) + pow(s2, 2) + pow(s2, -2) + pow(s3, 2) + pow(s3, -2));
          double exp_thing = exp(in_exp);

          s1_g *= exp_thing * exp_f;
          s2_g *= exp_thing * exp_f;
          s3_g *= exp_thing * exp_f;

          m_sing_new << sqrt(s1_g / (2 * (s1 - 1))), sqrt(s2_g / (2 * (s2 - 1))), sqrt(s3_g / (2 * (s3 - 1)));

          break;
        }
        case igl::SLIMData::CONFORMAL:
        {
          double common_div = 9 * (pow(s1 * s2 * s3, 5. / 3.));

          double s1_g = (-2 * s2 * s3 * (pow(s2, 2) + pow(s3, 2) - 2 * pow(s1, 2))) / common_div;
          double s2_g = (-2 * s1 * s3 * (pow(s1, 2) + pow(s3, 2) - 2 * pow(s2, 2))) / common_div;
          double s3_g = (-2 * s1 * s2 * (pow(s1, 2) + pow(s2, 2) - 2 * pow(s3, 2))) / common_div;

          double closest_s = sqrt(pow(s1, 2) + pow(s3, 2)) / sqrt_2;
          double s1_min = closest_s;
          double s2_min = closest_s;
          double s3_min = closest_s;

          m_sing_new << sqrt(s1_g / (2 * (s1 - s1_min))), sqrt(s2_g / (2 * (s2 - s2_min))), sqrt(
              s3_g / (2 * (s3 - s3_min)));

          // change local step
          closest_sing_vec << s1_min, s2_min, s3_min;
          ri = ui * closest_sing_vec.asDiagonal() * vi.transpose();
          break;
        }
        case igl::SLIMData::EXP_CONFORMAL:
        {
          // E_conf = (s1^2 + s2^2 + s3^2)/(3*(s1*s2*s3)^(2/3) )
          // dE_conf/ds1 = (-2*(s2*s3)*(s2^2+s3^2 -2*s1^2) ) / (9*(s1*s2*s3)^(5/3))
          // Argmin E_conf(s1): s1 = sqrt(s1^2+s2^2)/sqrt(2)
          double common_div = 9 * (pow(s1 * s2 * s3, 5. / 3.));

          double s1_g = (-2 * s2 * s3 * (pow(s2, 2) + pow(s3, 2) - 2 * pow(s1, 2))) / common_div;
          double s2_g = (-2 * s1 * s3 * (pow(s1, 2) + pow(s3, 2) - 2 * pow(s2, 2))) / common_div;
          double s3_g = (-2 * s1 * s2 * (pow(s1, 2) + pow(s2, 2) - 2 * pow(s3, 2))) / common_div;

          double in_exp = exp_f * ((pow(s1, 2) + pow(s2, 2) + pow(s3, 2)) / (3 * pow((s1 * s2 * s3), 2. / 3)));;
          double exp_thing = exp(in_exp);

          double closest_s = sqrt(pow(s1, 2) + pow(s3, 2)) / sqrt_2;
          double s1_min = closest_s;
          double s2_min = closest_s;
          double s3_min = closest_s;

          s1_g *= exp_thing * exp_f;
          s2_g *= exp_thing * exp_f;
          s3_g *= exp_thing * exp_f;

          m_sing_new << sqrt(s1_g / (2 * (s1 - s1_min))), sqrt(s2_g / (2 * (s2 - s2_min))), sqrt(
              s3_g / (2 * (s3 - s3_min)));

          // change local step
          closest_sing_vec << s1_min, s2_min, s3_min;
          ri = ui * closest_sing_vec.asDiagonal() * vi.transpose();
        }
      }
      if (abs(s1 - 1) < eps) m_sing_new(0) = 1;
      if (abs(s2 - 1) < eps) m_sing_new(1) = 1;
      if (abs(s3 - 1) < eps) m_sing_new(2) = 1;
      Mat3 mat_W;
      mat_W = ui * m_sing_new.asDiagonal() * ui.transpose();

      m_state.W_11(i) = mat_W(0, 0);
      m_state.W_12(i) = mat_W(0, 1);
      m_state.W_13(i) = mat_W(0, 2);
      m_state.W_21(i) = mat_W(1, 0);
      m_state.W_22(i) = mat_W(1, 1);
      m_state.W_23(i) = mat_W(1, 2);
      m_state.W_31(i) = mat_W(2, 0);
      m_state.W_32(i) = mat_W(2, 1);
      m_state.W_33(i) = mat_W(2, 2);

      // 2) Update closest rotations (not rotations in case of conformal energy)
      m_state.Ri(i, 0) = ri(0, 0);
      m_state.Ri(i, 1) = ri(1, 0);
      m_state.Ri(i, 2) = ri(2, 0);
      m_state.Ri(i, 3) = ri(0, 1);
      m_state.Ri(i, 4) = ri(1, 1);
      m_state.Ri(i, 5) = ri(2, 1);
      m_state.Ri(i, 6) = ri(0, 2);
      m_state.Ri(i, 7) = ri(1, 2);
      m_state.Ri(i, 8) = ri(2, 2);
    } // for loop end

  } // if dim end

}

void WeightedGlobalLocal::solve_weighted_arap(const Eigen::MatrixXd &V, const Eigen::MatrixXi &F,
                                              Eigen::MatrixXd &uv, Eigen::VectorXi &soft_b_p,
                                              Eigen::MatrixXd &soft_bc_p)
{
  using namespace Eigen;

  Eigen::SparseMatrix<double> L;
  build_linear_system(L);

  // solve
  Eigen::VectorXd Uc;
  if (m_state.dim == 2)
  {
    SimplicialLDLT<SparseMatrix<double> > solver;
    Uc = solver.compute(L).solve(m_state.rhs);
  }
  else
  { // seems like CG performs much worse for 2D and way better for 3D
    Eigen::VectorXd guess(uv.rows() * m_state.dim);
    for (int i = 0; i < m_state.dim; i++) for (int j = 0; j < m_state.dim; j++) guess(uv.rows() * i + j) = uv(i, j); // flatten vector
    ConjugateGradient<SparseMatrix<double>, Eigen::Lower | Upper> solver;
    solver.setTolerance(1e-8);
    Uc = solver.compute(L).solveWithGuess(m_state.rhs, guess);
  }

  for (int i = 0; i < m_state.dim; i++)
    uv.col(i) = Uc.block(i * m_state.v_n, 0, m_state.v_n, 1);
}

void WeightedGlobalLocal::pre_calc()
{
  if (!m_state.has_pre_calc)
  {
    m_state.v_n = m_state.v_num;
    m_state.f_n = m_state.f_num;

    if (m_state.F.cols() == 3)
    {
      m_state.dim = 2;
      Eigen::MatrixXd F1, F2, F3;
      igl::local_basis(m_state.V, m_state.F, F1, F2, F3);
      compute_surface_gradient_matrix(m_state.V, m_state.F, F1, F2, m_state.Dx, m_state.Dy);

      m_state.W_11.resize(m_state.f_n);
      m_state.W_12.resize(m_state.f_n);
      m_state.W_21.resize(m_state.f_n);
      m_state.W_22.resize(m_state.f_n);
    }
    else
    {
      m_state.dim = 3;
      Eigen::SparseMatrix<double> G;
      igl::grad(m_state.V, m_state.F, G,
                m_state.mesh_improvement_3d /*use normal gradient, or one from a "regular" tet*/);
      m_state.Dx = G.block(0, 0, m_state.F.rows(), m_state.V.rows());
      m_state.Dy = G.block(m_state.F.rows(), 0, m_state.F.rows(), m_state.V.rows());
      m_state.Dz = G.block(2 * m_state.F.rows(), 0, m_state.F.rows(), m_state.V.rows());


      m_state.W_11.resize(m_state.f_n);
      m_state.W_12.resize(m_state.f_n);
      m_state.W_13.resize(m_state.f_n);
      m_state.W_21.resize(m_state.f_n);
      m_state.W_22.resize(m_state.f_n);
      m_state.W_23.resize(m_state.f_n);
      m_state.W_31.resize(m_state.f_n);
      m_state.W_32.resize(m_state.f_n);
      m_state.W_33.resize(m_state.f_n);
    }

    m_state.Dx.makeCompressed();
    m_state.Dy.makeCompressed();
    m_state.Dz.makeCompressed();
    m_state.Ri.resize(m_state.f_n, m_state.dim * m_state.dim);
    m_state.Ji.resize(m_state.f_n, m_state.dim * m_state.dim);
    m_state.rhs.resize(m_state.dim * m_state.v_num);

    // flattened weight matrix
    m_state.WGL_M.resize(m_state.dim * m_state.dim * m_state.f_n);
    for (int i = 0; i < m_state.dim * m_state.dim; i++)
      for (int j = 0; j < m_state.f_n; j++)
        m_state.WGL_M(i * m_state.f_n + j) = m_state.M(j);

    m_state.first_solve = true;
    m_state.has_pre_calc = true;
  }
}

void WeightedGlobalLocal::build_linear_system(Eigen::SparseMatrix<double> &L)
{
  // formula (35) in paper
  Eigen::SparseMatrix<double> A(m_state.dim * m_state.dim * m_state.f_n, m_state.dim * m_state.v_n);
  buildA(A);

  Eigen::SparseMatrix<double> At = A.transpose();
  At.makeCompressed();

  Eigen::SparseMatrix<double> id_m(At.rows(), At.rows());
  id_m.setIdentity();

  // add proximal penalty
  L = At * m_state.WGL_M.asDiagonal() * A + m_state.proximal_p * id_m; //add also a proximal term
  L.makeCompressed();

  buildRhs(At);
  Eigen::SparseMatrix<double> OldL = L;
  add_soft_constraints(L);
  L.makeCompressed();
}

void WeightedGlobalLocal::add_soft_constraints(Eigen::SparseMatrix<double> &L)
{
  int v_n = m_state.v_num;
  for (int d = 0; d < m_state.dim; d++)
  {
    for (int i = 0; i < m_state.b.rows(); i++)
    {
      int v_idx = m_state.b(i);
      m_state.rhs(d * v_n + v_idx) += m_state.soft_const_p * m_state.bc(i, d); // rhs
      L.coeffRef(d * v_n + v_idx, d * v_n + v_idx) += m_state.soft_const_p; // diagonal of matrix
    }
  }
}

double WeightedGlobalLocal::compute_energy(Eigen::MatrixXd &V_new)
{
  compute_jacobians(V_new);
  return compute_energy_with_jacobians(m_state.V, m_state.F, m_state.Ji, V_new, m_state.M) +
         compute_soft_const_energy(m_state.V, m_state.F, V_new);
}

double WeightedGlobalLocal::compute_soft_const_energy(const Eigen::MatrixXd &V,
                                                      const Eigen::MatrixXi &F,
                                                      Eigen::MatrixXd &V_o)
{
  double e = 0;
  for (int i = 0; i < m_state.b.rows(); i++)
  {
    e += m_state.soft_const_p * (m_state.bc.row(i) - V_o.row(m_state.b(i))).squaredNorm();
  }
  return e;
}

double WeightedGlobalLocal::compute_energy_with_jacobians(const Eigen::MatrixXd &V,
                                                          const Eigen::MatrixXi &F, const Eigen::MatrixXd &Ji,
                                                          Eigen::MatrixXd &uv, Eigen::VectorXd &areas)
{

  double energy = 0;
  if (m_state.dim == 2)
  {
    Eigen::Matrix<double, 2, 2> ji;
    for (int i = 0; i < m_state.f_n; i++)
    {
      ji(0, 0) = Ji(i, 0);
      ji(0, 1) = Ji(i, 1);
      ji(1, 0) = Ji(i, 2);
      ji(1, 1) = Ji(i, 3);

      typedef Eigen::Matrix<double, 2, 2> Mat2;
      typedef Eigen::Matrix<double, 2, 1> Vec2;
      Mat2 ri, ti, ui, vi;
      Vec2 sing;
      igl::polar_svd(ji, ri, ti, ui, sing, vi);
      double s1 = sing(0);
      double s2 = sing(1);

      switch (m_state.slim_energy)
      {
        case igl::SLIMData::ARAP:
        {
          energy += areas(i) * (pow(s1 - 1, 2) + pow(s2 - 1, 2));
          break;
        }
        case igl::SLIMData::SYMMETRIC_DIRICHLET:
        {
          energy += areas(i) * (pow(s1, 2) + pow(s1, -2) + pow(s2, 2) + pow(s2, -2));
          break;
        }
        case igl::SLIMData::EXP_SYMMETRIC_DIRICHLET:
        {
          energy += areas(i) * exp(m_state.exp_factor * (pow(s1, 2) + pow(s1, -2) + pow(s2, 2) + pow(s2, -2)));
          break;
        }
        case igl::SLIMData::LOG_ARAP:
        {
          energy += areas(i) * (pow(log(s1), 2) + pow(log(s2), 2));
          break;
        }
        case igl::SLIMData::CONFORMAL:
        {
          energy += areas(i) * ((pow(s1, 2) + pow(s2, 2)) / (2 * s1 * s2));
          break;
        }
        case igl::SLIMData::EXP_CONFORMAL:
        {
          energy += areas(i) * exp(m_state.exp_factor * ((pow(s1, 2) + pow(s2, 2)) / (2 * s1 * s2)));
          break;
        }

      }

    }
  }
  else
  {
    Eigen::Matrix<double, 3, 3> ji;
    for (int i = 0; i < m_state.f_n; i++)
    {
      ji(0, 0) = Ji(i, 0);
      ji(0, 1) = Ji(i, 1);
      ji(0, 2) = Ji(i, 2);
      ji(1, 0) = Ji(i, 3);
      ji(1, 1) = Ji(i, 4);
      ji(1, 2) = Ji(i, 5);
      ji(2, 0) = Ji(i, 6);
      ji(2, 1) = Ji(i, 7);
      ji(2, 2) = Ji(i, 8);

      typedef Eigen::Matrix<double, 3, 3> Mat3;
      typedef Eigen::Matrix<double, 3, 1> Vec3;
      Mat3 ri, ti, ui, vi;
      Vec3 sing;
      igl::polar_svd(ji, ri, ti, ui, sing, vi);
      double s1 = sing(0);
      double s2 = sing(1);
      double s3 = sing(2);

      switch (m_state.slim_energy)
      {
        case igl::SLIMData::ARAP:
        {
          energy += areas(i) * (pow(s1 - 1, 2) + pow(s2 - 1, 2) + pow(s3 - 1, 2));
          break;
        }
        case igl::SLIMData::SYMMETRIC_DIRICHLET:
        {
          energy += areas(i) * (pow(s1, 2) + pow(s1, -2) + pow(s2, 2) + pow(s2, -2) + pow(s3, 2) + pow(s3, -2));
          break;
        }
        case igl::SLIMData::EXP_SYMMETRIC_DIRICHLET:
        {
          energy += areas(i) * exp(m_state.exp_factor *
                                   (pow(s1, 2) + pow(s1, -2) + pow(s2, 2) + pow(s2, -2) + pow(s3, 2) + pow(s3, -2)));
          break;
        }
        case igl::SLIMData::LOG_ARAP:
        {
          energy += areas(i) * (pow(log(s1), 2) + pow(log(abs(s2)), 2) + pow(log(abs(s3)), 2));
          break;
        }
        case igl::SLIMData::CONFORMAL:
        {
          energy += areas(i) * ((pow(s1, 2) + pow(s2, 2) + pow(s3, 2)) / (3 * pow(s1 * s2 * s3, 2. / 3.)));
          break;
        }
        case igl::SLIMData::EXP_CONFORMAL:
        {
          energy += areas(i) * exp((pow(s1, 2) + pow(s2, 2) + pow(s3, 2)) / (3 * pow(s1 * s2 * s3, 2. / 3.)));
          break;
        }
      }
    }
  }

  return energy;
}

void WeightedGlobalLocal::buildA(Eigen::SparseMatrix<double> &A)
{
  // formula (35) in paper
  std::vector<Triplet<double> > IJV;
  if (m_state.dim == 2)
  {
    IJV.reserve(4 * (m_state.Dx.outerSize() + m_state.Dy.outerSize()));

    /*A = [W11*Dx, W12*Dx;
         W11*Dy, W12*Dy;
         W21*Dx, W22*Dx;
         W21*Dy, W22*Dy];*/
    for (int k = 0; k < m_state.Dx.outerSize(); ++k)
    {
      for (SparseMatrix<double>::InnerIterator it(m_state.Dx, k); it; ++it)
      {
        int dx_r = it.row();
        int dx_c = it.col();
        double val = it.value();

        IJV.push_back(Triplet<double>(dx_r, dx_c, val * m_state.W_11(dx_r)));
        IJV.push_back(Triplet<double>(dx_r, m_state.v_n + dx_c, val * m_state.W_12(dx_r)));

        IJV.push_back(Triplet<double>(2 * m_state.f_n + dx_r, dx_c, val * m_state.W_21(dx_r)));
        IJV.push_back(Triplet<double>(2 * m_state.f_n + dx_r, m_state.v_n + dx_c, val * m_state.W_22(dx_r)));
      }
    }

    for (int k = 0; k < m_state.Dy.outerSize(); ++k)
    {
      for (SparseMatrix<double>::InnerIterator it(m_state.Dy, k); it; ++it)
      {
        int dy_r = it.row();
        int dy_c = it.col();
        double val = it.value();

        IJV.push_back(Triplet<double>(m_state.f_n + dy_r, dy_c, val * m_state.W_11(dy_r)));
        IJV.push_back(Triplet<double>(m_state.f_n + dy_r, m_state.v_n + dy_c, val * m_state.W_12(dy_r)));

        IJV.push_back(Triplet<double>(3 * m_state.f_n + dy_r, dy_c, val * m_state.W_21(dy_r)));
        IJV.push_back(Triplet<double>(3 * m_state.f_n + dy_r, m_state.v_n + dy_c, val * m_state.W_22(dy_r)));
      }
    }
  }
  else
  {

    /*A = [W11*Dx, W12*Dx, W13*Dx;
           W11*Dy, W12*Dy, W13*Dy;
           W11*Dz, W12*Dz, W13*Dz;
           W21*Dx, W22*Dx, W23*Dx;
           W21*Dy, W22*Dy, W23*Dy;
           W21*Dz, W22*Dz, W23*Dz;
           W31*Dx, W32*Dx, W33*Dx;
           W31*Dy, W32*Dy, W33*Dy;
           W31*Dz, W32*Dz, W33*Dz;];*/
    IJV.reserve(9 * (m_state.Dx.outerSize() + m_state.Dy.outerSize() + m_state.Dz.outerSize()));
    for (int k = 0; k < m_state.Dx.outerSize(); k++)
    {
      for (SparseMatrix<double>::InnerIterator it(m_state.Dx, k); it; ++it)
      {
        int dx_r = it.row();
        int dx_c = it.col();
        double val = it.value();

        IJV.push_back(Triplet<double>(dx_r, dx_c, val * m_state.W_11(dx_r)));
        IJV.push_back(Triplet<double>(dx_r, m_state.v_n + dx_c, val * m_state.W_12(dx_r)));
        IJV.push_back(Triplet<double>(dx_r, 2 * m_state.v_n + dx_c, val * m_state.W_13(dx_r)));

        IJV.push_back(Triplet<double>(3 * m_state.f_n + dx_r, dx_c, val * m_state.W_21(dx_r)));
        IJV.push_back(Triplet<double>(3 * m_state.f_n + dx_r, m_state.v_n + dx_c, val * m_state.W_22(dx_r)));
        IJV.push_back(Triplet<double>(3 * m_state.f_n + dx_r, 2 * m_state.v_n + dx_c, val * m_state.W_23(dx_r)));

        IJV.push_back(Triplet<double>(6 * m_state.f_n + dx_r, dx_c, val * m_state.W_31(dx_r)));
        IJV.push_back(Triplet<double>(6 * m_state.f_n + dx_r, m_state.v_n + dx_c, val * m_state.W_32(dx_r)));
        IJV.push_back(Triplet<double>(6 * m_state.f_n + dx_r, 2 * m_state.v_n + dx_c, val * m_state.W_33(dx_r)));
      }
    }

    for (int k = 0; k < m_state.Dy.outerSize(); k++)
    {
      for (SparseMatrix<double>::InnerIterator it(m_state.Dy, k); it; ++it)
      {
        int dy_r = it.row();
        int dy_c = it.col();
        double val = it.value();

        IJV.push_back(Triplet<double>(m_state.f_n + dy_r, dy_c, val * m_state.W_11(dy_r)));
        IJV.push_back(Triplet<double>(m_state.f_n + dy_r, m_state.v_n + dy_c, val * m_state.W_12(dy_r)));
        IJV.push_back(Triplet<double>(m_state.f_n + dy_r, 2 * m_state.v_n + dy_c, val * m_state.W_13(dy_r)));

        IJV.push_back(Triplet<double>(4 * m_state.f_n + dy_r, dy_c, val * m_state.W_21(dy_r)));
        IJV.push_back(Triplet<double>(4 * m_state.f_n + dy_r, m_state.v_n + dy_c, val * m_state.W_22(dy_r)));
        IJV.push_back(Triplet<double>(4 * m_state.f_n + dy_r, 2 * m_state.v_n + dy_c, val * m_state.W_23(dy_r)));

        IJV.push_back(Triplet<double>(7 * m_state.f_n + dy_r, dy_c, val * m_state.W_31(dy_r)));
        IJV.push_back(Triplet<double>(7 * m_state.f_n + dy_r, m_state.v_n + dy_c, val * m_state.W_32(dy_r)));
        IJV.push_back(Triplet<double>(7 * m_state.f_n + dy_r, 2 * m_state.v_n + dy_c, val * m_state.W_33(dy_r)));
      }
    }

    for (int k = 0; k < m_state.Dz.outerSize(); k++)
    {
      for (SparseMatrix<double>::InnerIterator it(m_state.Dz, k); it; ++it)
      {
        int dz_r = it.row();
        int dz_c = it.col();
        double val = it.value();

        IJV.push_back(Triplet<double>(2 * m_state.f_n + dz_r, dz_c, val * m_state.W_11(dz_r)));
        IJV.push_back(Triplet<double>(2 * m_state.f_n + dz_r, m_state.v_n + dz_c, val * m_state.W_12(dz_r)));
        IJV.push_back(Triplet<double>(2 * m_state.f_n + dz_r, 2 * m_state.v_n + dz_c, val * m_state.W_13(dz_r)));

        IJV.push_back(Triplet<double>(5 * m_state.f_n + dz_r, dz_c, val * m_state.W_21(dz_r)));
        IJV.push_back(Triplet<double>(5 * m_state.f_n + dz_r, m_state.v_n + dz_c, val * m_state.W_22(dz_r)));
        IJV.push_back(Triplet<double>(5 * m_state.f_n + dz_r, 2 * m_state.v_n + dz_c, val * m_state.W_23(dz_r)));

        IJV.push_back(Triplet<double>(8 * m_state.f_n + dz_r, dz_c, val * m_state.W_31(dz_r)));
        IJV.push_back(Triplet<double>(8 * m_state.f_n + dz_r, m_state.v_n + dz_c, val * m_state.W_32(dz_r)));
        IJV.push_back(Triplet<double>(8 * m_state.f_n + dz_r, 2 * m_state.v_n + dz_c, val * m_state.W_33(dz_r)));
      }
    }
  }
  A.setFromTriplets(IJV.begin(), IJV.end());
}

void WeightedGlobalLocal::buildRhs(const Eigen::SparseMatrix<double> &At)
{
  VectorXd f_rhs(m_state.dim * m_state.dim * m_state.f_n);
  f_rhs.setZero();
  if (m_state.dim == 2)
  {
    /*b = [W11*R11 + W12*R21; (formula (36))
         W11*R12 + W12*R22;
         W21*R11 + W22*R21;
         W21*R12 + W22*R22];*/
    for (int i = 0; i < m_state.f_n; i++)
    {
      f_rhs(i + 0 * m_state.f_n) = m_state.W_11(i) * m_state.Ri(i, 0) + m_state.W_12(i) * m_state.Ri(i, 1);
      f_rhs(i + 1 * m_state.f_n) = m_state.W_11(i) * m_state.Ri(i, 2) + m_state.W_12(i) * m_state.Ri(i, 3);
      f_rhs(i + 2 * m_state.f_n) = m_state.W_21(i) * m_state.Ri(i, 0) + m_state.W_22(i) * m_state.Ri(i, 1);
      f_rhs(i + 3 * m_state.f_n) = m_state.W_21(i) * m_state.Ri(i, 2) + m_state.W_22(i) * m_state.Ri(i, 3);
    }
  }
  else
  {
    /*b = [W11*R11 + W12*R21 + W13*R31;
         W11*R12 + W12*R22 + W13*R32;
         W11*R13 + W12*R23 + W13*R33;
         W21*R11 + W22*R21 + W23*R31;
         W21*R12 + W22*R22 + W23*R32;
         W21*R13 + W22*R23 + W23*R33;
         W31*R11 + W32*R21 + W33*R31;
         W31*R12 + W32*R22 + W33*R32;
         W31*R13 + W32*R23 + W33*R33;];*/
    for (int i = 0; i < m_state.f_n; i++)
    {
      f_rhs(i + 0 * m_state.f_n) = m_state.W_11(i) * m_state.Ri(i, 0) + m_state.W_12(i) * m_state.Ri(i, 1) + m_state.W_13(i) * m_state.Ri(i, 2);
      f_rhs(i + 1 * m_state.f_n) = m_state.W_11(i) * m_state.Ri(i, 3) + m_state.W_12(i) * m_state.Ri(i, 4) + m_state.W_13(i) * m_state.Ri(i, 5);
      f_rhs(i + 2 * m_state.f_n) = m_state.W_11(i) * m_state.Ri(i, 6) + m_state.W_12(i) * m_state.Ri(i, 7) + m_state.W_13(i) * m_state.Ri(i, 8);
      f_rhs(i + 3 * m_state.f_n) = m_state.W_21(i) * m_state.Ri(i, 0) + m_state.W_22(i) * m_state.Ri(i, 1) + m_state.W_23(i) * m_state.Ri(i, 2);
      f_rhs(i + 4 * m_state.f_n) = m_state.W_21(i) * m_state.Ri(i, 3) + m_state.W_22(i) * m_state.Ri(i, 4) + m_state.W_23(i) * m_state.Ri(i, 5);
      f_rhs(i + 5 * m_state.f_n) = m_state.W_21(i) * m_state.Ri(i, 6) + m_state.W_22(i) * m_state.Ri(i, 7) + m_state.W_23(i) * m_state.Ri(i, 8);
      f_rhs(i + 6 * m_state.f_n) = m_state.W_31(i) * m_state.Ri(i, 0) + m_state.W_32(i) * m_state.Ri(i, 1) + m_state.W_33(i) * m_state.Ri(i, 2);
      f_rhs(i + 7 * m_state.f_n) = m_state.W_31(i) * m_state.Ri(i, 3) + m_state.W_32(i) * m_state.Ri(i, 4) + m_state.W_33(i) * m_state.Ri(i, 5);
      f_rhs(i + 8 * m_state.f_n) = m_state.W_31(i) * m_state.Ri(i, 6) + m_state.W_32(i) * m_state.Ri(i, 7) + m_state.W_33(i) * m_state.Ri(i, 8);
    }
  }
  VectorXd uv_flat(m_state.dim *m_state.v_n);
  for (int i = 0; i < m_state.dim; i++)
    for (int j = 0; j < m_state.v_n; j++)
      uv_flat(m_state.v_n * i + j) = m_state.V_o(j, i);

  m_state.rhs = (At * m_state.WGL_M.asDiagonal() * f_rhs + m_state.proximal_p * uv_flat);
}



// #define TwoPi  6.28318530717958648
// const double eps=1e-14;

/// Slim Implementation

IGL_INLINE void igl::slim_precompute(Eigen::MatrixXd &V, Eigen::MatrixXi &F, Eigen::MatrixXd &V_init, SLIMData &data,
                                     SLIMData::SLIM_ENERGY slim_energy, Eigen::VectorXi &b, Eigen::MatrixXd &bc,
                                     double soft_p)
{

  data.V = V;
  data.F = F;
  data.V_o = V_init;

  data.v_num = V.rows();
  data.f_num = F.rows();

  data.slim_energy = slim_energy;

  data.b = b;
  data.bc = bc;
  data.soft_const_p = soft_p;

  data.proximal_p = 0.0001;

  igl::doublearea(V, F, data.M);
  data.M /= 2.;
  data.mesh_area = data.M.sum();
  data.mesh_improvement_3d = false; // whether to use a jacobian derived from a real mesh or an abstract regular mesh (used for mesh improvement)
  data.exp_factor = 1.0; // param used only for exponential energies (e.g exponential symmetric dirichlet)

  assert (F.cols() == 3 || F.cols() == 4);
  data.wGlobalLocal = new WeightedGlobalLocal(data);

  data.wGlobalLocal->pre_calc();
  data.energy = data.wGlobalLocal->compute_energy(data.V_o) / data.mesh_area;
}

IGL_INLINE void igl::slim_solve(SLIMData &data, int iter_num)
{

  for (int i = 0; i < iter_num; i++)
  {
    Eigen::MatrixXd dest_res;
    dest_res = data.V_o;
    data.wGlobalLocal->solve_weighted_proxy(dest_res);

    double old_energy = data.energy;

    std::function<double(Eigen::MatrixXd &)> compute_energy = [&](
        Eigen::MatrixXd &aaa) { return data.wGlobalLocal->compute_energy(aaa); };

    data.energy = igl::flip_avoiding_line_search(data.F, data.V_o, dest_res, compute_energy,
                                                 data.energy * data.mesh_area) / data.mesh_area;
  }
}
