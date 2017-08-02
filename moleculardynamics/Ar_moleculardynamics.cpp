﻿/*! \file Ar_moleculardynamics.h
    \brief アルゴンに対して、分子動力学シミュレーションを行うクラスの実装

    Copyright ©  2015 @dc1394 All Rights Reserved.
    This software is released under the BSD 2-Clause License.
*/

#include "Ar_moleculardynamics.h"
#include "myrandom/myrand.h"
#include <cmath>                    // for std::sqrt, std::pow
#include <boost/assert.hpp>         // for BOOST_ASSERT
#include <tbb/combinable.h>         // for tbb::combinable
#include <tbb/parallel_for.h>       // for tbb::parallel_for

namespace moleculardynamics {
    // #region static private 定数

    double const Ar_moleculardynamics::FIRSTSCALE = 1.0;

    double const Ar_moleculardynamics::FIRSTTEMP = 50.0;

    double const Ar_moleculardynamics::SIGMA = 3.405E-10;

    double const Ar_moleculardynamics::VDW_RADIUS = 1.88E-10;

    double const Ar_moleculardynamics::ALPHA = 0.2;

    double const Ar_moleculardynamics::ATM = 9.86923266716013E-6;

    double const Ar_moleculardynamics::AVOGADRO_CONSTANT = 6.022140857E+23;

    double const Ar_moleculardynamics::DT = 0.001;

    double const Ar_moleculardynamics::HARTREE = 4.35974465054E-18;

    double const Ar_moleculardynamics::KB = 1.3806488E-23;

    double const Ar_moleculardynamics::TAU =
        std::sqrt(0.039948 / Ar_moleculardynamics::AVOGADRO_CONSTANT * Ar_moleculardynamics::SIGMA * Ar_moleculardynamics::SIGMA / Ar_moleculardynamics::YPSILON);

    double const Ar_moleculardynamics::YPSILON = 1.6540172624E-21;

    // #endregion static private 定数

    // #region コンストラクタ

    Ar_moleculardynamics::Ar_moleculardynamics()
        :
        MD_iter([this] { return MD_iter_; }, nullptr),
        Nc([this] { return Nc_; }, nullptr),
        NumAtom([this] { return NumAtom_; }, nullptr),
        periodiclen([this] { return periodiclen_; }, nullptr),
        Uk([this] { return DimensionlessToHartree(Uk_); }, nullptr),
        Up([this] { return DimensionlessToHartree(Up_); }, nullptr),
        Utot([this] { return DimensionlessToHartree(Utot_); }, nullptr),
        X([this] { return std::cref(X_); }, nullptr),
        Y([this] { return std::cref(Y_); }, nullptr),
        Z([this] { return std::cref(Z_); }, nullptr),
        atoms_(Nc_ * Nc_ * Nc_ * 4),
        dt2(DT * DT),
        rc2_(rc_ * rc_),
        rcm6_(std::pow(rc_, -6.0)),
        rcm12_(std::pow(rc_, -12.0)),
        Tg_(Ar_moleculardynamics::FIRSTTEMP * Ar_moleculardynamics::KB / Ar_moleculardynamics::YPSILON),
        Vrc_(4.0 * (rcm12_ - rcm6_)),
        X_(Nc_ * Nc_ * Nc_ * 4),
        Y_(Nc_ * Nc_ * Nc_ * 4),
        Z_(Nc_ * Nc_ * Nc_ * 4)
    {
        // initalize parameters
        lat_ = std::pow(2.0, 2.0 / 3.0) * scale_;

        recalc();

        periodiclen_ = lat_ * static_cast<double>(Nc_);
    }

    // #endregion コンストラクタ

    // #region publicメンバ関数

    void Ar_moleculardynamics::runCalc()
    {
        calcForces();
        moveAtoms();
        periodic();

        // 繰り返し回数と時間を増加
        t_ = static_cast<double>(MD_iter_)* Ar_moleculardynamics::DT;
        MD_iter_++;
    }

