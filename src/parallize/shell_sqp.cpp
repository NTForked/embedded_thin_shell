#include "shell_sqp.h"

#include <iostream>
#include <fstream>
#include <zjucad/matrix/itr_matrix.h>
#include <zjucad/ptree/ptree.h>
#include <zjucad/matrix/io.h>
#include <hjlib/util/hrclock.h>
#include <Eigen/Sparse>
#include "../shell_deformer/sparse_transition.h"

using namespace zjucad::matrix;
using namespace std;
using namespace hj::sparse;

shell_sqp::shell_sqp(const shared_ptr<jtf_func>  &f,
                     const shared_ptr<jtf_func>  &f_spring,
                     const shared_ptr<jtf_func>  &f_bend,
                     const shared_ptr<jtf_func>  &f_pos,
                     const double                w1,
                     const double                w2,
                     const double                w3,
                     const double                *x)
    : f_(f), fs_(f_spring), fb_(f_bend), fp_(f_pos),
             ws_(w1),       wb_(w2),     wp_(w3) {
    slv_.reset(nullptr);
    size_t dim = f_->dim();
    g_ = zeros<double>(dim, 1);
    s_ = zeros<double>(dim, 1);
    D_ = zeros<double>(dim, 1);
    tmp_ = zeros<double>(dim, 1);
    {
        size_t hes_nnz = 0;
        size_t format = -1;
        fb_->hes(x, hes_nnz, format, 0, 0, 0);
        Hb_.resize(dim, dim, hes_nnz);
        fb_->hes(x, hes_nnz, format, 0, &Hb_.ptr()[0], &Hb_.idx()[0]);
        Hb_.val()(colon()) = 0;
        fb_->hes(x, hes_nnz, format, &Hb_.val()[0], &Hb_.ptr()[0], &Hb_.idx()[0]);
        Hb_.val() *= wb_;
    }
    {
        size_t hes_nnz = 0;
        size_t format = -1;
        fp_->hes(x, hes_nnz, format, 0, 0, 0);
        Hp_.resize(dim, dim, hes_nnz);
        fp_->hes(x, hes_nnz, format, 0, &Hp_.ptr()[0], &Hp_.idx()[0]);
        Hp_.val()(colon()) = 0;
        fp_->hes(x, hes_nnz, format, &Hp_.val()[0], &Hp_.ptr()[0], &Hp_.idx()[0]);
        Hp_.val() *= wp_;
    }
    Eigen::SparseMatrix<double> Hp_copy, Hb_copy;
    sparse_transition mat_trans;
    mat_trans.hj2eigen(Hb_, Hb_copy);
    mat_trans.hj2eigen(Hp_, Hp_copy);
    const_hes_ = Hb_copy + Hp_copy;
    cout << "[INFO] shell SQP optimizer initialization done!\n";
}

static double adjust_mu(double mu, double ratio)
{
    const double min_max_mu[] = {1e-5, 1e6};
    if(ratio < 0) {
        mu *= 4;
        if(mu < min_max_mu[0])
            mu = min_max_mu[0];
    }
    else if(ratio < 0.25) {
        mu *= 4;
        if(mu < min_max_mu[0])
            mu = min_max_mu[0];
    }
    else
        mu /= sqrt(ratio*4); // better than /2
    if(mu > min_max_mu[1])
        mu = min_max_mu[1];
    return mu;
}

template <typename VAL_TYPE, typename INT_TYPE>
inline void add_to_diag(hj::sparse::csc<VAL_TYPE, INT_TYPE> &A, VAL_TYPE *diag)
{
    for(INT_TYPE ci = 0; ci < A.size(2); ++ci) {
        INT_TYPE nzi = A.ptr()[ci];
        for(; nzi < A.ptr()[ci+1]; ++nzi) {
            if(A.idx()[nzi] == ci) {
                A.val()[nzi] += diag[ci];
                break;
            }
        }
        if(nzi == A.ptr()[ci+1]) {
            cout << "bad add to diag." << endl;
        }
    }
}

