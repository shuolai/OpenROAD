/////////////////////////////////////////////////////////////////////////////
//
// BSD 3-Clause License
//
// Copyright (c) 2019, The Regents of the University of California
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
///////////////////////////////////////////////////////////////////////////////

#include "cts/TritonCTS.h"
#include "Clock.h"
#include "CtsOptions.h"
#include "HTreeBuilder.h"
#include "PostCtsOpt.h"
#include "TechChar.h"
#include "TreeBuilder.h"
#include "LevelBalancer.h"

#include "odb/db.h"
#include "odb/dbShape.h"
#include "utl/Logger.h"
#include "sta/Sdc.hh"
#include "sta/Liberty.hh"
#include "db_sta/dbNetwork.hh"
#include "db_sta/dbSta.hh"

#include <chrono>
#include <ctime>
#include <fstream>
#include <unordered_set>
#include <iterator>

namespace cts {

using utl::CTS;

void TritonCTS::init(ord::OpenRoad* openroad)
{
  _openroad = openroad;
  _logger = openroad->getLogger();
  _db = _openroad->getDb();
  _network = _openroad->getDbNetwork();
  _openSta = _openroad->getSta();

  _options = new CtsOptions(_logger, openroad->getSteinerTreeBuilder());
  _techChar = new TechChar(_options,
                           _openroad,
                           _db,
                           _openSta,
                           _openroad->getResizer(),
                           _network,
                           _logger);
  _builders = new std::vector<TreeBuilder*>;
}

TritonCTS::~TritonCTS()
{
  delete _options;
  delete _techChar;
  delete _builders;
}

void TritonCTS::runTritonCts()
{
  setupCharacterization();
  findClockRoots();
  populateTritonCTS();
  if (_builders->empty()) {
    _logger->warn(CTS, 82, "No valid clock nets in the design.");
  } else {
    checkCharacterization();
    buildClockTrees();
    if (_options->runPostCtsOpt()) {
      runPostCtsOpt();
    }
    writeDataToDb();
  }
}

void TritonCTS::addBuilder(TreeBuilder* builder)
{
  _builders->push_back(builder);
}

void TritonCTS::setupCharacterization()
{
  // A new characteriztion is always created.
  _techChar->create();

  // Also resets metrics everytime the setup is done
  _options->setNumSinks(0);
  _options->setNumBuffersInserted(0);
  _options->setNumClockRoots(0);
  _options->setNumClockSubnets(0);
}

void TritonCTS::checkCharacterization()
{
  std::unordered_set<std::string> visitedMasters;
  _techChar->forEachWireSegment([&](unsigned idx, const WireSegment& wireSeg) {
    for (int buf = 0; buf < wireSeg.getNumBuffers(); ++buf) {
      std::string master = wireSeg.getBufferMaster(buf);
      if (visitedMasters.count(master) == 0) {
        if (masterExists(master)) {
          visitedMasters.insert(master);
        } else {
          _logger->error(CTS, 81, "Buffer {} is not in the loaded DB.", master);
        }
      }
    }
  });

  _logger->info(CTS, 97, "Characterization used {} buffer(s) types.",
                   visitedMasters.size());
}

void TritonCTS::findClockRoots()
{
  if (_options->getClockNets() != "") {
    _logger->info(CTS, 1, "Running TritonCTS with user-specified clock roots: {}.", _options->getClockNets());
  }
}

void TritonCTS::buildClockTrees()
{
  for (TreeBuilder* builder : *_builders) {
    builder->setTechChar(*_techChar);
    builder->run();
  }

  if (_options->getBalanceLevels()) {
    for (TreeBuilder* builder : *_builders) {
      if (!builder->getParent() && builder->getChildren().size()) {
        LevelBalancer balancer(builder, _options, _logger);
        balancer.run();
      }
    }
  }
}

void TritonCTS::runPostCtsOpt()
{
  if (_options->runPostCtsOpt()) {
    for (TreeBuilder* builder : *_builders) {
      PostCtsOpt opt(builder, _options, _techChar, _logger);
      opt.run();
    }
  }
}

void TritonCTS::initOneClockTree(odb::dbNet* driverNet, std::string sdcClockName, TreeBuilder* parent)
{
  TreeBuilder* clockBuilder = initClock(driverNet, sdcClockName, parent);
  visitedClockNets.insert(driverNet);
  odb::dbITerm* driver = driverNet->getFirstOutput();
  odb::dbSet<odb::dbITerm> iterms = driverNet->getITerms();
  for (odb::dbITerm* iterm : iterms) {
    if (iterm != driver && iterm->isInputSignal()) {
      if (!isSink(iterm) && (iterm->getInst()->isCore() || iterm->getInst()->isPad())) { // Clock Gate
        odb::dbITerm* outputPin = getSingleOutput(iterm->getInst(), iterm);
        if (outputPin && outputPin->getNet()) {
          odb::dbNet* outputNet = outputPin->getNet();
          if (visitedClockNets.find(outputNet) == visitedClockNets.end())
            initOneClockTree(outputNet, sdcClockName, clockBuilder);
        }
      }
    }
  }
}

void TritonCTS::countSinksPostDbWrite(TreeBuilder* builder, odb::dbNet* net,
                                      unsigned &sinks, unsigned &leafSinks,
                                      unsigned currWireLength, double &sinkWireLength,
                                      int& minDepth, int& maxDepth, int depth,
                                      bool fullTree)
{
  odb::dbSet<odb::dbITerm> iterms = net->getITerms();
  int driverX = 0;
  int driverY = 0;
  for (odb::dbITerm* iterm : iterms) {
    if (iterm->getIoType() != odb::dbIoType::INPUT) {
      iterm->getAvgXY(&driverX, &driverY);
      break;
    }
  }
  odb::dbSet<odb::dbBTerm> bterms = net->getBTerms();
  for (odb::dbBTerm* bterm : bterms) {
    if (bterm->getIoType() == odb::dbIoType::INPUT) {
      for (odb::dbBPin* pin : bterm->getBPins()) {
        odb::dbPlacementStatus status = pin->getPlacementStatus();
        if (status == odb::dbPlacementStatus::NONE
            || status == odb::dbPlacementStatus::UNPLACED) {
          continue;
        }
        for (odb::dbBox* box : pin->getBoxes()) {
          if (box) {
            driverX = box->xMin();
            driverY = box->yMin();
            break;
          }
        }
        break;
      }
    }
  }
  for (odb::dbITerm* iterm : iterms) {
    if (iterm->getIoType() == odb::dbIoType::INPUT) {
      std::string name = iterm->getInst()->getName();
      int receiverX, receiverY;
      iterm->getAvgXY(&receiverX, &receiverY);
      unsigned dist = abs(driverX - receiverX) + abs(driverY - receiverY);
      bool terminate = fullTree ? isSink(iterm) :
        !builder->isAnyTreeBuffer(getClockFromInst(iterm->getInst()));
      if (!terminate) {
        odb::dbITerm* outputPin = iterm->getInst()->getFirstOutput();
        if (outputPin)
          countSinksPostDbWrite(builder, outputPin->getNet(), sinks, leafSinks, (currWireLength + dist),
                                sinkWireLength, minDepth, maxDepth, depth+1, fullTree);
        else {
          _logger->report("Hanging buffer {}", name);
        }
        if (builder->isLeafBuffer(getClockFromInst(iterm->getInst())))
          leafSinks++;
      } else {
        sinks++;
        double currSinkWl = (dist + currWireLength)/double(_options->getDbUnits());
        sinkWireLength += currSinkWl;
        if (depth > maxDepth)
          maxDepth = depth;
        if ((minDepth > 0 && depth < minDepth) || (minDepth == 0))
          minDepth = depth;
      }
    }
  } // ignoring block pins/feedthrus
}

ClockInst* TritonCTS::getClockFromInst(odb::dbInst* inst)
{
  return (inst2clkbuf.find(inst) != inst2clkbuf.end())
    ? inst2clkbuf[inst] : nullptr;
}

void TritonCTS::writeDataToDb()
{
  for (TreeBuilder* builder : *_builders) {
    writeClockNetsToDb(builder->getClock());
  }

  for (TreeBuilder* builder : *_builders) {
    odb::dbNet* topClockNet = builder->getClock().getNetObj();
    unsigned sinkCount = 0;
    unsigned leafSinks = 0;
    double allSinkDistance = 0.0;
    int minDepth = 0;
    int maxDepth = 0;
    bool reportFullTree = !builder->getParent() && builder->getChildren().size() && _options->getBalanceLevels();
    countSinksPostDbWrite(builder, topClockNet, sinkCount, leafSinks, 0, allSinkDistance,
                          minDepth, maxDepth, 0, reportFullTree);
    _logger->info(CTS, 98, "Clock net \"{}\"", builder->getClock().getName());
    _logger->info(CTS, 99, " Sinks {}", sinkCount);
    _logger->info(CTS, 100, " Leaf buffers {}", leafSinks);
    double avgWL = allSinkDistance/sinkCount;
    _logger->info(CTS, 101, " Average sink wire length {:.3} um", avgWL);
    _logger->info(CTS, 102, " Path depth {} - {}", minDepth, maxDepth);
  }
}

void TritonCTS::forEachBuilder(
    const std::function<void(const TreeBuilder*)> func) const
{
  for (const TreeBuilder* builder : *_builders) {
    func(builder);
  }
}

void TritonCTS::reportCtsMetrics()
{
  std::string filename = _options->getMetricsFile();

  if (filename != "") {
    std::ofstream file(filename.c_str());

    if (!file.is_open()) {
      _logger->error(CTS, 87, "Could not open output metric file {}.", filename.c_str());
    }

    file << "Total number of Clock Roots: " << _options->getNumClockRoots() << ".\n";
    file << "Total number of Buffers Inserted: " << _options->getNumBuffersInserted() << ".\n";
    file << "Total number of Clock Subnets: " << _options->getNumClockSubnets() << ".\n";
    file << "Total number of Sinks: " << _options->getNumSinks() << ".\n";
    file.close();

  } else {
    _logger->info(CTS, 3, "Total number of Clock Roots: {}.", _options->getNumClockRoots());
    _logger->info(CTS, 4, "Total number of Buffers Inserted: {}.", _options->getNumBuffersInserted());
    _logger->info(CTS, 5, "Total number of Clock Subnets: {}.", _options->getNumClockSubnets());
    _logger->info(CTS, 6, "Total number of Sinks: {}.", _options->getNumSinks());
  }
}

int TritonCTS::setClockNets(const char* names)
{
  odb::dbDatabase* db = _openroad->getDb();
  odb::dbChip* chip = db->getChip();
  odb::dbBlock* block = chip->getBlock();

  _options->setClockNets(names);
  std::stringstream ss(names);
  std::istream_iterator<std::string> begin(ss);
  std::istream_iterator<std::string> end;
  std::vector<std::string> nets(begin, end);

  std::vector<odb::dbNet*> netObjects;

  for (std::string name : nets) {
    odb::dbNet* net = block->findNet(name.c_str());
    bool netFound = false;
    if (net != nullptr) {
      // Since a set is unique, only the nets not found by dbSta are added.
      netObjects.push_back(net);
      netFound = true;
    } else {
      // User input was a pin, transform it into an iterm if possible
      odb::dbITerm* iterm = block->findITerm(name.c_str());
      if (iterm != nullptr) {
        net = iterm->getNet();
        if (net != nullptr) {
          // Since a set is unique, only the nets not found by dbSta are added.
          netObjects.push_back(net);
          netFound = true;
        }
      }
    }
    if (!netFound) {
      return 1;
    }
  }
  _options->setClockNetsObjs(netObjects);
  return 0;
}

void TritonCTS::setBufferList(const char* buffers)
{
  std::stringstream ss(buffers);
  std::istream_iterator<std::string> begin(ss);
  std::istream_iterator<std::string> end;
  std::vector<std::string> bufferVector(begin, end);
  _options->setBufferList(bufferVector);
}

// db functions

void TritonCTS::populateTritonCTS()
{
  _block = _db->getChip()->getBlock();
  _options->setDbUnits(_block->getDbUnitsPerMicron());

  clearNumClocks();

  // Use dbSta to find all clock nets in the design.
  std::vector<std::pair<std::set<odb::dbNet*>, std::string>> clockNetsInfo;

  // Checks the user input in case there are other nets that need to be added to
  // the set.
  std::vector<odb::dbNet*> inputClkNets = _options->getClockNetsObjs();

  if (!inputClkNets.empty()) {
    std::set<odb::dbNet *> clockNets;
    for (odb::dbNet* net : inputClkNets) {
      // Since a set is unique, only the nets not found by dbSta are added.
      clockNets.insert(net);
    }
    clockNetsInfo.emplace_back(std::make_pair(clockNets, std::string("")));
  } else {
    staClockNets = _openSta->findClkNets();
    sta::Sdc *sdc = _openSta->sdc();
    for (auto clk : *sdc->clocks()) {
      std::string clkName = clk->name();
      std::set<odb::dbNet*> clkNets;
      findClockRoots(clk, clkNets);
      clockNetsInfo.emplace_back(make_pair(clkNets, clkName));
    }
  }

  // Iterate over all the nets found by the user-input and dbSta
  for (auto clockInfo : clockNetsInfo) {
     std::set<odb::dbNet *> clockNets = clockInfo.first;
     std::string clkName = clockInfo.second;
    for (odb::dbNet* net : clockNets) {
      if (net != nullptr) {
        if (clkName == "")
          _logger->info(CTS, 95, "Net \"{}\" found.", net->getName());
        else
          _logger->info(CTS, 7, "Net \"{}\" found for clock \"{}\".", net->getName(), clkName);
        // Initializes the net in TritonCTS. If the number of sinks is less than
        // 2, the net is discarded.
        initOneClockTree(net, clkName, nullptr);
      } else {
        _logger->warn(CTS, 40, "Net was not found in the design for {}, please check. Skipping...", clkName);
      }
    }
  }

  if (getNumClocks() == 0) {
    _logger->warn(CTS, 83, "No clock nets have been found.");
  }

  _logger->info(CTS, 8, "TritonCTS found {} clock nets.", getNumClocks());
  _options->setNumClockRoots(getNumClocks());
}

TreeBuilder* TritonCTS::initClock(odb::dbNet* net, std::string sdcClock, TreeBuilder* parentBuilder)
{
  std::string driver = "";
  odb::dbITerm* iterm = net->getFirstOutput();
  int xPin, yPin;
  if (iterm == nullptr) {
    odb::dbBTerm* bterm = net->get1stBTerm();  // Clock pin
    driver = bterm->getConstName();
    bterm->getFirstPinLocation(xPin, yPin);
  } else {
    odb::dbInst* inst = iterm->getInst();
    odb::dbMTerm* mterm = iterm->getMTerm();
    std::string driver = std::string(inst->getConstName()) + "/"
                         + std::string(mterm->getConstName());
    int xTmp, yTmp;
    computeITermPosition(iterm, xTmp, yTmp);
    xPin = xTmp;
    yPin = yTmp;
  }

  // Initialize clock net
  Clock clockNet(net->getConstName(), driver, sdcClock, xPin, yPin);
  clockNet.setDriverPin(iterm);

  for (odb::dbITerm* iterm : net->getITerms()) {
    odb::dbInst* inst = iterm->getInst();

    if (iterm->isInputSignal() && inst->isPlaced()) {
      odb::dbMTerm* mterm = iterm->getMTerm();
      std::string name = std::string(inst->getConstName()) + "/"
                         + std::string(mterm->getConstName());
      int x, y;
      computeITermPosition(iterm, x, y);
      clockNet.addSink(name, x, y, iterm, getInputPinCap(iterm));
    }
  }

  if (clockNet.getNumSinks() < 2) {
    _logger->warn(CTS, 41, "Net \"{}\" has {} sinks. Skipping...",
                  clockNet.getName(), clockNet.getNumSinks());
    return nullptr;
  } else {
    if (clockNet.getNumSinks() == 0) {
      _logger->warn(CTS, 42, "Net \"{}\" has no sinks. Skipping...", clockNet.getName());
      return nullptr;
    }
  }

  _logger->info(CTS, 10, " Clock net \"{}\" has {} sinks.", net->getConstName(), clockNet.getNumSinks());

  long int totalSinks = _options->getNumSinks() + clockNet.getNumSinks();
  _options->setNumSinks(totalSinks);

  incrementNumClocks();

  clockNet.setNetObj(net);
  HTreeBuilder* builder = new HTreeBuilder(_options, clockNet, parentBuilder, _logger);
  addBuilder(builder);
  return builder;
}

void TritonCTS::parseClockNames(std::vector<std::string>& clockNetNames) const
{
  std::stringstream allNames(_options->getClockNets());

  std::string tmpName = "";
  while (allNames >> tmpName) {
    clockNetNames.push_back(tmpName);
  }

  unsigned numClocks = clockNetNames.size();
  _logger->info(CTS, 11, " Number of user-input clocks: {}.", numClocks);

  if (numClocks > 0) {
    std::string rpt = " (";
    for (const std::string& name : clockNetNames) {
      rpt = rpt + " \"" + name + "\"";
    }
    rpt = rpt + " )";
    _logger->report("{}", rpt);
  }
}

void TritonCTS::computeITermPosition(odb::dbITerm* term, int& x, int& y) const
{
  odb::dbITermShapeItr itr;

  odb::dbShape shape;
  x = 0;
  y = 0;
  unsigned numShapes = 0;
  for (itr.begin(term); itr.next(shape);) {
    if (!shape.isVia()) {
      x += shape.xMin() + (shape.xMax() - shape.xMin()) / 2;
      y += shape.yMin() + (shape.yMax() - shape.yMin()) / 2;
      ++numShapes;
    }
  }
  if (numShapes > 0) {
    x /= numShapes;
    y /= numShapes;
  }
};

void TritonCTS::writeClockNetsToDb(Clock& clockNet)
{
  odb::dbNet* topClockNet = clockNet.getNetObj();

  disconnectAllSinksFromNet(topClockNet);

  createClockBuffers(clockNet);

  // connect top buffer on the clock pin
  std::string topClockInstName = "clkbuf_0_" + clockNet.getName();
  odb::dbInst* topClockInst = _block->findInst(topClockInstName.c_str());
  odb::dbITerm* topClockInstInputPin = getFirstInput(topClockInst);
  odb::dbITerm::connect(topClockInstInputPin, topClockNet);
  topClockNet->setSigType(odb::dbSigType::CLOCK);

  std::map<int, uint> fanoutcount;

  // create subNets
  _numClkNets = 0;
  _numFixedNets = 0;
  const Clock::SubNet* rootSubNet = nullptr;
  clockNet.forEachSubNet([&](const Clock::SubNet& subNet) {
    bool outputPinFound = true;
    bool inputPinFound = true;
    bool leafLevelNet = subNet.isLeafLevel();
    if (("clknet_0_" + clockNet.getName()) == subNet.getName()) {
      rootSubNet = &subNet;
    }
    odb::dbNet* clkSubNet
        = odb::dbNet::create(_block, subNet.getName().c_str());
    ++_numClkNets;
    clkSubNet->setSigType(odb::dbSigType::CLOCK);

    odb::dbInst* driver = subNet.getDriver()->getDbInst();
    odb::dbITerm* driverInputPin = getFirstInput(driver);
    odb::dbNet* inputNet = driverInputPin->getNet();
    odb::dbITerm* outputPin = driver->getFirstOutput();
    if (outputPin == nullptr) {
      outputPinFound = false;
    }
    odb::dbITerm::connect(outputPin, clkSubNet);

    if (subNet.getNumSinks() == 0) {
      inputPinFound = false;
    }

    subNet.forEachSink([&](ClockInst* inst) {
      odb::dbITerm* inputPin = nullptr;
      if (inst->isClockBuffer()) {
        odb::dbInst* sink = inst->getDbInst();
        inputPin = getFirstInput(sink);
      } else {
        inputPin = inst->getDbInputPin();
      }
      if (inputPin == nullptr) {
        inputPinFound = false;
      } else {
        if (!inputPin->getInst()->isPlaced()) {
          inputPinFound = false;
        }
      }
      odb::dbITerm::connect(inputPin, clkSubNet);
    });

    if (leafLevelNet) {
      // Report fanout values only for sink nets
      if (fanoutcount.find(subNet.getNumSinks()) == fanoutcount.end()) {
        fanoutcount[subNet.getNumSinks()] = 0;
      }
      fanoutcount[subNet.getNumSinks()] = fanoutcount[subNet.getNumSinks()] + 1;
    }

    if (!inputPinFound || !outputPinFound) {
      // Net not fully connected. Removing it.
      disconnectAllPinsFromNet(clkSubNet);
      odb::dbNet::destroy(clkSubNet);
      _numFixedNets++;
      --_numClkNets;
      odb::dbInst::destroy(driver);
      checkUpstreamConnections(inputNet);
    }
  });

  if (_options->writeOnlyClockNets()) {
    removeNonClockNets();
  }

  int minPath = std::numeric_limits<int>::max();
  int maxPath = std::numeric_limits<int>::min();
  rootSubNet->forEachSink([&](ClockInst* inst) {
    if (inst->isClockBuffer()) {
      std::pair<int, int> resultsForBranch
          = branchBufferCount(inst, 1, clockNet);
      if (resultsForBranch.first < minPath) {
        minPath = resultsForBranch.first;
      }
      if (resultsForBranch.second > maxPath) {
        maxPath = resultsForBranch.second;
      }
    }
  });

  _logger->info(CTS, 12, "    Minimum number of buffers in the clock path: {}.", minPath);
  _logger->info(CTS, 13, "    Maximum number of buffers in the clock path: {}.", maxPath);

  if (_numFixedNets > 0) {
    _logger->info(CTS, 14, "    {} clock nets were removed/fixed.", _numFixedNets);
  }

  _logger->info(CTS, 15, "    Created {} clock nets.", _numClkNets);
  long int totalNets = _options->getNumClockSubnets() + _numClkNets;
  _options->setNumClockSubnets(totalNets);

  std::string fanout = "";
  for (auto const& x : fanoutcount) {
    fanout += std::to_string(x.first) + ':' + std::to_string(x.second) + ", ";
  }

  _logger->info(CTS,
                16,
                "    Fanout distribution for the current clock = {}.",
                fanout.substr(0, fanout.size() - 2) + ".");
  _logger->info(CTS, 17, "    Max level of the clock tree: {}.", clockNet.getMaxLevel());
}

std::pair<int, int> TritonCTS::branchBufferCount(ClockInst* inst,
                                                 int bufCounter,
                                                 Clock& clockNet)
{
  odb::dbInst* sink = inst->getDbInst();
  odb::dbITerm* outITerm = sink->getFirstOutput();
  int minPath = std::numeric_limits<int>::max();
  int maxPath = std::numeric_limits<int>::min();
  for (odb::dbITerm* sinkITerms : outITerm->getNet()->getITerms()) {
    if (sinkITerms != outITerm) {
      ClockInst* clockInst
          = clockNet.findClockByName(sinkITerms->getInst()->getName());
      if (clockInst == nullptr) {
        int newResult = bufCounter + 1;
        if (newResult > maxPath) {
          maxPath = newResult;
        }
        if (newResult < minPath) {
          minPath = newResult;
        }
      } else {
        std::pair<int, int> newResults
            = branchBufferCount(clockInst, bufCounter + 1, clockNet);
        if (newResults.first < minPath) {
          minPath = newResults.first;
        }
        if (newResults.second > maxPath) {
          maxPath = newResults.second;
        }
      }
    }
  }
  std::pair<int, int> results(minPath, maxPath);
  return results;
}

void TritonCTS::disconnectAllSinksFromNet(odb::dbNet* net)
{
  odb::dbSet<odb::dbITerm> iterms = net->getITerms();
  for (odb::dbITerm* iterm : iterms) {
    if (iterm->getIoType() == odb::dbIoType::INPUT) {
      odb::dbITerm::disconnect(iterm);
    }
  }
}

void TritonCTS::disconnectAllPinsFromNet(odb::dbNet* net)
{
  odb::dbSet<odb::dbITerm> iterms = net->getITerms();
  for (odb::dbITerm* iterm : iterms) {
    odb::dbITerm::disconnect(iterm);
  }
}

void TritonCTS::checkUpstreamConnections(odb::dbNet* net)
{
  while (net->getITermCount() <= 1) {
    // Net is incomplete, only 1 pin.
    odb::dbITerm* firstITerm = net->get1stITerm();
    if (firstITerm == nullptr) {
      disconnectAllPinsFromNet(net);
      odb::dbNet::destroy(net);
      break;
    } else {
      odb::dbInst* bufferInst = firstITerm->getInst();
      odb::dbITerm* driverInputPin = getFirstInput(bufferInst);
      disconnectAllPinsFromNet(net);
      odb::dbNet::destroy(net);
      net = driverInputPin->getNet();
      ++_numFixedNets;
      --_numClkNets;
      odb::dbInst::destroy(bufferInst);
    }
  }
}

void TritonCTS::createClockBuffers(Clock& clockNet)
{
  unsigned numBuffers = 0;
  clockNet.forEachClockBuffer([&](ClockInst& inst) {
    odb::dbMaster* master = _db->findMaster(inst.getMaster().c_str());
    odb::dbInst* newInst
        = odb::dbInst::create(_block, master, inst.getName().c_str());
    inst.setInstObj(newInst);
    inst2clkbuf[newInst] = &inst;
    inst.setInputPinObj(getFirstInput(newInst));
    newInst->setLocation(inst.getX(), inst.getY());
    newInst->setPlacementStatus(odb::dbPlacementStatus::PLACED);
    ++numBuffers;
  });
  _logger->info(CTS, 18, "    Created {} clock buffers.", numBuffers);
  long int totalBuffers = _options->getNumBuffersInserted() + numBuffers;
  _options->setNumBuffersInserted(totalBuffers);
}

odb::dbITerm* TritonCTS::getFirstInput(odb::dbInst* inst) const
{
  odb::dbSet<odb::dbITerm> iterms = inst->getITerms();
  for (odb::dbITerm* iterm : iterms) {
    if (iterm->isInputSignal()) {
      return iterm;
    }
  }

  return nullptr;
}

odb::dbITerm* TritonCTS::getSingleOutput(odb::dbInst* inst, odb::dbITerm* input) const
{
  odb::dbSet<odb::dbITerm> iterms = inst->getITerms();
  odb::dbITerm *output = nullptr;
  for (odb::dbITerm* iterm : iterms) {
    if (iterm != input && iterm->isOutputSignal()) {
      odb::dbNet* net = iterm->getNet();
      if (net) {
        if (staClockNets.find(net) != staClockNets.end()) {
          output = iterm;
          break;
        }
      }
    }
  }
  return output;

}
bool TritonCTS::masterExists(const std::string& master) const
{
  return _db->findMaster(master.c_str());
};

void TritonCTS::removeNonClockNets()
{
  for (odb::dbNet* net : _block->getNets()) {
    if (net->getSigType() != odb::dbSigType::CLOCK) {
      odb::dbNet::destroy(net);
    }
  }
}

void TritonCTS::findClockRoots(sta::Clock* clk,
                               std::set<odb::dbNet*> &clockNets)
{
  for (sta::Pin* pin : clk->leafPins()) {
    odb::dbITerm* instTerm;
    odb::dbBTerm* port;
    _network->staToDb(pin, instTerm, port);
    odb::dbNet* net = instTerm ? instTerm->getNet() : port->getNet();
    clockNets.insert(net);
  }
}

float TritonCTS::getInputPinCap(odb::dbITerm* iterm)
{
  odb::dbInst* inst = iterm->getInst();
  sta::Cell* masterCell = _network->dbToSta(inst->getMaster());
  sta::LibertyCell* libertyCell = _network->libertyCell(masterCell);
  if (!libertyCell) {
    return 0.0;
  }

  sta::LibertyPort *inputPort = libertyCell->findLibertyPort(iterm->getMTerm()->getConstName());
  if (inputPort) {
    return inputPort->capacitance();
  } else {
    return 0.0;
  }
}

bool TritonCTS::isSink(odb::dbITerm* iterm)
{
  odb::dbInst* inst = iterm->getInst();
  sta::Cell* masterCell = _network->dbToSta(inst->getMaster());
  sta::LibertyCell* libertyCell = _network->libertyCell(masterCell);
  if (!libertyCell) {
    return false;
  }

  sta::LibertyPort *inputPort = libertyCell->findLibertyPort(iterm->getMTerm()->getConstName());
  if (inputPort) {
    return inputPort->isRegClk();
  } else {
    return false;
  }
}

}  // namespace cts
