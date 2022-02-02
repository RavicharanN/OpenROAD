/* Authors: Lutong Wang and Bangqi Xu */
/*
 * Copyright (c) 2019, The Regents of the University of California
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the University nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <iostream>
#include <sstream>

#include "FlexRP.h"
#include "db/gcObj/gcNet.h"
#include "db/gcObj/gcPin.h"
#include "db/gcObj/gcShape.h"
#include "db/infra/frTime.h"
#include "frProfileTask.h"
#include "gc/FlexGC.h"
#include "odb/db.h"
using namespace std;
using namespace fr;

void FlexRP::prep()
{
  ProfileTask profile("RP:prep");
  prep_via2viaForbiddenLen();
  prep_viaForbiddenTurnLen();
  prep_viaForbiddenPlanarLen();
  prep_lineForbiddenLen();
  prep_eolForbiddenLen();
  prep_cutSpcTbl();
  prep_viaForbiddenThrough();
  for (auto& ndr : tech_->nonDefaultRules) {
    prep_via2viaForbiddenLen(ndr.get());
    prep_viaForbiddenTurnLen(ndr.get());
  }
  prep_minStepViasCheck();
}

void FlexRP::prep_minStepViasCheck()
{
  auto bottomLayerNum = getDesign()->getTech()->getBottomLayerNum();
  auto topLayerNum = getDesign()->getTech()->getTopLayerNum();
  for (auto lNum = bottomLayerNum; lNum <= topLayerNum; lNum++) {
    frLayer* layer = tech_->getLayer(lNum);
    if (layer->getType() != dbTechLayerType::ROUTING) {
      continue;
    }
    if (lNum - 2 < bottomLayerNum || lNum + 2 > topLayerNum)
      continue;
    frViaDef* downVia
        = getDesign()->getTech()->getLayer(lNum - 1)->getDefaultViaDef();
    frViaDef* upVia
        = getDesign()->getTech()->getLayer(lNum + 1)->getDefaultViaDef();
    if (!downVia || !upVia)
      continue;
    auto minStepCons = layer->getMinStepConstraint();
    if (!minStepCons)
      continue;

    Rect upViaBox = upVia->getLayer1ShapeBox();
    Rect downViaBox = downVia->getLayer2ShapeBox();
    gtl::rectangle_data<frCoord> upViaRect(
        upViaBox.xMin(), upViaBox.yMin(), upViaBox.xMax(), upViaBox.yMax());
    gtl::rectangle_data<frCoord> downViaRect(downViaBox.xMin(),
                                             downViaBox.yMin(),
                                             downViaBox.xMax(),
                                             downViaBox.yMax());

    // joining the two via rects in one polygon
    gtl::polygon_90_set_data<frCoord> set;
    using namespace boost::polygon::operators;
    set += upViaRect;
    set += downViaRect;
    std::vector<gtl::polygon_90_with_holes_data<frCoord>> polys;
    set.get_polygons(polys);
    if (polys.size() != 1)
      continue;

    gtl::polygon_90_with_holes_data<frCoord> poly = *polys.begin();
    gcNet* testNet = new gcNet(0);
    gcPin* testPin = new gcPin(poly, lNum, testNet);
    testPin->setNet(testNet);

    bool first = true;
    std::vector<std::unique_ptr<gcSegment>> tmpEdges;
    gtl::point_data<frCoord> prev;
    for (gtl::point_data<frCoord> cur : poly) {
      if (first) {
        prev = cur;
        first = false;
      } else {
        auto edge = make_unique<gcSegment>();
        edge->setLayerNum(lNum);
        edge->addToPin(testPin);
        edge->addToNet(testNet);
        edge->setSegment(prev, cur);
        if (!tmpEdges.empty()) {
          edge->setPrevEdge(tmpEdges.back().get());
          tmpEdges.back()->setNextEdge(edge.get());
        }
        tmpEdges.push_back(std::move(edge));
        prev = cur;
      }
    }
    // last edge
    auto edge = make_unique<gcSegment>();
    edge->setLayerNum(lNum);
    edge->addToPin(testPin);
    edge->addToNet(testNet);
    edge->setSegment(prev, *poly.begin());
    edge->setPrevEdge(tmpEdges.back().get());
    tmpEdges.back()->setNextEdge(edge.get());
    // set first edge
    tmpEdges.front()->setPrevEdge(edge.get());
    edge->setNextEdge(tmpEdges.front().get());
    tmpEdges.push_back(std::move(edge));
    // add to polygon edges
    testPin->addPolygonEdges(tmpEdges);
    // check gc minstep violations
    FlexGCWorker worker(tech_, logger_);
    worker.checkMinStep(testPin);
    auto& markers = worker.getMarkers();
    if (!markers.empty()) {
      tech_->setVia2ViaMinStep(true);
      layer->setHasVia2ViaMinStepViol(true);
    }
  }
}

void FlexRP::prep_viaForbiddenThrough()
{
  auto bottomLayerNum = getDesign()->getTech()->getBottomLayerNum();
  auto topLayerNum = getDesign()->getTech()->getTopLayerNum();

  int i = 0;
  for (auto lNum = bottomLayerNum; lNum <= topLayerNum; lNum++) {
    if (tech_->getLayer(lNum)->getType() != dbTechLayerType::ROUTING) {
      continue;
    }
    frViaDef* downVia = nullptr;
    frViaDef* upVia = nullptr;
    if (getDesign()->getTech()->getBottomLayerNum() <= lNum - 1) {
      downVia = getDesign()->getTech()->getLayer(lNum - 1)->getDefaultViaDef();
    }
    if (getDesign()->getTech()->getTopLayerNum() >= lNum + 1) {
      upVia = getDesign()->getTech()->getLayer(lNum + 1)->getDefaultViaDef();
    }
    prep_viaForbiddenThrough_helper(lNum, i, 0, downVia, true);
    prep_viaForbiddenThrough_helper(lNum, i, 1, downVia, false);
    prep_viaForbiddenThrough_helper(lNum, i, 2, upVia, true);
    prep_viaForbiddenThrough_helper(lNum, i, 3, upVia, false);
    i++;
  }
}

void FlexRP::prep_viaForbiddenThrough_helper(const frLayerNum& lNum,
                                             const int& tableLayerIdx,
                                             const int& tableEntryIdx,
                                             frViaDef* viaDef,
                                             bool isCurrDirX)
{
  bool isThroughAllowed = true;

  if (prep_viaForbiddenThrough_minStep(lNum, viaDef, isCurrDirX)) {
    isThroughAllowed = false;
  }

  tech_->viaForbiddenThrough[tableLayerIdx][tableEntryIdx] = !isThroughAllowed;
}

bool FlexRP::prep_viaForbiddenThrough_minStep(const frLayerNum& lNum,
                                              frViaDef* viaDef,
                                              bool isCurrDirX)
{
  if (!viaDef) {
    return false;
  }

  if (lNum == 10 && viaDef->getName() == "CK_23_28_0_26_VH_CK") {
    return true;
  } else {
    return false;
  }
}

/**
 * Calculate the min end of line width in the eol rules.
 * If no eol rules found, consider default width.
 * @param[in] layer the layer we are calculating the min eol width for.
 * @return the min eol width from the eol rules -1 or the default width if no
 * eol rules found.
 */
inline frCoord getMinEol(frLayer* layer, frCoord minWidth)
{
  frCoord eol = INT_MAX;
  if (layer->hasEolSpacing())
    for (auto con : layer->getEolSpacing())
      eol = std::min(eol, con->getEolWidth());
  for (auto con : layer->getLef58SpacingEndOfLineConstraints())
    eol = std::min(eol, con->getEolWidth());
  for (auto con : layer->getLef58EolKeepOutConstraints())
    eol = std::min(eol, con->getEolWidth());
  for (auto con : layer->getLef58EolExtConstraints())
    eol = std::min(eol, con->getExtensionTable().getMinRow());
  if (eol == INT_MAX)
    eol = minWidth;
  else
    eol = std::max(eol - 1, (frCoord) minWidth);
  return eol;
}

