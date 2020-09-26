/*
 * CommitProxyServer.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <tuple>

#include <fdbclient/DatabaseContext.h>
#include "fdbclient/Atomic.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/Knobs.h"
#include "fdbclient/CommitProxyInterface.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/SystemData.h"
#include "fdbrpc/sim_validation.h"
#include "fdbserver/ApplyMetadataMutation.h"
#include "fdbserver/ConflictSet.h"
#include "fdbserver/DataDistributorInterface.h"
#include "fdbserver/FDBExecHelper.actor.h"
#include "fdbserver/IKeyValueStore.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/LogSystem.h"
#include "fdbserver/LogSystemDiskQueueAdapter.h"
#include "fdbserver/MasterInterface.h"
#include "fdbserver/MutationTracking.h"
#include "fdbserver/ProxyCommitData.actor.h"
#include "fdbserver/RatekeeperInterface.h"
#include "fdbserver/RecoveryState.h"
#include "fdbserver/WaitFailure.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "flow/ActorCollection.h"
#include "flow/IRandom.h"
#include "flow/Knobs.h"
#include "flow/Trace.h"
#include "flow/Tracing.h"

#include "flow/actorcompiler.h"  // This must be the last #include.

ACTOR Future<Void> broadcastTxnRequest(TxnStateRequest req, int sendAmount, bool sendReply) {
	state ReplyPromise<Void> reply = req.reply;
	resetReply( req );
	std::vector<Future<Void>> replies;
	int currentStream = 0;
	std::vector<Endpoint> broadcastEndpoints = req.broadcastInfo;
	for(int i = 0; i < sendAmount && currentStream < broadcastEndpoints.size(); i++) {
		std::vector<Endpoint> endpoints;
		RequestStream<TxnStateRequest> cur(broadcastEndpoints[currentStream++]);
		while(currentStream < broadcastEndpoints.size()*(i+1)/sendAmount) {
			endpoints.push_back(broadcastEndpoints[currentStream++]);
		}
		req.broadcastInfo = endpoints;
		replies.push_back(brokenPromiseToNever( cur.getReply( req ) ));
		resetReply( req );
	}
	wait( waitForAll(replies) );
	if(sendReply) {
		reply.send(Void());
	}
	return Void();
}

ACTOR void discardCommit(UID id, Future<LogSystemDiskQueueAdapter::CommitMessage> fcm, Future<Void> dummyCommitState) {
	ASSERT(!dummyCommitState.isReady());
	LogSystemDiskQueueAdapter::CommitMessage cm = wait(fcm);
	TraceEvent("Discarding", id).detail("Count", cm.messages.size());
	cm.acknowledge.send(Void());
	ASSERT(dummyCommitState.isReady());
}

struct ResolutionRequestBuilder {
	ProxyCommitData* self;
	vector<ResolveTransactionBatchRequest> requests;
	vector<vector<int>> transactionResolverMap;
	vector<CommitTransactionRef*> outTr;
	std::vector<std::vector<std::vector<int>>>
	    txReadConflictRangeIndexMap; // Used to report conflicting keys, the format is
	                                 // [CommitTransactionRef_Index][Resolver_Index][Read_Conflict_Range_Index_on_Resolver]
	                                 // -> read_conflict_range's original index in the commitTransactionRef

	ResolutionRequestBuilder(ProxyCommitData* self, Version version, Version prevVersion, Version lastReceivedVersion,
	                         Span& parentSpan)
	  : self(self), requests(self->resolvers.size()) {
		for (auto& req : requests) {
			req.spanContext = parentSpan.context;
			req.prevVersion = prevVersion;
			req.version = version;
			req.lastReceivedVersion = lastReceivedVersion;
		}
	}

	CommitTransactionRef& getOutTransaction(int resolver, Version read_snapshot) {
		CommitTransactionRef *& out = outTr[resolver];
		if (!out) {
			ResolveTransactionBatchRequest& request = requests[resolver];
			request.transactions.resize(request.arena, request.transactions.size() + 1);
			out = &request.transactions.back();
			out->read_snapshot = read_snapshot;
		}
		return *out;
	}

	void addTransaction(CommitTransactionRequest& trRequest, int transactionNumberInBatch) {
		auto& trIn = trRequest.transaction;
		// SOMEDAY: There are a couple of unnecessary O( # resolvers ) steps here
		outTr.assign(requests.size(), nullptr);
		ASSERT( transactionNumberInBatch >= 0 && transactionNumberInBatch < 32768 );

		bool isTXNStateTransaction = false;
		for (auto & m : trIn.mutations) {
			if (m.type == MutationRef::SetVersionstampedKey) {
				transformVersionstampMutation( m, &MutationRef::param1, requests[0].version, transactionNumberInBatch );
				trIn.write_conflict_ranges.push_back( requests[0].arena, singleKeyRange( m.param1, requests[0].arena ) );
			} else if (m.type == MutationRef::SetVersionstampedValue) {
				transformVersionstampMutation( m, &MutationRef::param2, requests[0].version, transactionNumberInBatch );
			}
			if (isMetadataMutation(m)) {
				isTXNStateTransaction = true;
				getOutTransaction(0, trIn.read_snapshot).mutations.push_back(requests[0].arena, m);
			}
		}
		if (isTXNStateTransaction && !trRequest.isLockAware()) {
			// This mitigates https://github.com/apple/foundationdb/issues/3647. Since this transaction is not lock
			// aware, if this transaction got a read version then \xff/dbLocked must not have been set at this
			// transaction's read snapshot. If that changes by commit time, then it won't commit on any proxy because of
			// a conflict. A client could set a read version manually so this isn't totally bulletproof.
			trIn.read_conflict_ranges.push_back(trRequest.arena, KeyRangeRef(databaseLockedKey, databaseLockedKeyEnd));
		}
		std::vector<std::vector<int>> rCRIndexMap(
		    requests.size()); // [resolver_index][read_conflict_range_index_on_the_resolver]
		                      // -> read_conflict_range's original index
		for (int idx = 0; idx < trIn.read_conflict_ranges.size(); ++idx) {
			const auto& r = trIn.read_conflict_ranges[idx];
			auto ranges = self->keyResolvers.intersectingRanges( r );
			std::set<int> resolvers;
			for(auto &ir : ranges) {
				auto& version_resolver = ir.value();
				for(int i = version_resolver.size()-1; i >= 0; i--) {
					resolvers.insert(version_resolver[i].second);
					if( version_resolver[i].first < trIn.read_snapshot )
						break;
				}
			}
			ASSERT(resolvers.size());
			for (int resolver : resolvers) {
				getOutTransaction( resolver, trIn.read_snapshot ).read_conflict_ranges.push_back( requests[resolver].arena, r );
				rCRIndexMap[resolver].push_back(idx);
			}
		}
		txReadConflictRangeIndexMap.push_back(std::move(rCRIndexMap));
		for(auto& r : trIn.write_conflict_ranges) {
			auto ranges = self->keyResolvers.intersectingRanges( r );
			std::set<int> resolvers;
			for(auto &ir : ranges)
				resolvers.insert(ir.value().back().second);
			ASSERT(resolvers.size());
			for(int resolver : resolvers)
				getOutTransaction( resolver, trIn.read_snapshot ).write_conflict_ranges.push_back( requests[resolver].arena, r );
		}
		if (isTXNStateTransaction)
			for (int r = 0; r<requests.size(); r++) {
				int transactionNumberInRequest = &getOutTransaction(r, trIn.read_snapshot) - requests[r].transactions.begin();
				requests[r].txnStateTransactions.push_back(requests[r].arena, transactionNumberInRequest);
			}

		vector<int> resolversUsed;
		for (int r = 0; r<outTr.size(); r++)
			if (outTr[r]) {
				resolversUsed.push_back(r);
				outTr[r]->report_conflicting_keys = trIn.report_conflicting_keys;
			}
		transactionResolverMap.emplace_back(std::move(resolversUsed));
	}
};

ACTOR Future<Void> commitBatcher(ProxyCommitData *commitData, PromiseStream<std::pair<std::vector<CommitTransactionRequest>, int> > out, FutureStream<CommitTransactionRequest> in, int desiredBytes, int64_t memBytesLimit) {
	wait(delayJittered(commitData->commitBatchInterval, TaskPriority::ProxyCommitBatcher));

	state double lastBatch = 0;

	loop{
		state Future<Void> timeout;
		state std::vector<CommitTransactionRequest> batch;
		state int batchBytes = 0;

		if(SERVER_KNOBS->MAX_COMMIT_BATCH_INTERVAL <= 0) {
			timeout = Never();
		}
		else {
			timeout = delayJittered(SERVER_KNOBS->MAX_COMMIT_BATCH_INTERVAL, TaskPriority::ProxyCommitBatcher);
		}

		while(!timeout.isReady() && !(batch.size() == SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_COUNT_MAX || batchBytes >= desiredBytes)) {
			choose{
				when(CommitTransactionRequest req = waitNext(in)) {
					//WARNING: this code is run at a high priority, so it needs to do as little work as possible
					int bytes = getBytes(req);

					// Drop requests if memory is under severe pressure
					if(commitData->commitBatchesMemBytesCount + bytes > memBytesLimit) {
						++commitData->stats.txnCommitErrors;
						req.reply.sendError(proxy_memory_limit_exceeded());
						TraceEvent(SevWarnAlways, "ProxyCommitBatchMemoryThresholdExceeded").suppressFor(60).detail("MemBytesCount", commitData->commitBatchesMemBytesCount).detail("MemLimit", memBytesLimit);
						continue;
					}

					if (bytes > FLOW_KNOBS->PACKET_WARNING) {
						TraceEvent(!g_network->isSimulated() ? SevWarnAlways : SevWarn, "LargeTransaction")
						    .suppressFor(1.0)
						    .detail("Size", bytes)
						    .detail("Client", req.reply.getEndpoint().getPrimaryAddress());
					}
					++commitData->stats.txnCommitIn;

					if(req.debugID.present()) {
						g_traceBatch.addEvent("CommitDebug", req.debugID.get().first(), "CommitProxyServer.batcher");
					}

					if(!batch.size()) {
						if(now() - lastBatch > commitData->commitBatchInterval) {
							timeout = delayJittered(SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_INTERVAL_FROM_IDLE, TaskPriority::ProxyCommitBatcher);
						}
						else {
							timeout = delayJittered(commitData->commitBatchInterval - (now() - lastBatch), TaskPriority::ProxyCommitBatcher);
						}
					}

					if((batchBytes + bytes > CLIENT_KNOBS->TRANSACTION_SIZE_LIMIT || req.firstInBatch()) && batch.size()) {
						out.send({ std::move(batch), batchBytes });
						lastBatch = now();
						timeout = delayJittered(commitData->commitBatchInterval, TaskPriority::ProxyCommitBatcher);
						batch.clear();
						batchBytes = 0;
					}

					batch.push_back(req);
					batchBytes += bytes;
					commitData->commitBatchesMemBytesCount += bytes;
				}
				when(wait(timeout)) {}
			}
		}
		out.send({ std::move(batch), batchBytes });
		lastBatch = now();
	}
}

void createWhitelistBinPathVec(const std::string& binPath, vector<Standalone<StringRef>>& binPathVec) {
	TraceEvent(SevDebug, "BinPathConverter").detail("Input", binPath);
	StringRef input(binPath);
	while (input != StringRef()) {
		StringRef token = input.eat(LiteralStringRef(","));
		if (token != StringRef()) {
			const uint8_t* ptr = token.begin();
			while (ptr != token.end() && *ptr == ' ') {
				ptr++;
			}
			if (ptr != token.end()) {
				Standalone<StringRef> newElement(token.substr(ptr - token.begin()));
				TraceEvent(SevDebug, "BinPathItem").detail("Element", newElement);
				binPathVec.push_back(newElement);
			}
		}
	}
	return;
}

bool isWhitelisted(const vector<Standalone<StringRef>>& binPathVec, StringRef binPath) {
	TraceEvent("BinPath").detail("Value", binPath);
	for (const auto& item : binPathVec) {
		TraceEvent("Element").detail("Value", item);
	}
	return std::find(binPathVec.begin(), binPathVec.end(), binPath) != binPathVec.end();
}

ACTOR Future<Void> addBackupMutations(ProxyCommitData* self, std::map<Key, MutationListRef>* logRangeMutations,
                                      LogPushData* toCommit, Version commitVersion, double* computeDuration, double* computeStart) {
	state std::map<Key, MutationListRef>::iterator logRangeMutation = logRangeMutations->begin();
	state int32_t version = commitVersion / CLIENT_KNOBS->LOG_RANGE_BLOCK_SIZE;
	state int yieldBytes = 0;
	state BinaryWriter valueWriter(Unversioned());

	// Serialize the log range mutations within the map
	for (; logRangeMutation != logRangeMutations->end(); ++logRangeMutation)
	{
		//FIXME: this is re-implementing the serialize function of MutationListRef in order to have a yield
		valueWriter = BinaryWriter(IncludeVersion(ProtocolVersion::withBackupMutations()));
		valueWriter << logRangeMutation->second.totalSize();

		state MutationListRef::Blob* blobIter = logRangeMutation->second.blob_begin;
		while(blobIter) {
			if(yieldBytes > SERVER_KNOBS->DESIRED_TOTAL_BYTES) {
				yieldBytes = 0;
				if(g_network->check_yield(TaskPriority::ProxyCommitYield1)) {
					*computeDuration += g_network->timer() - *computeStart;
					wait(delay(0, TaskPriority::ProxyCommitYield1));
					*computeStart = g_network->timer();
				}
			}
			valueWriter.serializeBytes(blobIter->data);
			yieldBytes += blobIter->data.size();
			blobIter = blobIter->next;
		}

		Key val = valueWriter.toValue();

		BinaryWriter wr(Unversioned());

		// Serialize the log destination
		wr.serializeBytes( logRangeMutation->first );

		// Write the log keys and version information
		wr << (uint8_t)hashlittle(&version, sizeof(version), 0);
		wr << bigEndian64(commitVersion);

		MutationRef backupMutation;
		backupMutation.type = MutationRef::SetValue;
		uint32_t* partBuffer = nullptr;

		for (int part = 0; part * CLIENT_KNOBS->MUTATION_BLOCK_SIZE < val.size(); part++) {

			// Assign the second parameter as the part
			backupMutation.param2 = val.substr(part * CLIENT_KNOBS->MUTATION_BLOCK_SIZE,
				std::min(val.size() - part * CLIENT_KNOBS->MUTATION_BLOCK_SIZE, CLIENT_KNOBS->MUTATION_BLOCK_SIZE));

			// Write the last part of the mutation to the serialization, if the buffer is not defined
			if (!partBuffer) {
				// Serialize the part to the writer
				wr << bigEndian32(part);

				// Define the last buffer part
				partBuffer = (uint32_t*) ((char*) wr.getData() + wr.getLength() - sizeof(uint32_t));
			}
			else {
				*partBuffer = bigEndian32(part);
			}

			// Define the mutation type and and location
			backupMutation.param1 = wr.toValue();
			ASSERT( backupMutation.param1.startsWith(logRangeMutation->first) );  // We are writing into the configured destination

			auto& tags = self->tagsForKey(backupMutation.param1);
			toCommit->addTags(tags);
			toCommit->addTypedMessage(backupMutation);

//			if (DEBUG_MUTATION("BackupProxyCommit", commitVersion, backupMutation)) {
//				TraceEvent("BackupProxyCommitTo", self->dbgid).detail("To", describe(tags)).detail("BackupMutation", backupMutation.toString())
//					.detail("BackupMutationSize", val.size()).detail("Version", commitVersion).detail("DestPath", logRangeMutation.first)
//					.detail("PartIndex", part).detail("PartIndexEndian", bigEndian32(part)).detail("PartData", backupMutation.param1);
//			}
		}
	}
	return Void();
}

ACTOR Future<Void> releaseResolvingAfter(ProxyCommitData* self, Future<Void> releaseDelay, int64_t localBatchNumber) {
	wait(releaseDelay);
	ASSERT(self->latestLocalCommitBatchResolving.get() == localBatchNumber-1);
	self->latestLocalCommitBatchResolving.set(localBatchNumber);
	return Void();
}

namespace CommitBatch {

struct CommitBatchContext {
	using StoreCommit_t = std::vector<std::pair<Future<LogSystemDiskQueueAdapter::CommitMessage>, Future<Void>>>;

	ProxyCommitData* const pProxyCommitData;
	std::vector<CommitTransactionRequest> trs;
	int currentBatchMemBytesCount;

	double startTime;

	Optional<UID> debugID;

	bool forceRecovery = false;

	int64_t localBatchNumber;
	LogPushData toCommit;

	int batchOperations = 0;

	Span span = Span("MP:commitBatch"_loc);

	int64_t batchBytes = 0;

	int latencyBucket = 0;

	Version commitVersion;
	Version prevVersion;

	int64_t maxTransactionBytes;
	std::vector<std::vector<int>> transactionResolverMap;
	std::vector<std::vector<std::vector<int>>> txReadConflictRangeIndexMap;

	Future<Void> releaseDelay;
	Future<Void> releaseFuture;

	std::vector<ResolveTransactionBatchReply> resolution;

	double computeStart;
	double computeDuration = 0;

	Arena arena;

	/// true if the batch is the 1st batch for this proxy, additional metadata
	/// processing is involved for this batch.
	bool isMyFirstBatch;
	bool firstStateMutations;

	Optional<Value> oldCoordinators;

	StoreCommit_t storeCommits;

	std::vector<uint8_t> committed;

	Optional<Key> lockedKey;
	bool locked;

	int commitCount = 0;

	std::vector<int> nextTr;

	bool lockedAfter;

	Optional<Value> metadataVersionAfter;

	int mutationCount = 0;
	int mutationBytes = 0;

	std::map<Key, MutationListRef> logRangeMutations;
	Arena logRangeMutationsArena;

	int transactionNum = 0;
	int yieldBytes = 0;

	LogSystemDiskQueueAdapter::CommitMessage msg;

	Future<Version> loggingComplete;

	double commitStartTime;

	CommitBatchContext(ProxyCommitData*, const std::vector<CommitTransactionRequest>*, const int);

	void setupTraceBatch();

private:
	void evaluateBatchSize();
};

CommitBatchContext::CommitBatchContext(ProxyCommitData* const pProxyCommitData_,
                                       const std::vector<CommitTransactionRequest>* trs_,
                                       const int currentBatchMemBytesCount)
  :

    pProxyCommitData(pProxyCommitData_), trs(std::move(*const_cast<std::vector<CommitTransactionRequest>*>(trs_))),
    currentBatchMemBytesCount(currentBatchMemBytesCount),

    startTime(g_network->now()),

    localBatchNumber(++pProxyCommitData->localCommitBatchesStarted), toCommit(pProxyCommitData->logSystem),

    committed(trs.size()) {

	evaluateBatchSize();

	if (batchOperations != 0) {
		latencyBucket = std::min<int>(
			SERVER_KNOBS->PROXY_COMPUTE_BUCKETS - 1,
			SERVER_KNOBS->PROXY_COMPUTE_BUCKETS * batchBytes /
				(batchOperations * (
					CLIENT_KNOBS->VALUE_SIZE_LIMIT +
					CLIENT_KNOBS->KEY_SIZE_LIMIT
				))
		);
	}

	// since we are using just the former to limit the number of versions actually in flight!
	ASSERT(SERVER_KNOBS->MAX_READ_TRANSACTION_LIFE_VERSIONS <= SERVER_KNOBS->MAX_VERSIONS_IN_FLIGHT);
}

void CommitBatchContext::setupTraceBatch() {
	for (const auto& tr : trs) {
		if (tr.debugID.present()) {
			if (!debugID.present()) {
				debugID = nondeterministicRandom()->randomUniqueID();
			}

			g_traceBatch.addAttach(
				"CommitAttachID",
				tr.debugID.get().first(),
				debugID.get().first()
			);
		}
		span.addParent(tr.spanContext);
	}

	if (debugID.present()) {
		g_traceBatch.addEvent("CommitDebug", debugID.get().first(), "CommitProxyServer.commitBatch.Before");
	}
}

void CommitBatchContext::evaluateBatchSize() {
	for (const auto& tr : trs) {
		const auto& mutations = tr.transaction.mutations;
		batchOperations += mutations.size();
		batchBytes += mutations.expectedSize();
	}
}

ACTOR Future<Void> preresolutionProcessing(CommitBatchContext* self) {

	state ProxyCommitData* const pProxyCommitData = self->pProxyCommitData;
	state std::vector<CommitTransactionRequest>& trs = self->trs;
	state const int64_t localBatchNumber = self->localBatchNumber;
	state const int latencyBucket = self->latencyBucket;
	state const Optional<UID>& debugID = self->debugID;

	// Pre-resolution the commits
	TEST(pProxyCommitData->latestLocalCommitBatchResolving.get() < localBatchNumber - 1);
	wait(pProxyCommitData->latestLocalCommitBatchResolving.whenAtLeast(localBatchNumber - 1));
	self->releaseDelay = delay(
		std::min(SERVER_KNOBS->MAX_PROXY_COMPUTE,
			self->batchOperations * pProxyCommitData->commitComputePerOperation[latencyBucket]),
		TaskPriority::ProxyMasterVersionReply
	);

	if (debugID.present()) {
		g_traceBatch.addEvent("CommitDebug", debugID.get().first(),
		                      "CommitProxyServer.commitBatch.GettingCommitVersion");
	}

	GetCommitVersionRequest req(self->span.context, pProxyCommitData->commitVersionRequestNumber++,
	                            pProxyCommitData->mostRecentProcessedRequestNumber, pProxyCommitData->dbgid);
	GetCommitVersionReply versionReply = wait(brokenPromiseToNever(
		pProxyCommitData->master.getCommitVersion.getReply(
			req, TaskPriority::ProxyMasterVersionReply
		)
	));

	pProxyCommitData->mostRecentProcessedRequestNumber = versionReply.requestNum;

	pProxyCommitData->stats.txnCommitVersionAssigned += trs.size();
	pProxyCommitData->stats.lastCommitVersionAssigned = versionReply.version;

	self->commitVersion = versionReply.version;
	self->prevVersion = versionReply.prevVersion;

	for(auto it : versionReply.resolverChanges) {
		auto rs = pProxyCommitData->keyResolvers.modify(it.range);
		for(auto r = rs.begin(); r != rs.end(); ++r)
			r->value().emplace_back(versionReply.resolverChangesVersion,it.dest);
	}

	//TraceEvent("ProxyGotVer", pProxyContext->dbgid).detail("Commit", commitVersion).detail("Prev", prevVersion);

	if (debugID.present()) {
		g_traceBatch.addEvent("CommitDebug", debugID.get().first(), "CommitProxyServer.commitBatch.GotCommitVersion");
	}

	return Void();
}

ACTOR Future<Void> getResolution(CommitBatchContext* self) {
	// Sending these requests is the fuzzy border between phase 1 and phase 2; it could conceivably overlap with
	// resolution processing but is still using CPU
	ProxyCommitData* pProxyCommitData = self->pProxyCommitData;
	std::vector<CommitTransactionRequest>& trs = self->trs;

	ResolutionRequestBuilder requests(
		pProxyCommitData,
		self->commitVersion,
		self->prevVersion,
		pProxyCommitData->version,
        self->span
	);
	int conflictRangeCount = 0;
	self->maxTransactionBytes = 0;
	for (int t = 0; t < trs.size(); t++) {
		requests.addTransaction(trs[t], t);
		conflictRangeCount +=
		    trs[t].transaction.read_conflict_ranges.size() + trs[t].transaction.write_conflict_ranges.size();
		//TraceEvent("MPTransactionDump", self->dbgid).detail("Snapshot", trs[t].transaction.read_snapshot);
		//for(auto& m : trs[t].transaction.mutations)
		self->maxTransactionBytes = std::max<int64_t>(
			self->maxTransactionBytes, trs[t].transaction.expectedSize()
		);
		//	TraceEvent("MPTransactionsDump", self->dbgid).detail("Mutation", m.toString());
	}
	pProxyCommitData->stats.conflictRanges += conflictRangeCount;

	for (int r = 1; r < pProxyCommitData->resolvers.size(); r++)
		ASSERT(requests.requests[r].txnStateTransactions.size() ==
			requests.requests[0].txnStateTransactions.size());

	pProxyCommitData->stats.txnCommitResolving += trs.size();
	std::vector<Future<ResolveTransactionBatchReply>> replies;
	for (int r = 0; r < pProxyCommitData->resolvers.size(); r++) {
		requests.requests[r].debugID = self->debugID;
		replies.push_back(brokenPromiseToNever(
			pProxyCommitData->resolvers[r].resolve.getReply(
				requests.requests[r], TaskPriority::ProxyResolverReply)));
	}

	self->transactionResolverMap.swap(requests.transactionResolverMap);
	// Used to report conflicting keys
	self->txReadConflictRangeIndexMap.swap(requests.txReadConflictRangeIndexMap);
	self->releaseFuture = releaseResolvingAfter(
		pProxyCommitData, self->releaseDelay, self->localBatchNumber
	);

	// Wait for the final resolution
	std::vector<ResolveTransactionBatchReply> resolutionResp = wait(getAll(replies));
	self->resolution.swap(*const_cast<std::vector<ResolveTransactionBatchReply>*>(&resolutionResp));

	if (self->debugID.present()) {
		g_traceBatch.addEvent("CommitDebug", self->debugID.get().first(),
		                      "CommitProxyServer.commitBatch.AfterResolution");
	}

	return Void();
}

void assertResolutionStateMutationsSizeConsistent(
		const std::vector<ResolveTransactionBatchReply>& resolution) {

	for (int r = 1; r < resolution.size(); r++) {
		ASSERT(resolution[r].stateMutations.size() == resolution[0].stateMutations.size());
		for(int s = 0; s < resolution[r].stateMutations.size(); s++) {
			ASSERT(resolution[r].stateMutations[s].size() == resolution[0].stateMutations[s].size());
		}
	}
}

// Compute and apply "metadata" effects of each other proxy's most recent batch
void applyMetadataEffect(CommitBatchContext* self) {
	bool initialState = self->isMyFirstBatch;
	self->firstStateMutations = self->isMyFirstBatch;
	for (int versionIndex = 0; versionIndex < self->resolution[0].stateMutations.size(); versionIndex++) {
		// pProxyCommitData->logAdapter->setNextVersion( ??? );  << Ideally we would be telling the log adapter that the pushes in this commit will be in the version at which these state mutations were committed by another proxy, but at present we don't have that information here.  So the disk queue may be unnecessarily conservative about popping.

		for (int transactionIndex = 0; transactionIndex < self->resolution[0].stateMutations[versionIndex].size() && !self->forceRecovery; transactionIndex++) {
			bool committed = true;
			for (int resolver = 0; resolver < self->resolution.size(); resolver++)
				committed = committed && self->resolution[resolver].stateMutations[versionIndex][transactionIndex].committed;
			if (committed) {
				applyMetadataMutations(*self->pProxyCommitData, self->arena, self->pProxyCommitData->logSystem,
				                       self->resolution[0].stateMutations[versionIndex][transactionIndex].mutations,
				                       /* pToCommit= */ nullptr, self->forceRecovery,
				                       /* popVersion= */ 0, /* initialCommit */ false);
			}
			if( self->resolution[0].stateMutations[versionIndex][transactionIndex].mutations.size() && self->firstStateMutations ) {
				ASSERT(committed);
				self->firstStateMutations = false;
				self->forceRecovery = false;
			}
		}

		// These changes to txnStateStore will be committed by the other proxy, so we simply discard the commit message
		auto fcm = self->pProxyCommitData->logAdapter->getCommitMessage();
		self->storeCommits.emplace_back(fcm, self->pProxyCommitData->txnStateStore->commit());

		if (initialState) {
			initialState = false;
			self->forceRecovery = false;
			self->pProxyCommitData->txnStateStore->resyncLog();

			for (auto &p : self->storeCommits) {
				ASSERT(!p.second.isReady());
				p.first.get().acknowledge.send(Void());
				ASSERT(p.second.isReady());
			}
			self->storeCommits.clear();
		}
	}
}

