////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Kaveh Vahedipour
////////////////////////////////////////////////////////////////////////////////

#include "Job.h"

static std::string const DBServer = "DBServer";

using namespace arangodb::consensus;

bool arangodb::consensus::compareServerLists(Slice plan, Slice current) {
  if (!plan.isArray() || !current.isArray()) {
    return false;
  }
  std::vector<std::string> planv, currv;
  for (auto const& srv : VPackArrayIterator(plan)) {
    if (srv.isString()) {
      planv.push_back(srv.copyString());
    }
  }
  for (auto const& srv : VPackArrayIterator(current)) {
    if (srv.isString()) {
      currv.push_back(srv.copyString());
    }
  }
  bool equalLeader = !planv.empty() && !currv.empty() &&
                     planv.front() == currv.front();
  std::sort(planv.begin(), planv.end());
  std::sort(currv.begin(), currv.end());
  return equalLeader && currv == planv;
}

Job::Job(Node const& snapshot, Agent* agent, std::string const& jobId,
         std::string const& creator)
  : _snapshot(snapshot),
    _agent(agent),
    _jobId(jobId),
    _creator(creator),
    _jb(nullptr) {
}

Job::~Job() {}

// this will be initialized in the AgencyFeature
std::string Job::agencyPrefix = "/arango";

JOB_STATUS Job::exists() const {

  Node const& target = _snapshot("/Target");
  
  if (target.exists(std::string("/ToDo/") + _jobId).size() == 2) {
    return TODO;
  } else if (target.exists(std::string("/Pending/") + _jobId).size() == 2) {
    return PENDING;
  } else if (target.exists(std::string("/Finished/") + _jobId).size() == 2) {
    return FINISHED;
  } else if (target.exists(std::string("/Failed/") + _jobId).size() == 2) {
    return FAILED;
  }
  
  return NOTFOUND;
  
}


bool Job::finish(std::string const& type, bool success,
                 std::string const& reason) const {
  
  Builder pending, finished;
  
  // Get todo entry
  pending.openArray();
  if (_snapshot.exists(pendingPrefix + _jobId).size() == 3) {
    _snapshot(pendingPrefix + _jobId).toBuilder(pending);
  } else if (_snapshot.exists(toDoPrefix + _jobId).size() == 3) {
    _snapshot(toDoPrefix + _jobId).toBuilder(pending);
  } else {
    LOG_TOPIC(DEBUG, Logger::AGENCY)
      << "Nothing in pending to finish up for job " << _jobId;
    return false;
  }
  pending.close();

  std::string jobType;
  try {
    jobType = pending.slice()[0].get("type").copyString();
  } catch (std::exception const&) {
    LOG_TOPIC(WARN, Logger::AGENCY)
      << "Failed to obtain type of job " << _jobId;
  }
  
  // Prepare pending entry, block toserver
  finished.openArray();
  
  // --- Add finished
  finished.openObject();
  finished.add(
    agencyPrefix + (success ? finishedPrefix : failedPrefix) + _jobId,
    VPackValue(VPackValueType::Object));
  finished.add(
    "timeFinished",
    VPackValue(timepointToString(std::chrono::system_clock::now())));
  for (auto const& obj : VPackObjectIterator(pending.slice()[0])) {
    finished.add(obj.key.copyString(), obj.value);
  }
  if (!reason.empty()) {
    finished.add("reason", VPackValue(reason));
  }
  finished.close();

  // --- Delete pending
  finished.add(agencyPrefix + pendingPrefix + _jobId,
               VPackValue(VPackValueType::Object));
  finished.add("op", VPackValue("delete"));
  finished.close();

  // --- Delete todo
  finished.add(agencyPrefix + toDoPrefix + _jobId,
               VPackValue(VPackValueType::Object));
  finished.add("op", VPackValue("delete"));
  finished.close();

  // --- Remove block if specified
  if (jobType == "moveShard") {
    for (auto const& shard :
           VPackArrayIterator(pending.slice()[0].get("shards"))) {
      finished.add(agencyPrefix + "/Supervision/Shards/" + shard.copyString(),
                   VPackValue(VPackValueType::Object));
      finished.add("op", VPackValue("delete"));
      finished.close();
    }
  } else if (type != "") {
    finished.add(agencyPrefix + "/Supervision/" + type,
                 VPackValue(VPackValueType::Object));
    finished.add("op", VPackValue("delete"));
    finished.close();
  }

  // --- Need precond?
  finished.close();
  finished.close();

  write_ret_t res = transact(_agent, finished);
  if (res.accepted && res.indices.size() == 1 && res.indices[0]) {
    LOG_TOPIC(DEBUG, Logger::AGENCY)
      << "Successfully finished job " << type << "(" << _jobId << ")";
    return true;
  }

  return false;
}


std::vector<std::string> Job::availableServers(Node const& snapshot) {

  std::vector<std::string> ret;

  // Get servers from plan
  Node::Children const& dbservers = snapshot(plannedServers).children();
  for (auto const& srv : dbservers) {
    ret.push_back(srv.first);
  }
  
  // Remove cleaned servers from ist
  try {
    for (auto const& srv :
           VPackArrayIterator(snapshot(cleanedPrefix).slice())) {
      ret.erase(
        std::remove(ret.begin(), ret.end(), srv.copyString()),
        ret.end());
    }
  } catch (...) {}

  
  // Remove failed servers from list
  try {
    for (auto const& srv :
           VPackArrayIterator(snapshot(failedServersPrefix).slice())) {
      ret.erase(
        std::remove(ret.begin(), ret.end(), srv.copyString()),
        ret.end());
    }
  } catch (...) {}
  
  return ret;
  
}

