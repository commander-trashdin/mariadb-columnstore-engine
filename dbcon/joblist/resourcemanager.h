/* Copyright (C) 2014 InfiniDB, Inc.
   Copyright (C) 2022 Mariadb Corporation.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA. */

/******************************************************************************************
 * $Id: resourcemanager.h 9655 2013-06-25 23:08:13Z xlou $
 *
 ******************************************************************************************/
/**
 * @file
 */
#pragma once

#include <vector>
#include <iostream>
#include <boost/thread.hpp>
#include <boost/algorithm/string.hpp>
#include <unistd.h>

#include "configcpp.h"
#include "calpontselectexecutionplan.h"
#include "resourcedistributor.h"
#include "installdir.h"
#include "branchpred.h"

#include "atomicops.h"

#if defined(_MSC_VER) && defined(JOBLIST_DLLEXPORT)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

namespace joblist
{
// aggfilterstep
const uint32_t defaultNumThreads = 8;
// joblistfactory
const uint32_t defaultFlushInterval = 8 * 1024;
const uint32_t defaultFifoSize = 10;
const uint64_t defaultHJMaxElems = 512 * 1024;  // hashjoin uses 8192
const int defaultHJMaxBuckets = 32;             // hashjoin uses 4
const uint64_t defaultHJPmMaxMemorySmallSide = 1 * 1024 * 1024 * 1024ULL;
const uint64_t defaultHJUmMaxMemorySmallSide = 4 * 1024 * 1024 * 1024ULL;
const uint64_t defaultTotalUmMemory = 8 * 1024 * 1024 * 1024ULL;
const uint32_t defaultJLThreadPoolSize = 100;

// pcolscan.cpp
const uint32_t defaultScanLbidReqThreshold = 5000;
const uint32_t defaultLogicalBlocksPerScan = 1024;  // added for bug 1264.
const uint32_t defaultScanReceiveThreads = 8;

// BatchPrimitiveStep
const uint32_t defaultRequestSize = 1;
const uint32_t defaultMaxOutstandingRequests = 20;
const uint32_t defaultProcessorThreadsPerScan = 16;
const uint32_t defaultJoinerChunkSize = 16 * 1024 * 1024;

// bucketreuse
const std::string defaultTempDiskPath = "/tmp";

const int defaultEMServerThreads = 50;
const int defaultEMSecondsBetweenMemChecks = 1;
const int defaultEMMaxPct = 95;
const int defaultEMPriority = 21;  // @Bug 3385
const int defaultEMExecQueueSize = 20;

const int defaultPSCount = 0;
const int defaultConnectionsPerPrimProc = 1;
const uint64_t defaultExtentRows = 8 * 1024 * 1024;

// DMLProc
// @bug 1886.  Knocked a 0 off the default below dropping it from 4M down to 256K.  Delete was consuming too
// much memory.
const uint64_t defaultDMLMaxDeleteRows = 256 * 1024;

// Connector
// @bug 2048.  To control batch insert memory usage.
const uint64_t defaultRowsPerBatch = 10000;

/* HJ CP feedback, see bug #1465 */
const uint32_t defaultHjCPUniqueLimit = 100;

const uint64_t defaultDECThrottleThreshold = 200000000;  // ~200 MB

const bool defaultAllowDiskAggregation = false;

/** @brief ResourceManager
 *	Returns requested values from Config
 *
 */
class ResourceManager
{
 public:
  /** @brief ctor
   *
   */
  EXPORT ResourceManager(bool runningInExeMgr = false, config::Config *aConfig = nullptr);
  static ResourceManager* instance(bool runningInExeMgr = false, config::Config *aConfig = nullptr);
  config::Config* getConfig()
  {
    return fConfig;
  }

  /** @brief dtor
   */
  virtual ~ResourceManager()
  {
  }

  typedef std::map<uint32_t, uint64_t> MemMap;

  // @MCOL-513 - Added threadpool to ExeMgr
  int getEmServerThreads() const
  {
    return getIntVal(fExeMgrStr, "ThreadPoolSize", defaultEMServerThreads);
  }
  std::string getExeMgrThreadPoolDebug() const
  {
    return getStringVal(fExeMgrStr, "ThreadPoolDebug", "N");
  }

  int getEmSecondsBetweenMemChecks() const
  {
    return getUintVal(fExeMgrStr, "SecondsBetweenMemChecks", defaultEMSecondsBetweenMemChecks);
  }
  int getEmMaxPct() const
  {
    return getUintVal(fExeMgrStr, "MaxPct", defaultEMMaxPct);
  }
  EXPORT int getEmPriority() const;   // FOr Windows only
  int getEmExecQueueSize() const
  {
    return getIntVal(fExeMgrStr, "ExecQueueSize", defaultEMExecQueueSize);
  }