/// Determine which transactions actually committed (conservatively) by combining results from the resolvers
void determineCommittedTransactions(CommitBatchContext* self) {
	auto pProxyCommitData = self->pProxyCommitData;
	const auto& trs = self->trs;

	ASSERT(self->transactionResolverMap.size() == self->committed.size());
	// For each commitTransactionRef, it is only sent to resolvers specified in transactionResolverMap
	// Thus, we use this nextTr to track the correct transaction index on each resolver.
	self->nextTr.resize(self->resolution.size());
	for (int t = 0; t < trs.size(); t++) {
		uint8_t commit = ConflictBatch::TransactionCommitted;
		for (int r : self->transactionResolverMap[t]) {
			commit = std::min(self->resolution[r].committed[self->nextTr[r]++], commit);
		}
		self->committed[t] = commit;
	}
	for (int r = 0; r < self->resolution.size(); r++)
		ASSERT(self->nextTr[r] == self->resolution[r].committed.size());

	pProxyCommitData->logAdapter->setNextVersion(self->commitVersion);

	self->lockedKey = pProxyCommitData->txnStateStore->readValue(databaseLockedKey).get();
	self->locked = self->lockedKey.present() && self->lockedKey.get().size();

	const Optional<Value> mustContainSystemKey = pProxyCommitData->txnStateStore->readValue(mustContainSystemMutationsKey).get();
	if (mustContainSystemKey.present() && mustContainSystemKey.get().size()) {
		for (int t = 0; t < trs.size(); t++) {
			if( self->committed[t] == ConflictBatch::TransactionCommitted ) {
				bool foundSystem = false;
				for(auto& m : trs[t].transaction.mutations) {
					if( ( m.type == MutationRef::ClearRange ? m.param2 : m.param1 ) >= nonMetadataSystemKeys.end) {
						foundSystem = true;
						break;
					}
				}
				if(!foundSystem) {
					self->committed[t] = ConflictBatch::TransactionConflict;
				}
			}
		}
	}
}