void FlexRP::prep_eolForbiddenLen_helper(frLayer* layer,
                                         const frCoord eolWidth,
                                         frCoord& eolSpace,
                                         frCoord& eolWithin)
{
  if (layer->hasEolSpacing()) {
    for (auto con : layer->getEolSpacing()) {
      if (eolWidth < con->getEolWidth()) {
        eolSpace = std::max(eolSpace, con->getMinSpacing());
        eolWithin = std::max(eolWithin, con->getEolWithin());
      }
    }
  }
  for (auto con : layer->getLef58SpacingEndOfLineConstraints()) {
    if (eolWidth < con->getEolWidth()) {
      eolSpace = std::max(eolSpace, con->getEolSpace());
      if (con->hasWithinConstraint()) {
        auto withinCon = con->getWithinConstraint();
        eolWithin = std::max(eolWithin, withinCon->getEolWithin());
        if (withinCon->hasEndToEndConstraint()) {
          auto endToEndCon = withinCon->getEndToEndConstraint();
          eolSpace = std::max(eolSpace, endToEndCon->getEndToEndSpace());
        }
      }
    }
  }
  for (auto con : layer->getLef58EolKeepOutConstraints()) {
    if (eolWidth < con->getEolWidth()) {
      eolSpace = std::max(eolSpace, con->getForwardExt());
      eolWithin = std::max(eolWithin, con->getSideExt());
    }
  }
  for (auto con : layer->getLef58EolExtConstraints()) {
    if (eolWidth < con->getExtensionTable().getMaxRow()) {
      eolSpace = std::max(
          eolSpace,
          con->getExtensionTable().find(eolWidth) + con->getMinSpacing());
    }
  }
}

void FlexRP::prep_eolForbiddenLen()
{
  auto bottomLayerNum = getDesign()->getTech()->getBottomLayerNum();
  auto topLayerNum = getDesign()->getTech()->getTopLayerNum();

  for (auto lNum = bottomLayerNum; lNum <= topLayerNum; lNum++) {
    auto layer = tech_->getLayer(lNum);
    if (layer->getType() != dbTechLayerType::ROUTING) {
      continue;
    }
    frCoord eolSpace = 0;
    frCoord eolWithin = 0;
    frCoord eolWidth = getMinEol(layer, layer->getWidth());
    prep_eolForbiddenLen_helper(layer, eolWidth, eolSpace, eolWithin);
    layer->setDrEolSpacingConstraint(eolWidth, eolSpace, eolWithin);
  }
  for (auto& ndr : tech_->getNondefaultRules()) {
    for (auto lNum = bottomLayerNum; lNum <= topLayerNum; lNum++) {
      auto layer = tech_->getLayer(lNum);
      if (layer->getType() != dbTechLayerType::ROUTING) {
        continue;
      }
      auto z = lNum / 2 - 1;
      frCoord minWidth = ndr->getWidth(z);
      if (minWidth == 0)
        continue;
      frCoord eolSpace = 0;
      frCoord eolWithin = 0;
      frCoord eolWidth = getMinEol(layer, minWidth);
      prep_eolForbiddenLen_helper(layer, eolWidth, eolSpace, eolWithin);
      drEolSpacingConstraint drCon(eolWidth, eolSpace, eolWithin);
      ndr->setDrEolConstraint(drCon, z);
    }
  }
}

void FlexRP::prep_cutSpcTbl()
{
  auto bottomLayerNum = getDesign()->getTech()->getBottomLayerNum();
  auto topLayerNum = getDesign()->getTech()->getTopLayerNum();

  for (auto lNum = bottomLayerNum; lNum <= topLayerNum; lNum++) {
    auto layer = tech_->getLayer(lNum);
    if (layer->getType() == odb::dbTechLayerType::CUT) {
      auto viaDef = layer->getDefaultViaDef();
      if (viaDef == nullptr)
        continue;
      frVia via(viaDef);
      Rect tmpBx;
      via.getCutBBox(tmpBx);
      frString cutClass1 = "";
      auto cutClassIdx1 = layer->getCutClassIdx(tmpBx.minDXDY(), tmpBx.maxDXDY());
      if (cutClassIdx1 >= 0)
        cutClass1 = layer->getCutClass(cutClassIdx1)->getName();
      if (layer->hasLef58DiffNetCutSpcTblConstraint()) {
        auto con = layer->getLef58DiffNetCutSpcTblConstraint();
        auto dbRule = con->getODBRule();
        con->setDefaultSpacing(
            {dbRule->getMaxSpacing(
                 cutClass1,
                 cutClass1,
                 odb::dbTechLayerCutSpacingTableDefRule::FIRST),
             dbRule->getMaxSpacing(
                 cutClass1,
                 cutClass1,
                 odb::dbTechLayerCutSpacingTableDefRule::SECOND)});
        con->setDefaultCenterToCenter(
            dbRule->isCenterToCenter(cutClass1, cutClass1));
        con->setDefaultCenterAndEdge(
            dbRule->isCenterAndEdge(cutClass1, cutClass1));
      }
      if (layer->hasLef58DefaultInterCutSpcTblConstraint()) {
        auto con = layer->getLef58DefaultInterCutSpcTblConstraint();
        auto dbRule = con->getODBRule();
        auto secondLayer = getDesign()->getTech()->getLayer(
            dbRule->getSecondLayer()->getName());
        viaDef = secondLayer->getDefaultViaDef();
        if (viaDef != nullptr) {
          via.getCutBBox(tmpBx);
          frString cutClass2 = "";
          auto cutClassIdx2
              = secondLayer->getCutClassIdx(tmpBx.minDXDY(), tmpBx.maxDXDY());
          if (cutClassIdx2 >= 0)
            cutClass2 = secondLayer->getCutClass(cutClassIdx2)->getName();
          con->setDefaultSpacing(
              {dbRule->getMaxSpacing(
                   cutClass1,
                   cutClass2,
                   odb::dbTechLayerCutSpacingTableDefRule::FIRST),
               dbRule->getMaxSpacing(
                   cutClass1,
                   cutClass2,
                   odb::dbTechLayerCutSpacingTableDefRule::SECOND)});
          con->setDefaultCenterToCenter(
              dbRule->isCenterToCenter(cutClass1, cutClass2));
          con->setDefaultCenterAndEdge(
              dbRule->isCenterAndEdge(cutClass1, cutClass2));
        }
      }
    }
  }
}

void FlexRP::prep_lineForbiddenLen()
{
  auto bottomLayerNum = getDesign()->getTech()->getBottomLayerNum();
  auto topLayerNum = getDesign()->getTech()->getTopLayerNum();

  int i = 0;
  for (auto lNum = bottomLayerNum; lNum <= topLayerNum; lNum++) {
    if (tech_->getLayer(lNum)->getType() != dbTechLayerType::ROUTING) {
      continue;
    }
    prep_lineForbiddenLen_helper(lNum, i, 0, true, true);
    prep_lineForbiddenLen_helper(lNum, i, 1, true, false);
    prep_lineForbiddenLen_helper(lNum, i, 2, false, true);
    prep_lineForbiddenLen_helper(lNum, i, 3, false, false);

    i++;
  }
}

void FlexRP::prep_lineForbiddenLen_helper(const frLayerNum& lNum,
                                          const int& tableLayerIdx,
                                          const int& tableEntryIdx,
                                          const bool isZShape,
                                          const bool isCurrDirX)
{
  ForbiddenRanges forbiddenRanges;
  prep_lineForbiddenLen_minSpc(lNum, isZShape, isCurrDirX, forbiddenRanges);

  // merge ranges
  boost::icl::interval_set<frCoord> forbiddenIntvSet;
  for (auto& range : forbiddenRanges) {
    forbiddenIntvSet.insert(
        boost::icl::interval<frCoord>::closed(range.first, range.second));
  }

  forbiddenRanges.clear();
  for (auto it = forbiddenIntvSet.begin(); it != forbiddenIntvSet.end(); it++) {
    auto beginCoord = it->lower();
    auto endCoord = it->upper();
    forbiddenRanges.push_back(make_pair(beginCoord + 1, endCoord - 1));
  }

  tech_->line2LineForbiddenLen[tableLayerIdx][tableEntryIdx] = forbiddenRanges;
}