  bool getAllowDiskAggregation() const
  {
    return fAllowedDiskAggregation;
  }

  uint64_t getDECConnectionsPerQuery() const
  {
    return fDECConnectionsPerQuery;
  }

  int getHjMaxBuckets() const
  {
    return getUintVal(fHashJoinStr, "MaxBuckets", defaultHJMaxBuckets);
  }
  unsigned getHjNumThreads() const
  {
    return fHjNumThreads;
  }  // getUintVal(fHashJoinStr, "NumThreads", defaultNumThreads); }
  uint64_t getHjMaxElems() const
  {
    return getUintVal(fHashJoinStr, "MaxElems", defaultHJMaxElems);
  }
  uint32_t getHjCPUniqueLimit() const
  {
    return getUintVal(fHashJoinStr, "CPUniqueLimit", defaultHjCPUniqueLimit);
  }
  uint64_t getPMJoinMemLimit() const
  {
    return pmJoinMemLimit;
  }

  uint32_t getJLFlushInterval() const
  {
    return getUintVal(fJobListStr, "FlushInterval", defaultFlushInterval);
  }
  uint32_t getJlFifoSize() const
  {
    return getUintVal(fJobListStr, "FifoSize", defaultFifoSize);
  }
  uint32_t getJlScanLbidReqThreshold() const
  {
    return getUintVal(fJobListStr, "ScanLbidReqThreshold", defaultScanLbidReqThreshold);
  }

  // @MCOL-513 - Added threadpool to JobSteps
  int getJLThreadPoolSize() const
  {
    return getIntVal(fJobListStr, "ThreadPoolSize", defaultJLThreadPoolSize);
  }
  std::string getJlThreadPoolDebug() const
  {
    return getStringVal(fJobListStr, "ThreadPoolDebug", "N");
  }
  std::string getDMLJlThreadPoolDebug() const
  {
    return getStringVal(fJobListStr, "DMLThreadPoolDebug", "N");
  }

  // @bug 1264 - Added LogicalBlocksPerScan configurable which determines the number of blocks contained in
  // each BPS scan request.
  uint32_t getJlLogicalBlocksPerScan() const
  {
    return getUintVal(fJobListStr, "LogicalBlocksPerScan", defaultLogicalBlocksPerScan);
  }
  uint32_t getJlNumScanReceiveThreads() const
  {
    return fJlNumScanReceiveThreads;
  }  // getUintVal(fJobListStr, "NumScanReceiveThreads", defaultScanReceiveThreads); }

  // @bug 1424,1298
  uint32_t getJlProcessorThreadsPerScan() const
  {
    return fJlProcessorThreadsPerScan;
  }  // getUintVal(fJobListStr,"ProcessorThreadsPerScan", defaultProcessorThreadsPerScan); }
  uint32_t getJlRequestSize() const
  {
    return getUintVal(fJobListStr, "RequestSize", defaultRequestSize);
  }
  uint32_t getJlMaxOutstandingRequests() const
  {
    return fJlMaxOutstandingRequests;
    // getUintVal(fJobListStr, "MaxOutstandingRequests", defaultMaxOutstandingRequests);
  }
  uint32_t getJlJoinerChunkSize() const
  {
    return getUintVal(fJobListStr, "JoinerChunkSize", defaultJoinerChunkSize);
  }

  int getPsCount() const
  {
    return getUintVal(fPrimitiveServersStr, "Count", defaultPSCount);
  }
  int getPsConnectionsPerPrimProc() const
  {
    return getUintVal(fPrimitiveServersStr, "ConnectionsPerPrimProc", defaultConnectionsPerPrimProc);
  }
  std::string getScTempDiskPath() const
  {
    return startup::StartUp::tmpDir();
  }
  std::string getScWorkingDir() const
  {
    return startup::StartUp::tmpDir();
  }

  uint64_t getExtentRows() const
  {
    return getUintVal(fExtentMapStr, "ExtentRows", defaultExtentRows);
  }

  uint32_t getDBRootCount() const
  {
    return getUintVal(fSystemConfigStr, "DBRootCount", 1);
  }
  uint32_t getPMCount() const
  {
    return getUintVal(fPrimitiveServersStr, "Count", 1);
  }

  uint64_t getDMLMaxDeleteRows() const
  {
    return getUintVal(fDMLProcStr, "MaxDeleteRows", defaultDMLMaxDeleteRows);
  }

  uint64_t getRowsPerBatch() const
  {
    return getUintVal(fBatchInsertStr, "RowsPerBatch", defaultRowsPerBatch);
  }

  uint64_t getDECThrottleThreshold() const
  {
    return getUintVal(fJobListStr, "DECThrottleThreshold", defaultDECThrottleThreshold);
  }