// This first pass through committed transactions deals with "metadata" effects (modifications of txnStateStore, changes to storage servers' responsibilities)
ACTOR Future<Void> applyMetadataToCommittedTransactions(CommitBatchContext* self) {
	auto pProxyCommitData = self->pProxyCommitData;
	const auto& trs = self->trs;

	int t;
	for (t = 0; t < trs.size() && !self->forceRecovery; t++) {
		if (self->committed[t] == ConflictBatch::TransactionCommitted && (!self->locked || trs[t].isLockAware())) {
			self->commitCount++;
			applyMetadataMutations(*pProxyCommitData, self->arena, pProxyCommitData->logSystem,
			                       trs[t].transaction.mutations, &self->toCommit, self->forceRecovery,
			                       self->commitVersion + 1, /* initialCommit= */ false);
		}
		if(self->firstStateMutations) {
			ASSERT(self->committed[t] == ConflictBatch::TransactionCommitted);
			self->firstStateMutations = false;
			self->forceRecovery = false;
		}
	}
	if (self->forceRecovery) {
		for (; t < trs.size(); t++)
			self->committed[t] = ConflictBatch::TransactionConflict;
		TraceEvent(SevWarn, "RestartingTxnSubsystem", pProxyCommitData->dbgid).detail("Stage", "AwaitCommit");
	}

	self->lockedKey = pProxyCommitData->txnStateStore->readValue(databaseLockedKey).get();
	self->lockedAfter = self->lockedKey.present() && self->lockedKey.get().size();

	self->metadataVersionAfter = pProxyCommitData->txnStateStore->readValue(metadataVersionKey).get();

	auto fcm = pProxyCommitData->logAdapter->getCommitMessage();
	self->storeCommits.emplace_back(fcm, pProxyCommitData->txnStateStore->commit());
	pProxyCommitData->version = self->commitVersion;
	if (!pProxyCommitData->validState.isSet()) pProxyCommitData->validState.send(Void());
	ASSERT(self->commitVersion);

	if (!self->isMyFirstBatch && pProxyCommitData->txnStateStore->readValue( coordinatorsKey ).get().get() != self->oldCoordinators.get()) {
		wait( brokenPromiseToNever( pProxyCommitData->master.changeCoordinators.getReply( ChangeCoordinatorsRequest( pProxyCommitData->txnStateStore->readValue( coordinatorsKey ).get().get() ) ) ) );
		ASSERT(false);   // ChangeCoordinatorsRequest should always throw
	}

	return Void();
}