void FlexRP::prep_lineForbiddenLen_minSpc(
    const frLayerNum& lNum,
    const bool isZShape,
    const bool isCurrDirX,
    ForbiddenRanges& forbiddenRanges)
{
  frCoord defaultWidth = tech_->getLayer(lNum)->getWidth();

  frCoord minNonOverlapDist = defaultWidth;

  frCoord minReqDist = INT_MIN;
  frCoord prl = isZShape ? defaultWidth : tech_->getLayer(lNum)->getPitch();
  auto con = tech_->getLayer(lNum)->getMinSpacing();
  if (con) {
    if (con->typeId() == frConstraintTypeEnum::frcSpacingConstraint) {
      minReqDist = static_cast<frSpacingConstraint*>(con)->getMinSpacing();
    } else if (con->typeId()
               == frConstraintTypeEnum::frcSpacingTablePrlConstraint) {
      minReqDist = static_cast<frSpacingTablePrlConstraint*>(con)->find(
          defaultWidth, prl);
    } else if (con->typeId()
               == frConstraintTypeEnum::frcSpacingTableTwConstraint) {
      minReqDist = static_cast<frSpacingTableTwConstraint*>(con)->find(
          defaultWidth, defaultWidth, prl);
    }
  }
  if (minReqDist != INT_MIN) {
    minReqDist += minNonOverlapDist;
  }
  if (minReqDist != INT_MIN) {
    forbiddenRanges.push_back(make_pair(minNonOverlapDist, minReqDist));
  }
}

void FlexRP::prep_viaForbiddenPlanarLen()
{
  auto bottomLayerNum = getDesign()->getTech()->getBottomLayerNum();
  auto topLayerNum = getDesign()->getTech()->getTopLayerNum();

  int i = 0;
  for (auto lNum = bottomLayerNum; lNum <= topLayerNum; lNum++) {
    if (getDesign()->getTech()->getLayer(lNum)->getType()
        != dbTechLayerType::ROUTING) {
      continue;
    }
    frViaDef* downVia = nullptr;
    frViaDef* upVia = nullptr;
    if (getDesign()->getTech()->getBottomLayerNum() <= lNum - 1) {
      downVia = getDesign()->getTech()->getLayer(lNum - 1)->getDefaultViaDef();
    }
    if (getDesign()->getTech()->getTopLayerNum() >= lNum + 1) {
      upVia = getDesign()->getTech()->getLayer(lNum + 1)->getDefaultViaDef();
    }
    prep_viaForbiddenPlanarLen_helper(lNum, i, 0, downVia, true);
    prep_viaForbiddenPlanarLen_helper(lNum, i, 1, downVia, false);
    prep_viaForbiddenPlanarLen_helper(lNum, i, 2, upVia, true);
    prep_viaForbiddenPlanarLen_helper(lNum, i, 3, upVia, false);

    i++;
  }
}

void FlexRP::prep_viaForbiddenPlanarLen_helper(const frLayerNum& lNum,
                                               const int& tableLayerIdx,
                                               const int& tableEntryIdx,
                                               frViaDef* viaDef,
                                               bool isCurrDirX)
{
  if (!viaDef) {
    return;
  }

  ForbiddenRanges forbiddenRanges;
  prep_viaForbiddenPlanarLen_minStep(lNum, viaDef, isCurrDirX, forbiddenRanges);

  // merge forbidden ranges
  boost::icl::interval_set<frCoord> forbiddenIntvSet;
  for (auto& range : forbiddenRanges) {
    forbiddenIntvSet.insert(
        boost::icl::interval<frCoord>::closed(range.first, range.second));
  }

  forbiddenRanges.clear();
  for (auto it = forbiddenIntvSet.begin(); it != forbiddenIntvSet.end(); it++) {
    auto beginCoord = it->lower();
    auto endCoord = it->upper();
    forbiddenRanges.push_back(make_pair(beginCoord + 1, endCoord - 1));
  }

  tech_->viaForbiddenPlanarLen[tableLayerIdx][tableEntryIdx] = forbiddenRanges;
}

void FlexRP::prep_viaForbiddenPlanarLen_minStep(
    const frLayerNum& lNum,
    frViaDef* viaDef,
    bool isCurrDirX,
    ForbiddenRanges& forbiddenRanges)
{
  return;
}

void FlexRP::prep_viaForbiddenTurnLen(frNonDefaultRule* ndr)
{
  auto bottomLayerNum = getDesign()->getTech()->getBottomLayerNum();
  auto topLayerNum = getDesign()->getTech()->getTopLayerNum();
  int bottom = BOTTOM_ROUTING_LAYER;
  int i = 0;
  for (auto lNum = bottomLayerNum; lNum <= topLayerNum; lNum++) {
    if (getDesign()->getTech()->getLayer(lNum)->getType()
        != dbTechLayerType::ROUTING) {
      continue;
    }
    frViaDef* downVia = nullptr;
    frViaDef* upVia = nullptr;
    if (ndr && bottom < lNum && ndr->getPrefVia((lNum - 2) / 2 - 1))
      downVia = ndr->getPrefVia((lNum - 2) / 2 - 1);
    else if (getDesign()->getTech()->getBottomLayerNum() <= lNum - 1)
      downVia = getDesign()->getTech()->getLayer(lNum - 1)->getDefaultViaDef();

    if (getDesign()->getTech()->getTopLayerNum() >= lNum + 1) {
      if (ndr && ndr->getPrefVia((lNum + 2) / 2 - 1))
        upVia = ndr->getPrefVia((lNum + 2) / 2 - 1);
      else
        upVia = getDesign()->getTech()->getLayer(lNum + 1)->getDefaultViaDef();
    }
    prep_viaForbiddenTurnLen_helper(lNum, i, 0, downVia, true, ndr);
    prep_viaForbiddenTurnLen_helper(lNum, i, 1, downVia, false, ndr);
    prep_viaForbiddenTurnLen_helper(lNum, i, 2, upVia, true, ndr);
    prep_viaForbiddenTurnLen_helper(lNum, i, 3, upVia, false, ndr);

    i++;
  }
}

// forbidden turn length range from via
void FlexRP::prep_viaForbiddenTurnLen_helper(const frLayerNum& lNum,
                                             const int& tableLayerIdx,
                                             const int& tableEntryIdx,
                                             frViaDef* viaDef,
                                             bool isCurrDirX,
                                             frNonDefaultRule* ndr)
{
  if (!viaDef) {
    return;
  }

  auto tech = getDesign()->getTech();

  ForbiddenRanges forbiddenRanges;
  prep_viaForbiddenTurnLen_minSpc(
      lNum, viaDef, isCurrDirX, forbiddenRanges, ndr);

  // merge forbidden ranges
  boost::icl::interval_set<frCoord> forbiddenIntvSet;
  for (auto& range : forbiddenRanges) {
    forbiddenIntvSet.insert(
        boost::icl::interval<frCoord>::closed(range.first, range.second));
  }

  forbiddenRanges.clear();
  for (auto it = forbiddenIntvSet.begin(); it != forbiddenIntvSet.end(); it++) {
    auto beginCoord = it->lower();
    auto endCoord = it->upper();
    forbiddenRanges.push_back(make_pair(beginCoord + 1, endCoord - 1));
  }
  if (ndr)
    ndr->viaForbiddenTurnLen[tableLayerIdx][tableEntryIdx] = forbiddenRanges;
  else
    tech->viaForbiddenTurnLen[tableLayerIdx][tableEntryIdx] = forbiddenRanges;
}

void FlexRP::prep_viaForbiddenTurnLen_minSpc(
    const frLayerNum& lNum,
    frViaDef* viaDef,
    bool isCurrDirX,
    ForbiddenRanges& forbiddenRanges,
    frNonDefaultRule* ndr)
{
  if (!viaDef) {
    return;
  }

  frCoord defaultWidth = tech_->getLayer(lNum)->getWidth();
  frCoord width = defaultWidth;
  if (ndr)
    width = max(width, ndr->getWidth(lNum / 2 - 1));

  frVia via1(viaDef);
  Rect viaBox1;
  if (viaDef->getLayer1Num() == lNum) {
    via1.getLayer1BBox(viaBox1);
  } else {
    via1.getLayer2BBox(viaBox1);
  }
  int width1 = viaBox1.minDXDY();
  bool isVia1Fat = isCurrDirX
                       ? (viaBox1.yMax() - viaBox1.yMin() > defaultWidth)
                       : (viaBox1.xMax() - viaBox1.xMin() > defaultWidth);
  auto prl1 = isCurrDirX ? (viaBox1.yMax() - viaBox1.yMin())
                         : (viaBox1.xMax() - viaBox1.xMin());

  frCoord minNonOverlapDist
      = isCurrDirX ? ((viaBox1.xMax() - viaBox1.xMin() + width) / 2)
                   : ((viaBox1.yMax() - viaBox1.yMin() + width) / 2);
  frCoord minReqDist = INT_MIN;
  if (isVia1Fat || ndr) {
    auto con = getDesign()->getTech()->getLayer(lNum)->getMinSpacing();
    if (con) {
      if (con->typeId() == frConstraintTypeEnum::frcSpacingConstraint) {
        minReqDist = static_cast<frSpacingConstraint*>(con)->getMinSpacing();
      } else if (con->typeId()
                 == frConstraintTypeEnum::frcSpacingTablePrlConstraint) {
        minReqDist = static_cast<frSpacingTablePrlConstraint*>(con)->find(
            max(width1, width), prl1);
      } else if (con->typeId()
                 == frConstraintTypeEnum::frcSpacingTableTwConstraint) {
        minReqDist = static_cast<frSpacingTableTwConstraint*>(con)->find(
            width1, width, prl1);
      }
    }
    if (ndr)
      minReqDist = max(minReqDist, ndr->getSpacing(lNum / 2 - 1));
    if (minReqDist != INT_MIN) {
      minReqDist += minNonOverlapDist;
    }
  }
  if (minReqDist != INT_MIN) {
    forbiddenRanges.push_back(make_pair(minNonOverlapDist, minReqDist));
  }
}

