/*
 * WriteThrottling.actor.cpp
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

#include "fdbserver/TesterInterface.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbserver/workloads/BulkSetup.actor.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/TagThrottle.h"
#include "flow/actorcompiler.h" // This must be the last #include.

constexpr int SAMPLE_SIZE = 10000;
// workload description:
// This workload aims to test whether we can throttling some bad clients that doing penetrating write on write hot-spot
// range. There are several good clientActor just randomly do read and write ops in transaction. Also, some bad
// clientActor has high probability to read and write a particular hot-spot. If the tag-based throttling works right on
// write-heavy tags, it will only limit the bad clientActor without influence other normal actors. This workload also
// output TPS and latency of read/set/clear operations to do eyeball check. We want this new feature would not cause
// more load to the cluster (Maybe some visualization tools is needed to show the trend of metrics)
struct WriteTagThrottlingWorkload : KVWorkload {

	// Performance metrics
	int goodActorTrNum = 0, goodActorRetries = 0, goodActorTooOldRetries = 0, goodActorCommitFailedRetries = 0;
	int badActorTrNum = 0, badActorRetries = 0, badActorTooOldRetries = 0, badActorCommitFailedRetries = 0;
	int goodActorThrottleRetries = 0, badActorThrottleRetries = 0;
	double badActorTotalLatency = 0.0, goodActorTotalLatency = 0.0;
	ContinuousSample<double> badActorReadLatency, goodActorReadLatency;
	ContinuousSample<double> badActorCommitLatency, goodActorCommitLatency;
	// Test configuration
	// KVWorkload::actorCount
	int goodActorPerClient, badActorPerClient;
	int numWritePerTr, numReadPerTr, numClearPerTr;
	int keyCount;
	double badOpRate, hotRangeRate;
	double testDuration;
	bool writeThrottle;
	bool populateData;

	// internal states
	double trInterval;
	TransactionTag badTag, goodTag;
	bool fastSuccess = false;
	int rangeEachBadActor = 0;
	std::set<std::string> throttledTags;
	static constexpr const char* NAME = "WriteTagThrottling";
	static constexpr int MIN_TAGS_PER_TRANSACTION = 1;
	static constexpr int MIN_TRANSACTION_TAG_LENGTH = 2;

	WriteTagThrottlingWorkload(WorkloadContext const& wcx)
	  : KVWorkload(wcx), badActorCommitLatency(SAMPLE_SIZE), badActorReadLatency(SAMPLE_SIZE),
	    goodActorCommitLatency(SAMPLE_SIZE), goodActorReadLatency(SAMPLE_SIZE) {
		testDuration = getOption(options, LiteralStringRef("testDuration"), 120.0);
		badOpRate = getOption(options, LiteralStringRef("badOpRate"), 0.9);
		numWritePerTr = getOption(options, LiteralStringRef("numWritePerTr"), 1);
		numReadPerTr = getOption(options, LiteralStringRef("numReadPerTr"), 1);
		numClearPerTr = getOption(options, LiteralStringRef("numClearPerTr"), 1);
		hotRangeRate = getOption(options, LiteralStringRef("hotRangeRate"), 0.1);
		populateData = getOption(options, LiteralStringRef("populateData"), true);

		writeThrottle = getOption(options, LiteralStringRef("writeThrottle"), false);
		badActorPerClient = getOption(options, LiteralStringRef("badActorPerClient"), 1);
		goodActorPerClient = getOption(options, LiteralStringRef("goodActorPerClient"), 1);
		actorCount = goodActorPerClient + badActorPerClient;

		keyCount = getOption(options, LiteralStringRef("keyCount"),
		                     std::max(3000, clientCount * actorCount * 3)); // enough keys to avoid too many conflicts
		trInterval = actorCount * 1.0 / getOption(options, LiteralStringRef("trPerSecond"), 1000);
		if(badActorPerClient > 0) {
			rangeEachBadActor = keyCount / (clientCount * badActorPerClient);
		}

		badTag = TransactionTag(std::string("bT"));
		goodTag = TransactionTag(std::string("gT"));
	}

	std::string description() const override { return WriteTagThrottlingWorkload::NAME; }

	ACTOR static Future<Void> _setup(Database cx, WriteTagThrottlingWorkload* self) {
		ASSERT(CLIENT_KNOBS->MAX_TAGS_PER_TRANSACTION >= MIN_TAGS_PER_TRANSACTION &&
		       CLIENT_KNOBS->MAX_TRANSACTION_TAG_LENGTH >= MIN_TRANSACTION_TAG_LENGTH);
		if(self->populateData) {
			wait(bulkSetup(cx, self, self->keyCount, Promise<double>()));
		}
		if(self->clientId == 0) {
			wait(ThrottleApi::enableAuto(cx, true));
		}
		return Void();
	}
	Future<Void> setup(const Database& cx) override {
		if (CLIENT_KNOBS->MAX_TAGS_PER_TRANSACTION < MIN_TAGS_PER_TRANSACTION ||
		    CLIENT_KNOBS->MAX_TRANSACTION_TAG_LENGTH < MIN_TRANSACTION_TAG_LENGTH ) {
			fastSuccess = true;
			return Void();
		}
		return _setup(cx, this);
	}
	ACTOR static Future<Void> _start(Database cx, WriteTagThrottlingWorkload* self) {
		vector<Future<Void>> clientActors;
		int actorId;
		for (actorId = 0; actorId < self->goodActorPerClient; ++actorId) {
			clientActors.push_back(clientActor(false, actorId, 0, cx, self));
		}
		for (actorId = 0; actorId < self->badActorPerClient; ++actorId) {
			clientActors.push_back(clientActor(true, actorId, self->badOpRate, cx, self));
		}
		clientActors.push_back(throttledTagUpdater(cx, self));
		wait(timeout(waitForAll(clientActors), self->testDuration, Void()));
		return Void();
	}
	Future<Void> start(Database const& cx) override {
		if(fastSuccess) return Void();
		return _start(cx, this);
	}
	Future<bool> check(Database const& cx) override {
		if(fastSuccess) return true;
		if (writeThrottle) {
			if (!badActorThrottleRetries && !goodActorThrottleRetries) {
				TraceEvent(SevWarn, "NoThrottleTriggered");
			}
			if (badActorThrottleRetries < goodActorThrottleRetries) {
				TraceEvent(SevWarnAlways, "IncorrectThrottle")
				    .detail("BadActorThrottleRetries", badActorThrottleRetries)
				    .detail("GoodActorThrottleRetries", goodActorThrottleRetries);
			}
			if(!throttledTags.empty() && throttledTags.count(badTag.toString()) == 0) {
				TraceEvent(SevWarnAlways, "IncorrectThrottle")
				    .detail("ThrottledTagNumber", throttledTags.size())
					.detail("ThrottledTags", setToString(throttledTags));
				return false;
			}
			// NOTE also do eyeball check of Retries.throttle and Avg Latency
		}
		return true;
	}
	void getMetrics(vector<PerfMetric>& m) override {
		m.push_back(PerfMetric("Transactions (badActor)", badActorTrNum, false));
		m.push_back(PerfMetric("Transactions (goodActor)", goodActorTrNum, false));
		m.push_back(PerfMetric("Avg Latency (ms, badActor)", 1000 * badActorTotalLatency / badActorTrNum, true));
		m.push_back(PerfMetric("Avg Latency (ms, goodActor)", 1000 * goodActorTotalLatency / goodActorTrNum, true));

		m.push_back(PerfMetric("Retries (badActor)", badActorRetries, false));
		m.push_back(PerfMetric("Retries (goodActor)", goodActorRetries, false));

		m.push_back(PerfMetric("Retries.throttle (badActor)", badActorThrottleRetries, false));
		m.push_back(PerfMetric("Retries.throttle (goodActor)", goodActorThrottleRetries, false));

		m.push_back(PerfMetric("Retries.too_old (badActor)", badActorTooOldRetries, false));
		m.push_back(PerfMetric("Retries.too_old (goodActor)", goodActorTooOldRetries, false));

		m.push_back(PerfMetric("Retries.commit_failed (badActor)", badActorCommitFailedRetries, false));
		m.push_back(PerfMetric("Retries.commit_failed (goodActor)", goodActorCommitFailedRetries, false));

		// Read Sampleing
		m.push_back(PerfMetric("Avg Read Latency (ms, badActor)", 1000 * badActorReadLatency.mean(), true));
		m.push_back(PerfMetric("Avg Read Latency (ms, goodActor)", 1000 * goodActorReadLatency.mean(), true));
		m.push_back(PerfMetric("95% Read Latency (ms, badActor)", 1000 * badActorReadLatency.percentile(0.95), true));
		m.push_back(PerfMetric("95% Read Latency (ms, goodActor)", 1000 * goodActorReadLatency.percentile(0.95), true));
		m.push_back(PerfMetric("50% Read Latency (ms, badActor)", 1000 * badActorReadLatency.median(), true));
		m.push_back(PerfMetric("50% Read Latency (ms, goodActor)", 1000 * goodActorReadLatency.median(), true));

		// Commit Sampleing
		m.push_back(PerfMetric("Avg Commit Latency (ms, badActor)", 1000 * badActorCommitLatency.mean(), true));
		m.push_back(PerfMetric("Avg Commit Latency (ms, goodActor)", 1000 * goodActorCommitLatency.mean(), true));
		m.push_back(
		    PerfMetric("95% Commit Latency (ms, badActor)", 1000 * badActorCommitLatency.percentile(0.95), true));
		m.push_back(
		    PerfMetric("95% Commit Latency (ms, goodActor)", 1000 * goodActorCommitLatency.percentile(0.95), true));
		m.push_back(PerfMetric("50% Commit Latency (ms, badActor)", 1000 * badActorCommitLatency.median(), true));
		m.push_back(PerfMetric("50% Commit Latency (ms, goodActor)", 1000 * goodActorCommitLatency.median(), true));
	}

	Standalone<KeyValueRef> operator()(uint64_t n) {
		return KeyValueRef(keyForIndex(n), generateVal());
	}
	// return a key based on useReadKey
	Key generateKey(bool useReadKey, int startIdx, int availableRange) {
		if (useReadKey) {
			return keyForIndex(startIdx + deterministicRandom()->randomInt(0, availableRange), false);
		}
		return getRandomKey();
	}
	// return a range based on useClearKey
	KeyRange generateRange(bool useClearKey, int startIdx, int availableRange) {
		int a, b;
		if(useClearKey) {
			a = deterministicRandom()->randomInt(startIdx, availableRange + startIdx);
			b = deterministicRandom()->randomInt(startIdx, availableRange + startIdx);
		}
		else {
			a = deterministicRandom()->randomInt(0, keyCount);
			b = deterministicRandom()->randomInt(0, keyCount);
		}
		if (a > b) std::swap(a, b);
		if (a == b) return singleKeyRange(keyForIndex(a, false));
		return KeyRange(KeyRangeRef(keyForIndex(a, false), keyForIndex(b, false)));
	}
	Value generateVal() { return Value(deterministicRandom()->randomAlphaNumeric(maxValueBytes)); }

	// read and write value on particular/random Key
	ACTOR static Future<Void> clientActor(bool isBadActor, int actorId, double badOpRate, Database cx,
	                                      WriteTagThrottlingWorkload* self) {
		state int startIdx = (self->clientId * self->badActorPerClient + actorId) * self->rangeEachBadActor;
		state int availableRange = std::max(int(self->rangeEachBadActor * self->hotRangeRate), 1);
		state double lastTime = now();
		state double opStart;
		state Key key;
		try {
			loop {
				wait(poisson(&lastTime, self->trInterval));
				state double trStart;
				state Transaction tr(cx);
				state int i;
				// give tag to client
				if (self->writeThrottle) {
					ASSERT(CLIENT_KNOBS->MAX_TAGS_PER_TRANSACTION >= MIN_TAGS_PER_TRANSACTION);
					tr.options.tags.clear();
					tr.options.readTags.clear();
					if (isBadActor) {
						tr.setOption(FDBTransactionOptions::AUTO_THROTTLE_TAG, self->badTag);
					} else if (deterministicRandom()->coinflip()) {
						tr.setOption(FDBTransactionOptions::AUTO_THROTTLE_TAG, self->goodTag);
					}
				}

				trStart = now();
				while (true) {
					try {
						for (i = 0; i < self->numClearPerTr; ++i) {
							bool useClearKey = deterministicRandom()->random01() < badOpRate;
							tr.clear(self->generateRange(useClearKey, startIdx, availableRange));
						}
						for (i = 0; i < self->numWritePerTr; ++i) {
							bool useReadKey = deterministicRandom()->random01() < badOpRate;
							key = self->generateKey(useReadKey, startIdx, availableRange);
							tr.set(key, self->generateVal());
						}
						for (i = 0; i < self->numReadPerTr; ++i) {
							bool useReadKey = deterministicRandom()->random01() < badOpRate;
							ASSERT(self->keyCount >= actorId);
							key = self->generateKey(useReadKey, startIdx, availableRange);
							opStart = now();
							Optional<Value> v = wait(tr.get(key));
							double duration = now() - opStart;
							isBadActor ? self->badActorReadLatency.addSample(duration)
							           : self->goodActorReadLatency.addSample(duration);
						}
						opStart = now();
						wait(tr.commit());
						double duration = now() - opStart;
						isBadActor ? self->badActorCommitLatency.addSample(duration)
						           : self->goodActorCommitLatency.addSample(duration);
						break;
					} catch (Error& e) {
						if (e.code() == error_code_transaction_too_old) {
							isBadActor ? ++self->badActorTooOldRetries : ++self->goodActorTooOldRetries;
						} else if (e.code() == error_code_not_committed) {
							isBadActor ? ++self->badActorCommitFailedRetries : ++self->goodActorCommitFailedRetries;
						} else if (e.code() == error_code_tag_throttled) {
							isBadActor ? ++self->badActorThrottleRetries : ++self->goodActorThrottleRetries;
						}
						wait(tr.onError(e));
					}
					isBadActor ? ++self->badActorRetries : ++self->goodActorRetries;
				}
				double duration = now() - trStart;
				if (isBadActor) {
					++self->badActorTrNum;
					self->badActorTotalLatency += duration;
				} else {
					++self->goodActorTrNum;
					self->goodActorTotalLatency += duration;
				}
			}
		} catch (Error& e) {
			TraceEvent(SevError, "WriteThrottling").error(e);
			throw;
		}
	}

	void recordThrottledTags(std::vector<TagThrottleInfo>& tags) {
		for(auto& tag: tags) {
			throttledTags.insert(tag.tag.toString());
		}
	}
	ACTOR static Future<Void> throttledTagUpdater(Database cx, WriteTagThrottlingWorkload* self) {
		state std::vector<TagThrottleInfo> tags;
		loop {
			wait(delay(1.0));
			wait(store(tags, ThrottleApi::getThrottledTags(cx, CLIENT_KNOBS->TOO_MANY, true)));
			self->recordThrottledTags(tags);
		};
	}

	static std::string setToString(const std::set<std::string>& myset) {
		std::string res;
		for(auto& s: myset) {
			res.append(s).push_back(' ');
		}
		return res;
	}
};

WorkloadFactory<WriteTagThrottlingWorkload> WriteTagThrottlingWorkloadFactory(WriteTagThrottlingWorkload::NAME);