/// This second pass through committed transactions assigns the actual mutations to the appropriate storage servers' tags
ACTOR Future<Void> assignMutationsToStorageServers(CommitBatchContext* self) {
	state ProxyCommitData* const pProxyCommitData = self->pProxyCommitData;
	state std::vector<CommitTransactionRequest>& trs = self->trs;

	for (; self->transactionNum < trs.size(); self->transactionNum++) {
		if (!(self->committed[self->transactionNum] == ConflictBatch::TransactionCommitted && (!self->locked || trs[self->transactionNum].isLockAware()))) {
			continue;
		}

		state bool checkSample = trs[self->transactionNum].commitCostEstimation.present();
		state Optional<ClientTrCommitCostEstimation>* trCost = &trs[self->transactionNum].commitCostEstimation;
		state int mutationNum = 0;
		state VectorRef<MutationRef>* pMutations = &trs[self->transactionNum].transaction.mutations;
		for (; mutationNum < pMutations->size(); mutationNum++) {
			if(self->yieldBytes > SERVER_KNOBS->DESIRED_TOTAL_BYTES) {
				self->yieldBytes = 0;
				if(g_network->check_yield(TaskPriority::ProxyCommitYield1)) {
					self->computeDuration += g_network->timer() - self->computeStart;
					wait(delay(0, TaskPriority::ProxyCommitYield1));
					self->computeStart = g_network->timer();
				}
			}

			auto& m = (*pMutations)[mutationNum];
			self->mutationCount++;
			self->mutationBytes += m.expectedSize();
			self->yieldBytes += m.expectedSize();
			// Determine the set of tags (responsible storage servers) for the mutation, splitting it
			// if necessary.  Serialize (splits of) the mutation into the message buffer and add the tags.

			if (isSingleKeyMutation((MutationRef::Type) m.type)) {
				auto& tags = pProxyCommitData->tagsForKey(m.param1);

				// sample single key mutation based on cost
				// the expectation of sampling is every COMMIT_SAMPLE_COST sample once
				if (checkSample) {
					double totalCosts = trCost->get().writeCosts;
					double cost = getWriteOperationCost(m.expectedSize());
					double mul = std::max(1.0, totalCosts / std::max(1.0, (double)CLIENT_KNOBS->COMMIT_SAMPLE_COST));
					ASSERT(totalCosts > 0);
					double prob = mul * cost / totalCosts;

					if (deterministicRandom()->random01() < prob) {
						for (const auto& ssInfo : pProxyCommitData->keyInfo[m.param1].src_info) {
							auto id = ssInfo->interf.id();
							// scale cost
							cost = cost < CLIENT_KNOBS->COMMIT_SAMPLE_COST ? CLIENT_KNOBS->COMMIT_SAMPLE_COST : cost;
							pProxyCommitData->updateSSTagCost(id, trs[self->transactionNum].tagSet.get(), m, cost);
						}
					}
				}

				if(pProxyCommitData->singleKeyMutationEvent->enabled) {
					KeyRangeRef shard = pProxyCommitData->keyInfo.rangeContaining(m.param1).range();
					pProxyCommitData->singleKeyMutationEvent->tag1 = (int64_t)tags[0].id;
					pProxyCommitData->singleKeyMutationEvent->tag2 = (int64_t)tags[1].id;
					pProxyCommitData->singleKeyMutationEvent->tag3 = (int64_t)tags[2].id;
					pProxyCommitData->singleKeyMutationEvent->shardBegin = shard.begin;
					pProxyCommitData->singleKeyMutationEvent->shardEnd = shard.end;
					pProxyCommitData->singleKeyMutationEvent->log();
				}

				DEBUG_MUTATION("ProxyCommit", self->commitVersion, m).detail("Dbgid", pProxyCommitData->dbgid).detail("To", tags).detail("Mutation", m);
				self->toCommit.addTags(tags);
				if(pProxyCommitData->cacheInfo[m.param1]) {
					self->toCommit.addTag(cacheTag);
				}
				self->toCommit.addTypedMessage(m);
			}
			else if (m.type == MutationRef::ClearRange) {
				KeyRangeRef clearRange(KeyRangeRef(m.param1, m.param2));
				auto ranges = pProxyCommitData->keyInfo.intersectingRanges(clearRange);
				auto firstRange = ranges.begin();
				++firstRange;
				if (firstRange == ranges.end()) {
					// Fast path
					DEBUG_MUTATION("ProxyCommit", self->commitVersion, m).detail("Dbgid", pProxyCommitData->dbgid).detail("To", ranges.begin().value().tags).detail("Mutation", m);

					ranges.begin().value().populateTags();
					self->toCommit.addTags(ranges.begin().value().tags);

					// check whether clear is sampled
					if (checkSample && !trCost->get().clearIdxCosts.empty() &&
					    trCost->get().clearIdxCosts[0].first == mutationNum) {
						for (const auto& ssInfo : ranges.begin().value().src_info) {
							auto id = ssInfo->interf.id();
							pProxyCommitData->updateSSTagCost(id, trs[self->transactionNum].tagSet.get(), m,
							                                  trCost->get().clearIdxCosts[0].second);
						}
						trCost->get().clearIdxCosts.pop_front();
					}
				}
				else {
					TEST(true); //A clear range extends past a shard boundary
					std::set<Tag> allSources;
					for (auto r : ranges) {
						r.value().populateTags();
						allSources.insert(r.value().tags.begin(), r.value().tags.end());

						// check whether clear is sampled
						if (checkSample && !trCost->get().clearIdxCosts.empty() &&
						    trCost->get().clearIdxCosts[0].first == mutationNum) {
							for (const auto& ssInfo : r.value().src_info) {
								auto id = ssInfo->interf.id();
								pProxyCommitData->updateSSTagCost(id, trs[self->transactionNum].tagSet.get(), m,
								                                  trCost->get().clearIdxCosts[0].second);
							}
							trCost->get().clearIdxCosts.pop_front();
						}
					}
					DEBUG_MUTATION("ProxyCommit", self->commitVersion, m).detail("Dbgid", pProxyCommitData->dbgid).detail("To", allSources).detail("Mutation", m);

					self->toCommit.addTags(allSources);
				}

				if(pProxyCommitData->needsCacheTag(clearRange)) {
					self->toCommit.addTag(cacheTag);
				}
				self->toCommit.addTypedMessage(m);
			} else {
				UNREACHABLE();
			}

			// Check on backing up key, if backup ranges are defined and a normal key
			if (!(pProxyCommitData->vecBackupKeys.size() > 1 && (normalKeys.contains(m.param1) || m.param1 == metadataVersionKey))) {
				continue;
			}

			if (m.type != MutationRef::Type::ClearRange) {
				// Add the mutation to the relevant backup tag
				for (auto backupName : pProxyCommitData->vecBackupKeys[m.param1]) {
					self->logRangeMutations[backupName].push_back_deep(self->logRangeMutationsArena, m);
				}
			}
			else {
				KeyRangeRef mutationRange(m.param1, m.param2);
				KeyRangeRef intersectionRange;

				// Identify and add the intersecting ranges of the mutation to the array of mutations to serialize
				for (auto backupRange : pProxyCommitData->vecBackupKeys.intersectingRanges(mutationRange))
				{
					// Get the backup sub range
					const auto&		backupSubrange = backupRange.range();

					// Determine the intersecting range
					intersectionRange = mutationRange & backupSubrange;

					// Create the custom mutation for the specific backup tag
					MutationRef		backupMutation(MutationRef::Type::ClearRange, intersectionRange.begin, intersectionRange.end);

					// Add the mutation to the relevant backup tag
					for (auto backupName : backupRange.value()) {
						self->logRangeMutations[backupName].push_back_deep(self->logRangeMutationsArena, backupMutation);
					}
				}
			}
		}

		if (checkSample) {
			self->pProxyCommitData->stats.txnExpensiveClearCostEstCount +=
			    trs[self->transactionNum].commitCostEstimation.get().expensiveCostEstCount;
		}
	}

	return Void();
}