static void compute_D(double *D, const csc<double, int32_t> &H)
{
    matrix<double> D1 = zeros<double>(H.size(1), 1);
    for(size_t ci = 0; ci < H.size(2); ++ci) {
        for(size_t nzi = H.ptr()[ci]; nzi < H.ptr()[ci+1]; ++nzi) {
            if(H.idx()[nzi] == ci) {
                D1[ci] = H.val()[nzi];
                if(D1[ci] < 0) {

                    //cerr << "negative H diag: " << D1[ci] << endl;
                    D1[ci] = 0;
                }
            }
        }
    }
    D1 = sqrt(D1); // STRANGE: should be squared, DTD, according to the
    // book, but removing the sqrt leads to slow
    // convergence.
    for(ptrdiff_t i = 0; i < H.size(1); ++i) {
        if(D[i] < D1[i])
            D[i] = D1[i];
    }
}


int shell_sqp::solve(double *x, boost::property_tree::ptree &pt)
{
    const double eps[3] = {
        pt.get<double>("epsg.value", 1e-6),
        pt.get<double>("epsf.value", 1e-6),
        pt.get<double>("epsx.value", 1e-20)
    };
    double norm2_of_f_g[2], mu = 1e-6, radius= 1e4;
    pt.put("GS_LM_mu.desc","mu for trust region.");
    if(zjucad::has("GS_LM_mu.value", pt))
        mu = pt.get<double>("GS_LM_mu.value");

    const size_t dim = f_->dim();
    itr_matrix<double *> x0(dim, 1, x);
    int iter_num = pt.get<int>("iter.value");

    for(int i = 0; i < iter_num; ++i) {
        cerr << endl << i;
        norm2_of_f_g[0] = 0;
        f_->val(x, norm2_of_f_g[0]);
        g_(colon()) = 0;

        f_->gra(x, &g_[0]);

        norm2_of_f_g[1] = dot(g_, g_);

        cerr << "\t" << norm2_of_f_g[0] << "\t" << norm2_of_f_g[1] << "\t" << mu << "\t" << radius;
        if(norm2_of_f_g[1] < eps[0]) {
            cerr << "\ngradient converge." << endl;
            break;
        }

        size_t hes_nnz = 0;
        size_t format = -1;

        static hj::util::high_resolution_clock hrc;
        double start = hrc.ms();

//***************************HESSIAN ASSEMBLING**************************************
        static Eigen::SparseMatrix<double> const_H(const_hes_);

        if( nnz(Hs_) == 0 ) { // the first run
            //if(nnz(H_) == 0 || H_.size(1) != dim) { // the first run, JTF modify.
            fs_->hes(x, hes_nnz, format, 0, 0, 0);
            Hs_.resize(dim, dim, hes_nnz);
            fs_->hes(x, hes_nnz, format, 0, &Hs_.ptr()[0], &Hs_.idx()[0]);
        }
        else
            hes_nnz = nnz(Hs_);
        Hs_.val()(colon()) = 0;
        fs_->hes(x, hes_nnz, format, &Hs_.val()[0], &Hs_.ptr()[0], &Hs_.idx()[0]);
        Hs_.val()(colon()) *= ws_;

        Eigen::SparseMatrix<double> Hs_copy, temp;

        sparse_transition mat_trans;
        mat_trans.hj2eigen(Hs_, Hs_copy);

        temp = const_H + Hs_copy;

        mat_trans.eigen2hj(temp, H_);
        cout << "time for assemble Hessian : " << hrc.ms() - start << endl;
//***********************************************************************************
        bool at_border = true;
        int step_type = -1; // 0 TC, 1 NP, 2 DL
        // Cauchy point
        tmp_ = zeros<double>(dim, 1);
        mv(false, H_, g_, tmp_); // corrected_H?
        const double Cauchy_len = norm2_of_f_g[1]/dot(tmp_, g_), norm_g = sqrt(norm2_of_f_g[1]);
        if(Cauchy_len*norm_g > radius) {
            s_ = -g_*radius/norm_g; // truncate
            step_type = 0;
            cerr << "\tTC";
        }
        else { // Cauchy point is inside, evaluate Newton point
            compute_D(&D_[0], H_);
            D_ *= mu;
            //std::cerr << D_ << std::endl;
            corrected_H_ = H_;
            add_to_diag(corrected_H_, &D_[0]);

            //cerr << "create solver beg" << endl;
            cj::linear_solver* a = cj::linear_solver::create(corrected_H_, pt);
            slv_.reset(a);

            //cerr << "create solver end" << endl;
            if(slv_.get()) {

                hj::util::high_resolution_clock hrc;
                double begin = hrc.ms();

                slv_->solve(&g_[0], &s_[0], 1, pt);
                cout << "solve linear equation : " << hrc.ms() - begin << endl;
                s_ = -s_;

                if(norm(s_) < radius) { // Newton point is inside of the radius
                    //        x0 += s_;
                    step_type = 1;
                    cerr << "\tNP";
                    at_border = false;
                }
                else { // intersect with radius
                    const matrix<double> CP = -g_*Cauchy_len;
                    const double gamma = dot(CP, CP)/dot(s_, s_);
                    matrix<double> C2N = (0.8*gamma+0.2)*s_-CP;
                    const double quadric_eq[3] = {dot(C2N, C2N), 2*dot(CP, C2N), dot(CP, CP)-radius*radius};
                    double Delta = quadric_eq[1]*quadric_eq[1]-4*quadric_eq[0]*quadric_eq[2];
                    assert(Delta > 0);
                    Delta = sqrt(Delta);
                    if(quadric_eq[0] < 0) {
                        cerr << "\tbad dog leg: " << quadric_eq[0] << endl;
                    }
                    const double roots[2] = {
                        (-quadric_eq[1]+Delta)/(2*quadric_eq[0]),
                        (-quadric_eq[1]-Delta)/(2*quadric_eq[0])
                    };
                    // it's highly possible that dot(CP, C2N) > 0
                    if(roots[0] >= 0 && roots[0] <= 1) {
                        if(roots[1] >= 0 && roots[1] <= 1)
                            cerr << "two intersection?" << endl;
                        s_ = CP+roots[0]*C2N;
                    }
                    else if(roots[1] >= 0 && roots[1] <= 1) {
                        cerr << "strange why the second root?" << endl;
                        s_ = CP+roots[1]*C2N;
                    }
                    else {
                        if(roots[0] > 0) {
                            s_ = CP+roots[0]*C2N;
                        }
                        if(roots[1] > 0) {
                            s_ = CP+roots[1]*C2N;
                        }
                        if(roots[0]*roots[1] > 0)
                            cerr << "\nunbelievable that no root is OK: " << roots[0] << " " << roots[1] << endl;
                    }
                    step_type = 2;
                    cerr << "\tDL";
                }
            }
            else { // H is not SPD
                mu *= 16;
                continue;
            }
        }
        matrix<double> ori_x = x0;
        if(max(fabs(s_)) < eps[2]) {
            cerr << "\nstep converge." << endl;
            break;
        }
        x0 += s_;
        // update mu and radius
        double new_val = 0, est_val;
        f_->val(x, new_val);
        tmp_(colon()) = 0;
        mv(false, H_, s_, tmp_);
        est_val = norm2_of_f_g[0] + dot(s_, g_) + dot(s_, tmp_)/2;
        double ratio = -1;
        if(new_val < norm2_of_f_g[0]) {
            const double delta_f = new_val-norm2_of_f_g[0];
            double t = est_val-norm2_of_f_g[0];
            if(fabs(t) < 1e-10)
                t = 1e-5;
            ratio = delta_f/t;
        }
        //    cerr << "\t" << est_val << "\t" << new_val << "\t" << norm2_of_f_g[0] << "\t" << ratio;
        if(ratio < 0 && step_type == 0) { // accept bad step for NP and DL
            x0 = ori_x;
        }
        mu = adjust_mu(mu, ratio);
        if(ratio < 0.25) {
            radius /= 4;
        }
        else if(ratio > 0.75 || at_border) {
            radius *= 2;
        }
        cerr << endl;
    }
    cerr << "\noptimization over." << endl;
    return 0;
}