void FlexRP::prep_via2viaForbiddenLen(frNonDefaultRule* ndr)
{
  auto bottomLayerNum = getDesign()->getTech()->getBottomLayerNum();
  auto topLayerNum = getDesign()->getTech()->getTopLayerNum();
  int bottom = BOTTOM_ROUTING_LAYER;
  int i = 0;
  for (auto lNum = bottomLayerNum; lNum <= topLayerNum; lNum++) {
    if (getDesign()->getTech()->getLayer(lNum)->getType()
        != dbTechLayerType::ROUTING) {
      continue;
    }
    frViaDef* downVia = nullptr;
    frViaDef* upVia = nullptr;
    if (ndr && bottom < lNum && ndr->getPrefVia((lNum - 2) / 2 - 1))
      downVia = ndr->getPrefVia((lNum - 2) / 2 - 1);
    else if (getDesign()->getTech()->getBottomLayerNum() <= lNum - 1)
      downVia = getDesign()->getTech()->getLayer(lNum - 1)->getDefaultViaDef();
    if (getDesign()->getTech()->getTopLayerNum() >= lNum + 1) {
      if (ndr && ndr->getPrefVia((lNum + 2) / 2 - 1))
        upVia = ndr->getPrefVia((lNum + 2) / 2 - 1);
      else
        upVia = getDesign()->getTech()->getLayer(lNum + 1)->getDefaultViaDef();
    }
    prep_via2viaForbiddenLen_helper(lNum, i, 0, downVia, downVia, true, ndr);
    prep_via2viaForbiddenLen_helper(lNum, i, 1, downVia, downVia, false, ndr);
    prep_via2viaForbiddenLen_helper(lNum, i, 2, downVia, upVia, true, ndr);
    prep_via2viaForbiddenLen_helper(lNum, i, 3, downVia, upVia, false, ndr);
    prep_via2viaForbiddenLen_helper(lNum, i, 4, upVia, downVia, true, ndr);
    prep_via2viaForbiddenLen_helper(lNum, i, 5, upVia, downVia, false, ndr);
    prep_via2viaForbiddenLen_helper(lNum, i, 6, upVia, upVia, true, ndr);
    prep_via2viaForbiddenLen_helper(lNum, i, 7, upVia, upVia, false, ndr);

    i++;
  }
}

// assume via is always centered at (0,0) for shapes on all three layers
// currently only support single-cut via and min-square cut class
void FlexRP::prep_via2viaForbiddenLen_helper(const frLayerNum& lNum,
                                             const int& tableLayerIdx,
                                             const int& tableEntryIdx,
                                             frViaDef* viaDef1,
                                             frViaDef* viaDef2,
                                             bool isHorizontal,
                                             frNonDefaultRule* ndr)
{
  auto tech = getDesign()->getTech();
  // non-shape-based rule
  ForbiddenRanges forbiddenRanges;
  prep_via2viaForbiddenLen_minSpc(
      lNum, viaDef1, viaDef2, isHorizontal, forbiddenRanges, ndr);
  prep_via2viaForbiddenLen_minimumCut(
      lNum, viaDef1, viaDef2, isHorizontal, forbiddenRanges);
  prep_via2viaForbiddenLen_cutSpc(
      lNum, viaDef1, viaDef2, isHorizontal, forbiddenRanges);
  prep_via2viaForbiddenLen_lef58CutSpc(
      lNum, viaDef1, viaDef2, isHorizontal, forbiddenRanges);
  prep_via2viaForbiddenLen_lef58CutSpcTbl(
      lNum, viaDef1, viaDef2, isHorizontal, forbiddenRanges);
  prep_via2viaForbiddenLen_minStep(
      lNum, viaDef1, viaDef2, !isHorizontal, forbiddenRanges);

  // merge forbidden ranges
  boost::icl::interval_set<frCoord> forbiddenIntvSet;
  for (auto& range : forbiddenRanges) {
    forbiddenIntvSet.insert(
        boost::icl::interval<frCoord>::closed(range.first, range.second));
  }

  forbiddenRanges.clear();
  for (auto it = forbiddenIntvSet.begin(); it != forbiddenIntvSet.end(); it++) {
    auto beginCoord = it->lower();
    auto endCoord = it->upper();
    forbiddenRanges.push_back(make_pair(beginCoord + 1, endCoord - 1));
  }
  if (ndr)
    ndr->via2ViaForbiddenLen[tableLayerIdx][tableEntryIdx] = forbiddenRanges;
  else {
    tech->via2ViaForbiddenLen[tableLayerIdx][tableEntryIdx] = forbiddenRanges;

    // shape-base rule
    forbiddenRanges.clear();
    forbiddenIntvSet = boost::icl::interval_set<frCoord>();
    prep_via2viaForbiddenLen_minStepGF12(
        lNum, viaDef1, viaDef2, isHorizontal, forbiddenRanges);

    // merge forbidden ranges
    for (auto& range : forbiddenRanges) {
      forbiddenIntvSet.insert(
          boost::icl::interval<frCoord>::closed(range.first, range.second));
    }

    forbiddenRanges.clear();
    for (auto it = forbiddenIntvSet.begin(); it != forbiddenIntvSet.end();
         it++) {
      auto beginCoord = it->lower();
      auto endCoord = it->upper();
      forbiddenRanges.push_back(make_pair(beginCoord, endCoord - 1));
    }

    tech->via2ViaForbiddenOverlapLen[tableLayerIdx][tableEntryIdx]
        = forbiddenRanges;
  }
}

bool FlexRP::hasMinStepViol(Rect& r1, Rect& r2, frLayerNum lNum) {
    gtl::rectangle_data<frCoord> rect1(
        r1.xMin(), r1.yMin(), r1.xMax(), r1.yMax());
    gtl::rectangle_data<frCoord> rect2(r2.xMin(),
                                             r2.yMin(),
                                             r2.xMax(),
                                             r2.yMax());

    // joining the two via rects in one polygon
    gtl::polygon_90_set_data<frCoord> set;
    using namespace boost::polygon::operators;
    set += rect1;
    set += rect2;
    std::vector<gtl::polygon_90_with_holes_data<frCoord>> polys;
    set.get_polygons(polys);
    if (polys.size() != 1)
      return false;

    gtl::polygon_90_with_holes_data<frCoord> poly = *polys.begin();
    gcNet* testNet = new gcNet(0);
    gcPin* testPin = new gcPin(poly, lNum, testNet);
    testPin->setNet(testNet);

    bool first = true;
    std::vector<std::unique_ptr<gcSegment>> tmpEdges;
    gtl::point_data<frCoord> prev;
    for (gtl::point_data<frCoord> cur : poly) {
      if (first) {
        prev = cur;
        first = false;
      } else {
        auto edge = make_unique<gcSegment>();
        edge->setLayerNum(lNum);
        edge->addToPin(testPin);
        edge->addToNet(testNet);
        edge->setSegment(prev, cur);
        if (!tmpEdges.empty()) {
          edge->setPrevEdge(tmpEdges.back().get());
          tmpEdges.back()->setNextEdge(edge.get());
        }
        tmpEdges.push_back(std::move(edge));
        prev = cur;
      }
    }
    // last edge
    auto edge = make_unique<gcSegment>();
    edge->setLayerNum(lNum);
    edge->addToPin(testPin);
    edge->addToNet(testNet);
    edge->setSegment(prev, *poly.begin());
    edge->setPrevEdge(tmpEdges.back().get());
    tmpEdges.back()->setNextEdge(edge.get());
    // set first edge
    tmpEdges.front()->setPrevEdge(edge.get());
    edge->setNextEdge(tmpEdges.front().get());
    tmpEdges.push_back(std::move(edge));
    // add to polygon edges
    testPin->addPolygonEdges(tmpEdges);
    // check gc minstep violations
    FlexGCWorker worker(tech_, logger_);
    worker.checkMinStep(testPin);
    return !worker.getMarkers().empty();
}

