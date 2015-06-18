// wiredtiger_session_cache.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include <boost/thread.hpp>

#include "mongo/db/stats/timer_stats.h"
#include "mongo/db/commands/server_status_metric.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/log.h"

namespace mongo {

    WiredTigerSession::WiredTigerSession(WT_CONNECTION* conn, int epoch, uint64_t id)
        : _epoch(epoch),
          _session(NULL),
          _cursorsOut(0) {
        _next = NULL;
	sessionId = id;
        int ret = conn->open_session(conn, NULL, "isolation=snapshot", &_session);
        invariantWTOK(ret);
    }

    WiredTigerSession::~WiredTigerSession() {
        if (_session) {
            int ret = _session->close(_session, NULL);
            invariantWTOK(ret);
        }
    }

    WT_CURSOR* WiredTigerSession::getCursor(const std::string& uri,
                                            uint64_t id,
                                            bool forRecordStore) {
        {
            if ( !_curmap.empty() ) {
                for (CursorCache::iterator i = _curmap.begin(); i != _curmap.end(); ++i ) {
                    if ( i->first == id ) {
                        WT_CURSOR* save = i->second;
                        _curmap.erase(i);
                        _cursorsOut++;
                        return save;
                    }
                }
            }
        }
        WT_CURSOR* c = NULL;
        int ret = _session->open_cursor(_session,
                                        uri.c_str(),
                                        NULL,
                                        forRecordStore ? "" : "overwrite=false",
                                        &c);
        if (ret != ENOENT)
            invariantWTOK(ret);
        if ( c ) _cursorsOut++;
        return c;
    }

    void WiredTigerSession::releaseCursor(uint64_t id, WT_CURSOR *cursor) {
        invariant( _session );
        invariant( cursor );
        _cursorsOut--;

        // We want to store at most 10 cursors, each with a different id
        if ( _curmap.size() > 10u ) {
            invariantWTOK( cursor->close(cursor) );
        }
        else {
            invariantWTOK( cursor->reset( cursor ) );
            for (CursorCache::iterator i = _curmap.begin(); i != _curmap.end(); ++i ) {
                if ( i->first == id ) {
                    invariantWTOK( cursor->close(cursor) );
                    return;
                }
            }
            _curmap.push_back( std::make_pair(id, cursor) );
        }
    }

    void WiredTigerSession::closeAllCursors() {
        invariant( _session );
        for (CursorCache::iterator i = _curmap.begin(); i != _curmap.end(); ++i ) {
            WT_CURSOR *cursor = i->second;
            if (cursor) {
                int ret = cursor->close(cursor);
                invariantWTOK(ret);
            }
        }
        _curmap.clear();
    }

    namespace {
        AtomicUInt64 nextCursorId(1);
        AtomicUInt64 currSessionsInCache(1);
	AtomicUInt64 nextSessionId(1);
    }
    // static
    uint64_t WiredTigerSession::genCursorId() {
        return nextCursorId.fetchAndAdd(1);
    }

    // -----------------------

    WiredTigerSessionCache::WiredTigerSessionCache( WiredTigerKVEngine* engine )
        : _engine( engine ), _conn( engine->getConnection() ),
          _sessionsOut(0), _shuttingDown(0), _highWaterMark(1) {
        _head = NULL;
    }

    WiredTigerSessionCache::WiredTigerSessionCache( WT_CONNECTION* conn )
        : _engine( NULL ), _conn( conn ),
          _sessionsOut(0), _shuttingDown(0), _highWaterMark(1) {
        _head = NULL;
    }

    WiredTigerSessionCache::~WiredTigerSessionCache() {
        shuttingDown();
    }

    void WiredTigerSessionCache::shuttingDown() {
        if (_shuttingDown.load()) return;
        _shuttingDown.store(1);

        {
            // This ensures that any calls, which are currently inside of getSession/releaseSession
            // will be able to complete before we start cleaning up the pool. Any others, which are
            // about to enter will return immediately because of _shuttingDown == true.
            stdx::lock_guard<boost::shared_mutex> lk(_shutdownLock);
        }

        closeAll();
    }