ACTOR Future<Void> postResolution(CommitBatchContext* self) {
	state ProxyCommitData* const pProxyCommitData = self->pProxyCommitData;
	state std::vector<CommitTransactionRequest>& trs = self->trs;
	state const int64_t localBatchNumber = self->localBatchNumber;
	state const Optional<UID>& debugID = self->debugID;

	TEST(pProxyCommitData->latestLocalCommitBatchLogging.get() < localBatchNumber - 1); // Queuing post-resolution commit processing
	wait(pProxyCommitData->latestLocalCommitBatchLogging.whenAtLeast(localBatchNumber - 1));
	wait(yield(TaskPriority::ProxyCommitYield1));

	self->computeStart = g_network->timer();

	pProxyCommitData->stats.txnCommitResolved += trs.size();

	if (debugID.present()) {
		g_traceBatch.addEvent("CommitDebug", debugID.get().first(),
		                      "CommitProxyServer.commitBatch.ProcessingMutations");
	}

	self->isMyFirstBatch = !pProxyCommitData->version;
	self->oldCoordinators = pProxyCommitData->txnStateStore->readValue(coordinatorsKey).get();

	assertResolutionStateMutationsSizeConsistent(self->resolution);

	applyMetadataEffect(self);

	determineCommittedTransactions(self);

	if(self->forceRecovery) {
		wait( Future<Void>(Never()) );
	}

	// First pass
	wait(applyMetadataToCommittedTransactions(self));

	// Second pass
	wait(assignMutationsToStorageServers(self));

	// Serialize and backup the mutations as a single mutation
	if ((pProxyCommitData->vecBackupKeys.size() > 1) && self->logRangeMutations.size()) {
		wait( addBackupMutations(pProxyCommitData, &self->logRangeMutations, &self->toCommit, self->commitVersion, &self->computeDuration, &self->computeStart) );
	}

	pProxyCommitData->stats.mutations += self->mutationCount;
	pProxyCommitData->stats.mutationBytes += self->mutationBytes;

	// Storage servers mustn't make durable versions which are not fully committed (because then they are impossible to roll back)
	// We prevent this by limiting the number of versions which are semi-committed but not fully committed to be less than the MVCC window
	if (pProxyCommitData->committedVersion.get() < self->commitVersion - SERVER_KNOBS->MAX_READ_TRANSACTION_LIFE_VERSIONS) {
		self->computeDuration += g_network->timer() - self->computeStart;
		state Span waitVersionSpan;
		while (pProxyCommitData->committedVersion.get() < self->commitVersion - SERVER_KNOBS->MAX_READ_TRANSACTION_LIFE_VERSIONS) {
			// This should be *extremely* rare in the real world, but knob buggification should make it happen in simulation
			TEST(true);  // Semi-committed pipeline limited by MVCC window
			//TraceEvent("ProxyWaitingForCommitted", pProxyCommitData->dbgid).detail("CommittedVersion", pProxyCommitData->committedVersion.get()).detail("NeedToCommit", commitVersion);
			waitVersionSpan = Span(deterministicRandom()->randomUniqueID(), "MP:overMaxReadTransactionLifeVersions"_loc, {self->span.context});
			choose{
				when(wait(pProxyCommitData->committedVersion.whenAtLeast(self->commitVersion - SERVER_KNOBS->MAX_READ_TRANSACTION_LIFE_VERSIONS))) {
					wait(yield());
					break;
				}
				when(wait(pProxyCommitData->cx->onProxiesChanged())) {}
				when(GetRawCommittedVersionReply v = wait(pProxyCommitData->master.getLiveCommittedVersion.getReply(
				         GetRawCommittedVersionRequest(waitVersionSpan.context, debugID), TaskPriority::GetLiveCommittedVersionReply))) {
					if(v.version > pProxyCommitData->committedVersion.get()) {
						pProxyCommitData->locked = v.locked;
						pProxyCommitData->metadataVersion = v.metadataVersion;
						pProxyCommitData->committedVersion.set(v.version);
					}

					if (pProxyCommitData->committedVersion.get() < self->commitVersion - SERVER_KNOBS->MAX_READ_TRANSACTION_LIFE_VERSIONS)
						wait(delay(SERVER_KNOBS->PROXY_SPIN_DELAY));
				}
			}
		}
		waitVersionSpan = Span{};
		self->computeStart = g_network->timer();
	}

	self->msg = self->storeCommits.back().first.get();

	if (self->debugID.present())
		g_traceBatch.addEvent("CommitDebug", self->debugID.get().first(),
		                      "CommitProxyServer.commitBatch.AfterStoreCommits");

	// txnState (transaction subsystem state) tag: message extracted from log adapter
	bool firstMessage = true;
	for(auto m : self->msg.messages) {
		if(firstMessage) {
			self->toCommit.addTxsTag();
		}
		self->toCommit.addMessage(StringRef(m.begin(), m.size()), !firstMessage);
		firstMessage = false;
	}

	if ( self->prevVersion && self->commitVersion - self->prevVersion < SERVER_KNOBS->MAX_VERSIONS_IN_FLIGHT/2 )
		debug_advanceMaxCommittedVersion( UID(), self->commitVersion );  //< Is this valid?

	//TraceEvent("ProxyPush", pProxyCommitData->dbgid).detail("PrevVersion", prevVersion).detail("Version", commitVersion)
	//	.detail("TransactionsSubmitted", trs.size()).detail("TransactionsCommitted", commitCount).detail("TxsPopTo", msg.popTo);

	if ( self->prevVersion && self->commitVersion - self->prevVersion < SERVER_KNOBS->MAX_VERSIONS_IN_FLIGHT/2 )
		debug_advanceMaxCommittedVersion(UID(), self->commitVersion);

	self->commitStartTime = now();
	pProxyCommitData->lastStartCommit = self->commitStartTime;
	self->loggingComplete = pProxyCommitData->logSystem->push( self->prevVersion, self->commitVersion, pProxyCommitData->committedVersion.get(), pProxyCommitData->minKnownCommittedVersion, self->toCommit, self->debugID );

	if (!self->forceRecovery) {
		ASSERT(pProxyCommitData->latestLocalCommitBatchLogging.get() == self->localBatchNumber-1);
		pProxyCommitData->latestLocalCommitBatchLogging.set(self->localBatchNumber);
	}

	self->computeDuration += g_network->timer() - self->computeStart;
	if(self->computeDuration > SERVER_KNOBS->MIN_PROXY_COMPUTE && self->batchOperations > 0) {
		double computePerOperation = self->computeDuration / self->batchOperations;
		if(computePerOperation <= pProxyCommitData->commitComputePerOperation[self->latencyBucket]) {
			pProxyCommitData->commitComputePerOperation[self->latencyBucket] = computePerOperation;
		} else {
			pProxyCommitData->commitComputePerOperation[self->latencyBucket] = SERVER_KNOBS->PROXY_COMPUTE_GROWTH_RATE*computePerOperation + ((1.0-SERVER_KNOBS->PROXY_COMPUTE_GROWTH_RATE)*pProxyCommitData->commitComputePerOperation[self->latencyBucket]);
		}
	}

	return Void();
}

ACTOR Future<Void> transactionLogging(CommitBatchContext* self) {
	state ProxyCommitData* const pProxyCommitData = self->pProxyCommitData;

	try {
		choose {
			when(Version ver = wait(self->loggingComplete)) {
				pProxyCommitData->minKnownCommittedVersion = std::max(pProxyCommitData->minKnownCommittedVersion, ver);
			}
			when(wait(pProxyCommitData->committedVersion.whenAtLeast( self->commitVersion + 1 ))) {}
		}
	} catch(Error &e) {
		if(e.code() == error_code_broken_promise) {
			throw master_tlog_failed();
		}
		throw;
	}

	pProxyCommitData->lastCommitLatency = now() - self->commitStartTime;
	pProxyCommitData->lastCommitTime = std::max(pProxyCommitData->lastCommitTime.get(), self->commitStartTime);

	wait(yield(TaskPriority::ProxyCommitYield2));

	if( pProxyCommitData->popRemoteTxs && self->msg.popTo > ( pProxyCommitData->txsPopVersions.size() ? pProxyCommitData->txsPopVersions.back().second : pProxyCommitData->lastTxsPop ) ) {
		if(pProxyCommitData->txsPopVersions.size() >= SERVER_KNOBS->MAX_TXS_POP_VERSION_HISTORY) {
			TraceEvent(SevWarnAlways, "DiscardingTxsPopHistory").suppressFor(1.0);
			pProxyCommitData->txsPopVersions.pop_front();
		}

		pProxyCommitData->txsPopVersions.emplace_back(self->commitVersion, self->msg.popTo);
	}
	pProxyCommitData->logSystem->popTxs(self->msg.popTo);

	return Void();
}

ACTOR Future<Void> reply(CommitBatchContext* self) {
	state ProxyCommitData* const pProxyCommitData = self->pProxyCommitData;

	const Optional<UID>& debugID = self->debugID;

	if ( self->prevVersion && self->commitVersion - self->prevVersion < SERVER_KNOBS->MAX_VERSIONS_IN_FLIGHT/2 )
		debug_advanceMinCommittedVersion(UID(), self->commitVersion);

	//TraceEvent("ProxyPushed", pProxyCommitData->dbgid).detail("PrevVersion", prevVersion).detail("Version", commitVersion);
	if (debugID.present())
		g_traceBatch.addEvent("CommitDebug", debugID.get().first(), "CommitProxyServer.commitBatch.AfterLogPush");

	for (auto &p : self->storeCommits) {
		ASSERT(!p.second.isReady());
		p.first.get().acknowledge.send(Void());
		ASSERT(p.second.isReady());
	}

	// After logging finishes, we report the commit version to master so that every other proxy can get the most
	// up-to-date live committed version. We also maintain the invariant that master's committed version >= self->committedVersion
	// by reporting commit version first before updating self->committedVersion. Otherwise, a client may get a commit
	// version that the master is not aware of, and next GRV request may get a version less than self->committedVersion.
	TEST(pProxyCommitData->committedVersion.get() > self->commitVersion);   // A later version was reported committed first
	if (self->commitVersion >= pProxyCommitData->committedVersion.get()) {
		wait(pProxyCommitData->master.reportLiveCommittedVersion.getReply(
		    ReportRawCommittedVersionRequest(self->commitVersion, self->lockedAfter, self->metadataVersionAfter,
		                                     pProxyCommitData->minKnownCommittedVersion),
		    TaskPriority::ProxyMasterVersionReply));
	}
	if( self->commitVersion > pProxyCommitData->committedVersion.get() ) {
		pProxyCommitData->locked = self->lockedAfter;
		pProxyCommitData->metadataVersion = self->metadataVersionAfter;
		pProxyCommitData->committedVersion.set(self->commitVersion);
	}

	if (self->forceRecovery) {
		TraceEvent(SevWarn, "RestartingTxnSubsystem", pProxyCommitData->dbgid).detail("Stage", "ProxyShutdown");
		throw worker_removed();
	}

	// Send replies to clients
	double endTime = g_network->timer();
	// Reset all to zero, used to track the correct index of each commitTransacitonRef on each resolver

	std::fill(self->nextTr.begin(), self->nextTr.end(), 0);
	for (int t = 0; t < self->trs.size(); t++) {
		auto& tr = self->trs[t];
		if (self->committed[t] == ConflictBatch::TransactionCommitted && (!self->locked || tr.isLockAware())) {
			ASSERT_WE_THINK(self->commitVersion != invalidVersion);
			tr.reply.send(CommitID(self->commitVersion, t, self->metadataVersionAfter));
		}
		else if (self->committed[t] == ConflictBatch::TransactionTooOld) {
			tr.reply.sendError(transaction_too_old());
		}
		else {
			// If enable the option to report conflicting keys from resolvers, we send back all keyranges' indices
			// through CommitID
			if (tr.transaction.report_conflicting_keys) {
				Standalone<VectorRef<int>> conflictingKRIndices;
				for (int resolverInd : self->transactionResolverMap[t]) {
					auto const& cKRs =
					    self->resolution[resolverInd]
					        .conflictingKeyRangeMap[self->nextTr[resolverInd]]; // nextTr[resolverInd] -> index of this trs[t]
					                                                      // on the resolver
					for (auto const& rCRIndex : cKRs)
						// read_conflict_range can change when sent to resolvers, mapping the index from resolver-side
						// to original index in commitTransactionRef
						conflictingKRIndices.push_back(conflictingKRIndices.arena(),
						                               self->txReadConflictRangeIndexMap[t][resolverInd][rCRIndex]);
				}
				// At least one keyRange index should be returned
				ASSERT(conflictingKRIndices.size());
				tr.reply.send(CommitID(invalidVersion, t, Optional<Value>(),
				                           Optional<Standalone<VectorRef<int>>>(conflictingKRIndices)));
			} else {
				tr.reply.sendError(not_committed());
			}
		}

		// Update corresponding transaction indices on each resolver
		for (int resolverInd : self->transactionResolverMap[t]) self->nextTr[resolverInd]++;

		// TODO: filter if pipelined with large commit
		const double duration = endTime - tr.requestTime();
		pProxyCommitData->stats.commitLatencySample.addMeasurement(duration);
		if(pProxyCommitData->latencyBandConfig.present()) {
			bool filter = self->maxTransactionBytes > pProxyCommitData->latencyBandConfig.get().commitConfig.maxCommitBytes.orDefault(std::numeric_limits<int>::max());
			pProxyCommitData->stats.commitLatencyBands.addMeasurement(duration, filter);
		}
	}

	++pProxyCommitData->stats.commitBatchOut;
	pProxyCommitData->stats.txnCommitOut += self->trs.size();
	pProxyCommitData->stats.txnConflicts += self->trs.size() - self->commitCount;
	pProxyCommitData->stats.txnCommitOutSuccess += self->commitCount;

	if(now() - pProxyCommitData->lastCoalesceTime > SERVER_KNOBS->RESOLVER_COALESCE_TIME) {
		pProxyCommitData->lastCoalesceTime = now();
		int lastSize = pProxyCommitData->keyResolvers.size();
		auto rs = pProxyCommitData->keyResolvers.ranges();
		Version oldestVersion = self->prevVersion - SERVER_KNOBS->MAX_WRITE_TRANSACTION_LIFE_VERSIONS;
		for(auto r = rs.begin(); r != rs.end(); ++r) {
			while(r->value().size() > 1 && r->value()[1].first < oldestVersion)
				r->value().pop_front();
			if(r->value().size() && r->value().front().first < oldestVersion)
				r->value().front().first = 0;
		}
		pProxyCommitData->keyResolvers.coalesce(allKeys);
		if(pProxyCommitData->keyResolvers.size() != lastSize)
			TraceEvent("KeyResolverSize", pProxyCommitData->dbgid).detail("Size", pProxyCommitData->keyResolvers.size());
	}

	// Dynamic batching for commits
	double target_latency = (now() - self->startTime) * SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_INTERVAL_LATENCY_FRACTION;
	pProxyCommitData->commitBatchInterval = std::max(
	    SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_INTERVAL_MIN,
	    std::min(SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_INTERVAL_MAX,
	             target_latency * SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_INTERVAL_SMOOTHER_ALPHA +
	                 pProxyCommitData->commitBatchInterval * (1 - SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_INTERVAL_SMOOTHER_ALPHA)));

	pProxyCommitData->commitBatchesMemBytesCount -= self->currentBatchMemBytesCount;
	ASSERT_ABORT(pProxyCommitData->commitBatchesMemBytesCount >= 0);
	wait(self->releaseFuture);

	return Void();
}

}	// namespace CommitBatch