  EXPORT void emServerThreads();
  EXPORT void emServerQueueSize();
  EXPORT void emSecondsBetweenMemChecks();
  EXPORT void emMaxPct();
  EXPORT void emPriority();
  EXPORT void emExecQueueSize();

  EXPORT void hjNumThreads();
  EXPORT void hjMaxBuckets();
  EXPORT void hjMaxElems();
  EXPORT void hjFifoSizeLargeSide();
  EXPORT void hjPmMaxMemorySmallSide();

  /* new HJ/Union/Aggregation mem interface, used by TupleBPS */
  /* sessionLimit is a pointer to the var holding the session-scope limit, should be JobInfo.umMemLimit
     for the query. */
  /* Temporary parameter 'patience', will wait for up to 10s to get the memory. */
  EXPORT bool getMemory(int64_t amount, boost::shared_ptr<int64_t> sessionLimit, bool patience = true);
  inline void returnMemory(int64_t amount, boost::shared_ptr<int64_t> sessionLimit)
  {
    atomicops::atomicAdd(&totalUmMemLimit, amount);
    atomicops::atomicAdd(sessionLimit.get(), amount);
  }
  inline int64_t availableMemory() const
  {
    return totalUmMemLimit;
  }

  /* old HJ mem interface, used by HashJoin */
  uint64_t getHjPmMaxMemorySmallSide(uint32_t sessionID)
  {
    return fHJPmMaxMemorySmallSideSessionMap.getSessionResource(sessionID);
  }
  uint64_t getHjUmMaxMemorySmallSide(uint32_t sessionID)
  {
    return fHJUmMaxMemorySmallSideDistributor.getSessionResource(sessionID);
  }
  uint64_t getHjTotalUmMaxMemorySmallSide() const
  {
    return fHJUmMaxMemorySmallSideDistributor.getTotalResource();
  }

  EXPORT void addHJUmMaxSmallSideMap(uint32_t sessionID, uint64_t mem);

  void removeHJUmMaxSmallSideMap(uint32_t sessionID)
  {
    fHJUmMaxMemorySmallSideDistributor.removeSession(sessionID);
  }

  EXPORT void addHJPmMaxSmallSideMap(uint32_t sessionID, uint64_t mem);
  void removeHJPmMaxSmallSideMap(uint32_t sessionID)
  {
    fHJPmMaxMemorySmallSideSessionMap.removeSession(sessionID);
  }

  void removeSessionMaps(uint32_t sessionID)
  {
    fHJPmMaxMemorySmallSideSessionMap.removeSession(sessionID);
    fHJUmMaxMemorySmallSideDistributor.removeSession(sessionID);
  }

  uint64_t requestHJMaxMemorySmallSide(uint32_t sessionID, uint64_t amount)
  {
    return fHJUmMaxMemorySmallSideDistributor.requestResource(sessionID, amount);
  }

  uint64_t requestHJUmMaxMemorySmallSide(uint32_t sessionID)
  {
    return fHJUmMaxMemorySmallSideDistributor.requestResource(sessionID);
  }
  void returnHJUmMaxMemorySmallSide(uint64_t mem)
  {
    fHJUmMaxMemorySmallSideDistributor.returnResource(mem);
  }

  void setTraceFlags(uint32_t flags)
  {
    fTraceFlags = flags;
    fHJUmMaxMemorySmallSideDistributor.setTrace(
        ((fTraceFlags & execplan::CalpontSelectExecutionPlan::TRACE_RESRCMGR) != 0));
  }
  bool rmtraceOn() const
  {
    return ((fTraceFlags & execplan::CalpontSelectExecutionPlan::TRACE_RESRCMGR) != 0);
  }

  void numCores(unsigned numCores)
  {
    fNumCores = numCores;
  }
  unsigned numCores() const
  {
    return fNumCores;
  }

  void aggNumThreads(uint32_t numThreads)
  {
    fAggNumThreads = numThreads;
  }
  uint32_t aggNumThreads() const
  {
    return fAggNumThreads;
  }

  void aggNumBuckets(uint32_t numBuckets)
  {
    fAggNumBuckets = numBuckets;
  }
  uint32_t aggNumBuckets() const
  {
    return fAggNumBuckets;
  }

  void aggNumRowGroups(uint32_t numRowGroups)
  {
    fAggNumRowGroups = numRowGroups;
  }
  uint32_t aggNumRowGroups() const
  {
    return fAggNumRowGroups;
  }

  void windowFunctionThreads(uint32_t n)
  {
    fWindowFunctionThreads = n;
  }
  uint32_t windowFunctionThreads() const
  {
    return fWindowFunctionThreads;
  }

  bool useHdfs() const
  {
    return fUseHdfs;
  }

  EXPORT bool getMysqldInfo(std::string& h, std::string& u, std::string& w, unsigned int& p) const;
  EXPORT bool queryStatsEnabled() const;
  EXPORT bool userPriorityEnabled() const;