void FlexRP::prep_via2viaForbiddenLen_minStep(
    const frLayerNum& lNum,
    frViaDef* viaDef1,
    frViaDef* viaDef2,
    bool isVertical,
    ForbiddenRanges& forbiddenRanges)
{
  if (!viaDef1 || !viaDef2) {
    return;
  }
  frMinStepConstraint* con = tech_->getLayer(lNum)->getMinStepConstraint();
  if (!con)
      return;
  if (viaDef1->getLayer1Num() == viaDef2->getLayer1Num()) {
    return;
  }
//  cout << "viaDef1 " << *viaDef1 << "\nviaDef2 " << *viaDef2 << "\n";
  Rect enclosureBox1, enclosureBox2;
  frVia via1(viaDef1);
  frVia via2(viaDef2);
  if (viaDef1->getLayer1Num() == lNum) {
    via1.getLayer1BBox(enclosureBox1);
    via2.getLayer2BBox(enclosureBox2);
  } else {
    via1.getLayer2BBox(enclosureBox1);
    via2.getLayer1BBox(enclosureBox2);
  }
  Rect* shifting, *other;
  //get the rect with lesser width (the shifting one)
  if (isVertical) {
      if (enclosureBox1.dx() < enclosureBox2.dx()) {
          shifting = &enclosureBox1;
          other = &enclosureBox2;
      } else if (enclosureBox2.dx() < enclosureBox1.dx()) {
          shifting = &enclosureBox2;
          other = &enclosureBox1;
      } else return;
  } else {
      if (enclosureBox1.dy() < enclosureBox2.dy()) {
          shifting = &enclosureBox1;
          other = &enclosureBox2;
      } else if (enclosureBox2.dy() < enclosureBox1.dy()) {
          shifting = &enclosureBox2;
          other = &enclosureBox1;
      } else return;
  }
  //example where shifting is vertical and other is horizontal
  //     shiftingHigh
  //     _____
  //    |     | <-shiftingEdge (the vertical one)
  //  __|     |__ otherEdge (the horizontal one)
  // |           |  
  // |__       __|  
  //    |     |
  //    |_____|
  //    shiftingLow
  int shiftingEdge, otherEdge, shiftingLow, otherLow, otherHigh, minRange = 0;
  if (other->contains(*shifting)) {
      if (isVertical) {
        minRange = other->ur().y() - shifting->ur().y() + 1;
        shifting->moveDelta(0, minRange);
      } else {
        minRange = other->ur().x() - shifting->ur().x() + 1;
        shifting->moveDelta(minRange, 0);
      }
  }
  if (isVertical) {
      shiftingEdge = shifting->ur().y() - other->ur().y();
      otherEdge = other->ur().x() - shifting->ur().x();
      shiftingLow = shifting->ll().y();
      otherLow = other->ll().y();
      otherHigh = other->ur().y();
  } else {
      shiftingEdge = shifting->ur().x() - other->ur().x();
      otherEdge = other->ur().y() - shifting->ur().y();
      shiftingLow = shifting->ll().x();
      otherLow = other->ll().x();
      otherHigh = other->ur().x();
  }
  int shift;
  if (hasMinStepViol(*shifting, *other, lNum)) {
      if (shiftingEdge < con->getMinStepLength()) {
          shift = con->getMinStepLength() - shiftingEdge - 1;
          if (shiftingLow < otherLow)
              shift = std::max(shift, otherLow - shiftingLow - 1);
          if (isVertical)
              shifting->moveDelta(0, shift+1);
          else shifting->moveDelta(shift+1, 0);
          if (hasMinStepViol(*shifting, *other, lNum))
              shift = otherHigh - shiftingLow;
      } else {
          assert(otherEdge < con->getMinStepLength());
          shift = otherHigh - shiftingLow;
      }
  } else {
      if (shiftingEdge < con->getMinStepLength()) {
          if (con->getMaxLength() > 0) {
              int div = 2; 
              int length = shiftingEdge;
              int topEdge_shifting = isVertical ? shifting->dx() : shifting->dy();
              int topEdge_other = isVertical ? other->dy() : other->dx();
              if (topEdge_shifting < con->getMinStepLength()) {
                  length += topEdge_shifting + shiftingEdge;
                  if (otherEdge < con->getMinStepLength()) {
                    length += 2*otherEdge;
                    if (topEdge_other < con->getMinStepLength())
                        return;
                  }
              } else if (otherEdge < con->getMinStepLength()) {
                  length += otherEdge;
                  div = 1;
                  if (topEdge_other < con->getMinStepLength())
                      length += topEdge_other +otherEdge + shiftingEdge;
              }
              shift = (con->getMaxLength() - length)/div + 1;
              if (shift < 0)
                  return;
              if (isVertical)
                  shifting->moveDelta(0, shift);
              else
                  shifting->moveDelta(shift, 0);
              if (hasMinStepViol(*shifting, *other, lNum)) {
                  minRange = shift;
                  shift = otherHigh - shiftingLow;
              } else return;
          } else return;
      } else {
            minRange = otherLow - shiftingLow - con->getMinStepLength() + 1;
            if (isVertical)
                  shifting->moveDelta(0, minRange);
            else
                shifting->moveDelta(minRange, 0);
            if (hasMinStepViol(*shifting, *other, lNum)) {
                shift = con->getMinStepLength()-2;
                if (isVertical)
                  shifting->moveDelta(0, shift+1);
                else
                  shifting->moveDelta(shift+1, 0);
              if (hasMinStepViol(*shifting, *other, lNum))
                  shift = otherHigh - shiftingLow;
            } else return;
      }
  }
//  cout << "putting [" << minRange << " " << (minRange+shift) << " for lNum " << lNum << " isVertical " << isVertical << "\n";
  forbiddenRanges.push_back(make_pair(minRange-1, minRange+shift+1));
}



// only partial support of GF14
void FlexRP::prep_via2viaForbiddenLen_lef58CutSpc(
    const frLayerNum& lNum,
    frViaDef* viaDef1,
    frViaDef* viaDef2,
    bool isCurrDirX,
    ForbiddenRanges& forbiddenRanges)
{
  if (!viaDef1 || !viaDef2) {
    return;
  }

  if (DBPROCESSNODE != "GF14_13M_3Mx_2Cx_4Kx_2Hx_2Gx_LB") {
    return;
  }

  bool isCurrDirY = !isCurrDirX;
  if (lNum != 10 || !isCurrDirY) {
    return;
  }

  const bool match12
      = (viaDef1->getLayer1Num() == lNum) && (viaDef2->getLayer2Num() == lNum);
  const bool match21
      = (viaDef1->getLayer2Num() == lNum) && (viaDef2->getLayer1Num() == lNum);
  if (!match12 && !match21) {
    return;
  }

  Rect enclosureBox1, enclosureBox2, cutBox1, cutBox2;
  frVia via1(viaDef1);
  frVia via2(viaDef2);
  if (viaDef1->getLayer1Num() == lNum) {
    via1.getLayer1BBox(enclosureBox1);
  } else {
    via1.getLayer2BBox(enclosureBox1);
  }
  if (viaDef2->getLayer1Num() == lNum) {
    via2.getLayer1BBox(enclosureBox2);
  } else {
    via2.getLayer2BBox(enclosureBox2);
  }
  via1.getCutBBox(cutBox1);
  via2.getCutBBox(cutBox2);
  pair<frCoord, frCoord> range;
  frCoord reqSpcVal = 0;
  // check via1 cut layer to lNum
  auto via1CutLNum = viaDef1->getCutLayerNum();
  if (!tech_->getLayer(via1CutLNum)
           ->getLef58CutSpacingConstraints(false)
           .empty()) {
    for (auto con :
         tech_->getLayer(via1CutLNum)->getLef58CutSpacingConstraints(false)) {
      if (con->getSecondLayerNum() != lNum) {
        continue;
      }
      if (!con->isConcaveCorner()) {
        continue;
      }
      reqSpcVal = con->getCutSpacing();
      prep_via2viaForbiddenLen_lef58CutSpc_helper(
          enclosureBox1, enclosureBox2, cutBox1, reqSpcVal, range);
      forbiddenRanges.push_back(range);
    }
  } else {
    for (auto con :
         tech_->getLayer(via1CutLNum)->getLef58CutSpacingConstraints(true)) {
      if (con->getSecondLayerNum() != lNum) {
        continue;
      }
      if (!con->isConcaveCorner()) {
        continue;
      }
      reqSpcVal = con->getCutSpacing();
      prep_via2viaForbiddenLen_lef58CutSpc_helper(
          enclosureBox1, enclosureBox2, cutBox1, reqSpcVal, range);
      forbiddenRanges.push_back(range);
    }
  }

  // check via2 cut layer to lNum
  auto via2CutLNum = viaDef2->getCutLayerNum();
  if (!tech_->getLayer(via2CutLNum)
           ->getLef58CutSpacingConstraints(false)
           .empty()) {
    for (auto con :
         tech_->getLayer(via2CutLNum)->getLef58CutSpacingConstraints(false)) {
      if (con->getSecondLayerNum() != lNum) {
        continue;
      }
      if (!con->isConcaveCorner()) {
        continue;
      }
      reqSpcVal = con->getCutSpacing();
      prep_via2viaForbiddenLen_lef58CutSpc_helper(
          enclosureBox1, enclosureBox2, cutBox2, reqSpcVal, range);
      forbiddenRanges.push_back(range);
    }
  } else {
    for (auto con :
         tech_->getLayer(via2CutLNum)->getLef58CutSpacingConstraints(true)) {
      if (con->getSecondLayerNum() != lNum) {
        continue;
      }
      if (!con->isConcaveCorner()) {
        continue;
      }
      reqSpcVal = con->getCutSpacing();
      prep_via2viaForbiddenLen_lef58CutSpc_helper(
          enclosureBox1, enclosureBox2, cutBox2, reqSpcVal, range);
      forbiddenRanges.push_back(range);
    }
  }
}

