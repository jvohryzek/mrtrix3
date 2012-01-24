/*
   Copyright 2008 Brain Research Institute, Melbourne, Australia

   Written by J-Donald Tournier, 27/06/08.

   This file is part of MRtrix.

   MRtrix is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   MRtrix is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with MRtrix.  If not, see <http://www.gnu.org/licenses/>.

*/

#ifndef __dwi_sdeconv_constrained_h__
#define __dwi_sdeconv_constrained_h__

#include "app.h"
#include "image/header.h"
#include "dwi/gradient.h"
#include "math/SH.h"
#include "math/directions.h"
#include "math/least_squares.h"

#define NORM_LAMBDA_MULTIPLIER 0.0002

namespace MR
{
  namespace DWI
  {

    extern const App::OptionGroup CSD_options;

    template <typename T> class CSDeconv
    {
      public:
        typedef T value_type;

        class Shared
        {
          public:
            Shared () : 
              neg_lambda (1.0),
              norm_lambda (1.0),
              threshold (0.1) { }

            Shared (const Math::Vector<value_type>& response,
                const Math::Vector<value_type>& init_filter,
                const Math::Matrix<value_type>& DW_dirs,
                const Math::Matrix<value_type>& HR_dirs,
                int l_max = 8) :
              neg_lambda (1.0),
              norm_lambda (1.0),
              threshold (0.1) { init (response, init_filter, DW_dirs, HR_dirs); }

            Shared (const Image::Header& dwi_header, 
                const std::string& response_file) :
              neg_lambda (1.0),
              norm_lambda (1.0),
              threshold (0.1) { init (dwi_header, response_file); }

            void init (const Image::Header& dwi_header, const std::string& response_file) {
              using namespace App;
              Math::Matrix<value_type> grad = DWI::get_DW_scheme<value_type> (dwi_header);

              DWI::guess_DW_directions (dwis, bzeros, grad);

              Math::Matrix<value_type> DW_dirs;
              DWI::gen_direction_matrix (DW_dirs, grad, dwis);

              Options opt = get_options ("lmax");
              lmax = opt.size() ? opt[0][0] : Math::SH::LforN (dwis.size());
              info ("calculating even spherical harmonic components up to order " + str (lmax));

              info ("setting response function from file \"" + response_file + "\"");
              Math::Vector<value_type> response;
              response.load (response_file);
              info ("setting response function using even SH coefficients: " + str (response));

              opt = get_options ("filter");
              Math::Vector<value_type> filter;
              if (opt.size())
                filter.load (opt[0][0]);
              else {
                filter.allocate (response.size());
                filter.zero();
                filter[0] = filter[1] = filter[2] = 1.0;
              }
              info ("using initial filter coefficients: " + str (filter));

              opt = get_options ("directions");
              Math::Matrix<value_type> HR_dirs;
              if (opt.size())
                HR_dirs.load (opt[0][0]);
              else 
                Math::directions_300 (HR_dirs);

              opt = get_options ("neg_lambda");
              value_type neg_lambda = 1.0;
              if (opt.size())
                neg_lambda = opt[0][0];

              opt = get_options ("norm_lambda");
              value_type norm_lambda = 1.0;
              if (opt.size())
                norm_lambda = opt[0][0];

              opt = get_options ("threshold");
              value_type threshold = 0.1;
              if (opt.size())
                threshold = opt[0][0];

              opt = get_options ("niter");
              niter = 50;
              if (opt.size())
                niter = opt[0][0];

              init (response, filter, DW_dirs, HR_dirs, lmax);
            }



            void init (const Math::Vector<value_type>& response,
                const Math::Vector<value_type>& init_filter,
                const Math::Matrix<value_type>& DW_dirs,
                const Math::Matrix<value_type>& HR_dirs,
                int l_max = 8) {
              lmax = l_max;
              int lmax_data = (response.size()-1) *2;
              int n = Math::SH::LforN (DW_dirs.rows());
              if (lmax_data > n) lmax_data = n;
              if (lmax_data > lmax) lmax_data = lmax;
              info ("calculating even spherical harmonic components up to order " + str (lmax_data) + " for initialisation");

              if (init_filter.size() < size_t (lmax_data/2) +1)
                throw Exception ("not enough initial filter coefficients supplied for lmax = " + str (lmax_data));

              Math::Vector<value_type> RH;
              Math::SH::SH2RH (RH, response);

              Math::Matrix<value_type> fconv;
              Math::SH::init_transform (fconv, DW_dirs, lmax_data);
              rconv.allocate (fconv.columns(), fconv.rows());
              Math::pinv (rconv, fconv);

              size_t l = 0, nl = 1;
              for (size_t row = 0; row < rconv.rows(); row++) {
                if (row >= nl) {
                  l++;
                  nl = Math::SH::NforL (2*l);
                }
                for (size_t col = 0; col < rconv.columns(); col++) {
                  rconv (row, col) *= init_filter[l] / RH[l];
                  fconv (col,row) *= RH[l];
                }
              }

              Math::SH::init_transform (HR_trans, HR_dirs, lmax);
              HR_trans *= neg_lambda * value_type (fconv.rows()) * response[0] / value_type (HR_trans.rows());

              M.allocate (DW_dirs.rows(), HR_trans.columns());
              M.sub (0,M.rows(),0,fconv.columns()) = fconv;
              M.sub (0,M.rows(),fconv.columns(),M.columns()) = 0.0;
              Mt_M.allocate (M.columns(), M.columns());
              rankN_update (Mt_M, M, CblasTrans, CblasLower, value_type (1.0), value_type (0.0));

              info ("constrained spherical deconvolution initiated successfully");
            }

            size_t nSH () const {
              return HR_trans.columns();
            }

            Math::Matrix<value_type> rconv, HR_trans, M, Mt_M;
            value_type neg_lambda, norm_lambda, threshold;
            std::vector<int> bzeros, dwis;
            int lmax;
            size_t niter;
        };


        CSDeconv (const Shared& shared) :
          P (shared),
          work (P.Mt_M.rows(), P.Mt_M.columns()),
          HR_T (P.HR_trans.rows(), P.HR_trans.columns()),
          F (P.HR_trans.columns()),
          init_F (P.rconv.rows()),
          HR_amps (P.HR_trans.rows()),
          Mt_b (P.HR_trans.columns()),
          old_neg (P.HR_trans.rows()) {
            norm_lambda = NORM_LAMBDA_MULTIPLIER * P.norm_lambda * P.Mt_M (0,0);
          }

        CSDeconv (const CSDeconv& c) :
          P (c.P),
          work (P.Mt_M.rows(), P.Mt_M.columns()),
          HR_T (P.HR_trans.rows(), P.HR_trans.columns()),
          F (P.HR_trans.columns()),
          init_F (P.rconv.rows()),
          HR_amps (P.HR_trans.rows()),
          Mt_b (P.HR_trans.columns()),
          old_neg (P.HR_trans.rows()) {
            norm_lambda = NORM_LAMBDA_MULTIPLIER * P.norm_lambda * P.Mt_M (0,0);
          }

        ~CSDeconv() { }

        void set (const Math::Vector<value_type>& DW_signals) {
          Math::mult (init_F, P.rconv, DW_signals);
          F.sub (0, init_F.size()) = init_F;
          F.sub (init_F.size(), F.size()) = 0.0;
          old_neg.assign (P.HR_trans.rows(), -1);
          threshold = P.threshold * init_F[0] * P.HR_trans (0,0);
          computed_once = false;

          mult (Mt_b, value_type (0.0), value_type (1.0), CblasTrans, P.M, DW_signals);
        }

        bool iterate() {
          neg.clear();
          Math::mult (HR_amps, P.HR_trans, F);
          for (size_t n = 0; n < HR_amps.size(); n++)
            if (HR_amps[n] < threshold)
              neg.push_back (n);

          if (computed_once && old_neg == neg)
            if (old_neg == neg)
              return true;

          for (size_t i = 0; i < work.rows(); i++) {
            for (size_t j = 0; j < i; j++)
              work (i,j) = P.Mt_M (i,j);
            work (i,i) = P.Mt_M (i,i) + norm_lambda;
          }

          if (neg.size()) {
            HR_T.allocate (neg.size(), P.HR_trans.columns());
            for (size_t i = 0; i < neg.size(); i++)
              HR_T.row (i) = P.HR_trans.row (neg[i]);
            rankN_update (work, HR_T, CblasTrans, CblasLower, value_type (1.0), value_type (1.0));
          }

          F = Mt_b;

          Math::Cholesky::decomp (work);
          Math::Cholesky::solve (F, work);
          computed_once = true;
          old_neg = neg;

          return false;
        }

        const Math::Vector<value_type>& FOD () const         {
          return F;
        }


        const Shared& P;

      protected:
        value_type threshold, norm_lambda;
        Math::Matrix<value_type> work, HR_T;
        Math::Vector<value_type> F, init_F, HR_amps, Mt_b;
        std::vector<int> neg, old_neg;
        bool computed_once;
    };


  }
}

#endif


