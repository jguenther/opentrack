/* Copyright (c) 2012-2013 Stanislaw Halik <sthalik@misaki.pl>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

/*
 * this file appeared originally in facetracknoir, was rewritten completely
 * following opentrack fork.
 *
 * originally written by Wim Vriend.
 */


#include "tracker.h"
#include <cmath>
#include <algorithm>

#if defined(_WIN32)
#   include <windows.h>
#endif

Tracker::Tracker(main_settings& s, Mappings &m, SelectedLibraries &libs) :
    s(s),
    m(m),
    centerp(false),
    enabledp(true),
    zero_(false),
    should_quit(false),
    libs(libs),
    r_b(dmat<3,3>::eye()),
    t_b {0,0,0}
{
}

Tracker::~Tracker()
{
    should_quit = true;
    wait();
}

double Tracker::map(double pos, bool invertp, Mapping& axis)
{
    bool altp = (pos < 0) == !invertp && axis.opts.altp;
    axis.curve.setTrackingActive( !altp );
    axis.curveAlt.setTrackingActive( altp );
    auto& fc = altp ? axis.curveAlt : axis.curve;
    return fc.getValue(pos) + axis.opts.zero;
}

// http://stackoverflow.com/a/18436193
static dmat<3, 1> rmat_to_euler(const dmat<3, 3>& R)
{
    static constexpr double pi = 3.141592653;
    const double up = 90 * pi / 180.;
    static constexpr double bound = 1. - 2e-4;
    if (R(0, 2) > bound)
    {
        double roll = atan(R(1, 0) / R(2, 0));
        return dmat<3, 1>({0., up, roll});
    }
    if (R(0, 2) < -bound)
    {
        double roll = atan(R(1, 0) / R(2, 0));
        return dmat<3, 1>({0., -up, roll});
    }
    double pitch = asin(-R(0, 2));
    double roll = atan2(R(1, 2), R(2, 2));
    double yaw = atan2(R(0, 1), R(0, 0));
    return dmat<3, 1>({yaw, pitch, roll});
}

// tait-bryan angles, not euler
static dmat<3, 3> euler_to_rmat(const double* input)
{
    static constexpr double pi = 3.141592653;
    auto H = input[0] * pi / 180;
    auto P = input[1] * pi / 180;
    auto B = input[2] * pi / 180;

    const auto c1 = cos(H);
    const auto s1 = sin(H);
    const auto c2 = cos(P);
    const auto s2 = sin(P);
    const auto c3 = cos(B);
    const auto s3 = sin(B);

    double foo[] = {
        // z
        c1 * c2,
        c1 * s2 * s3 - c3 * s1,
        s1 * s3 + c1 * c3 * s2,
        // y
        c2 * s1,
        c1 * c3 + s1 * s2 * s3,
        c3 * s1 * s2 - c1 * s3,
        // x
        -s2,
        c2 * s3,
        c2 * c3
    };

    return dmat<3, 3>(foo);
}

void Tracker::t_compensate(const dmat<3, 3>& rmat, const double* xyz, double* output, bool rz)
{
    // TY is really yaw axis. need swapping accordingly.
    dmat<3, 1> tvec({ xyz[2], -xyz[0], -xyz[1] });
    const dmat<3, 1> ret = rmat * tvec;
    if (!rz)
        output[2] = ret(0, 0);
    else
        output[2] = xyz[2];
    output[1] = -ret(2, 0);
    output[0] = -ret(1, 0);
}

void Tracker::logic()
{
    bool inverts[6] = {
        m(0).opts.invert,
        m(1).opts.invert,
        m(2).opts.invert,
        m(3).opts.invert,
        m(4).opts.invert,
        m(5).opts.invert,
    };
    
    static constexpr double pi = 3.141592653;
    static constexpr double r2d = 180. / pi;
    
    Pose value, raw;
    
    if (!zero_)
        for (int i = 0; i < 6; i++)
        {
            value(i) = newpose[i];
            raw(i) = newpose[i];
        }
    else
    {
        auto mat = rmat_to_euler(r_b);
        
        for (int i = 0; i < 3; i++)
        {
            raw(i+3) = value(i+3) = mat(i, 0) * r2d;
            raw(i) = value(i) = t_b[i];
        }
    }
    
    if (centerp)
    {
        centerp = false;
        for (int i = 0; i < 3; i++)
            t_b[i] = value(i);
        r_b = euler_to_rmat(&value[Yaw]);
    }
    
    {
        const dmat<3, 3> rmat = euler_to_rmat(&value[Yaw]);
        const dmat<3, 3> m_ = rmat * r_b.t();
        const dmat<3, 1> euler = rmat_to_euler(m_);
        for (int i = 0; i < 3; i++)
        {
            value(i) -= t_b[i];
            value(i+3) = euler(i, 0) * r2d;
        }
    }

    for (int i = 0; i < 6; i++)
        value(i) = map(value(i), inverts[i], m(i));
    
    {
        Pose tmp = value;
        
        if (libs.pFilter)
            libs.pFilter->filter(tmp, value);
    }

    // must invert early as euler_to_rmat's sensitive to sign change
    for (int i = 0; i < 6; i++)
        value[i] *= inverts[i] ? -1. : 1.;

    if (s.tcomp_p)
        t_compensate(euler_to_rmat(&value[Yaw]),
                     value,
                     value,
                     s.tcomp_tz);

    Pose output_pose_;

    for (int i = 0; i < 6; i++)
    {
        auto& axis = m(i);
        int k = axis.opts.src;
        if (k < 0 || k >= 6)
            output_pose_(i) = 0;
        else
            output_pose_(i) = value(i);
    }


    libs.pProtocol->pose(output_pose_);

    QMutexLocker foo(&mtx);
    output_pose = output_pose_;
    raw_6dof = raw;
}

void Tracker::run() {
    const int sleep_ms = 3;

#if defined(_WIN32)
    (void) timeBeginPeriod(1);
#endif

    while (!should_quit)
    {
        t.start();
        
        double tmp[6];
        libs.pTracker->data(tmp);
        
        if (enabledp)
            for (int i = 0; i < 6; i++)
                newpose[i] = tmp[i];
        
        logic();

        long q = sleep_ms * 1000L - t.elapsed()/1000L;
        usleep(std::max(1L, q));
    }

    {
        // do one last pass with origin pose
        for (int i = 0; i < 6; i++)
            newpose[i] = 0;
        logic();
        // filter may inhibit exact origin
        Pose p;
        libs.pProtocol->pose(p);
    }

#if defined(_WIN32)
    (void) timeEndPeriod(1);
#endif

    for (int i = 0; i < 6; i++)
    {
        m(i).curve.setTrackingActive(false);
        m(i).curveAlt.setTrackingActive(false);
    }
}

void Tracker::get_raw_and_mapped_poses(double* mapped, double* raw) const {
    QMutexLocker foo(&const_cast<Tracker&>(*this).mtx);
    for (int i = 0; i < 6; i++)
    {
        raw[i] = raw_6dof(i);
        mapped[i] = output_pose(i);
    }
}