void FlexRP::prep_via2viaForbiddenLen_lef58CutSpcTbl(
    const frLayerNum& lNum,
    frViaDef* viaDef1,
    frViaDef* viaDef2,
    bool isCurrDirX,
    ForbiddenRanges& forbiddenRanges)
{
  if (!viaDef1 || !viaDef2) {
    return;
  }
  bool swapped = false;
  if (viaDef2->getCutLayerNum() > viaDef1->getCutLayerNum()) {
    // swap
    frViaDef* temp = viaDef2;
    viaDef2 = viaDef1;
    viaDef1 = temp;
    swapped = true;
  }
  bool isCurrDirY = !isCurrDirX;
  frVia via1(viaDef1);
  Rect viaBox1, viaBox2, cutBox1, cutBox2;
  if (viaDef1->getLayer1Num() == lNum) {
    via1.getLayer1BBox(viaBox1);
  } else {
    via1.getLayer2BBox(viaBox1);
  }

  frVia via2(viaDef2);
  if (viaDef2->getLayer1Num() == lNum) {
    via2.getLayer1BBox(viaBox2);
  } else {
    via2.getLayer2BBox(viaBox2);
  }
  via1.getCutBBox(cutBox1);
  via2.getCutBBox(cutBox2);
  frCoord reqSpcVal = 0;
  auto layer1 = tech_->getLayer(viaDef1->getCutLayerNum());
  auto layer2 = tech_->getLayer(viaDef2->getCutLayerNum());
  auto cutClassIdx1 = layer1->getCutClassIdx(cutBox1.minDXDY(), cutBox1.maxDXDY());
  auto cutClassIdx2 = layer2->getCutClassIdx(cutBox2.minDXDY(), cutBox2.maxDXDY());
  frString cutClass1, cutClass2;
  if (cutClassIdx1 != -1)
    cutClass1 = layer1->getCutClass(cutClassIdx1)->getName();
  if (cutClassIdx2 != -1)
    cutClass2 = layer2->getCutClass(cutClassIdx2)->getName();
  bool isSide1;
  bool isSide2;
  if (isCurrDirY) {
    isSide1 = (cutBox1.xMax() - cutBox1.xMin())
              > (cutBox1.yMax() - cutBox1.yMin());
    isSide2 = (cutBox2.xMax() - cutBox2.xMin())
              > (cutBox2.yMax() - cutBox2.yMin());
  } else {
    isSide1 = (cutBox1.xMax() - cutBox1.xMin())
              < (cutBox1.yMax() - cutBox1.yMin());
    isSide2 = (cutBox2.xMax() - cutBox2.xMin())
              < (cutBox2.yMax() - cutBox2.yMin());
  }
  if (layer1->getLayerNum() == layer2->getLayerNum()) {
    frLef58CutSpacingTableConstraint* lef58con = nullptr;
    if (layer1->hasLef58SameMetalCutSpcTblConstraint())
      lef58con = layer1->getLef58SameMetalCutSpcTblConstraint();
    else if (layer1->hasLef58SameNetCutSpcTblConstraint())
      lef58con = layer1->getLef58SameNetCutSpcTblConstraint();
    else if (layer1->hasLef58DiffNetCutSpcTblConstraint())
      lef58con = layer1->getLef58DiffNetCutSpcTblConstraint();
    if (lef58con != nullptr) {
      auto dbRule = lef58con->getODBRule();
      reqSpcVal = dbRule->getSpacing(cutClass1, isSide1, cutClass2, isSide2);
      if (!dbRule->isCenterToCenter(cutClass1, cutClass2)
          && !dbRule->isCenterAndEdge(cutClass1, cutClass2)) {
        if (!swapped)
          reqSpcVal += isCurrDirY ? (cutBox1.yMax() - cutBox1.yMin())
                                  : (cutBox1.xMax() - cutBox1.xMin());
        else
          reqSpcVal += isCurrDirY ? (cutBox2.yMax() - cutBox2.yMin())
                                  : (cutBox2.xMax() - cutBox2.xMin());
      }
      if (reqSpcVal != 0)
        forbiddenRanges.push_back(make_pair(0, reqSpcVal));
    }
  } else {
    frLef58CutSpacingTableConstraint* con;
    if (layer1->hasLef58SameMetalInterCutSpcTblConstraint())
      con = layer1->getLef58SameMetalInterCutSpcTblConstraint();
    else if (layer1->hasLef58SameNetInterCutSpcTblConstraint())
      con = layer1->getLef58SameNetInterCutSpcTblConstraint();
    else
      return;
    auto dbRule = con->getODBRule();
    if (dbRule->isSameNet() || dbRule->isSameMetal())
      if (!dbRule->isNoStack())
        return;
    reqSpcVal = dbRule->getSpacing(cutClass1, isSide1, cutClass2, isSide2);
    if (reqSpcVal == 0)
      return;
    if (!dbRule->isCenterToCenter(cutClass1, cutClass2)
        && !dbRule->isCenterAndEdge(cutClass1, cutClass2)) {
      reqSpcVal += isCurrDirY ? ((cutBox1.yMax() - cutBox1.yMin()
                                  + cutBox2.yMax() - cutBox2.yMin())
                                 / 2)
                              : ((cutBox1.xMax() - cutBox1.xMin()
                                  + cutBox2.xMax() - cutBox2.xMin())
                                 / 2);
    }
    forbiddenRanges.push_back(make_pair(0, reqSpcVal));
  }
}

void FlexRP::prep_via2viaForbiddenLen_lef58CutSpc_helper(
    const Rect& enclosureBox1,
    const Rect& enclosureBox2,
    const Rect& cutBox,
    frCoord reqSpcVal,
    pair<frCoord, frCoord>& range)
{
  frCoord overlapLen = min((enclosureBox1.yMax() - enclosureBox1.yMin()),
                           (enclosureBox2.yMax() - enclosureBox2.yMin()));
  frCoord cutLen = cutBox.yMax() - cutBox.yMin();
  frCoord forbiddenLowerBound, forbiddenUpperBound;
  forbiddenLowerBound = max(0, (overlapLen - cutLen) / 2 - reqSpcVal);
  forbiddenUpperBound = reqSpcVal + (overlapLen + cutLen) / 2;
  range = make_pair(forbiddenLowerBound, forbiddenUpperBound);
}

