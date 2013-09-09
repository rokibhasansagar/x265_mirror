/*****************************************************************************
 * Copyright (C) 2013 x265 project
 *
 * Authors: Sumalatha Polureddy <sumalatha@multicorewareinc.com>
 *          Aarthi Priya Thirumalai <aarthi@multicorewareinc.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 * This program is also available under a commercial proprietary license.
 * For more information, contact us at licensing@multicorewareinc.com.
 *****************************************************************************/

#include "TLibCommon/TComPic.h"
#include "slicetype.h"
#include "ratecontrol.h"
#include <math.h>

using namespace x265;

#define BASE_FRAME_DURATION 0.04

/* Arbitrary limitations as a sanity check. */
#define MAX_FRAME_DURATION 1.00
#define MIN_FRAME_DURATION 0.01

#define CLIP_DURATION(f) Clip3(MIN_FRAME_DURATION, MAX_FRAME_DURATION, f)

/* The qscale - qp conversion is specified in the standards.
Approx qscale increases by 12%  with every qp increment */
static inline double qScale2qp(double qScale)
{
    return 12.0 + 6.0 * (double)X265_LOG2(qScale / 0.85);
}

static inline double qp2qScale(double qp)
{
    return 0.85 * pow(2.0, (qp - 12.0) / 6.0);
}

RateControl::RateControl(x265_param_t * param)
{
    keyFrameInterval = param->keyframeMax;
    frameThreads = param->frameNumThreads;
    framerate = param->frameRate;
    bframes = param->bframes;
    rateTolerance = param->rc.rateTolerance;
    bitrate = param->rc.bitrate * 1000;
    frameDuration = 1.0 / param->frameRate;
    rateControlMode = (RcMethod)(param->rc.rateControlMode);
    ncu = (int)((param->sourceHeight * param->sourceWidth) / pow((int)param->maxCUSize, 2.0));
    lastNonBPictType = -1;
    baseQp = param->rc.qp;
    qpm = qp = baseQp;

    // heuristics- encoder specific
    qCompress = param->rc.qCompress; // tweak and test for x265.
    ipFactor = param->rc.ipFactor;
    pbFactor = param->rc.pbFactor;
    totalBits = 0;
    shortTermCplxSum = 0;
    shortTermCplxCount = 0;
    if (rateControlMode == X265_RC_ABR)
    {
        //TODO : confirm this value. obtained it from x264 when crf is disabled , abr enabled.
        //h->param.rc.i_rc_method == X264_RC_CRF ? h->param.rc.f_rf_constant : 24  -- can be tweaked for x265.
#define ABR_INIT_QP (24  + QP_BD_OFFSET)
        accumPNorm = .01;
        accumPQp = (ABR_INIT_QP)*accumPNorm;
        /* estimated ratio that produces a reasonable QP for the first I-frame  */
        cplxrSum = .01 * pow(7.0e5, qCompress) * pow(2 *ncu, 0.5);
        wantedBitsWindow = bitrate * frameDuration;
        lastNonBPictType = I_SLICE;
    }
    ipOffset = 6.0 * (float)(X265_LOG2(param->rc.ipFactor));
    pbOffset = 6.0 * (float)(X265_LOG2(param->rc.pbFactor));
    for (int i = 0; i < 3; i++)
    {
        lastQScaleFor[i] = qp2qScale(ABR_INIT_QP);
        lmin[i] = qp2qScale(MIN_QP);
        lmax[i] = qp2qScale(MAX_QP);  
    }

    if (rateControlMode == X265_RC_CQP)
    {
        qpConstant[P_SLICE] = baseQp;
        qpConstant[I_SLICE] = Clip3(0, MAX_QP, (int)(baseQp - ipOffset + 0.5));
        qpConstant[B_SLICE] = Clip3(0, MAX_QP, (int)(baseQp + pbOffset + 0.5));
    }

    //qstep - value set as encoder specific.
    lstep = pow(2, param->rc.qpStep / 6.0);
    cbrDecay = 1.0;
}

void RateControl::rateControlStart(TComPic* pic, Lookahead *l, RateControlEntry* rce)
{
    curFrame = pic->getSlice();
    frameType = curFrame->getSliceType();

    switch (rateControlMode)
    {
    case X265_RC_ABR:
        {
            lastSatd = l->getEstimatedPictureCost(pic);
            double q = qScale2qp(rateEstimateQscale(rce));
            qp = Clip3(MIN_QP, MAX_QP, (int)(q + 0.5));
            rce->qpaRc = qpm = q;    
            rce->newQp = qp;
            accumPQpUpdate();
            break;
        }

    case X265_RC_CQP:
        qp = qpConstant[frameType];
        break;

    case X265_RC_CRF:
    default:
        assert(!"unimplemented");
        break;
    }

    if (frameType != B_SLICE)
        lastNonBPictType = frameType;

    /* set the final QP to slice structure */
    curFrame->setSliceQp(qp);
    curFrame->setSliceQpBase(qp);
}

void RateControl::accumPQpUpdate()
{
    accumPQp   *= .95;
    accumPNorm *= .95;
    accumPNorm += 1;
    if (frameType == I_SLICE)
        accumPQp += qpm + ipOffset;
    else
        accumPQp += qpm;
}