std::vector<Job::shard_t> Job::clones(
  Node const& snapshot, std::string const& database,
  std::string const& collection, std::string const& shard) {

  std::vector<shard_t> ret;
  ret.emplace_back(collection, shard);  // add (collection, shard) as first item

  try {
    std::string databasePath = planColPrefix + database,
      planPath = databasePath + "/" + collection + "/shards";

    auto myshards = snapshot(planPath).children();
    auto steps = std::distance(myshards.begin(), myshards.find(shard));

    for (const auto& colptr : snapshot(databasePath).children()) { // collections

      auto const col = *colptr.second;
      auto const otherCollection = colptr.first;

      try {
        std::string const& prototype =
          col("distributeShardsLike").slice().copyString();
        if (otherCollection != collection && prototype == collection) {
          auto othershards = col("shards").children();
          auto opos = othershards.begin();
          std::advance(opos, steps);
          auto const& otherShard = opos->first;
          ret.emplace_back(otherCollection, otherShard);
        }
      } catch(...) {}
      
    }
  } catch (...) {
    ret.clear();
    ret.emplace_back(collection, shard);
  }
  
  return ret;
}


std::string Job::uuidLookup (std::string const& shortID) {
  for (auto const& uuid : _snapshot(mapUniqueToShortID).children()) {
    if ((*uuid.second)("ShortName").getString() == shortID) {
      return uuid.first;
    }
  }
  return std::string();
}


std::string Job::id(std::string const& idOrShortName) {
  std::string id = uuidLookup(idOrShortName);
  if (!id.empty()) {
    return id;
  }
  return idOrShortName;
}

bool Job::abortable(Node const& snapshot, std::string const& jobId) {

  auto const& job = snapshot(blockedServersPrefix + jobId);
  auto const& type = job("type").getString();

  if (type == "failedServer" || type == "failedLeader") {
    return false;
  } else if (type == "addFollower" || type == "moveShard" ||
             type == "cleanOutServer") {
    return true;
  }

  // We should never get here
  TRI_ASSERT(false);
  
}

void Job::doForAllShards(Node const& snapshot,
	std::string& database,
	std::vector<shard_t>& shards,
  std::function<void(Slice plan, Slice current, std::string& planPath)> worker) {
	for (auto const& collShard : shards) {
		std::string shard = collShard.shard;
		std::string collection = collShard.collection;

    std::string planPath =
      planColPrefix + database + "/" + collection + "/shards/" + shard;
    std::string curPath = curColPrefix + database + "/" + collection
                          + "/" + shard + "/servers";

		Slice plan = snapshot(planPath).slice();
		Slice current = snapshot(curPath).slice();

    worker(plan, current, planPath);
  }
}

void Job::addIncreasePlanVersion(Builder& trx) {
  trx.add(VPackValue(agencyPrefix + planVersion));
  { VPackObjectBuilder guard(&trx);
    trx.add("op", VPackValue("increment"));
  }
}

void Job::addRemoveJobFromSomewhere(Builder& trx, std::string where,
  std::string jobId) {
  trx.add(VPackValue(agencyPrefix + "/Target/" + where + "/" + jobId));
  { VPackObjectBuilder guard(&trx);
    trx.add("op", VPackValue("delete"));
  }
}

void Job::addPutJobIntoSomewhere(Builder& trx, std::string where, Slice job,
    std::string reason) {
  Slice jobIdSlice = job.get("id");
  TRI_ASSERT(jobIdSlice.isString());
  std::string jobId = jobIdSlice.copyString();
  trx.add(VPackValue(agencyPrefix + "/Target/" + where + "/" + jobId));
  { VPackObjectBuilder guard(&trx);
    trx.add("timeFinished",
      VPackValue(timepointToString(std::chrono::system_clock::now())));
    for (auto const& obj : VPackObjectIterator(job)) {
      trx.add(obj.key.copyString(), obj.value);
    }
    if (!reason.empty()) {
      trx.add("reason", VPackValue(reason));
    }
  }
}

void Job::addPreconditionCollectionStillThere(Builder& pre,
    std::string database, std::string collection) {
  std::string planPath
      = agencyPrefix + planColPrefix + database + "/" + collection;
  pre.add(VPackValue(planPath));
  { VPackObjectBuilder guard(&pre);
    pre.add("oldEmpty", VPackValue(false));
  }
}

void Job::addPreconditionServerNotBlocked(Builder& pre, std::string server) {
	pre.add(VPackValue(agencyPrefix + blockedServersPrefix + server));
	{ VPackObjectBuilder shardLockEmpty(&pre);
		pre.add("oldEmpty", VPackValue(true));
	}
}

void Job::addPreconditionShardNotBlocked(Builder& pre, std::string shard) {
	pre.add(VPackValue(agencyPrefix + blockedShardsPrefix + shard));
	{ VPackObjectBuilder shardLockEmpty(&pre);
		pre.add("oldEmpty", VPackValue(true));
	}
}

void Job::addPreconditionUnchanged(Builder& pre,
    std::string key, Slice value) {
  pre.add(VPackValue(agencyPrefix + key));
  { VPackObjectBuilder guard(&pre);
    pre.add("old", value);
  }
}

void Job::addBlockServer(Builder& trx, std::string server, std::string jobId) {
  trx.add(agencyPrefix + blockedServersPrefix + server, VPackValue(jobId));
}

void Job::addBlockShard(Builder& trx, std::string shard, std::string jobId) {
  trx.add(agencyPrefix + blockedShardsPrefix + shard, VPackValue(jobId));
}