// Commit one batch of transactions trs
ACTOR Future<Void> commitBatch(
		ProxyCommitData* self,
		vector<CommitTransactionRequest>* trs,
		int currentBatchMemBytesCount) {
	//WARNING: this code is run at a high priority (until the first delay(0)), so it needs to do as little work as possible
	state CommitBatch::CommitBatchContext context(self, trs, currentBatchMemBytesCount);

	// Active load balancing runs at a very high priority (to obtain accurate estimate of memory used by commit batches) so we need to downgrade here
	wait(delay(0, TaskPriority::ProxyCommit));

	context.pProxyCommitData->lastVersionTime = context.startTime;
	++context.pProxyCommitData->stats.commitBatchIn;
	context.setupTraceBatch();

	/////// Phase 1: Pre-resolution processing (CPU bound except waiting for a version # which is separately pipelined and *should* be available by now (unless empty commit); ordered; currently atomic but could yield)
	wait(CommitBatch::preresolutionProcessing(&context));

	/////// Phase 2: Resolution (waiting on the network; pipelined)
	wait(CommitBatch::getResolution(&context));

	////// Phase 3: Post-resolution processing (CPU bound except for very rare situations; ordered; currently atomic but doesn't need to be)
	wait(CommitBatch::postResolution(&context));

	/////// Phase 4: Logging (network bound; pipelined up to MAX_READ_TRANSACTION_LIFE_VERSIONS (limited by loop above))
	wait(CommitBatch::transactionLogging(&context));

	/////// Phase 5: Replies (CPU bound; no particular order required, though ordered execution would be best for latency)
	wait(CommitBatch::reply(&context));

	return Void();
}

ACTOR static Future<Void> doKeyServerLocationRequest( GetKeyServerLocationsRequest req, ProxyCommitData* commitData ) {
	// We can't respond to these requests until we have valid txnStateStore
	wait(commitData->validState.getFuture());
	wait(delay(0, TaskPriority::DefaultEndpoint));

	GetKeyServerLocationsReply rep;
	if(!req.end.present()) {
		auto r = req.reverse ? commitData->keyInfo.rangeContainingKeyBefore(req.begin) : commitData->keyInfo.rangeContaining(req.begin);
		vector<StorageServerInterface> ssis;
		ssis.reserve(r.value().src_info.size());
		for(auto& it : r.value().src_info) {
			ssis.push_back(it->interf);
		}
		rep.results.push_back(std::make_pair(r.range(), ssis));
	} else if(!req.reverse) {
		int count = 0;
		for(auto r = commitData->keyInfo.rangeContaining(req.begin); r != commitData->keyInfo.ranges().end() && count < req.limit && r.begin() < req.end.get(); ++r) {
			vector<StorageServerInterface> ssis;
			ssis.reserve(r.value().src_info.size());
			for(auto& it : r.value().src_info) {
				ssis.push_back(it->interf);
			}
			rep.results.push_back(std::make_pair(r.range(), ssis));
			count++;
		}
	} else {
		int count = 0;
		auto r = commitData->keyInfo.rangeContainingKeyBefore(req.end.get());
		while( count < req.limit && req.begin < r.end() ) {
			vector<StorageServerInterface> ssis;
			ssis.reserve(r.value().src_info.size());
			for(auto& it : r.value().src_info) {
				ssis.push_back(it->interf);
			}
			rep.results.push_back(std::make_pair(r.range(), ssis));
			if(r == commitData->keyInfo.ranges().begin()) {
				break;
			}
			count++;
			--r;
		}
	}
	req.reply.send(rep);
	++commitData->stats.keyServerLocationOut;
	return Void();
}

ACTOR static Future<Void> readRequestServer(CommitProxyInterface proxy, PromiseStream<Future<Void>> addActor,
                                            ProxyCommitData* commitData) {
	loop {
		GetKeyServerLocationsRequest req = waitNext(proxy.getKeyServersLocations.getFuture());
		//WARNING: this code is run at a high priority, so it needs to do as little work as possible
		if(req.limit != CLIENT_KNOBS->STORAGE_METRICS_SHARD_LIMIT && //Always do data distribution requests
		   commitData->stats.keyServerLocationIn.getValue() - commitData->stats.keyServerLocationOut.getValue() > SERVER_KNOBS->KEY_LOCATION_MAX_QUEUE_SIZE) {
			++commitData->stats.keyServerLocationErrors;
			req.reply.sendError(proxy_memory_limit_exceeded());
			TraceEvent(SevWarnAlways, "ProxyLocationRequestThresholdExceeded").suppressFor(60);
		} else {
			++commitData->stats.keyServerLocationIn;
			addActor.send(doKeyServerLocationRequest(req, commitData));
		}
	}
}

ACTOR static Future<Void> rejoinServer(CommitProxyInterface proxy, ProxyCommitData* commitData) {
	// We can't respond to these requests until we have valid txnStateStore
	wait(commitData->validState.getFuture());

	TraceEvent("ProxyReadyForReads", proxy.id());

	loop {
		GetStorageServerRejoinInfoRequest req = waitNext(proxy.getStorageServerRejoinInfo.getFuture());
		if (commitData->txnStateStore->readValue(serverListKeyFor(req.id)).get().present()) {
			GetStorageServerRejoinInfoReply rep;
			rep.version = commitData->version;
			rep.tag = decodeServerTagValue( commitData->txnStateStore->readValue(serverTagKeyFor(req.id)).get().get() );
			Standalone<RangeResultRef> history = commitData->txnStateStore->readRange(serverTagHistoryRangeFor(req.id)).get();
			for(int i = history.size()-1; i >= 0; i-- ) {
				rep.history.push_back(std::make_pair(decodeServerTagHistoryKey(history[i].key), decodeServerTagValue(history[i].value)));
			}
			auto localityKey = commitData->txnStateStore->readValue(tagLocalityListKeyFor(req.dcId)).get();
			rep.newLocality = false;
			if( localityKey.present() ) {
				int8_t locality = decodeTagLocalityListValue(localityKey.get());
				if(rep.tag.locality != tagLocalityUpgraded && locality != rep.tag.locality) {
					TraceEvent(SevWarnAlways, "SSRejoinedWithChangedLocality").detail("Tag", rep.tag.toString()).detail("DcId", req.dcId).detail("NewLocality", locality);
				} else if(locality != rep.tag.locality) {
					uint16_t tagId = 0;
					std::vector<uint16_t> usedTags;
					auto tagKeys = commitData->txnStateStore->readRange(serverTagKeys).get();
					for( auto& kv : tagKeys ) {
						Tag t = decodeServerTagValue( kv.value );
						if(t.locality == locality) {
							usedTags.push_back(t.id);
						}
					}
					auto historyKeys = commitData->txnStateStore->readRange(serverTagHistoryKeys).get();
					for( auto& kv : historyKeys ) {
						Tag t = decodeServerTagValue( kv.value );
						if(t.locality == locality) {
							usedTags.push_back(t.id);
						}
					}
					std::sort(usedTags.begin(), usedTags.end());

					int usedIdx = 0;
					for(; usedTags.size() > 0 && tagId <= usedTags.end()[-1]; tagId++) {
						if(tagId < usedTags[usedIdx]) {
							break;
						} else {
							usedIdx++;
						}
					}
					rep.newTag = Tag(locality, tagId);
				}
			} else if(rep.tag.locality != tagLocalityUpgraded) {
				TraceEvent(SevWarnAlways, "SSRejoinedWithUnknownLocality").detail("Tag", rep.tag.toString()).detail("DcId", req.dcId);
			} else {
				rep.newLocality = true;
				int8_t maxTagLocality = -1;
				auto localityKeys = commitData->txnStateStore->readRange(tagLocalityListKeys).get();
				for( auto& kv : localityKeys ) {
					maxTagLocality = std::max(maxTagLocality, decodeTagLocalityListValue( kv.value ));
				}
				rep.newTag = Tag(maxTagLocality+1,0);
			}
			req.reply.send(rep);
		} else {
			req.reply.sendError(worker_removed());
		}
	}
}

