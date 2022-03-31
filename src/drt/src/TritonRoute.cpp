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

#include "triton_route/TritonRoute.h"

#include <fstream>
#include <iostream>

#include "DesignCallBack.h"
#include "db/tech/frTechObject.h"
#include "distributed/RoutingCallBack.h"
#include "distributed/frArchive.h"
#include "dr/FlexDR.h"
#include "dr/FlexDR_graphics.h"
#include "dst/Distributed.h"
#include "frDesign.h"
#include "gc/FlexGC.h"
#include "global.h"
#include "gr/FlexGR.h"
#include "gui/gui.h"
#include "io/io.h"
#include "ord/OpenRoad.hh"
#include "pa/FlexPA.h"
#include "rp/FlexRP.h"
#include "serialization.h"
#include "sta/StaMain.hh"
#include "stt/SteinerTreeBuilder.h"
#include "ta/FlexTA.h"
#include "distributed/drUpdate.h"
using namespace std;
using namespace fr;
using namespace triton_route;

namespace sta {
// Tcl files encoded into strings.
extern const char* drt_tcl_inits[];
}  // namespace sta

extern "C" {
extern int Drt_Init(Tcl_Interp* interp);
}

TritonRoute::TritonRoute()
    : debug_(std::make_unique<frDebugSettings>()),
      db_callback_(std::make_unique<DesignCallBack>(this)),
      num_drvs_(-1),
      gui_(gui::Gui::get()),
      distributed_(false),
      results_sz_(0)
{
}

TritonRoute::~TritonRoute()
{
}

void TritonRoute::setDebugDR(bool on)
{
  debug_->debugDR = on;
}

void TritonRoute::setDebugDumpDR(bool on)
{
  debug_->debugDumpDR = on;
}

void TritonRoute::setDebugMaze(bool on)
{
  debug_->debugMaze = on;
}

void TritonRoute::setDebugPA(bool on)
{
  debug_->debugPA = on;
}

void TritonRoute::setDistributed(bool on)
{
  distributed_ = on;
}

void TritonRoute::setWorkerIpPort(const char* ip, unsigned short port)
{
  dist_ip_ = ip;
  dist_port_ = port;
}

void TritonRoute::setSharedVolume(const std::string& vol)
{
  shared_volume_ = vol;
  if (!shared_volume_.empty() && shared_volume_.back() != '/') {
    shared_volume_ += '/';
  }
}

void TritonRoute::setDebugNetName(const char* name)
{
  debug_->netName = name;
}

void TritonRoute::setDebugPinName(const char* name)
{
  debug_->pinName = name;
}

void TritonRoute::setDebugWorker(int x, int y)
{
  debug_->x = x;
  debug_->y = y;
}

void TritonRoute::setDebugIter(int iter)
{
  debug_->iter = iter;
}

void TritonRoute::setDebugPaMarkers(bool on)
{
  debug_->paMarkers = on;
}

void TritonRoute::setDebugPaEdge(bool on)
{
  debug_->paEdge = on;
}

void TritonRoute::setDebugPaCommit(bool on)
{
  debug_->paCommit = on;
}

int TritonRoute::getNumDRVs() const
{
  if (num_drvs_ < 0) {
    logger_->error(DRT, 2, "Detailed routing has not been run yet.");
  }
  return num_drvs_;
}

std::string TritonRoute::runDRWorker(const std::string& workerStr)
{
  bool on = debug_->debugDR;
  std::unique_ptr<FlexDRGraphics> graphics_
      = on && FlexDRGraphics::guiActive() ? std::make_unique<FlexDRGraphics>(
            debug_.get(), design_.get(), db_, logger_)
                                          : nullptr;
  auto worker
      = FlexDRWorker::load(workerStr, logger_, design_.get(), graphics_.get());
  worker->setSharedVolume(shared_volume_);
  worker->setDebugSettings(debug_.get());
  if (graphics_)
    graphics_->startIter(worker->getDRIter());
  std::string result = worker->reloadedMain();
  return result;
}