double RateControl::rateEstimateQscale(RateControlEntry *rce)
{
    double q;
    // ratecontrol_entry_t rce = UNINIT(rce);
    int pictType = frameType;

    if (pictType == B_SLICE)
    {
        /* B-frames don't have independent rate control, but rather get the
         * average QP of the two adjacent P-frames + an offset */

        int i0 = curFrame->getRefPic(REF_PIC_LIST_0, 0)->getSlice()->getSliceType() == I_SLICE;
        int i1 = curFrame->getRefPic(REF_PIC_LIST_1, 0)->getSlice()->getSliceType() == I_SLICE;
        int dt0 = abs(curFrame->getPOC() - curFrame->getRefPic(REF_PIC_LIST_0, 0)->getPOC());
        int dt1 = abs(curFrame->getPOC() - curFrame->getRefPic(REF_PIC_LIST_1, 0)->getPOC());

        //TODO:need to figure out this
        double q0 = curFrame->getRefPic(REF_PIC_LIST_0, 0)->getSlice()->getSliceQp();
        double q1 = curFrame->getRefPic(REF_PIC_LIST_1, 0)->getSlice()->getSliceQp();

        if (i0 && i1)
            q = (q0 + q1) / 2 + ipOffset;
        else if (i0)
            q = q1;
        else if (i1)
            q = q0;
        else
            q = (q0 * dt1 + q1 * dt0) / (dt0 + dt1);

        if (curFrame->isReferenced())
            q += pbOffset / 2;
        else
            q += pbOffset;
        
        double qScale = qp2qScale(q);

        lastQScaleFor[P_SLICE] = lastQScale = qScale/pbFactor;

        return qScale;
    }
    else
    {
        double abrBuffer = 0.9 * rateTolerance * bitrate;

        /* 1pass ABR */

        /* Calculate the quantizer which would have produced the desired
         * average bitrate if it had been applied to all frames so far.
         * Then modulate that quant based on the current frame's complexity
         * relative to the average complexity so far (using the 2pass RCEQ).
         * Then bias the quant up or down if total size so far was far from
         * the target.
         * Result: Depending on the value of rate_tolerance, there is a
         * tradeoff between quality and bitrate precision. But at large
         * tolerances, the bit distribution approaches that of 2pass. */

        double wantedBits, overflow = 1;
        rce->pCount = ncu;

        shortTermCplxSum *= 0.5;
        shortTermCplxCount *= 0.5;
        shortTermCplxSum += lastSatd / (CLIP_DURATION(frameDuration) / BASE_FRAME_DURATION);
        shortTermCplxCount++;
        rce->texBits = lastSatd;
        rce->blurredComplexity = shortTermCplxSum / shortTermCplxCount;
        rce->mvBits = 0;
        rce->pictType = pictType;
        //need to checked where it is initialized
        q = getQScale(rce, wantedBitsWindow / cplxrSum);

        /* ABR code can potentially be counterproductive in CBR, so just don't bother.
         * Don't run it if the frame complexity is zero either. */
        if (lastSatd)
        {
            double iFrameDone = curFrame->getPOC() + 1 - frameThreads;
            double timeDone = iFrameDone / framerate;
            wantedBits = timeDone * bitrate;
            if (wantedBits > 0)
            {
                abrBuffer *= X265_MAX(1, sqrt(timeDone));
                overflow = Clip3(.5, 2.0, 1.0 + (totalBits - wantedBits) / abrBuffer);
                q *= overflow;
            }
        }

        if (pictType == I_SLICE && keyFrameInterval > 1
            /* should test _next_ pict type, but that isn't decided yet */
            && lastNonBPictType != I_SLICE)
        {
            q = qp2qScale(accumPQp / accumPNorm);
            q /= fabs(ipFactor);
        }
        else if (curFrame->getPOC() > 0)
        {
            if (rateControlMode != X265_RC_CRF)
            {
                /* Asymmetric clipping, because symmetric would prevent
                 * overflow control in areas of rapidly oscillating complexity */
                double lqmin = lastQScaleFor[pictType] / lstep;
                double lqmax = lastQScaleFor[pictType] * lstep;
                if (overflow > 1.1 && curFrame->getPOC() > 3)
                    lqmax *= lstep;
                else if (overflow < 0.9)
                    lqmin /= lstep;

                q = Clip3(lqmin, lqmax, q);
            }
        }

        //FIXME use get_diff_limited_q() ?
        double lmin1 = lmin[pictType];
        double lmax1 = lmax[pictType];
        q = Clip3(lmin1, lmax1, q);
        lastQScaleFor[pictType] =
            lastQScale = q;

        if (curFrame->getPOC() == 0)
            lastQScaleFor[P_SLICE] = q * fabs(ipFactor);

        return q;
    }
}

/**
 * modify the bitrate curve from pass1 for one frame
 */
double RateControl::getQScale(RateControlEntry *rce, double rateFactor)
{
    double q;

    q = pow(rce->blurredComplexity, 1 - qCompress);

    // avoid NaN's in the rc_eq
    if (rce->texBits + rce->mvBits == 0)
        q = lastQScaleFor[rce->pictType];
    else
    {
        rce->lastRceq = q;
        q /= rateFactor;
        lastQScale = q;
    }
    return q;
}

/* After encoding one frame, save stats and update ratecontrol state */
int RateControl::rateControlEnd(int64_t bits, RateControlEntry* rce)
{
    if (rateControlMode == X265_RC_ABR)
    {
        if (frameType != B_SLICE)
            cplxrSum +=  1.1 *bits * qp2qScale(rce->qpaRc) / rce->lastRceq;
        else
        {
            /* Depends on the fact that B-frame's QP is an offset from the following P-frame's.
             * Not perfectly accurate with B-refs, but good enough. */
            cplxrSum += bits * qp2qScale(rce->qpaRc) / (rce->lastRceq * fabs(0.5 * pbFactor));
        }
        cplxrSum *= cbrDecay;
        wantedBitsWindow += frameDuration * bitrate;
        wantedBitsWindow *= cbrDecay;       
        rce = NULL;
    }
    totalBits += bits;
    return 0;
}