ACTOR Future<Void> ddMetricsRequestServer(CommitProxyInterface proxy, Reference<AsyncVar<ServerDBInfo>> db) {
	loop {
		choose {
			when(state GetDDMetricsRequest req = waitNext(proxy.getDDMetrics.getFuture()))
			{
				if (!db->get().distributor.present()) {
					req.reply.sendError(dd_not_found());
					continue;
				}
				ErrorOr<GetDataDistributorMetricsReply> reply =
				    wait(errorOr(db->get().distributor.get().dataDistributorMetrics.getReply(
				        GetDataDistributorMetricsRequest(req.keys, req.shardLimit))));
				if (reply.isError()) {
					req.reply.sendError(reply.getError());
				} else {
					GetDDMetricsReply newReply;
					newReply.storageMetricsList = reply.get().storageMetricsList;
					req.reply.send(newReply);
				}
			}
		}
	}
}

ACTOR Future<Void> monitorRemoteCommitted(ProxyCommitData* self) {
	loop {
		wait(delay(0)); //allow this actor to be cancelled if we are removed after db changes.
		state Optional<std::vector<OptionalInterface<TLogInterface>>> remoteLogs;
		if(self->db->get().recoveryState >= RecoveryState::ALL_LOGS_RECRUITED) {
			for(auto& logSet : self->db->get().logSystemConfig.tLogs) {
				if(!logSet.isLocal) {
					remoteLogs = logSet.tLogs;
					for(auto& tLog : logSet.tLogs) {
						if(!tLog.present()) {
							remoteLogs = Optional<std::vector<OptionalInterface<TLogInterface>>>();
							break;
						}
					}
					break;
				}
			}
		}

		if(!remoteLogs.present()) {
			wait(self->db->onChange());
			continue;
		}
		self->popRemoteTxs = true;

		state Future<Void> onChange = self->db->onChange();
		loop {
			state std::vector<Future<TLogQueuingMetricsReply>> replies;
			for(auto &it : remoteLogs.get()) {
				replies.push_back(brokenPromiseToNever( it.interf().getQueuingMetrics.getReply( TLogQueuingMetricsRequest() ) ));
			}
			wait( waitForAll(replies) || onChange );

			if(onChange.isReady()) {
				break;
			}

			//FIXME: use the configuration to calculate a more precise minimum recovery version.
			Version minVersion = std::numeric_limits<Version>::max();
			for(auto& it : replies) {
				minVersion = std::min(minVersion, it.get().v);
			}

			while(self->txsPopVersions.size() && self->txsPopVersions.front().first <= minVersion) {
				self->lastTxsPop = self->txsPopVersions.front().second;
				self->logSystem->popTxs(self->txsPopVersions.front().second, tagLocalityRemoteLog);
				self->txsPopVersions.pop_front();
			}

			wait( delay(SERVER_KNOBS->UPDATE_REMOTE_LOG_VERSION_INTERVAL) || onChange );
			if(onChange.isReady()) {
				break;
			}
		}
	}
}

ACTOR Future<Void> proxySnapCreate(ProxySnapRequest snapReq, ProxyCommitData* commitData) {
	TraceEvent("SnapCommitProxy_SnapReqEnter")
	    .detail("SnapPayload", snapReq.snapPayload)
	    .detail("SnapUID", snapReq.snapUID);
	try {
		// whitelist check
		ExecCmdValueString execArg(snapReq.snapPayload);
		StringRef binPath = execArg.getBinaryPath();
		if (!isWhitelisted(commitData->whitelistedBinPathVec, binPath)) {
			TraceEvent("SnapCommitProxy_WhiteListCheckFailed")
			    .detail("SnapPayload", snapReq.snapPayload)
			    .detail("SnapUID", snapReq.snapUID);
			throw snap_path_not_whitelisted();
		}
		// db fully recovered check
		if (commitData->db->get().recoveryState != RecoveryState::FULLY_RECOVERED)  {
			// Cluster is not fully recovered and needs TLogs
			// from previous generation for full recovery.
			// Currently, snapshot of old tlog generation is not
			// supported and hence failing the snapshot request until
			// cluster is fully_recovered.
			TraceEvent("SnapCommitProxy_ClusterNotFullyRecovered")
			    .detail("SnapPayload", snapReq.snapPayload)
			    .detail("SnapUID", snapReq.snapUID);
			throw snap_not_fully_recovered_unsupported();
		}

		auto result =
			commitData->txnStateStore->readValue(LiteralStringRef("log_anti_quorum").withPrefix(configKeysPrefix)).get();
		int logAntiQuorum = 0;
		if (result.present()) {
			logAntiQuorum = atoi(result.get().toString().c_str());
		}
		// FIXME: logAntiQuorum not supported, remove it later,
		// In version2, we probably don't need this limtiation, but this needs to be tested.
		if (logAntiQuorum > 0) {
			TraceEvent("SnapCommitProxy_LogAnitQuorumNotSupported")
			    .detail("SnapPayload", snapReq.snapPayload)
			    .detail("SnapUID", snapReq.snapUID);
			throw snap_log_anti_quorum_unsupported();
		}

		// send a snap request to DD
		if (!commitData->db->get().distributor.present()) {
			TraceEvent(SevWarnAlways, "DataDistributorNotPresent").detail("Operation", "SnapRequest");
			throw dd_not_found();
		}
		state Future<ErrorOr<Void>> ddSnapReq =
			commitData->db->get().distributor.get().distributorSnapReq.tryGetReply(DistributorSnapRequest(snapReq.snapPayload, snapReq.snapUID));
		try {
			wait(throwErrorOr(ddSnapReq));
		} catch (Error& e) {
			TraceEvent("SnapCommitProxy_DDSnapResponseError")
			    .detail("SnapPayload", snapReq.snapPayload)
			    .detail("SnapUID", snapReq.snapUID)
			    .error(e, true /*includeCancelled*/);
			throw e;
		}
		snapReq.reply.send(Void());
	} catch (Error& e) {
		TraceEvent("SnapCommitProxy_SnapReqError")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID)
		    .error(e, true /*includeCancelled*/);
		if (e.code() != error_code_operation_cancelled) {
			snapReq.reply.sendError(e);
		} else {
			throw e;
		}
	}
	TraceEvent("SnapCommitProxy_SnapReqExit")
	    .detail("SnapPayload", snapReq.snapPayload)
	    .detail("SnapUID", snapReq.snapUID);
	return Void();
}

ACTOR Future<Void> proxyCheckSafeExclusion(Reference<AsyncVar<ServerDBInfo>> db, ExclusionSafetyCheckRequest req) {
	TraceEvent("SafetyCheckCommitProxyBegin");
	state ExclusionSafetyCheckReply reply(false);
	if (!db->get().distributor.present()) {
		TraceEvent(SevWarnAlways, "DataDistributorNotPresent").detail("Operation", "ExclusionSafetyCheck");
		req.reply.send(reply);
		return Void();
	}
	try {
		state Future<ErrorOr<DistributorExclusionSafetyCheckReply>> safeFuture =
		    db->get().distributor.get().distributorExclCheckReq.tryGetReply(
		        DistributorExclusionSafetyCheckRequest(req.exclusions));
		DistributorExclusionSafetyCheckReply _reply = wait(throwErrorOr(safeFuture));
		reply.safe = _reply.safe;
	} catch (Error& e) {
		TraceEvent("SafetyCheckCommitProxyResponseError").error(e);
		if (e.code() != error_code_operation_cancelled) {
			req.reply.sendError(e);
			return Void();
		} else {
			throw e;
		}
	}
	TraceEvent("SafetyCheckCommitProxyFinish");
	req.reply.send(reply);
	return Void();
}

ACTOR Future<Void> reportTxnTagCommitCost(UID myID, Reference<AsyncVar<ServerDBInfo>> db,
                                          UIDTransactionTagMap<TransactionCommitCostEstimation>* ssTrTagCommitCost) {
	state Future<Void> nextRequestTimer = Never();
	state Future<Void> nextReply = Never();
	if (db->get().ratekeeper.present()) nextRequestTimer = Void();
	loop choose {
		when(wait(db->onChange())) {
			if (db->get().ratekeeper.present()) {
				TraceEvent("ProxyRatekeeperChanged", myID).detail("RKID", db->get().ratekeeper.get().id());
				nextRequestTimer = Void();
			} else {
				TraceEvent("ProxyRatekeeperDied", myID);
				nextRequestTimer = Never();
			}
		}
		when(wait(nextRequestTimer)) {
			nextRequestTimer = Never();
			if (db->get().ratekeeper.present()) {
				nextReply = brokenPromiseToNever(db->get().ratekeeper.get().reportCommitCostEstimation.getReply(
				    ReportCommitCostEstimationRequest(*ssTrTagCommitCost)));
			} else {
				nextReply = Never();
			}
		}
		when(wait(nextReply)) {
			nextReply = Never();
			ssTrTagCommitCost->clear();
			nextRequestTimer = delay(SERVER_KNOBS->REPORT_TRANSACTION_COST_ESTIMATION_DELAY);
		}
	}
}