    void Ar_moleculardynamics::calcForces()
    {
        // 各原子に働く力の初期化
        for (auto n = 0; n < NumAtom_; n++) {
            atoms_[n].f = Eigen::Vector4d::Zero();
        }
        
        // ポテンシャルエネルギーの初期化
        Up_ = 0.0;

        tbb::combinable<double> Up;
        tbb::combinable<double> virial;
        
        tbb::parallel_for(
            tbb::blocked_range<std::int32_t>(0, NumAtom_),
            [this, &Up, &virial](tbb::blocked_range<std::int32_t> const & range) {
            for (auto && n = range.begin(); n != range.end(); ++n) {
                for (auto m = 0; m < NumAtom_; m++) {

                    // ±ncp_分のセル内の原子との相互作用を計算
                    for (auto i = -ncp_; i <= ncp_; i++) {
                        for (auto j = -ncp_; j <= ncp_; j++) {
                            for (auto k = -ncp_; k <= ncp_; k++) {
                                auto const sx = static_cast<double>(i) * periodiclen_;
                                auto const sy = static_cast<double>(j) * periodiclen_;
                                auto const sz = static_cast<double>(k) * periodiclen_;

                                // 自分自身との相互作用を排除
                                if (n != m || i != 0 || j != 0 || k != 0) {
                                    auto const dx = X_[n] - (X_[m] + sx);
                                    auto const dy = Y_[n] - (Y_[m] + sy);
                                    auto const dz = Z_[n] - (Z_[m] + sz);

                                    Eigen::Vector3d r2vec(dx, dy, dz);
                                    auto const r2 = r2vec.squaredNorm();
                                    // 打ち切り距離内であれば計算
                                    if (r2 <= rc2_) {
                                        auto const r = std::sqrt(r2);
                                        auto const rm6 = 1.0 / (r2 * r2 * r2);
                                        auto const rm7 = rm6 / r;
                                        auto const rm12 = rm6 * rm6;
                                        auto const rm13 = rm12 / r;

                                        auto const Fr = 48.0 * rm13 - 24.0 * rm7;
                                        
                                        atoms_[n].f += Eigen::Vector4d(dx / r * Fr, dy / r * Fr, dz / r * Fr, 0.0);

                                        // エネルギーの計算、ただし二重計算のために0.5をかけておく
                                        Up.local() += 0.5 * (4.0 * (rm12 - rm6) - Vrc_);
                                        virial.local() += 0.5 * r * Fr;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        });

        Up_ = Up.combine(std::plus<double>());
        virial_ = virial.combine(std::plus<double>());
    }
    
    double Ar_moleculardynamics::getDeltat() const
    {
        return Ar_moleculardynamics::TAU * t_ * 1.0E+12;
    }

    float Ar_moleculardynamics::getForce(std::int32_t n) const
    {
        return static_cast<float>(atoms_[n].f.norm());
    }

    double Ar_moleculardynamics::getLatticeconst() const
    {
        return Ar_moleculardynamics::SIGMA * lat_ * 1.0E+9;
    }
    
    double Ar_moleculardynamics::getPeriodiclen() const
    {
        return Ar_moleculardynamics::SIGMA * periodiclen_ * 1.0E+9;
    }

    double Ar_moleculardynamics::getPressure() const
    {
        auto const V = std::pow(Ar_moleculardynamics::SIGMA * periodiclen_, 3);
        auto const ideal = NumAtom * Ar_moleculardynamics::YPSILON * Tc_;

        return (ideal - virial_ * Ar_moleculardynamics::YPSILON / 3.0) / V * Ar_moleculardynamics::ATM;
    }

    double Ar_moleculardynamics::getTcalc() const
    {
        return Ar_moleculardynamics::YPSILON / Ar_moleculardynamics::KB * Tc_;
    }

    double Ar_moleculardynamics::getTgiven() const
    {
        return Ar_moleculardynamics::YPSILON / Ar_moleculardynamics::KB * Tg_;
    }
    
    void Ar_moleculardynamics::moveAtoms()
    {
        // 運動エネルギーの初期化
        Uk_ = 0.0;

        // calculate temperture
        for (auto n = 0; n < NumAtom_; n++) {
            Uk_ += atoms_[n].v.squaredNorm();
        }

        // 運動エネルギーの計算
        Uk_ *= 0.5;

        // 全エネルギー（運動エネルギー+ポテンシャルエネルギー）の計算
        Utot_ = Uk_ + Up_;

        // 温度の計算
        Tc_ = Uk_ / (1.5 * static_cast<double>(NumAtom_));

        auto const s = std::sqrt((Tg_ + Ar_moleculardynamics::ALPHA * (Tc_ - Tg_)) / Tc_);

        switch (MD_iter_) {
        case 1:
            // update the coordinates by the second order Euler method
            // 最初のステップだけ修正Euler法で時間発展
            tbb::parallel_for(
                tbb::blocked_range<std::int32_t>(0, NumAtom_),
                [this, s](tbb::blocked_range<std::int32_t> const & range) {
                for (auto && n = range.begin(); n != range.end(); ++n) {
                    //atoms_[n].r1 = atoms_[n].r;
                    atoms_[n].r1 = { X_[n], Y_[n], Z_[n], 0.0 };
                    
                    // update coordinates and velocity
                    //atoms_[n].r += Ar_moleculardynamics::DT * atoms_[n].v + 0.5 * atoms_[n].f * dt2;

                    // scaling of velocity
                    atoms_[n].v *= s;

                    // update coordinates and velocity
                    X_[n] += Ar_moleculardynamics::DT * atoms_[n].v[0] + 0.5 * atoms_[n].f[0] * dt2;
                    Y_[n] += Ar_moleculardynamics::DT * atoms_[n].v[1] + 0.5 * atoms_[n].f[1] * dt2;
                    Z_[n] += Ar_moleculardynamics::DT * atoms_[n].v[2] + 0.5 * atoms_[n].f[2] * dt2;

                    atoms_[n].v += Ar_moleculardynamics::DT * atoms_[n].f;
                }
            });
            break;

        default:
            // update the coordinates by the Verlet method
            tbb::parallel_for(
                tbb::blocked_range<std::int32_t>(0, NumAtom_),
                [this, s](tbb::blocked_range<std::int32_t> const & range) {
                    for (auto && n = range.begin(); n != range.end(); ++n) {
                        //auto const rtmp = atoms_[n].r;

                        //switch (ensemble_) {
                        //case EnsembleType::NVE:
                        //    atoms_[n].r = 2.0 * atoms_[n].r - atoms_[n].r1 + atoms_[n].f * dt2;
                        //    break;

                        //case EnsembleType::NVT:
                        //    // update coordinates and velocity
                        //    // Verlet法の座標更新式において速度成分を抜き出し、その部分をスケールする
                        //    atoms_[n].r += s * (atoms_[n].r - atoms_[n].r1) + atoms_[n].f * dt2;
                        //    break;

                        //default:
                        //    BOOST_ASSERT(!"何かがおかしい！");
                        //}

                        //atoms_[n].v = 0.5 * (atoms_[n].r - atoms_[n].r1) / Ar_moleculardynamics::DT;
                        //atoms_[n].r1 = rtmp;
                        Eigen::Vector4d rtmp(X_[n], Y_[n], Z_[n], 0.0);

                        switch (ensemble_) {
                        case EnsembleType::NVE:
                            X_[n] = 2.0 * X_[n] - atoms_[n].r1[0] + atoms_[n].f[0] * dt2;
                            Y_[n] = 2.0 * Y_[n] - atoms_[n].r1[1] + atoms_[n].f[1] * dt2;
                            Z_[n] = 2.0 * Z_[n] - atoms_[n].r1[2] + atoms_[n].f[2] * dt2;
                            break;

                        case EnsembleType::NVT:
                            // update coordinates and velocity
                            // Verlet法の座標更新式において速度成分を抜き出し、その部分をスケールする
                            X_[n] += s * (X_[n] - atoms_[n].r1[0]) + atoms_[n].f[0] * dt2;
                            Y_[n] += s * (Y_[n] - atoms_[n].r1[1]) + atoms_[n].f[1] * dt2;
                            Z_[n] += s * (Z_[n] - atoms_[n].r1[2]) + atoms_[n].f[2] * dt2;
                            break;

                        default:
                            BOOST_ASSERT(!"何かがおかしい！");
                        }

                        atoms_[n].v = Eigen::Vector4d(
                            0.5 * (X_[n] - atoms_[n].r1[0]) / Ar_moleculardynamics::DT,
                            0.5 * (Y_[n] - atoms_[n].r1[1]) / Ar_moleculardynamics::DT,
                            0.5 * (Z_[n] - atoms_[n].r1[2]) / Ar_moleculardynamics::DT,
                            0.0);

                        atoms_[n].r1 = rtmp;
                    }
            });
            break;
        }
    }
    
    void Ar_moleculardynamics::periodic()
    {
        // consider the periodic boundary condination
        // セルの外側に出たら座標をセル内に戻す
        tbb::parallel_for(
            tbb::blocked_range<std::int32_t>(0, NumAtom_),
            [this](tbb::blocked_range<std::int32_t> const & range) {
            for (auto && n = range.begin(); n != range.end(); ++n) {
                if (X_[n] > periodiclen_) {
                    X_[n] -= periodiclen_;
                    atoms_[n].r1[0] -= periodiclen_;
                }
                else if (X_[n] < 0.0) {
                    X_[n] += periodiclen_;
                    atoms_[n].r1[0] += periodiclen_;
                }
                if (Y_[n] > periodiclen_) {
                    Y_[n] -= periodiclen_;
                    atoms_[n].r1[1] -= periodiclen_;
                }
                else if (Y_[n] < 0.0) {
                    Y_[n] += periodiclen_;
                    atoms_[n].r1[1] += periodiclen_;
                }
                if (Z_[n] > periodiclen_) {
                    Z_[n] -= periodiclen_;
                    atoms_[n].r1[2] -= periodiclen_;
                }
                else if (Z_[n] < 0.0) {
                    Z_[n] += periodiclen_;
                    atoms_[n].r1[2] += periodiclen_;
                }
            }
        });
    }

    void Ar_moleculardynamics::recalc()
    {
        t_ = 0.0;
        MD_iter_ = 1;

        MD_initPos();
        MD_initVel();
    }
    
    void Ar_moleculardynamics::setEnsemble(EnsembleType ensemble)
    {
        ensemble_ = ensemble;
        recalc();
    }

    void Ar_moleculardynamics::setNc(std::int32_t Nc)
    {
        Nc_ = Nc;
        X_.resize(Nc_ * Nc_ * Nc_ * 4);
        Y_.resize(Nc_ * Nc_ * Nc_ * 4);
        Z_.resize(Nc_ * Nc_ * Nc_ * 4);
        atoms_.resize(Nc_ * Nc_ * Nc_ * 4);

        ModLattice();
    }

    void Ar_moleculardynamics::setScale(double scale)
    {
        scale_ = scale;
        ModLattice();
    }

    void Ar_moleculardynamics::setTgiven(double Tgiven)
    {
        Tg_ = Tgiven * Ar_moleculardynamics::KB / Ar_moleculardynamics::YPSILON;
    }

    // #endregion publicメンバ関数

    // #region privateメンバ関数

    double Ar_moleculardynamics::DimensionlessToHartree(double e) const
    {
        return e * Ar_moleculardynamics::YPSILON / Ar_moleculardynamics::HARTREE;
    }

    void Ar_moleculardynamics::MD_initPos()
    {
        double sx, sy, sz;
        auto n = 0;

        for (auto i = 0; i < Nc_; i++) {
            for (auto j = 0; j < Nc_; j++) {
                for (auto k = 0; k < Nc_; k++) {
                    // 基本セルをコピーする
                    sx = static_cast<double>(i) * lat_;
                    sy = static_cast<double>(j) * lat_;
                    sz = static_cast<double>(k) * lat_;

                    // 基本セル内には4つの原子がある
                    X_[n] = sx;
                    Y_[n] = sy;
                    Z_[n] = sz;
                    n++;

                    X_[n] = 0.5 * lat_ + sx;
                    Y_[n] = 0.5 * lat_ + sy;
                    Z_[n] = sz;
                    n++;

                    X_[n] = sx;
                    Y_[n] = 0.5 * lat_ + sy;
                    Z_[n] = 0.5 * lat_ + sz;
                    n++;

                    X_[n] = 0.5 * lat_ + sx;
                    Y_[n] = sy;
                    Z_[n] = 0.5 * lat_ + sz;
                    n++;
                }
            }
        }

        NumAtom_ = n;

        // move the center of mass to the origin
        // 系の重心を座標系の原点とする
        sx = 0.0;
        sy = 0.0;
        sz = 0.0;

        for (auto n = 0; n < NumAtom_; n++) {
            sx += X_[n];
            sy += Y_[n];
            sz += Z_[n];
        }

        sx /= static_cast<double>(NumAtom_);
        sy /= static_cast<double>(NumAtom_);
        sz /= static_cast<double>(NumAtom_);

        for (auto n = 0; n < NumAtom_; n++) {
            X_[n] -= sx;
            Y_[n] -= sy;
            Z_[n] -= sz;
        }
    }

    void Ar_moleculardynamics::MD_initVel()
    {
        auto const v = std::sqrt(3.0 * Tg_);

        myrandom::MyRand mr(-1.0, 1.0);

        for (auto n = 0; n < NumAtom_; n++) {
            Eigen::Vector4d rnd(mr.myrand(), mr.myrand(), mr.myrand(), 0.0);
            auto const tmp = 1.0 / rnd.norm();
            rnd *= tmp;

            // 方向はランダムに与える
            atoms_[n].v = v * rnd;
        }

        Eigen::Vector4d s(0.0, 0.0, 0.0, 0.0);

        for (auto n = 0; n < NumAtom_; n++) {
            s += atoms_[n].v;
        }

        s /= static_cast<double>(NumAtom_);

        // 重心の並進運動を避けるために、速度の和がゼロになるように補正
        for (auto n = 0; n < NumAtom_; n++) {
            atoms_[n].v -= s;
        }
    }

    void Ar_moleculardynamics::ModLattice()
    {
        lat_ = std::pow(2.0, 2.0 / 3.0) * scale_;
        recalc();
        periodiclen_ = lat_ * static_cast<double>(Nc_);
    }

    // #endregion privateメンバ関数
}