void TritonRoute::updateGlobals(const char* file_name)
{
  std::ifstream file(file_name);
  if (!file.good())
    return;
  frIArchive ar(file);
  register_types(ar);
  serialize_globals(ar);
  file.close();
}
void TritonRoute::resetDesign(const char* file_name)
{
  std::ifstream file(file_name);
  if (!file.good())
    return;
  design_ = std::make_unique<frDesign>();
  frIArchive ar(file);
  ar.setDeepSerialize(true);
  register_types(ar);
  ar >> *(design_.get());
  file.close();
}
void TritonRoute::updateDesign(const char* file_name)
{
  std::ifstream file(file_name);
  if (!file.good())
    return;
  std::vector<drUpdate> updates;
  frIArchive ar(file);
  ar.setDeepSerialize(false);
  ar.setDesign(design_.get());
  register_types(ar);
  ar >> updates;
  auto topBlock = design_->getTopBlock();
  auto regionQuery = design_->getRegionQuery();
  for(auto& update : updates)
  {
    switch (update.getType())
    {
    case drUpdate::REMOVE_FROM_BLOCK:
    {
      auto id = update.getOrderInOwner();
      auto marker = design_->getTopBlock()->getMarker(id);
      regionQuery->removeMarker(marker);
      topBlock->removeMarker(marker);
      break;
    }
    case drUpdate::REMOVE_FROM_NET:
    {
      auto net = update.getNet();
      auto id = update.getOrderInOwner();
      auto pinfig = net->getPinFig(id);
      switch (pinfig->typeId())
      {
      case frcPathSeg:
      {
        auto seg = static_cast<frPathSeg*>(pinfig);
        regionQuery->removeDRObj(seg);
        net->removeShape(seg);
        break;
      }
      case frcPatchWire:
      {
        auto pwire = static_cast<frPatchWire*>(pinfig);
        regionQuery->removeDRObj(pwire);
        net->removePatchWire(pwire);
        break;
      }
      case frcVia:
      {
        auto via = static_cast<frVia*>(pinfig);
        regionQuery->removeDRObj(via);
        net->removeVia(via);
        break;
      }
      default:
        logger_->error(DRT, 9999, "unknown update type {}", pinfig->typeId());
        break;
      }
      break;
    }
    case drUpdate::ADD:
    {
      auto obj = update.getObj();
      switch (obj.which())
      {
      case 0:
      {
        auto net = update.getNet();
        frPathSeg seg = boost::get<frPathSeg>(obj);
        std::unique_ptr<frShape> uShape = std::make_unique<frPathSeg>(seg);
        auto sptr = uShape.get();
        net->addShape(std::move(uShape));
        regionQuery->addDRObj(sptr);
        break;
      }
      case 1:
      {
        auto net = update.getNet();
        frPatchWire pwire = boost::get<frPatchWire>(obj);
        std::unique_ptr<frShape> uShape = std::make_unique<frPatchWire>(pwire);
        auto sptr = uShape.get();
        net->addPatchWire(std::move(uShape));
        regionQuery->addDRObj(sptr);
        break;
      }
      case 2:
      {
        auto net = update.getNet();
        frVia via = boost::get<frVia>(obj);
        auto uVia = std::make_unique<frVia>(via);
        auto sptr = uVia.get();
        net->addVia(std::move(uVia));
        regionQuery->addDRObj(sptr);
        break;
      }
      default:
      {
        frMarker marker = boost::get<frMarker>(obj);
        auto uMarker = std::make_unique<frMarker>(marker);
        auto sptr = uMarker.get();
        topBlock->addMarker(std::move(uMarker));
        regionQuery->addMarker(sptr);
        break;
      }
      }
      break;
    }
    }
  }
}

void TritonRoute::init(Tcl_Interp* tcl_interp,
                       odb::dbDatabase* db,
                       Logger* logger,
                       dst::Distributed* dist,
                       stt::SteinerTreeBuilder* stt_builder)
{
  db_ = db;
  logger_ = logger;
  dist_ = dist;
  stt_builder_ = stt_builder;
  design_ = std::make_unique<frDesign>(logger_);
  dist->addCallBack(new fr::RoutingCallBack(this, dist, logger));
  // Define swig TCL commands.
  Drt_Init(tcl_interp);
  sta::evalTclInit(tcl_interp, sta::drt_tcl_inits);
  FlexDRGraphics::init();
}