// only partial support of GF14
void FlexRP::prep_via2viaForbiddenLen_minStepGF12(
    const frLayerNum& lNum,
    frViaDef* viaDef1,
    frViaDef* viaDef2,
    bool isCurrDirX,
    ForbiddenRanges& forbiddenRanges)
{
  if (!viaDef1 || !viaDef2) {
    return;
  }

  if (DBPROCESSNODE != "GF14_13M_3Mx_2Cx_4Kx_2Hx_2Gx_LB") {
    return;
  }

  bool isCurrDirY = !isCurrDirX;
  if (lNum != 10 || !isCurrDirY) {
    return;
  }
  const bool match12
      = (viaDef1->getLayer1Num() == lNum) && (viaDef2->getLayer2Num() == lNum);
  const bool match21
      = (viaDef1->getLayer2Num() == lNum) && (viaDef2->getLayer1Num() == lNum);
  if (!match12 && !match21) {
    return;
  }
  Rect enclosureBox1, enclosureBox2;
  frVia via1(viaDef1);
  frVia via2(viaDef2);
  if (viaDef1->getLayer1Num() == lNum) {
    via1.getLayer1BBox(enclosureBox1);
  } else {
    via1.getLayer2BBox(enclosureBox1);
  }
  if (viaDef2->getLayer1Num() == lNum) {
    via2.getLayer1BBox(enclosureBox2);
  } else {
    via2.getLayer2BBox(enclosureBox2);
  }

  frCoord enclosureBox1Span = enclosureBox1.yMax() - enclosureBox1.yMin();
  frCoord enclosureBox2Span = enclosureBox2.yMax() - enclosureBox2.yMin();
  frCoord maxForbiddenLen = (enclosureBox1Span > enclosureBox2Span)
                                ? (enclosureBox1Span - enclosureBox2Span)
                                : (enclosureBox2Span - enclosureBox1Span);
  maxForbiddenLen /= 2;

  forbiddenRanges.push_back(make_pair(0, maxForbiddenLen));
}

void FlexRP::prep_via2viaForbiddenLen_minimumCut(
    const frLayerNum& lNum,
    frViaDef* viaDef1,
    frViaDef* viaDef2,
    bool isCurrDirX,
    ForbiddenRanges& forbiddenRanges)
{
  if (!viaDef1 || !viaDef2) {
    return;
  }

  bool isH = (getDesign()->getTech()->getLayer(lNum)->getDir()
              == dbTechLayerDir::HORIZONTAL);

  bool isVia1Above = false;
  frVia via1(viaDef1);
  Rect viaBox1, cutBox1;
  if (viaDef1->getLayer1Num() == lNum) {
    via1.getLayer1BBox(viaBox1);
    isVia1Above = true;
  } else {
    via1.getLayer2BBox(viaBox1);
    isVia1Above = false;
  }
  via1.getCutBBox(cutBox1);
  int width1 = viaBox1.minDXDY();
  int length1 = viaBox1.maxDXDY();

  bool isVia2Above = false;
  frVia via2(viaDef2);
  Rect viaBox2, cutBox2;
  if (viaDef2->getLayer1Num() == lNum) {
    via2.getLayer1BBox(viaBox2);
    isVia2Above = true;
  } else {
    via2.getLayer2BBox(viaBox2);
    isVia2Above = false;
  }
  via2.getCutBBox(cutBox2);
  int width2 = viaBox2.minDXDY();
  int length2 = viaBox2.maxDXDY();

  for (auto& con :
       getDesign()->getTech()->getLayer(lNum)->getMinimumcutConstraints()) {
    frCoord minReqDist = INT_MIN;
    // check via2cut to via1metal
    // no length OR metal1 shape satisfies --> check via2
    if ((!con->hasLength() || (con->hasLength() && length1 > con->getLength()))
        && width1 > con->getWidth()) {
      bool checkVia2 = false;
      if (!con->hasConnection()) {
        checkVia2 = true;
      } else {
        if (con->getConnection() == frMinimumcutConnectionEnum::FROMABOVE
            && isVia2Above) {
          checkVia2 = true;
        } else if (con->getConnection() == frMinimumcutConnectionEnum::FROMBELOW
                   && !isVia2Above) {
          checkVia2 = true;
        }
      }
      if (!checkVia2) {
        continue;
      }
      if (isH) {
        minReqDist = max(minReqDist,
                         (con->hasLength() ? con->getDistance() : 0)
                             + max(cutBox2.xMax() - 0 + 0 - viaBox1.xMin(),
                                   viaBox1.xMax() - 0 + 0 - cutBox2.xMin()));
      } else {
        minReqDist = max(minReqDist,
                         (con->hasLength() ? con->getDistance() : 0)
                             + max(cutBox2.yMax() - 0 + 0 - viaBox1.yMin(),
                                   viaBox1.yMax() - 0 + 0 - cutBox2.yMin()));
      }
      forbiddenRanges.push_back(make_pair(0, minReqDist));
    }
    minReqDist = INT_MIN;
    // check via1cut to via2metal
    if ((!con->hasLength() || (con->hasLength() && length2 > con->getLength()))
        && width2 > con->getWidth()) {
      bool checkVia1 = false;
      if (!con->hasConnection()) {
        checkVia1 = true;
      } else {
        if (con->getConnection() == frMinimumcutConnectionEnum::FROMABOVE
            && isVia1Above) {
          checkVia1 = true;
        } else if (con->getConnection() == frMinimumcutConnectionEnum::FROMBELOW
                   && !isVia1Above) {
          checkVia1 = true;
        }
      }
      if (!checkVia1) {
        continue;
      }
      if (isH) {
        minReqDist = max(minReqDist,
                         (con->hasLength() ? con->getDistance() : 0)
                             + max(cutBox1.xMax() - 0 + 0 - viaBox2.xMin(),
                                   viaBox2.xMax() - 0 + 0 - cutBox1.xMin()));
      } else {
        minReqDist = max(minReqDist,
                         (con->hasLength() ? con->getDistance() : 0)
                             + max(cutBox1.yMax() - 0 + 0 - viaBox2.yMin(),
                                   viaBox2.yMax() - 0 + 0 - cutBox1.yMin()));
      }
      forbiddenRanges.push_back(make_pair(0, minReqDist));
    }
  }
}