    void WiredTigerSessionCache::closeAll() {
        // Increment the epoch as we are now closing all sessions with this epoch
        _epoch++;
	log() << "closeall sessions called";
        // Grab each session from the list and delete
        while ( _head.load() != NULL ){
            WiredTigerSession* cachedSession = _head.load();
            // Keep trying to remove the head until we succeed
            while ( !_head.compare_exchange_weak(cachedSession, cachedSession->_next)) {
                cachedSession = _head.load();
                if ( cachedSession == NULL)
                    return;
            }

            currSessionsInCache.fetchAndSubtract(1);
            delete cachedSession;
       }
    }

    WiredTigerSession* WiredTigerSessionCache::getSession() {
        boost::shared_lock<boost::shared_mutex> shutdownLock(_shutdownLock);

        // We should never be able to get here after _shuttingDown is set, because no new
        // operations should be allowed to start.
        invariant(!_shuttingDown.loadRelaxed());

        _sessionsOut++;
        // Grab the current top session
        WiredTigerSession* cachedSession = _head.load();
            /**
             * We are popping here, compare_exchange will try and replace the
             * current head (our session) with the next session in the queue
             */
        while (cachedSession && !_head.compare_exchange_weak(cachedSession, cachedSession->_next)) { }
	if ( cachedSession != NULL ) {
            // Mark the next session as NULL for when we put it back
            cachedSession->_next = NULL;
            currSessionsInCache.fetchAndSubtract(1);
            log() << " took session: " << cachedSession->sessionId;
            return cachedSession;
       }

        // Outside of the cache partition lock, but on release will be put back on the cache
	        nextSessionId.fetchAndAdd(1);
        return new WiredTigerSession(_conn, _epoch, nextSessionId.load());
    }

    void WiredTigerSessionCache::releaseSession( WiredTigerSession* session ) {
        invariant( session );
        invariant(session->cursorsOut() == 0);

        boost::shared_lock<boost::shared_mutex> shutdownLock(_shutdownLock);
        if (_shuttingDown.loadRelaxed()) {
            // Leak the session in order to avoid race condition with clean shutdown, where the
            // storage engine is ripped from underneath transactions, which are not "active"
            // (i.e., do not have any locks), but are just about to delete the recovery unit.
            // See SERVER-16031 for more information.
            return;
        }

        // This checks that we are only caching idle sessions and not something which might hold
        // locks or otherwise prevent truncation.
        {
            WT_SESSION* ss = session->getSession();
            uint64_t range;
            invariantWTOK(ss->transaction_pinned_range(ss, &range));
            invariant(range == 0);
        }

        bool returnedToCache = false;

        invariant(session->_getEpoch() <= _epoch);

        // Set the high water mark if we need too
        if( _sessionsOut.load() > _highWaterMark.load()) {
            _highWaterMark = _sessionsOut.load();
        }

        /**
         * In this case we only want to return sessions until we hit the maximum number of
         * sessions we have ever seen demand for concurrently. We also want to immediately
         * delete any session that is from a non-current epoch.
         */

        if (session->_getEpoch() == _epoch && currSessionsInCache.load() < _highWaterMark.load() ) {
            session->_next = _head.load();
            // Switch in the new head
            while ( !_head.compare_exchange_weak(session->_next, session)) {
                // Should check the session sizing in here.
            }
            returnedToCache = true;
            currSessionsInCache.fetchAndAdd(1);
        }

        _sessionsOut--;
            log() << " returning session: " << session->sessionId;
        // Do all cleanup outside of the cache partition spinlock.
        if (!returnedToCache) {
            log() << "deleted session: " << session->sessionId;
            delete session;
        }

        // Do all cleanup outside of the cache partition spinlock.
        if (!returnedToCache) {
            delete session;
        }

        if (_engine && _engine->haveDropsQueued()) {
            _engine->dropAllQueued();
        }
    }
}