void TritonRoute::initGuide()
{
  if (DBPROCESSNODE == "GF14_13M_3Mx_2Cx_4Kx_2Hx_2Gx_LB")
    USENONPREFTRACKS = false;
  io::Parser parser(getDesign(), logger_);
  if (!GUIDE_FILE.empty()) {
    parser.readGuide();
    parser.postProcessGuide(db_);
  }
  parser.initRPin();
}
void TritonRoute::initDesign()
{
  if (getDesign()->getTopBlock() != nullptr)
    return;
  io::Parser parser(getDesign(), logger_);
  parser.readDb(db_);
  auto tech = getDesign()->getTech();
  if (!BOTTOM_ROUTING_LAYER_NAME.empty()) {
    frLayer* layer = tech->getLayer(BOTTOM_ROUTING_LAYER_NAME);
    if (layer) {
      BOTTOM_ROUTING_LAYER = layer->getLayerNum();
    } else {
      logger_->warn(utl::DRT,
                    272,
                    "bottomRoutingLayer {} not found.",
                    BOTTOM_ROUTING_LAYER_NAME);
    }
  }

  if (!TOP_ROUTING_LAYER_NAME.empty()) {
    frLayer* layer = tech->getLayer(TOP_ROUTING_LAYER_NAME);
    if (layer) {
      TOP_ROUTING_LAYER = layer->getLayerNum();
    } else {
      logger_->warn(utl::DRT,
                    273,
                    "topRoutingLayer {} not found.",
                    TOP_ROUTING_LAYER_NAME);
    }
  }

  if (!VIAINPIN_BOTTOMLAYER_NAME.empty()) {
    frLayer* layer = tech->getLayer(VIAINPIN_BOTTOMLAYER_NAME);
    if (layer) {
      VIAINPIN_BOTTOMLAYERNUM = layer->getLayerNum();
    } else {
      logger_->warn(utl::DRT,
                    606,
                    "via in pin bottom layer {} not found.",
                    VIAINPIN_BOTTOMLAYERNUM);
    }
  }

  if (!VIAINPIN_TOPLAYER_NAME.empty()) {
    frLayer* layer = tech->getLayer(VIAINPIN_TOPLAYER_NAME);
    if (layer) {
      VIAINPIN_TOPLAYERNUM = layer->getLayerNum();
    } else {
      logger_->warn(utl::DRT,
                    607,
                    "via in pin top layer {} not found.",
                    VIAINPIN_TOPLAYERNUM);
    }
  }
  parser.postProcess();
  if (db_ != nullptr && db_->getChip() != nullptr
      && db_->getChip()->getBlock() != nullptr)
    db_callback_->addOwner(db_->getChip()->getBlock());
}

void TritonRoute::prep()
{
  FlexRP rp(getDesign(), getDesign()->getTech(), logger_);
  rp.main();
}

void TritonRoute::gr()
{
  FlexGR gr(getDesign(), logger_, stt_builder_);
  gr.main(db_);
}

void TritonRoute::ta()
{
  FlexTA ta(getDesign(), logger_);
  ta.main();
}

void TritonRoute::dr()
{
  num_drvs_ = -1;
  FlexDR dr(this, getDesign(), logger_, db_);
  dr.setDebug(debug_.get());
  if (distributed_)
    dr.setDistributed(dist_, dist_ip_, dist_port_, shared_volume_);
  dr.main();
}

void TritonRoute::endFR()
{
  io::Writer writer(getDesign(), logger_);
  writer.updateDb(db_);
}

void TritonRoute::reportConstraints()
{
  getDesign()->getTech()->printAllConstraints(logger_);
}

int TritonRoute::main()
{
  MAX_THREADS = ord::OpenRoad::openRoad()->getThreadCount();
  initDesign();
  if (!NO_PA) {
    FlexPA pa(getDesign(), logger_);
    pa.setDebug(debug_.get(), db_);
    pa.main();
  }
  initGuide();
  if (GUIDE_FILE == string("")) {
    gr();
    io::Parser parser(getDesign(), logger_);
    GUIDE_FILE = OUTGUIDE_FILE;
    ENABLE_VIA_GEN = true;
    parser.readGuide();
    parser.initDefaultVias();
    parser.postProcessGuide(db_);
  }
  prep();
  ta();
  dr();
  endFR();

  num_drvs_ = design_->getTopBlock()->getNumMarkers();

  return 0;
}

void TritonRoute::pinAccess(std::vector<odb::dbInst*> target_insts)
{
  MAX_THREADS = ord::OpenRoad::openRoad()->getThreadCount();
  ENABLE_VIA_GEN = true;
  initDesign();
  FlexPA pa(getDesign(), logger_);
  pa.setTargetInstances(target_insts);
  pa.setDebug(debug_.get(), db_);
  pa.main();
  io::Writer writer(getDesign(), logger_);
  writer.updateDb(db_, true);
}

