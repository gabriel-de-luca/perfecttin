/******************************************************/
/*                                                    */
/* adjelev.cpp - adjust elevations of points          */
/*                                                    */
/******************************************************/
/* Copyright 2019,2020 Pierre Abbat.
 * This file is part of PerfectTIN.
 *
 * PerfectTIN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * PerfectTIN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License and Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * and Lesser General Public License along with PerfectTIN. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <iostream>
#include <cmath>
#include <cassert>
#include "leastsquares.h"
#include "adjelev.h"
#include "angle.h"
#include "manysum.h"
#include "octagon.h"
#include "neighbor.h"
#include "threads.h"
using namespace std;

vector<adjustRecord> adjustmentLog;

void checkIntElev(vector<point *> corners)
/* Check whether any of the elevations is an integer.
 * Some elevations have been integers like 0 or 256.
 */
{
  int i;
  for (i=0;i<corners.size();i++)
    if (frac(corners[i]->elev())==0)
      cout<<"Point "<<net.revpoints[corners[i]]<<" elevation "<<corners[i]->elev()<<endl;
}

adjustRecord adjustElev(vector<triangle *> tri,vector<point *> pnt)
/* Adjusts the points by least squares to fit all the dots in the triangles.
 * The triangles should be all those that have at least one corner in
 * the list of points. Corners of triangles which are not in pnt will not
 * be adjusted.
 *
 * This function spends approximately equal amounts of time in areaCoord,
 * elevation, and linearLeastSquares (most of which is transmult). To spread
 * the work over threads when there are few triangles, therefore, I should
 * create tasks consisting of a block of about 65536 dots all from one triangle,
 * which are to be run through areaCoord, elevation, and transmult, and combine
 * the resulting square matrices and column matrices once all the tasks are done.
 * Any idle thread can do a task; this function will do whatever tasks are not
 * picked up by other threads.
 */
{
  int i,j,k,ndots;
  matrix a;
  double localLow=INFINITY,localHigh=-INFINITY,localClipLow,localClipHigh;
  double pointLow=INFINITY,point2Low=INFINITY,pointHigh=-INFINITY,point2High=-INFINITY;
  double pointClipLow,pointClipHigh;
  edge *ed;
  adjustRecord ret{true,0};
  vector<double> b,x,xsq,nextCornerElev;
  vector<point *> nearPoints; // includes points held still
  double oldelev;
  nearPoints=pointNeighbors(tri);
  for (ndots=i=0;i<tri.size();i++)
  {
    ndots+=tri[i]->dots.size();
    tri[i]->flatten(); // sets sarea, needed for areaCoord
    if (net.revtriangles.count(tri[i]))
      markBucketDirty(net.revtriangles[tri[i]]);
  }
  a.resize(ndots,pnt.size());
  /* Clip the adjusted elevations to the interval from the lowest dot to the
   * highest dot, and from the second lowest point to the second highest point,
   * including neighboring points not being adjusted, expanded by 3. The reason
   * it's the second highest/lowest point is that, if there's already a spike,
   * we want to clip it.
   */
  for (ndots=i=0;i<tri.size();i++)
    for (j=0;j<tri[i]->dots.size();j++,ndots++)
    {
      for (k=0;k<pnt.size();k++)
	a[ndots][k]=tri[i]->areaCoord(tri[i]->dots[j],pnt[k]);
      b.push_back(tri[i]->dots[j].elev()-tri[i]->elevation(tri[i]->dots[j]));
      if (tri[i]->dots[j].elev()>localHigh)
	localHigh=tri[i]->dots[j].elev();
      if (tri[i]->dots[j].elev()<localLow)
	localLow=tri[i]->dots[j].elev();
    }
  for (i=0;i<nearPoints.size();i++)
  {
    if (nearPoints[i]->elev()>pointHigh)
    {
      point2High=pointHigh;
      pointHigh=nearPoints[i]->elev();
    }
    else if (nearPoints[i]->elev()>point2High)
      point2High=nearPoints[i]->elev();
    if (nearPoints[i]->elev()<pointLow)
    {
      point2Low=pointLow;
      pointLow=nearPoints[i]->elev();
    }
    else if (nearPoints[i]->elev()<point2Low)
      point2Low=nearPoints[i]->elev();
  }
  pointClipHigh=1.125*point2High-0.125*point2Low;
  pointClipLow=1.125*point2Low-0.125*point2High;
  if (ndots<pnt.size())
    x=minimumNorm(a,b);
  else
    x=linearLeastSquares(a,b);
  assert(x.size()==pnt.size());
  localClipHigh=2*localHigh-localLow;
  localClipLow=2*localLow-localHigh;
  if (localClipHigh>clipHigh)
    localClipHigh=clipHigh;
  if (localClipLow<clipLow)
    localClipLow=clipLow;
  if (localClipHigh>pointClipHigh && nearPoints.size()>pnt.size())
    localClipHigh=pointClipHigh;
  if (localClipLow<pointClipLow && nearPoints.size()>pnt.size())
    localClipLow=pointClipLow;
  for (k=0;k<pnt.size();k++)
  {
    if (std::isfinite(x[k]) && fabs(x[k])<16384 && ndots)
    /* Ground elevation is in the range [-11000,8850]. Roundoff error in a
     * singular matrix produces numbers around 1e18. Any number too big
     * means that the matrix is singular, even if the number is finite.
     */
    {
      //if (fabs(x[k])>2000)
	//cout<<"Big adjustment!\n";
      oldelev=pnt[k]->elev();
      pnt[k]->raise(x[k]);
      if (pnt[k]->elev()>localClipHigh)
	pnt[k]->raise(localClipHigh-pnt[k]->elev());
      if (pnt[k]->elev()<localClipLow)
	pnt[k]->raise(localClipLow-pnt[k]->elev());
    }
    else
    {
      oldelev=pnt[k]->elev();
      nextCornerElev.clear();
      for (ed=pnt[k]->line,j=0;ed!=pnt[k]->line || j==0;ed=ed->next(pnt[k]),j++)
      nextCornerElev.push_back(ed->otherend(pnt[k])->elev());
      pnt[k]->raise(pairwisesum(nextCornerElev)/nextCornerElev.size()-pnt[k]->elev());
      ret.validMatrix=false;
    }
    xsq.push_back(sqr(pnt[k]->elev()-oldelev));
  }
  ret.msAdjustment=pairwisesum(xsq)/xsq.size();
  for (i=0;i<tri.size();i++)
    if (net.revtriangles.count(tri[i]))
      markBucketDirty(net.revtriangles[tri[i]]);
  //if (singular)
    //cout<<"Matrix in least squares is singular"<<endl;
  return ret;
}

