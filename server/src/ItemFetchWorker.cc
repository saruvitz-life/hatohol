/*
 * Copyright (C) 2014 Project Hatohol
 *
 * This file is part of Hatohol.
 *
 * Hatohol is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License, version 3
 * as published by the Free Software Foundation.
 *
 * Hatohol is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Hatohol. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <semaphore.h>
#include <ReadWriteLock.h>
#include <Reaper.h>
#include <SmartTime.h>
#include "ItemFetchWorker.h"
#include "UnifiedDataStore.h"

using namespace std;
using namespace mlpl;

struct FetcherJob {
	LocalHostIdType hostId;
	shared_ptr<DataStore> dataStore;
};

struct ItemFetchWorker::Impl
{
	const static size_t      maxRunningFetchers = 8;
	const static timespec    minUpdateInterval;

	ReadWriteLock           rwlock;
	std::deque<FetcherJob>  fetcherJobQueue;
	size_t                  remainingFetchersCount;
	SmartTime               nextAllowedUpdateTime;
	sem_t                   updatedSemaphore;
	Signal0                 itemFetchedSignal;

	Impl(void)
	: remainingFetchersCount(0)
	{
		sem_init(&updatedSemaphore, 0, 0);
	}

	virtual ~Impl()
	{
		sem_destroy(&updatedSemaphore);
	};
};

// Currently item data cache mechanism is inactive by setting the following
// variable to zero. This reason is to avoid the problem that cannot get
// items if it's requested soon after getting items under another server,
// as written in #2442.
// However, it has not been discussed well whether the cache mechanism is
// unnecessary or not. So the following variable and the related code
// are still remaining.
const timespec ItemFetchWorker::Impl::minUpdateInterval = {0, 0};

// ---------------------------------------------------------------------------
// Public methods
// ---------------------------------------------------------------------------
ItemFetchWorker::ItemFetchWorker(void)
: m_impl(new Impl())
{
}

ItemFetchWorker::~ItemFetchWorker()
{
}

bool ItemFetchWorker::start(
  const ItemsQueryOption &option, Closure0 *closure)
{
	DataStoreVector allDataStores =
	  UnifiedDataStore::getInstance()->getDataStoreVector();

	const ServerIdType targetServerId = option.getTargetServerId();
	const LocalHostIdType targetHostId = option.getTargetHostId();
	LocalHostIdVector targetHostIds;
	if (targetHostId != ALL_LOCAL_HOSTS) {
		targetHostIds.push_back(targetHostId);
	}

	if (allDataStores.empty())
		return false;

	m_impl->rwlock.writeLock();
	if (closure)
		m_impl->itemFetchedSignal.connect(closure);
	m_impl->remainingFetchersCount = allDataStores.size();
	for (size_t i = 0; i < allDataStores.size(); i++) {
		shared_ptr<DataStore> dataStore = allDataStores[i];

		bool shouldWake = true;
		if (targetServerId != ALL_SERVERS &&
		    targetServerId != dataStore->getMonitoringServerInfo().id)
			shouldWake = false;
		else if (!dataStore->isFetchItemsSupported())
			shouldWake = false;

		if (!shouldWake) {
			m_impl->remainingFetchersCount--;
			continue;
		}

		if (i < Impl::maxRunningFetchers){
			if (!runFetcher(targetHostIds, dataStore))
				m_impl->remainingFetchersCount--;
		} else {
			m_impl->fetcherJobQueue.push_back({targetHostId, dataStore});
		}
	}

	bool started = m_impl->remainingFetchersCount > 0;
	m_impl->rwlock.unlock();

	return started;
}

bool ItemFetchWorker::updateIsNeeded(void)
{
	m_impl->rwlock.readLock();
	Reaper<ReadWriteLock> lockReaper(&m_impl->rwlock, ReadWriteLock::unlock);

	if (m_impl->remainingFetchersCount > 0)
		return false;

	SmartTime currTime(SmartTime::INIT_CURR_TIME);
	return currTime >= m_impl->nextAllowedUpdateTime;
}

void ItemFetchWorker::waitCompletion(void)
{
	if (sem_wait(&m_impl->updatedSemaphore) == -1)
		MLPL_ERR("Failed to call sem_wait: %d\n", errno);
}

// ---------------------------------------------------------------------------
// Protected methods
// ---------------------------------------------------------------------------
void ItemFetchWorker::updatedCallback(Closure0 *closure)
{
	m_impl->rwlock.writeLock();
	Reaper<ReadWriteLock> lockReaper(&m_impl->rwlock, ReadWriteLock::unlock);

	auto &fetcherJob = m_impl->fetcherJobQueue;
	if (!fetcherJob.empty()) {
		runFetcher({fetcherJob.front().hostId}, fetcherJob.front().dataStore);
		fetcherJob.pop_front();
	}

	if (m_impl->remainingFetchersCount <= 0)
		return;
	else
		m_impl->remainingFetchersCount--;

	if (m_impl->remainingFetchersCount > 0)
		return;

	if (sem_post(&m_impl->updatedSemaphore) == -1)
		MLPL_ERR("Failed to call sem_post: %d\n", errno);
	m_impl->nextAllowedUpdateTime.setCurrTime();
	m_impl->nextAllowedUpdateTime += m_impl->minUpdateInterval;
	m_impl->itemFetchedSignal();
	m_impl->itemFetchedSignal.clear();
}

bool ItemFetchWorker::runFetcher(const LocalHostIdVector targetHostIds,
				 shared_ptr<DataStore> dataStore)
{
	struct ClosureWithDataStore : public ClosureTemplate0<ItemFetchWorker>
	{
		shared_ptr<DataStore> dataStore;

		ClosureWithDataStore(ItemFetchWorker *impl, shared_ptr<DataStore> ds)
		: ClosureTemplate0<ItemFetchWorker>(
		    impl, &ItemFetchWorker::updatedCallback),
		  dataStore(ds)
		{
		}
	};

	return dataStore->startOnDemandFetchItems(
	  targetHostIds, new ClosureWithDataStore(this, dataStore));
}