void TritonRoute::readParams(const string& fileName)
{
  logger_->warn(utl::DRT, 252, "params file is deprecated. Use tcl arguments.");

  int readParamCnt = 0;
  ifstream fin(fileName.c_str());
  string line;
  if (fin.is_open()) {
    while (fin.good()) {
      getline(fin, line);
      if (line[0] != '#') {
        char delimiter = ':';
        int pos = line.find(delimiter);
        string field = line.substr(0, pos);
        string value = line.substr(pos + 1);
        stringstream ss(value);
        if (field == "lef") {
          logger_->warn(utl::DRT, 148, "Deprecated lef param in params file.");
        } else if (field == "def") {
          logger_->warn(utl::DRT, 227, "Deprecated def param in params file.");
        } else if (field == "guide") {
          GUIDE_FILE = value;
          ++readParamCnt;
        } else if (field == "outputTA") {
          logger_->warn(
              utl::DRT, 266, "Deprecated outputTA param in params file.");
        } else if (field == "output") {
          logger_->warn(
              utl::DRT, 205, "Deprecated output param in params file.");
        } else if (field == "outputguide") {
          OUTGUIDE_FILE = value;
          ++readParamCnt;
        } else if (field == "outputMaze") {
          OUT_MAZE_FILE = value;
          ++readParamCnt;
        } else if (field == "outputDRC") {
          DRC_RPT_FILE = value;
          ++readParamCnt;
        } else if (field == "outputCMap") {
          CMAP_FILE = value;
          ++readParamCnt;
        } else if (field == "threads") {
          logger_->warn(utl::DRT,
                        274,
                        "Deprecated threads param in params file."
                        " Use 'set_thread_count'.");
          ++readParamCnt;
        } else if (field == "verbose")
          VERBOSE = atoi(value.c_str());
        else if (field == "dbProcessNode") {
          DBPROCESSNODE = value;
          ++readParamCnt;
        } else if (field == "viaInPinBottomLayer") {
          VIAINPIN_BOTTOMLAYER_NAME = value;
          ++readParamCnt;
        } else if (field == "viaInPinTopLayer") {
          VIAINPIN_TOPLAYER_NAME = value;
          ++readParamCnt;
        } else if (field == "drouteEndIterNum") {
          END_ITERATION = atoi(value.c_str());
          ++readParamCnt;
        } else if (field == "OR_SEED") {
          OR_SEED = atoi(value.c_str());
          ++readParamCnt;
        } else if (field == "OR_K") {
          OR_K = atof(value.c_str());
          ++readParamCnt;
        } else if (field == "bottomRoutingLayer") {
          BOTTOM_ROUTING_LAYER_NAME = value;
          ++readParamCnt;
        } else if (field == "topRoutingLayer") {
          TOP_ROUTING_LAYER_NAME = value;
          ++readParamCnt;
        } else if (field == "initRouteShapeCost") {
          ROUTESHAPECOST = atoi(value.c_str());
          ++readParamCnt;
        } else if (field == "clean_patches")
          CLEAN_PATCHES = true;
      }
    }
    fin.close();
  }

  if (readParamCnt < 2) {
    logger_->error(DRT, 1, "Error reading param file: {}.", fileName);
  }
}

void TritonRoute::addUserSelectedVia(const std::string& viaName)
{
  if (db_->getChip() == nullptr || db_->getChip()->getBlock() == nullptr
      || db_->getTech() == nullptr) {
    logger_->error(DRT, 610, "Load design before setting default vias");
  }
  auto block = db_->getChip()->getBlock();
  auto tech = db_->getTech();
  if (tech->findVia(viaName.c_str()) == nullptr
      && block->findVia(viaName.c_str()) == nullptr) {
    logger_->error(utl::DRT, 611, "Via {} not found", viaName);
  } else {
    design_->addUserSelectedVia(viaName);
  }
}

void TritonRoute::setParams(const ParamStruct& params)
{
  GUIDE_FILE = params.guideFile;
  OUTGUIDE_FILE = params.outputGuideFile;
  OUT_MAZE_FILE = params.outputMazeFile;
  DRC_RPT_FILE = params.outputDrcFile;
  CMAP_FILE = params.outputCmapFile;
  VERBOSE = params.verbose;
  ENABLE_VIA_GEN = params.enableViaGen;
  DBPROCESSNODE = params.dbProcessNode;
  CLEAN_PATCHES = params.cleanPatches;
  NO_PA = params.noPa;
  if (!params.viaInPinBottomLayer.empty()) {
    VIAINPIN_BOTTOMLAYER_NAME = params.viaInPinBottomLayer;
  }
  if (!params.viaInPinTopLayer.empty()) {
    VIAINPIN_TOPLAYER_NAME = params.viaInPinTopLayer;
  }
  if (params.drouteEndIter >= 0) {
    END_ITERATION = params.drouteEndIter;
  }
  OR_SEED = params.orSeed;
  OR_K = params.orK;
  if (!params.bottomRoutingLayer.empty()) {
    BOTTOM_ROUTING_LAYER_NAME = params.bottomRoutingLayer;
  }
  if (!params.topRoutingLayer.empty()) {
    TOP_ROUTING_LAYER_NAME = params.topRoutingLayer;
  }
}

void TritonRoute::addWorkerResults(const std::vector<std::pair<int, std::string>>& results)
{
  std::unique_lock<std::mutex> lock(results_mutex_);
  workers_results_.insert(workers_results_.end(), results.begin(), results.end());
  results_sz_ = workers_results_.size();
}

bool TritonRoute::getWorkerResults(std::vector<std::pair<int, std::string>>& results)
{
  std::unique_lock<std::mutex> lock(results_mutex_);
  if(workers_results_.empty())
    return false;
  results = workers_results_;
  workers_results_.clear();
  results_sz_ = 0;
  return true;
}

int TritonRoute::getWorkerResultsSize()
{
  return results_sz_;
}