void logAdjustment(adjustRecord rec)
{
  adjLog.lock();
  adjustmentLog.push_back(rec);
  adjLog.unlock();
}

double rmsAdjustment()
// Returns the square root of the average of recent adjustments.
{
  int nRecent;
  vector<double> xsq;
  int i,sz;
  adjLog.lock();
  sz=adjustmentLog.size();
  adjLog.unlock();
  nRecent=lrint(sqrt(sz));
  for (i=sz-1;i>=0 && xsq.size()<nRecent;i--)
    if (std::isfinite(adjustmentLog[i].msAdjustment))
      xsq.push_back(adjustmentLog[i].msAdjustment);
  if (std::isnan(sqrt(pairwisesum(xsq)/xsq.size())))
    i++;
  return sqrt(pairwisesum(xsq)/xsq.size());
}

void adjustLooseCorners(double tolerance)
/* A loose corner is a point which is not a corner of any triangle with dots in it.
 * Loose corners are not adjusted by adjustElev; their elevation can be 0
 * or wild values left over from when they were the far corners of triangles
 * with only a few dots.
 */
{
  vector<point *> looseCorners,oneCorner;
  vector<triangle *> nextTriangles;
  edge *ed;
  vector<double> nextCornerElev;
  int i,j,ndots;
  oneCorner.resize(1);
  for (i=1;i<=net.points.size();i++)
  {
    oneCorner[0]=&net.points[i];
    nextTriangles=triangleNeighbors(oneCorner);
    for (ndots=j=0;j<nextTriangles.size();j++)
      ndots+=nextTriangles[j]->dots.size();
    if (ndots==0)
      looseCorners.push_back(oneCorner[0]);
  }
  for (i=0;i<looseCorners.size();i++)
  {
    nextCornerElev.clear();
    for (ed=looseCorners[i]->line,j=0;ed!=looseCorners[i]->line || j==0;ed=ed->next(looseCorners[i]),j++)
      nextCornerElev.push_back(ed->otherend(looseCorners[i])->elev());
    looseCorners[i]->raise(pairwisesum(nextCornerElev)/nextCornerElev.size()-looseCorners[i]->elev());
  }
}

void clearLog()
{
  adjLog.lock();
  adjustmentLog.clear();
  adjLog.unlock();
}