ACTOR Future<Void> commitProxyServerCore(CommitProxyInterface proxy, MasterInterface master,
                                         Reference<AsyncVar<ServerDBInfo>> db, LogEpoch epoch,
                                         Version recoveryTransactionVersion, bool firstProxy,
                                         std::string whitelistBinPaths) {
	state ProxyCommitData commitData(proxy.id(), master, proxy.getConsistentReadVersion, recoveryTransactionVersion, proxy.commit, db, firstProxy);

	state Future<Sequence> sequenceFuture = (Sequence)0;
	state PromiseStream< std::pair<vector<CommitTransactionRequest>, int> > batchedCommits;
	state Future<Void> commitBatcherActor;
	state Future<Void> lastCommitComplete = Void();

	state PromiseStream<Future<Void>> addActor;
	state Future<Void> onError = transformError( actorCollection(addActor.getFuture()), broken_promise(), master_tlog_failed() );
	state double lastCommit = 0;
	state std::set<Sequence> txnSequences;
	state Sequence maxSequence = std::numeric_limits<Sequence>::max();

	state GetHealthMetricsReply healthMetricsReply;
	state GetHealthMetricsReply detailedHealthMetricsReply;

	addActor.send( waitFailureServer(proxy.waitFailure.getFuture()) );
	addActor.send(traceRole(Role::COMMIT_PROXY, proxy.id()));

	//TraceEvent("CommitProxyInit1", proxy.id());

	// Wait until we can load the "real" logsystem, since we don't support switching them currently
	while (!(commitData.db->get().master.id() == master.id() && commitData.db->get().recoveryState >= RecoveryState::RECOVERY_TRANSACTION)) {
		//TraceEvent("ProxyInit2", proxy.id()).detail("LSEpoch", db->get().logSystemConfig.epoch).detail("Need", epoch);
		wait(commitData.db->onChange());
	}
	state Future<Void> dbInfoChange = commitData.db->onChange();
	//TraceEvent("ProxyInit3", proxy.id());

	commitData.resolvers = commitData.db->get().resolvers;
	ASSERT(commitData.resolvers.size() != 0);

	auto rs = commitData.keyResolvers.modify(allKeys);
	for(auto r = rs.begin(); r != rs.end(); ++r)
		r->value().emplace_back(0,0);

	commitData.logSystem = ILogSystem::fromServerDBInfo(proxy.id(), commitData.db->get(), false, addActor);
	commitData.logAdapter = new LogSystemDiskQueueAdapter(commitData.logSystem, Reference<AsyncVar<PeekTxsInfo>>(), 1, false);
	commitData.txnStateStore = keyValueStoreLogSystem(commitData.logAdapter, proxy.id(), 2e9, true, true, true);
	createWhitelistBinPathVec(whitelistBinPaths, commitData.whitelistedBinPathVec);

	commitData.updateLatencyBandConfig(commitData.db->get().latencyBandConfig);

	// ((SERVER_MEM_LIMIT * COMMIT_BATCHES_MEM_FRACTION_OF_TOTAL) / COMMIT_BATCHES_MEM_TO_TOTAL_MEM_SCALE_FACTOR) is only a approximate formula for limiting the memory used.
	// COMMIT_BATCHES_MEM_TO_TOTAL_MEM_SCALE_FACTOR is an estimate based on experiments and not an accurate one.
	state int64_t commitBatchesMemoryLimit = std::min(SERVER_KNOBS->COMMIT_BATCHES_MEM_BYTES_HARD_LIMIT, static_cast<int64_t>((SERVER_KNOBS->SERVER_MEM_LIMIT * SERVER_KNOBS->COMMIT_BATCHES_MEM_FRACTION_OF_TOTAL) / SERVER_KNOBS->COMMIT_BATCHES_MEM_TO_TOTAL_MEM_SCALE_FACTOR));
	TraceEvent(SevInfo, "CommitBatchesMemoryLimit").detail("BytesLimit", commitBatchesMemoryLimit);

	addActor.send(monitorRemoteCommitted(&commitData));
	addActor.send(readRequestServer(proxy, addActor, &commitData));
	addActor.send(rejoinServer(proxy, &commitData));
	addActor.send(ddMetricsRequestServer(proxy, db));
	addActor.send(reportTxnTagCommitCost(proxy.id(), db, &commitData.ssTrTagCommitCost));

	// wait for txnStateStore recovery
	wait(success(commitData.txnStateStore->readValue(StringRef())));

	int commitBatchByteLimit =
	    (int)std::min<double>(SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_BYTES_MAX,
	                          std::max<double>(SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_BYTES_MIN,
	                                           SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_BYTES_SCALE_BASE *
	                                               pow(commitData.db->get().client.commitProxies.size(),
	                                                   SERVER_KNOBS->COMMIT_TRANSACTION_BATCH_BYTES_SCALE_POWER)));

	commitBatcherActor = commitBatcher(&commitData, batchedCommits, proxy.commit.getFuture(), commitBatchByteLimit, commitBatchesMemoryLimit);
	loop choose{
		when( wait( dbInfoChange ) ) {
			dbInfoChange = commitData.db->onChange();
			if(commitData.db->get().master.id() == master.id() && commitData.db->get().recoveryState >= RecoveryState::RECOVERY_TRANSACTION) {
				commitData.logSystem = ILogSystem::fromServerDBInfo(proxy.id(), commitData.db->get(), false, addActor);
				for(auto it : commitData.tag_popped) {
					commitData.logSystem->pop(it.second, it.first);
				}
				commitData.logSystem->popTxs(commitData.lastTxsPop, tagLocalityRemoteLog);
			}

			commitData.updateLatencyBandConfig(commitData.db->get().latencyBandConfig);
		}
		when(wait(onError)) {}
		when(std::pair<vector<CommitTransactionRequest>, int> batchedRequests = waitNext(batchedCommits.getFuture())) {
			//WARNING: this code is run at a high priority, so it needs to do as little work as possible
			const vector<CommitTransactionRequest> &trs = batchedRequests.first;
			int batchBytes = batchedRequests.second;
			//TraceEvent("CommitProxyCTR", proxy.id()).detail("CommitTransactions", trs.size()).detail("TransactionRate", transactionRate).detail("TransactionQueue", transactionQueue.size()).detail("ReleasedTransactionCount", transactionCount);
			if (trs.size() || (commitData.db->get().recoveryState >= RecoveryState::ACCEPTING_COMMITS && now() - lastCommit >= SERVER_KNOBS->MAX_COMMIT_BATCH_INTERVAL)) {
				lastCommit = now();

				if (trs.size() || lastCommitComplete.isReady()) {
					lastCommitComplete = commitBatch(
						&commitData,
						const_cast<std::vector<CommitTransactionRequest>*>(&batchedRequests.first),
						batchBytes
					);
					addActor.send(lastCommitComplete);
				}
			}
		}
		when(ProxySnapRequest snapReq = waitNext(proxy.proxySnapReq.getFuture())) {
			TraceEvent(SevDebug, "SnapMasterEnqueue");
			addActor.send(proxySnapCreate(snapReq, &commitData));
		}
		when(ExclusionSafetyCheckRequest exclCheckReq = waitNext(proxy.exclusionSafetyCheckReq.getFuture())) {
			addActor.send(proxyCheckSafeExclusion(db, exclCheckReq));
		}
		when(state TxnStateRequest req = waitNext(proxy.txnState.getFuture())) {
			state ReplyPromise<Void> reply = req.reply;
			if(req.last) maxSequence = req.sequence + 1;
			if (!txnSequences.count(req.sequence)) {
				txnSequences.insert(req.sequence);

				ASSERT(!commitData.validState.isSet()); // Although we may receive the CommitTransactionRequest for the recovery transaction before all of the TxnStateRequest, we will not get a resolution result from any resolver until the master has submitted its initial (sequence 0) resolution request, which it doesn't do until we have acknowledged all TxnStateRequests

				for(auto& kv : req.data)
					commitData.txnStateStore->set(kv, &req.arena);
				commitData.txnStateStore->commit(true);

				if(txnSequences.size() == maxSequence) {
					state KeyRange txnKeys = allKeys;
					Standalone<RangeResultRef> UIDtoTagMap = commitData.txnStateStore->readRange( serverTagKeys ).get();
					state std::map<Tag, UID> tag_uid;
					for (const KeyValueRef kv : UIDtoTagMap) {
						tag_uid[decodeServerTagValue(kv.value)] = decodeServerTagKey(kv.key);
					}
					loop {
						wait(yield());
						Standalone<RangeResultRef> data = commitData.txnStateStore->readRange(txnKeys, SERVER_KNOBS->BUGGIFIED_ROW_LIMIT, SERVER_KNOBS->APPLY_MUTATION_BYTES).get();
						if(!data.size()) break;
						((KeyRangeRef&)txnKeys) = KeyRangeRef( keyAfter(data.back().key, txnKeys.arena()), txnKeys.end );

						MutationsVec mutations;
						std::vector<std::pair<MapPair<Key,ServerCacheInfo>,int>> keyInfoData;
						vector<UID> src, dest;
						ServerCacheInfo info;
						for(auto &kv : data) {
							if( kv.key.startsWith(keyServersPrefix) ) {
								KeyRef k = kv.key.removePrefix(keyServersPrefix);
								if(k != allKeys.end) {
									decodeKeyServersValue(tag_uid, kv.value, src, dest);
									info.tags.clear();
									info.src_info.clear();
									info.dest_info.clear();
									for (const auto& id : src) {
										auto storageInfo = getStorageInfo(id, &commitData.storageCache, commitData.txnStateStore);
										ASSERT(storageInfo->tag != invalidTag);
										info.tags.push_back( storageInfo->tag );
										info.src_info.push_back( storageInfo );
									}
									for (const auto& id : dest) {
										auto storageInfo = getStorageInfo(id, &commitData.storageCache, commitData.txnStateStore);
										ASSERT(storageInfo->tag != invalidTag);
										info.tags.push_back( storageInfo->tag );
										info.dest_info.push_back( storageInfo );
									}
									uniquify(info.tags);
									keyInfoData.emplace_back(MapPair<Key,ServerCacheInfo>(k, info), 1);
								}
							} else {
								mutations.emplace_back(mutations.arena(), MutationRef::SetValue, kv.key, kv.value);
							}
						}

						//insert keyTag data separately from metadata mutations so that we can do one bulk insert which avoids a lot of map lookups.
						commitData.keyInfo.rawInsert(keyInfoData);

						Arena arena;
						bool confChanges;
						applyMetadataMutations(commitData, arena, Reference<ILogSystem>(), mutations,
						                       /* pToCommit= */ nullptr, confChanges,
						                       /* popVersion= */ 0, /* initialCommit= */ true);
					}

					auto lockedKey = commitData.txnStateStore->readValue(databaseLockedKey).get();
					commitData.locked = lockedKey.present() && lockedKey.get().size();
					commitData.metadataVersion = commitData.txnStateStore->readValue(metadataVersionKey).get();

					commitData.txnStateStore->enableSnapshot();
				}
			}
			addActor.send(broadcastTxnRequest(req, SERVER_KNOBS->TXN_STATE_SEND_AMOUNT, true));
			wait(yield());
		}
	}
}

ACTOR Future<Void> checkRemoved(Reference<AsyncVar<ServerDBInfo>> db, uint64_t recoveryCount,
                                CommitProxyInterface myInterface) {
	loop{
		if (db->get().recoveryCount >= recoveryCount &&
		    !std::count(db->get().client.commitProxies.begin(), db->get().client.commitProxies.end(), myInterface)) {
			throw worker_removed();
		}
		wait(db->onChange());
	}
}

ACTOR Future<Void> commitProxyServer(CommitProxyInterface proxy, InitializeCommitProxyRequest req,
                                     Reference<AsyncVar<ServerDBInfo>> db, std::string whitelistBinPaths) {
	try {
		state Future<Void> core =
		    commitProxyServerCore(proxy, req.master, db, req.recoveryCount, req.recoveryTransactionVersion,
		                          req.firstProxy, whitelistBinPaths);
		wait(core || checkRemoved(db, req.recoveryCount, proxy));
	}
	catch (Error& e) {
		TraceEvent("CommitProxyTerminated", proxy.id()).error(e, true);

		if (e.code() != error_code_worker_removed && e.code() != error_code_tlog_stopped &&
			e.code() != error_code_master_tlog_failed && e.code() != error_code_coordinators_changed &&
			e.code() != error_code_coordinated_state_conflict && e.code() != error_code_new_coordinators_timed_out) {
			throw;
		}
	}
	return Void();
}