void FlexRP::prep_via2viaForbiddenLen_cutSpc(
    const frLayerNum& lNum,
    frViaDef* viaDef1,
    frViaDef* viaDef2,
    bool isCurrDirX,
    ForbiddenRanges& forbiddenRanges)
{
  if (!viaDef1 || !viaDef2) {
    return;
  }

  bool isCurrDirY = !isCurrDirX;

  frVia via1(viaDef1);
  Rect viaBox1, cutBox1;
  if (viaDef1->getLayer1Num() == lNum) {
    via1.getLayer1BBox(viaBox1);
  } else {
    via1.getLayer2BBox(viaBox1);
  }
  via1.getCutBBox(cutBox1);

  frVia via2(viaDef2);
  Rect viaBox2, cutBox2;
  if (viaDef2->getLayer1Num() == lNum) {
    via2.getLayer1BBox(viaBox2);
  } else {
    via2.getLayer2BBox(viaBox2);
  }
  via2.getCutBBox(cutBox2);

  // same layer (use samenet rule if exist, otherwise use diffnet rule)
  if (viaDef1->getCutLayerNum() == viaDef2->getCutLayerNum()) {
    auto samenetCons = getDesign()
                           ->getTech()
                           ->getLayer(viaDef1->getCutLayerNum())
                           ->getCutSpacing(true);
    auto diffnetCons = getDesign()
                           ->getTech()
                           ->getLayer(viaDef1->getCutLayerNum())
                           ->getCutSpacing(false);
    if (!samenetCons.empty()) {
      // check samenet spacing rule if exists
      for (auto con : samenetCons) {
        if (con == nullptr) {
          continue;
        }
        // filter rule, assuming default via will never trigger cutArea
        if (con->hasSecondLayer() || con->isAdjacentCuts()
            || con->isParallelOverlap() || con->isArea()
            || !con->hasSameNet()) {
          continue;
        }
        auto reqSpcVal = con->getCutSpacing();
        if (!con->hasCenterToCenter()) {
          reqSpcVal += isCurrDirY ? (cutBox1.yMax() - cutBox1.yMin())
                                  : (cutBox1.xMax() - cutBox1.xMin());
        }
        forbiddenRanges.push_back(make_pair(0, reqSpcVal));
      }
    } else {
      // check diffnet spacing rule if samenet rule does not exist
      // filter rule, assuming default via will never trigger cutArea
      for (auto con : diffnetCons) {
        if (con == nullptr) {
          continue;
        }
        if (con->hasSecondLayer() || con->isAdjacentCuts()
            || con->isParallelOverlap() || con->isArea() || con->hasSameNet()) {
          continue;
        }
        auto reqSpcVal = con->getCutSpacing();
        if (!con->hasCenterToCenter()) {
          reqSpcVal += isCurrDirY ? (cutBox1.yMax() - cutBox1.yMin())
                                  : (cutBox1.xMax() - cutBox1.xMin());
        }
        forbiddenRanges.push_back(make_pair(0, reqSpcVal));
      }
    }
  } else {
    auto layerNum1 = viaDef1->getCutLayerNum();
    auto layerNum2 = viaDef2->getCutLayerNum();
    frCutSpacingConstraint* samenetCon = nullptr;
    if (getDesign()->getTech()->getLayer(layerNum1)->hasInterLayerCutSpacing(
            layerNum2, true)) {
      samenetCon = getDesign()
                       ->getTech()
                       ->getLayer(layerNum1)
                       ->getInterLayerCutSpacing(layerNum2, true);
    }
    if (getDesign()->getTech()->getLayer(layerNum2)->hasInterLayerCutSpacing(
            layerNum1, true)) {
      if (samenetCon) {
        logger_->warn(DRT,
                      92,
                      "Duplicate diff layer samenet cut spacing, skipping cut "
                      "spacing from {} to {}.",
                      layerNum2,
                      layerNum1);
      } else {
        samenetCon = getDesign()
                         ->getTech()
                         ->getLayer(layerNum2)
                         ->getInterLayerCutSpacing(layerNum1, true);
      }
    }
    if (samenetCon == nullptr) {
      if (getDesign()->getTech()->getLayer(layerNum1)->hasInterLayerCutSpacing(
              layerNum2, false)) {
        samenetCon = getDesign()
                         ->getTech()
                         ->getLayer(layerNum1)
                         ->getInterLayerCutSpacing(layerNum2, false);
      }
      if (getDesign()->getTech()->getLayer(layerNum2)->hasInterLayerCutSpacing(
              layerNum1, false)) {
        if (samenetCon) {
          logger_->warn(DRT,
                        93,
                        "Duplicate diff layer diffnet cut spacing, skipping "
                        "cut spacing from {} to {}.",
                        layerNum2,
                        layerNum1);
        } else {
          samenetCon = getDesign()
                           ->getTech()
                           ->getLayer(layerNum2)
                           ->getInterLayerCutSpacing(layerNum1, false);
        }
      }
    }
    if (samenetCon) {
      // filter rule, assuming default via will never trigger cutArea
      auto reqSpcVal = samenetCon->getCutSpacing();
      if (reqSpcVal == 0) {
        ;
      } else {
        if (!samenetCon->hasCenterToCenter()) {
          reqSpcVal += isCurrDirY ? ((cutBox1.yMax() - cutBox1.yMin()
                                      + cutBox2.yMax() - cutBox2.yMin())
                                     / 2)
                                  : ((cutBox1.xMax() - cutBox1.xMin()
                                      + cutBox2.xMax() - cutBox2.xMin())
                                     / 2);
        }
      }
      if (reqSpcVal != 0 && !samenetCon->hasStack()) {
        forbiddenRanges.push_back(make_pair(0, reqSpcVal));
      }
    }
  }
}

void FlexRP::prep_via2viaForbiddenLen_minSpc(
    frLayerNum lNum,
    frViaDef* viaDef1,
    frViaDef* viaDef2,
    bool isCurrDirX,
    ForbiddenRanges& forbiddenRanges,
    frNonDefaultRule* ndr)
{
  if (!viaDef1 || !viaDef2) {
    return;
  }

  // bool isCurrDirY = !isCurrDirX;
  frCoord defaultWidth = getDesign()->getTech()->getLayer(lNum)->getWidth();

  frVia via1(viaDef1);
  Rect viaBox1;
  if (viaDef1->getLayer1Num() == lNum) {
    via1.getLayer1BBox(viaBox1);
  } else {
    via1.getLayer2BBox(viaBox1);
  }
  auto width1 = viaBox1.minDXDY();
  bool isVia1Fat = isCurrDirX
                       ? (viaBox1.yMax() - viaBox1.yMin() > defaultWidth)
                       : (viaBox1.xMax() - viaBox1.xMin() > defaultWidth);
  auto prl1 = isCurrDirX ? (viaBox1.yMax() - viaBox1.yMin())
                         : (viaBox1.xMax() - viaBox1.xMin());

  frVia via2(viaDef2);
  Rect viaBox2;
  if (viaDef2->getLayer1Num() == lNum) {
    via2.getLayer1BBox(viaBox2);
  } else {
    via2.getLayer2BBox(viaBox2);
  }
  auto width2 = viaBox2.minDXDY();
  bool isVia2Fat = isCurrDirX
                       ? (viaBox2.yMax() - viaBox2.yMin() > defaultWidth)
                       : (viaBox2.xMax() - viaBox2.xMin() > defaultWidth);
  auto prl2 = isCurrDirX ? (viaBox2.yMax() - viaBox2.yMin())
                         : (viaBox2.xMax() - viaBox2.xMin());

  frCoord minNonOverlapDist = isCurrDirX ? ((viaBox1.xMax() - viaBox1.xMin()
                                             + viaBox2.xMax() - viaBox2.xMin())
                                            / 2)
                                         : ((viaBox1.yMax() - viaBox1.yMin()
                                             + viaBox2.yMax() - viaBox2.yMin())
                                            / 2);
  frCoord minReqDist = INT_MIN;

  // check minSpc rule
  if (isVia1Fat && isVia2Fat) {
    auto con = getDesign()->getTech()->getLayer(lNum)->getMinSpacing();
    if (con) {
      if (con->typeId() == frConstraintTypeEnum::frcSpacingConstraint) {
        minReqDist = static_cast<frSpacingConstraint*>(con)->getMinSpacing();
      } else if (con->typeId()
                 == frConstraintTypeEnum::frcSpacingTablePrlConstraint) {
        minReqDist = static_cast<frSpacingTablePrlConstraint*>(con)->find(
            max(width1, width2), min(prl1, prl2));
      } else if (con->typeId()
                 == frConstraintTypeEnum::frcSpacingTableTwConstraint) {
        minReqDist = static_cast<frSpacingTableTwConstraint*>(con)->find(
            width1, width2, min(prl1, prl2));
      }
    }
    if (ndr)
      minReqDist = max(minReqDist, ndr->getSpacing(lNum / 2 - 1));
    if (minReqDist != INT_MIN) {
      minReqDist += minNonOverlapDist;
    }
  }

  if (minReqDist != INT_MIN) {
    forbiddenRanges.push_back(make_pair(minNonOverlapDist, minReqDist));
  }

  // check in layer2 if two vias are in same layer
  if (viaDef1 == viaDef2) {
    if (viaDef1->getLayer1Num() == lNum) {
      via1.getLayer2BBox(viaBox1);
      lNum = lNum + 2;
    } else {
      via1.getLayer1BBox(viaBox1);
      lNum = lNum - 2;
    }
    minNonOverlapDist = isCurrDirX ? (viaBox1.xMax() - viaBox1.xMin())
                                   : (viaBox2.xMax() - viaBox2.xMin());

    width1 = viaBox1.minDXDY();
    prl1 = isCurrDirX ? (viaBox1.yMax() - viaBox1.yMin())
                      : (viaBox1.xMax() - viaBox1.xMin());
    minReqDist = INT_MIN;
    auto con = getDesign()->getTech()->getLayer(lNum)->getMinSpacing();
    if (con) {
      if (con->typeId() == frConstraintTypeEnum::frcSpacingConstraint) {
        minReqDist = static_cast<frSpacingConstraint*>(con)->getMinSpacing();
      } else if (con->typeId()
                 == frConstraintTypeEnum::frcSpacingTablePrlConstraint) {
        minReqDist = static_cast<frSpacingTablePrlConstraint*>(con)->find(
            max(width1, width2), prl1);
      } else if (con->typeId()
                 == frConstraintTypeEnum::frcSpacingTableTwConstraint) {
        minReqDist = static_cast<frSpacingTableTwConstraint*>(con)->find(
            width1, width2, prl1);
      }
      if (ndr)
        minReqDist = max(minReqDist, ndr->getSpacing(lNum / 2 - 1));
      if (minReqDist != INT_MIN) {
        minReqDist += minNonOverlapDist;
      }
    }

    if (minReqDist != INT_MIN) {
      forbiddenRanges.push_back(make_pair(minNonOverlapDist, minReqDist));
    }
  }
}