  uint64_t getConfiguredUMMemLimit() const
  {
    return configuredUmMemLimit;
  }

 private:
  void logResourceChangeMessage(logging::LOG_TYPE logType, uint32_t sessionID, uint64_t newvalue,
                                uint64_t value, const std::string& source, logging::Message::MessageID mid);
  /** @brief get name's value from section
   *
   * get name's value from section in the current config file or default value .
   * @param section the name of the config file section to search
   * @param name the param name whose value is to be returned
   * @param defVal the default value returned if the value is missing
   */
  std::string getStringVal(const std::string& section, const std::string& name, const std::string& defVal,
                           const bool reReadConfigIfNeeded = false) const;

  template <typename IntType>
  IntType getUintVal(const std::string& section, const std::string& name, IntType defval) const;

  template <typename IntType>
  IntType getIntVal(const std::string& section, const std::string& name, IntType defval) const;

  bool getBoolVal(const std::string& section, const std::string& name, bool defval) const;

  void logMessage(logging::LOG_TYPE logLevel, logging::Message::MessageID mid, uint64_t value = 0,
                  uint32_t sessionId = 0);

  std::string fExeMgrStr;
  inline static const std::string fHashJoinStr = "HashJoin";
  inline static const std::string fJobListStr = "JobList";
  inline static const std::string fPrimitiveServersStr = "PrimitiveServers";
  /*static	const*/ std::string fSystemConfigStr;
  inline static const std::string fExtentMapStr = "ExtentMap";
  /*static	const*/ std::string fDMLProcStr;
  /*static	const*/ std::string fBatchInsertStr;
  inline static const std::string fOrderByLimitStr = "OrderByLimit";
  inline static const std::string fRowAggregationStr = "RowAggregation";
  config::Config* fConfig;
  static ResourceManager* fInstance;
  uint32_t fTraceFlags;

  unsigned fNumCores;
  unsigned fHjNumThreads;
  uint32_t fJlProcessorThreadsPerScan;
  uint32_t fJlNumScanReceiveThreads;
  uint32_t fJlMaxOutstandingRequests;

  /* old HJ support */
  ResourceDistributor fHJUmMaxMemorySmallSideDistributor;
  LockedSessionMap fHJPmMaxMemorySmallSideSessionMap;

  /* new HJ/Union/Aggregation support */
  volatile int64_t totalUmMemLimit;  // mem limit for join, union, and aggregation on the UM
  uint64_t configuredUmMemLimit;
  uint64_t pmJoinMemLimit;  // mem limit on individual PM joins

  /* multi-thread aggregate */
  uint32_t fAggNumThreads;
  uint32_t fAggNumBuckets;
  uint32_t fAggNumRowGroups;

  // window function
  uint32_t fWindowFunctionThreads;

  bool isExeMgr;
  bool fUseHdfs;
  bool fAllowedDiskAggregation{false};
  uint64_t fDECConnectionsPerQuery;
};

inline std::string ResourceManager::getStringVal(const std::string& section, const std::string& name,
                                                 const std::string& defval,
                                                 const bool reReadConfigIfNeeded) const
{
  std::string val = UNLIKELY(reReadConfigIfNeeded) ? fConfig->getFromActualConfig(section, name)
                                                   : fConfig->getConfig(section, name);
#ifdef DEBUGRM
  std::cout << "RM getStringVal for " << section << " : " << name << " val: " << val << " default: " << defval
            << std::endl;
#endif
  if (val.empty())
    val = defval;
  return val;
}

template <typename IntType>
inline IntType ResourceManager::getUintVal(const std::string& section, const std::string& name,
                                           IntType defval) const
{
  IntType val = fConfig->uFromText(fConfig->getConfig(section, name));
#ifdef DEBUGRM
  std::cout << "RM getUintVal val: " << section << " : " << name << " val: " << val << " default: " << defval
            << std::endl;
#endif
  return (0 == val ? defval : val);
}

template <typename IntType>
inline IntType ResourceManager::getIntVal(const std::string& section, const std::string& name,
                                          IntType defval) const
{
  std::string retStr = fConfig->getConfig(section, name);
#ifdef DEBUGRM
  std::cout << "RM getIntVal val: " << section << " : " << name << " val: " << retStr
            << " default: " << defval << std::endl;
#endif
  return (0 == retStr.length() ? defval : fConfig->fromText(retStr));
}

inline bool ResourceManager::getBoolVal(const std::string& section, const std::string& name,
                                        bool defval) const
{
  auto retStr = fConfig->getConfig(section, name);
  return (0 == retStr.length() ? defval : (retStr == "y" || retStr == "Y"));
}

}  // namespace joblist

#undef EXPORT
