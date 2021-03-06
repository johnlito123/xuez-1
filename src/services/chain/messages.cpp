/*
 * This file is part of the Eccoin project
 * Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2016 The Bitcoin Core developers
 * Copyright (c) 2014-2018 The Eccoin developers
 * Copyright (c) 2017-2019 The Xuez developers
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "messages.h"

#include "services/ans.h"
#include "serialize.h"
#include "sync.h"
#include "chain/tx.h"
#include "services/servicetx.h"
#include "src/txmempool.h"
#include "src/main.h"
#include "src/util.h"
#include "src/services/args.h"
#include "src/addrman.h"
#include "version.h"
#include "protocol.h"
#include "src/merkleblock.h"
#include "validationinterface.h"
#include "src/utilstrencodings.h"
#include "src/chain.h"
#include "consensus/validation.h"
#include "networks/networktemplate.h"
#include "networks/netman.h"
#include "processblock.h"
#include "processheader.h"
#include "src/init.h"
#include "services/chain/netmessagemaker.h"
#include "policy/policy.h"
#include "policy/fees.h"
#include "services/mempool.h"
#include "src/services/chain/processtx.h"

#include <algorithm>
#include <vector>
#include <map>
#include <boost/thread.hpp>
#include <string>
#include <boost/foreach.hpp>
#include <boost/range/adaptor/reversed.hpp>

//////////////////////////////////////////////////////////////////////////////
//
// Messages
//

void EraseOrphansFor(NodeId peer) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/**
 * Filter for transactions that were recently rejected by
 * AcceptToMemoryPool. These are not rerequested until the chain tip
 * changes, at which point the entire filter is reset. Protected by
 * cs_main.
 *
 * Without this filter we'd be re-requesting txs from each of our peers,
 * increasing bandwidth consumption considerably. For instance, with 100
 * peers, half of which relay a tx we don't accept, that might be a 50x
 * bandwidth increase. A flooding attacker attempting to roll-over the
 * filter using minimum-sized, 60byte, transactions might manage to send
 * 1000/sec if we have fast peers, so we pick 120,000 to give our peers a
 * two minute window to send invs to us.
 *
 * Decreasing the false positive rate is fairly cheap, so we pick one in a
 * million to make it highly unlikely for users to have issues with this
 * filter.
 *
 * Memory used: 1.3 MB
 */
std::unique_ptr<CRollingBloomFilter> recentRejects;
std::map<uint256, std::pair<NodeId, std::list<QueuedBlock>::iterator> > mapBlocksInFlight;
uint256 hashRecentRejectsChainTip;
std::map<uint256, std::pair<NodeId, bool>> mapBlockSource;
/** Relay map, protected by cs_main. */
typedef std::map<uint256, CTransaction> MapRelay;
MapRelay mapRelay;
std::deque<std::pair<int64_t, MapRelay::iterator>> vRelayExpiration;

static CCriticalSection cs_most_recent_block;
static std::shared_ptr<const CBlock> most_recent_block;
static uint256 most_recent_block_hash;

/** Map maintaining per-node state. Requires cs_main. */
std::map<NodeId, CNodeState> mapNodeState;

std::map<uint256, int64_t> pendingStx;
CCriticalSection cs_pendstx;


// Requires cs_main.
CNodeState *State(NodeId pnode) {
    std::map<NodeId, CNodeState>::iterator it = mapNodeState.find(pnode);
    if (it == mapNodeState.end()) {
        return nullptr;
    }

    return &it->second;
}

uint32_t GetFetchFlags(CNode *pfrom, const CBlockIndex *pprev,
                       const Consensus::Params &chainparams) {
    uint32_t nFetchFlags = 0;
    return nFetchFlags;
}

void PushNodeVersion(CNode *pnode, CConnman &connman, int64_t nTime) {
    ServiceFlags nLocalNodeServices = pnode->GetLocalServices();
    uint64_t nonce = pnode->GetLocalNonce();
    int nNodeStartingHeight = pnode->GetMyStartingHeight();
    NodeId nodeid = pnode->GetId();
    CAddress addr = pnode->addr;

    CAddress addrYou = (addr.IsRoutable() && !IsProxy(addr)
                            ? addr
                            : CAddress(CService(), addr.nServices));
    CAddress addrMe = CAddress(CService(), nLocalNodeServices);

    connman.PushMessage(pnode,
                        CNetMsgMaker(MIN_PROTO_VERSION)
                            .Make(NetMsgType::VERSION, PROTOCOL_VERSION,
                                  (uint64_t)nLocalNodeServices, nTime, addrYou,
                                  addrMe, nonce, strSubVersion,
                                  nNodeStartingHeight, ::fRelayTxes));

    if (fLogIPs) {
        LogPrintf("send version message: version %d, blocks=%d, "
                             "us=%s, them=%s, peer=%d\n",
                 PROTOCOL_VERSION, nNodeStartingHeight, addrMe.ToString(),
                 addrYou.ToString(), nodeid);
    } else {
        LogPrintf(
            "send version message: version %d, blocks=%d, us=%s, peer=%d\n",
            PROTOCOL_VERSION, nNodeStartingHeight, addrMe.ToString(), nodeid);
    }
}

void InitializeNode(CNode *pnode, CConnman &connman) {
    CAddress addr = pnode->addr;
    std::string addrName = pnode->GetAddrName();
    NodeId nodeid = pnode->GetId();
    {
        LOCK(cs_main);
        mapNodeState.emplace_hint(
            mapNodeState.end(), std::piecewise_construct,
            std::forward_as_tuple(nodeid),
            std::forward_as_tuple(addr, std::move(addrName)));
    }

    if (!pnode->fInbound) {
        PushNodeVersion(pnode, connman, GetTime());
    }
}

void FinalizeNode(NodeId nodeid, bool &fUpdateConnectionTime) {
    fUpdateConnectionTime = false;
    LOCK(cs_main);
    CNodeState *state = State(nodeid);

    if (state->fSyncStarted) {
        nSyncStarted--;
    }

    if (state->nMisbehavior == 0 && state->fCurrentlyConnected) {
        fUpdateConnectionTime = true;
    }

    for (const QueuedBlock &entry : state->vBlocksInFlight) {
        mapBlocksInFlight.erase(entry.hash);
    }
    // Get rid of stale mapBlockSource entries for this peer as they may leak
    // if we don't clean them up (I saw on the order of ~100 stale entries on
    // a full resynch in my testing -- these entries stay forever).
    // Performance note: most of the time mapBlockSource has 0 or 1 entries.
    // During synch of blockchain it may end up with as many as 1000 entries,
    // which still only takes ~1ms to iterate through on even old hardware.
    // So this memleak cleanup is not expensive and worth doing since even
    // small leaks are bad. :)
    for (auto it = mapBlockSource.begin(); it != mapBlockSource.end(); /*NA*/) {
        if (it->second.first == nodeid) {
            mapBlockSource.erase(it++);
        } else {
            ++it;
        }
    }

    EraseOrphansFor(nodeid);
    nPreferredDownload -= state->fPreferredDownload;
    nPeersWithValidatedDownloads -= (state->nBlocksInFlightValidHeaders != 0);
    assert(nPeersWithValidatedDownloads >= 0);

    mapNodeState.erase(nodeid);

    if (mapNodeState.empty()) {
        // Do a consistency check after the last peer is removed.
        assert(mapBlocksInFlight.empty());
        assert(nPreferredDownload == 0);
        assert(nPeersWithValidatedDownloads == 0);
    }
}

bool GetNodeStateStats(NodeId nodeid, CNodeStateStats &stats) {
    LOCK(cs_main);
    CNodeState *state = State(nodeid);
    if (state == nullptr) {
        return false;
    }
    stats.nMisbehavior = state->nMisbehavior;
    stats.nSyncHeight =
        state->pindexBestKnownBlock ? state->pindexBestKnownBlock->nHeight : -1;
    stats.nCommonHeight = state->pindexLastCommonBlock
                              ? state->pindexLastCommonBlock->nHeight
                              : -1;
    for (const QueuedBlock &queue : state->vBlocksInFlight) {
        if (queue.pindex) {
            stats.vHeightInFlight.push_back(queue.pindex->nHeight);
        }
    }
    return true;
}

// Requires cs_main.
void Misbehaving(NodeId pnode, int howmuch, const std::string &reason) {
    if (howmuch == 0) {
        return;
    }

    CNodeState *state = State(pnode);
    if (state == nullptr) {
        return;
    }

    state->nMisbehavior += howmuch;
    int banscore = gArgs.GetArg("-banscore", DEFAULT_BANSCORE_THRESHOLD);
    if (state->nMisbehavior >= banscore &&
        state->nMisbehavior - howmuch < banscore) {
        LogPrintf(
            "%s: %s peer=%d (%d -> %d) reason: %s BAN THRESHOLD EXCEEDED\n",
            __func__, state->name, pnode, state->nMisbehavior - howmuch,
            state->nMisbehavior, reason.c_str());
        state->fShouldBan = true;
    } else {
        LogPrintf("%s: %s peer=%d (%d -> %d) reason: %s\n", __func__,
                  state->name, pnode, state->nMisbehavior - howmuch,
                  state->nMisbehavior, reason.c_str());
    }
}

// overloaded variant of above to operate on CNode*s
static void Misbehaving(CNode *node, int howmuch, const std::string &reason) {
    Misbehaving(node->GetId(), howmuch, reason);
}



// Requires cs_main.
// Returns a bool indicating whether we requested this block.
bool MarkBlockAsReceived(const uint256& hash)
{
    std::map<uint256, std::pair<NodeId, std::list<QueuedBlock>::iterator> >::iterator itInFlight = mapBlocksInFlight.find(hash);
    if (itInFlight != mapBlocksInFlight.end())
    {
        CNodeState *state = State(itInFlight->second.first);
        state->nBlocksInFlightValidHeaders -= itInFlight->second.second->fValidatedHeaders;
        if (state->nBlocksInFlightValidHeaders == 0 && itInFlight->second.second->fValidatedHeaders)
        {
            // Last validated block on the queue was received.
            nPeersWithValidatedDownloads--;
        }
        if (state->vBlocksInFlight.begin() == itInFlight->second.second)
        {
            // First block on the queue was received, update the start download time for the next one
            state->nDownloadingSince = std::max(state->nDownloadingSince, GetTimeMicros());
        }
        state->vBlocksInFlight.erase(itInFlight->second.second);
        state->nBlocksInFlight--;
        state->nStallingSince = 0;
        mapBlocksInFlight.erase(itInFlight);
        return true;
    }
    return false;
}

/** Find the last common ancestor two blocks have.
 *  Both pa and pb must be non-NULL. */
const CBlockIndex* LastCommonAncestor(const CBlockIndex* pa, const CBlockIndex* pb)
{
    if (pa->nHeight > pb->nHeight) {
        pa = pa->GetAncestor(pb->nHeight);
    } else if (pb->nHeight > pa->nHeight) {
        pb = pb->GetAncestor(pa->nHeight);
    }

    while (pa != pb && pa && pb) {
        pa = pa->pprev;
        pb = pb->pprev;
    }

    // Eventually all chain branches meet at the genesis block.
    assert(pa == pb);
    return pa;
}

// Requires cs_main
bool CanDirectFetch(const Consensus::Params &consensusParams)
{
    int64_t targetSpacing = consensusParams.nTargetSpacing;
    if(pnetMan->getChainActive()->chainActive.Tip()->GetMedianTimePast() > SERVICE_UPGRADE_HARDFORK)
    {
        targetSpacing = 150;
    }
    return pnetMan->getChainActive()->chainActive.Tip()->GetBlockTime() > GetAdjustedTime() - targetSpacing * 20;
}

static void RelayTransaction(const CTransaction &tx, CConnman &connman)
{
    CInv inv(MSG_TX, tx.GetId());
    connman.ForEachNode([&inv](CNode *pnode) { pnode->PushInventory(inv); });
}

void RelayServiceTransaction(const CServiceTransaction &stx, CConnman &connman)
{
    CInv inv(MSG_STX, stx.GetHash());
    connman.ForEachNode([&inv](CNode *pnode) { pnode->PushInventory(inv); });
}

static void RelayAddress(const CAddress &addr, bool fReachable,
                         CConnman &connman) {
    // Limited relaying of addresses outside our network(s)
    unsigned int nRelayNodes = fReachable ? 2 : 1;

    // Relay to a limited number of other nodes.
    // Use deterministic randomness to send to the same nodes for 24 hours at a
    // time so the addrKnowns of the chosen nodes prevent repeats.
    uint64_t hashAddr = addr.GetHash();
    const CSipHasher hasher =
        connman.GetDeterministicRandomizer(RANDOMIZER_ID_ADDRESS_RELAY)
            .Write(hashAddr << 32)
            .Write((GetTime() + hashAddr) / (24 * 60 * 60));
    FastRandomContext insecure_rand;

    std::array<std::pair<uint64_t, CNode *>, 2> best{
        {{0, nullptr}, {0, nullptr}}};
    assert(nRelayNodes <= best.size());

    auto sortfunc = [&best, &hasher, nRelayNodes](CNode *pnode)
    {
        uint64_t hashKey = CSipHasher(hasher).Write(pnode->id).Finalize();
        for (unsigned int i = 0; i < nRelayNodes; i++) {
            if (hashKey > best[i].first) {
                std::copy(best.begin() + i, best.begin() + nRelayNodes - 1,
                          best.begin() + i + 1);
                best[i] = std::make_pair(hashKey, pnode);
                break;
            }
        }
    };

    auto pushfunc = [&addr, &best, nRelayNodes, &insecure_rand] {
        for (unsigned int i = 0; i < nRelayNodes && best[i].first != 0; i++) {
            best[i].second->PushAddress(addr, insecure_rand);
        }
    };

    connman.ForEachNodeThen(std::move(sortfunc), std::move(pushfunc));
}

bool AddOrphanTx(const CTransaction& tx, NodeId peer) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    uint256 hash = tx.GetHash();
    if (mapOrphanTransactions.count(hash))
        return false;

    // Ignore big transactions, to avoid a
    // send-big-orphans memory exhaustion attack. If a peer has a legitimate
    // large transaction with a missing parent then we assume
    // it will rebroadcast it later, after the parent transaction(s)
    // have been mined or received.
    // 10,000 orphans, each of which is at most 5,000 bytes big is
    // at most 500 megabytes of orphans:
    unsigned int sz = GetSerializeSize(tx, SER_NETWORK, CTransaction::CURRENT_VERSION);
    if (sz > 5000)
    {
        LogPrint("mempool", "ignoring large orphan tx (size: %u, hash: %s)\n", sz, hash.ToString());
        return false;
    }

    mapOrphanTransactions[hash].tx = tx;
    mapOrphanTransactions[hash].fromPeer = peer;
    for(const CTxIn& txin : tx.vin)
        mapOrphanTransactionsByPrev[txin.prevout.hash].insert(hash);

    LogPrint("mempool", "stored orphan tx %s (mapsz %u prevsz %u)\n", hash.ToString(),
             mapOrphanTransactions.size(), mapOrphanTransactionsByPrev.size());
    return true;
}


void static EraseOrphanTx(uint256 hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    std::map<uint256, COrphanTx>::iterator it = mapOrphanTransactions.find(hash);
    if (it == mapOrphanTransactions.end())
        return;
    for(const CTxIn& txin : it->second.tx.vin)
    {
        std::map<uint256, std::set<uint256> >::iterator itPrev = mapOrphanTransactionsByPrev.find(txin.prevout.hash);
        if (itPrev == mapOrphanTransactionsByPrev.end())
            continue;
        itPrev->second.erase(hash);
        if (itPrev->second.empty())
            mapOrphanTransactionsByPrev.erase(itPrev);
    }
    mapOrphanTransactions.erase(it);
}

void EraseOrphansFor(NodeId peer)
{
    int nErased = 0;
    std::map<uint256, COrphanTx>::iterator iter = mapOrphanTransactions.begin();
    while (iter != mapOrphanTransactions.end())
    {
        std::map<uint256, COrphanTx>::iterator maybeErase = iter++; // increment to avoid iterator becoming invalid
        if (maybeErase->second.fromPeer == peer)
        {
            EraseOrphanTx(maybeErase->second.tx.GetHash());
            ++nErased;
        }
    }
    if (nErased > 0) LogPrint("mempool", "Erased %d orphan tx from peer %d\n", nErased, peer);
}


unsigned int LimitOrphanTxSize(unsigned int nMaxOrphans) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    unsigned int nEvicted = 0;
    while (mapOrphanTransactions.size() > nMaxOrphans)
    {
        // Evict a random orphan:
        uint256 randomhash = GetRandHash();
        std::map<uint256, COrphanTx>::iterator it = mapOrphanTransactions.lower_bound(randomhash);
        if (it == mapOrphanTransactions.end())
            it = mapOrphanTransactions.begin();
        EraseOrphanTx(it->first);
        ++nEvicted;
    }
    return nEvicted;
}

// Requires cs_main.
void MarkBlockAsInFlight(NodeId nodeid, const uint256& hash, const Consensus::Params& consensusParams, const CBlockIndex *pindex = NULL) {
    CNodeState *state = State(nodeid);
    assert(state != NULL);

    // Make sure it's not listed somewhere already.
    MarkBlockAsReceived(hash);

    QueuedBlock newentry = {hash, pindex, pindex != NULL};
    std::list<QueuedBlock>::iterator it = state->vBlocksInFlight.insert(state->vBlocksInFlight.end(), newentry);
    state->nBlocksInFlight++;
    state->nBlocksInFlightValidHeaders += newentry.fValidatedHeaders;
    if (state->nBlocksInFlight == 1) {
        // We're starting a block download (batch) from this peer.
        state->nDownloadingSince = GetTimeMicros();
    }
    if (state->nBlocksInFlightValidHeaders == 1 && pindex != NULL) {
        nPeersWithValidatedDownloads++;
    }
    mapBlocksInFlight[hash] = std::make_pair(nodeid, it);
}

/** Check whether the last unknown block a peer advertized is not yet known. */
void ProcessBlockAvailability(NodeId nodeid) {
    CNodeState *state = State(nodeid);
    assert(state != NULL);

    if (!state->hashLastUnknownBlock.IsNull()) {
        BlockMap::iterator itOld = pnetMan->getChainActive()->mapBlockIndex.find(state->hashLastUnknownBlock);
        if (itOld != pnetMan->getChainActive()->mapBlockIndex.end() && itOld->second->nChainWork > 0) {
            if (state->pindexBestKnownBlock == NULL || itOld->second->nChainWork >= state->pindexBestKnownBlock->nChainWork)
                state->pindexBestKnownBlock = itOld->second;
            state->hashLastUnknownBlock.SetNull();
        }
    }
}

void UpdatePreferredDownload(CNode* node, CNodeState* state)
{
    nPreferredDownload -= state->fPreferredDownload;

    // Whether this node should be marked as a preferred download node.
    state->fPreferredDownload = (!node->fInbound || node->fWhitelisted) && !node->fOneShot && !node->fClient;

    nPreferredDownload += state->fPreferredDownload;
}

/** Update pindexLastCommonBlock and add not-in-flight missing successors to vBlocks, until it has
 *  at most count entries. */
void FindNextBlocksToDownload(NodeId nodeid, unsigned int count, std::vector<const CBlockIndex*>& vBlocks, NodeId& nodeStaller) {
    if (count == 0)
        return;

    vBlocks.reserve(vBlocks.size() + count);
    CNodeState *state = State(nodeid);
    assert(state != NULL);

    // Make sure pindexBestKnownBlock is up to date, we'll need it.
    ProcessBlockAvailability(nodeid);

    if (state->pindexBestKnownBlock == NULL || state->pindexBestKnownBlock->nChainWork < pnetMan->getChainActive()->chainActive.Tip()->nChainWork) {
        // This peer has nothing interesting.
        return;
    }

    if (state->pindexLastCommonBlock == NULL) {
        // Bootstrap quickly by guessing a parent of our best tip is the forking point.
        // Guessing wrong in either direction is not a problem.
        state->pindexLastCommonBlock = pnetMan->getChainActive()->chainActive[std::min(state->pindexBestKnownBlock->nHeight, pnetMan->getChainActive()->chainActive.Height())];
    }

    // If the peer reorganized, our previous pindexLastCommonBlock may not be an ancestor
    // of its current tip anymore. Go back enough to fix that.
    state->pindexLastCommonBlock = LastCommonAncestor(state->pindexLastCommonBlock, state->pindexBestKnownBlock);
    if (state->pindexLastCommonBlock == state->pindexBestKnownBlock)
        return;

    std::vector<const CBlockIndex*> vToFetch;
    const CBlockIndex *pindexWalk = state->pindexLastCommonBlock;
    // Never fetch further than the best block we know the peer has, or more than BLOCK_DOWNLOAD_WINDOW + 1 beyond the last
    // linked block we have in common with this peer. The +1 is so we can detect stalling, namely if we would be able to
    // download that next block if the window were 1 larger.
    int nWindowEnd = state->pindexLastCommonBlock->nHeight + BLOCK_DOWNLOAD_WINDOW;
    int nMaxHeight = std::min<int>(state->pindexBestKnownBlock->nHeight, nWindowEnd + 1);
    NodeId waitingfor = -1;
    while (pindexWalk->nHeight < nMaxHeight) {
        // Read up to 128 (or more, if more blocks than that are needed) successors of pindexWalk (towards
        // pindexBestKnownBlock) into vToFetch. We fetch 128, because CBlockIndex::GetAncestor may be as expensive
        // as iterating over ~100 CBlockIndex* entries anyway.
        int nToFetch = std::min(nMaxHeight - pindexWalk->nHeight, std::max<int>(count - vBlocks.size(), 128));
        vToFetch.resize(nToFetch);
        pindexWalk = state->pindexBestKnownBlock->GetAncestor(pindexWalk->nHeight + nToFetch);
        vToFetch[nToFetch - 1] = pindexWalk;
        for (unsigned int i = nToFetch - 1; i > 0; i--) {
            vToFetch[i - 1] = vToFetch[i]->pprev;
        }

        // Iterate over those blocks in vToFetch (in forward direction), adding the ones that
        // are not yet downloaded and not in flight to vBlocks. In the mean time, update
        // pindexLastCommonBlock as long as all ancestors are already downloaded, or if it's
        // already part of our chain (and therefore don't need it even if pruned).
        for(const CBlockIndex* pindex : vToFetch) {
            if (!pindex->IsValid(BLOCK_VALID_TREE)) {
                // We consider the chain that this peer is on invalid.
                return;
            }
            if (pindex->nStatus & BLOCK_HAVE_DATA || pnetMan->getChainActive()->chainActive.Contains(pindex))
            {
                if (pindex->nChainTx)
                {
                    state->pindexLastCommonBlock = pindex;
                }
            }
            else if (mapBlocksInFlight.count(pindex->GetBlockHash()) == 0)
            {
                // The block is not already downloaded, and not yet in flight.
                if (pindex->nHeight > nWindowEnd)
                {
                    // We reached the end of the window.
                    if (vBlocks.size() == 0 && waitingfor != nodeid)
                    {
                        // We aren't able to fetch anything, but we would be if the download window was one larger.
                        nodeStaller = waitingfor;
                    }
                    return;
                }
                vBlocks.push_back(pindex);
                if (vBlocks.size() == count)
                {
                    return;
                }
            }
            else if (waitingfor == -1)
            {
                // This is the first already-in-flight block.
                waitingfor = mapBlocksInFlight[pindex->GetBlockHash()].first;
            }
        }
    }
}

// Requires cs_main
bool PeerHasHeader(CNodeState *state, CBlockIndex *pindex)
{
    if (state->pindexBestKnownBlock && pindex == state->pindexBestKnownBlock->GetAncestor(pindex->nHeight))
        return true;
    if (state->pindexBestHeaderSent && pindex == state->pindexBestHeaderSent->GetAncestor(pindex->nHeight))
        return true;
    return false;
}

/** Update tracking information about which blocks a peer is assumed to have. */
void UpdateBlockAvailability(NodeId nodeid, const uint256 &hash) {
    CNodeState *state = State(nodeid);
    assert(state != NULL);

    ProcessBlockAvailability(nodeid);

    BlockMap::iterator it = pnetMan->getChainActive()->mapBlockIndex.find(hash);
    if (it != pnetMan->getChainActive()->mapBlockIndex.end() && it->second->nChainWork > 0) {
        // An actually better block was announced.
        if (state->pindexBestKnownBlock == NULL || it->second->nChainWork >= state->pindexBestKnownBlock->nChainWork)
            state->pindexBestKnownBlock = it->second;
    } else {
        // An unknown block was announced; just assume that the latest one is the best one.
        state->hashLastUnknownBlock = hash;
    }
}

static bool SendRejectsAndCheckIfBanned(CNode *pnode, CConnman &connman) {
    AssertLockHeld(cs_main);
    CNodeState &state = *State(pnode->GetId());

    for (const CBlockReject &reject : state.rejects)
    {
        connman.PushMessage(pnode,CNetMsgMaker(MIN_PROTO_VERSION).Make(NetMsgType::REJECT,
                      std::string(NetMsgType::BLOCK),
                      reject.chRejectCode,
                      reject.strRejectReason,
                      reject.hashBlock));
    }
    state.rejects.clear();

    if (state.fShouldBan)
    {
        state.fShouldBan = false;
        if (pnode->fWhitelisted)
        {
            LogPrintf("Warning: not punishing whitelisted peer %s!\n",pnode->addr.ToString());
        }
        else if (pnode->fAddnode)
        {
            LogPrintf("Warning: not punishing addnoded peer %s!\n", pnode->addr.ToString());
        }
        else
        {
            pnode->fDisconnect = true;
            if (pnode->addr.IsLocal())
            {
                LogPrintf("Warning: not banning local peer %s!\n", pnode->addr.ToString());
            }
            else
            {
                connman.Ban(pnode->addr, BanReasonNodeMisbehaving);
            }
        }
        return true;
    }
    return false;
}

// Requires cs_main
bool PeerHasHeader(CNodeState *state, const CBlockIndex *pindex)
{
    if (state->pindexBestKnownBlock && pindex == state->pindexBestKnownBlock->GetAncestor(pindex->nHeight))
    {
        return true;
    }
    if (state->pindexBestHeaderSent && pindex == state->pindexBestHeaderSent->GetAncestor(pindex->nHeight))
    {
        return true;
    }
    return false;
}

//////////////////////////////////////////////////////////////////////////////
//
// blockchain -> download logic notification
//

PeerLogicValidation::PeerLogicValidation(CConnman *connmanIn)
    : connman(connmanIn) {
    // Initialize global variables that cannot be constructed at startup.
    recentRejects.reset(new CRollingBloomFilter(120000, 0.000001));
}

void PeerLogicValidation::BlockConnected(
    const std::shared_ptr<const CBlock> &pblock, const CBlockIndex *pindex,
    const std::vector<CTransactionRef> &vtxConflicted) {
    LOCK(cs_main);

    std::vector<uint256> vOrphanErase;

    for (const CTransactionRef &ptx : pblock->vtx)
    {
        const CTransaction tx = *ptx;

        // Which orphan pool entries must we evict?
        for (size_t j = 0; j < tx.vin.size(); j++)
        {
            auto itByPrev = mapOrphanTransactionsByPrev.find(tx.vin[j].prevout.hash);
            if (itByPrev == mapOrphanTransactionsByPrev.end())
            {
                continue;
            }
            for (auto mi = itByPrev->second.begin(); mi != itByPrev->second.end(); ++mi)
            {
                const uint256 &orphanHash = *mi;
                vOrphanErase.push_back(orphanHash);
            }
        }
    }

    // Erase orphan transactions include or precluded by this block
    if (vOrphanErase.size())
    {
        for (uint256 &orphanId : vOrphanErase)
        {
            EraseOrphanTx(orphanId);
        }
    }
}

void PeerLogicValidation::NewPoWValidBlock(const CBlockIndex *pindex, const std::shared_ptr<const CBlock> &pblock)
{
    const CNetMsgMaker msgMaker(PROTOCOL_VERSION);
    LOCK(cs_main);
    static int nHighestFastAnnounce = 0;
    if (pindex->nHeight <= nHighestFastAnnounce)
    {
        return;
    }
    nHighestFastAnnounce = pindex->nHeight;
    uint256 hashBlock(pblock->GetHash());
    {
        LOCK(cs_most_recent_block);
        most_recent_block_hash = hashBlock;
        most_recent_block = pblock;
    }
    connman->ForEachNode([this, pindex, &msgMaker, &hashBlock](CNode *pnode)
    {
        // TODO: Avoid the repeated-serialization here
        if (pnode->fDisconnect)
        {
            return;
        }
        ProcessBlockAvailability(pnode->GetId());
        CNodeState &state = *State(pnode->GetId());
        // If the peer has, or we announced to them the previous block already,
        // but we don't think they have this one, go ahead and announce it.
        if (!PeerHasHeader(&state, pindex) && PeerHasHeader(&state, pindex->pprev))
        {
            LogPrint("net", "%s sending header-and-ids %s to peer=%d\n",
                     "PeerLogicValidation::NewPoWValidBlock",
                     hashBlock.ToString(), pnode->id);
            std::vector<CBlock> vHeaders;
            vHeaders.push_back(pindex->GetBlockHeader());
            connman->PushMessage(pnode, msgMaker.Make(NetMsgType::HEADERS, vHeaders));
            state.pindexBestHeaderSent = pindex;
        }
    });
}

void PeerLogicValidation::UpdatedBlockTip(const CBlockIndex *pindexNew,
                                          const CBlockIndex *pindexFork,
                                          bool fInitialDownload) {
    const int nNewHeight = pindexNew->nHeight;
    connman->SetBestHeight(nNewHeight);

    if (!fInitialDownload) {
        // Find the hashes of all blocks that weren't previously in the best
        // chain.
        std::vector<uint256> vHashes;
        const CBlockIndex *pindexToAnnounce = pindexNew;
        while (pindexToAnnounce != pindexFork) {
            vHashes.push_back(pindexToAnnounce->GetBlockHash());
            pindexToAnnounce = pindexToAnnounce->pprev;
            if (vHashes.size() == MAX_BLOCKS_TO_ANNOUNCE) {
                // Limit announcements in case of a huge reorganization. Rely on
                // the peer's synchronization mechanism in that case.
                break;
            }
        }
        // Relay inventory, but don't relay old inventory during initial block
        // download.
        connman->ForEachNode([nNewHeight, &vHashes](CNode *pnode) {
            if (nNewHeight > (pnode->nStartingHeight != -1
                                  ? pnode->nStartingHeight - 2000
                                  : 0)) {
                for (const uint256 &hash : boost::adaptors::reverse(vHashes)) {
                    pnode->PushBlockHash(hash);
                }
            }
        });
        connman->WakeMessageHandler();
    }

    nTimeBestReceived = GetTime();
}

void PeerLogicValidation::BlockChecked(const CBlock &block, const CValidationState &state)
{
    LOCK(cs_main);
    const uint256 hash(block.GetHash());
    std::map<uint256, std::pair<NodeId, bool>>::iterator it = mapBlockSource.find(hash);
    int nDoS = 0;
    if (state.IsInvalid(nDoS))
    {
        // Don't send reject message with code 0 or an internal reject code.
        if (it != mapBlockSource.end() && State(it->second.first) && state.GetRejectCode() > 0 && state.GetRejectCode() < REJECT_INTERNAL)
        {
            CBlockReject reject = {uint8_t(state.GetRejectCode()), state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH), hash};
            State(it->second.first)->rejects.push_back(reject);
            if (nDoS > 0 && it->second.second) {
                Misbehaving(it->second.first, nDoS, state.GetRejectReason());
            }
        }
    }
    if (it != mapBlockSource.end())
    {
        mapBlockSource.erase(it);
    }
}

bool AlreadyHave(const CInv& inv) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    switch (inv.type)
    {
    case MSG_TX:
        {
            assert(recentRejects);
            if (pnetMan->getChainActive()->chainActive.Tip()->GetBlockHash() != hashRecentRejectsChainTip)
            {
                // If the chain tip has changed previously rejected transactions
                // might be now valid, e.g. due to a nLockTime'd tx becoming valid,
                // or a double-spend. Reset the rejects filter and give those
                // txs a second chance.
                hashRecentRejectsChainTip = pnetMan->getChainActive()->chainActive.Tip()->GetBlockHash();
                recentRejects->reset();
            }

            return recentRejects->contains(inv.hash) ||
                   mempool.exists(inv.hash) ||
                   mapOrphanTransactions.count(inv.hash) ||
                   pnetMan->getChainActive()->pcoinsTip->HaveCoinInCache(COutPoint(inv.hash, 0)) || // Best effort: only try output 0 and 1
                   pnetMan->getChainActive()->pcoinsTip->HaveCoinInCache(COutPoint(inv.hash, 1));
        }
    case MSG_BLOCK:
        return pnetMan->getChainActive()->mapBlockIndex.count(inv.hash);
    case MSG_STX:
        return g_stxmempool->exists(inv.hash);

    }
    // Don't know what it is, just say we already got one
    return true;
}

void static ProcessGetData(CNode* pfrom, CConnman &connman, const Consensus::Params& consensusParams, const std::atomic<bool> &interruptMsgProc)
{
    const CNetMsgMaker msgMaker(pfrom->GetSendVersion());
    std::deque<CInv>::iterator it = pfrom->vRecvGetData.begin();

    std::vector<CInv> vNotFound;
    LOCK(cs_main);

     while (it != pfrom->vRecvGetData.end())
     {
         // Don't bother if send buffer is too full to respond anyway.
         if (pfrom->fPauseSend)
         {
             break;
         }

         const CInv &inv = *it;
         {
             if (interruptMsgProc)
             {
                 return;
             }

             it++;

             if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
             {
                 bool send = false;
                 BlockMap::iterator mi = pnetMan->getChainActive()->mapBlockIndex.find(inv.hash);
                 if (mi != pnetMan->getChainActive()->mapBlockIndex.end())
                 {

/*
                     if (mi->second->nChainTx &&
                         !mi->second->IsValid(BLOCK_VALID_SCRIPTS) &&
                         mi->second->IsValid(BLOCK_VALID_TREE))
                     {
                         // If we have the block and all of its parents, but have
                         // not yet validated it, we might be in the middle of
                         // connecting it (ie in the unlock of cs_main before
                         // ActivateBestChain but after AcceptBlock). In this
                         // case, we need to run ActivateBestChain prior to
                         // checking the relay conditions below.
                         std::shared_ptr<const CBlock> a_recent_block;
                         {
                             LOCK(cs_most_recent_block);
                             a_recent_block = most_recent_block;
                         }
                         CValidationState dummy;
                         ActivateBestChain(dummy, pnetMan->getActivePaymentNetwork(), RECEIVED, a_recent_block);
                     }

*/
                     if (pnetMan->getChainActive()->chainActive.Contains(mi->second))
                     {
                         send = true;
                     }
                     else
                     {
                         static const int nOneMonth = 30 * 24 * 60 * 60;
                         // To prevent fingerprinting attacks, only send blocks
                         // outside of the active chain if they are valid, and no
                         // more than a month older (both in time, and in best
                         // equivalent proof of work) than the best header chain
                         // we know about.
                         send = mi->second->IsValid(BLOCK_VALID_SCRIPTS) &&
                                (pnetMan->getChainActive()->pindexBestHeader != nullptr) &&
                                (pnetMan->getChainActive()->pindexBestHeader->GetBlockTime() -
                                     mi->second->GetBlockTime() <
                                 nOneMonth) &&
                                (GetBlockProofEquivalentTime(
                                     *pnetMan->getChainActive()->pindexBestHeader, *mi->second,
                                     *pnetMan->getChainActive()->pindexBestHeader,
                                     consensusParams) < nOneMonth);
                         if (!send) {
                             LogPrintf("%s: ignoring request from peer=%i for "
                                       "old block that isn't in the main "
                                       "chain\n",
                                       __func__, pfrom->GetId());
                         }
                     }
                 }

                 // Disconnect node in case we have reached the outbound limit
                 // for serving historical blocks never disconnect whitelisted
                 // nodes.
                 // assume > 1 week = historical
                 static const int nOneWeek = 7 * 24 * 60 * 60;
                 if (send && connman.OutboundTargetReached(true) &&
                     (((pnetMan->getChainActive()->pindexBestHeader != nullptr) &&
                       (pnetMan->getChainActive()->pindexBestHeader->GetBlockTime() - mi->second->GetBlockTime() > nOneWeek)) ||
                      inv.type == MSG_FILTERED_BLOCK) && !pfrom->fWhitelisted)
                 {
                     LogPrintf("historical block serving limit "
                                          "reached, disconnect peer=%d\n",
                              pfrom->GetId());

                     // disconnect node
                     pfrom->fDisconnect = true;
                     send = false;
                 }
                 // Pruned nodes may have deleted the block, so check whether
                 // it's available before trying to send.
                 if (send && (mi->second->nStatus & BLOCK_HAVE_DATA))
                 {
                     // Send block from disk
                     CBlock block;
                     if (!ReadBlockFromDisk(block, (*mi).second, consensusParams))
                     {
                         LogPrintf("cannot load block from disk");
                         assert(false);
                     }
                     if (inv.type == MSG_BLOCK)
                     {
                         connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::BLOCK, block));
                     }
                     else if (inv.type == MSG_FILTERED_BLOCK)
                     {
                         bool sendMerkleBlock = false;
                         CMerkleBlock merkleBlock;
                         {
                             LOCK(pfrom->cs_filter);
                             if (pfrom->pfilter)
                             {
                                 sendMerkleBlock = true;
                                 merkleBlock =
                                     CMerkleBlock(block, *pfrom->pfilter);
                             }
                         }
                         if (sendMerkleBlock)
                         {
                             connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::MERKLEBLOCK, merkleBlock));
                             // CMerkleBlock just contains hashes, so also push
                             // any transactions in the block the client did not
                             // see. This avoids hurting performance by
                             // pointlessly requiring a round-trip. Note that
                             // there is currently no way for a node to request
                             // any single transactions we didn't send here -
                             // they must either disconnect and retry or request
                             // the full block. Thus, the protocol spec specified
                             // allows for us to provide duplicate txn here,
                             // however we MUST always provide at least what the
                             // remote peer needs.
                             typedef std::pair<unsigned int, uint256> PairType;
                             for (PairType &pair : merkleBlock.vMatchedTxn)
                             {
                                 connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::TX, block.vtx[pair.first]));
                             }
                         }
                         // else
                         // no response
                     }

                     // Trigger the peer node to send a getblocks request for the
                     // next batch of inventory.
                     if (inv.hash == pfrom->hashContinue)
                     {
                         // Bypass PushInventory, this must send even if
                         // redundant, and we want it right after the last block
                         // so they don't wait for other stuff first.
                         std::vector<CInv> vInv;
                         vInv.push_back(CInv(MSG_BLOCK, pnetMan->getChainActive()->chainActive.Tip()->GetBlockHash()));
                         connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::INV, vInv));
                         pfrom->hashContinue.SetNull();
                     }
                 }
             }
             else if (inv.type == MSG_TX)
             {
                 // Send stream from relay memory
                 bool push = false;
                 auto mi = mapRelay.find(inv.hash);
                 int nSendFlags = 0;
                 if (mi != mapRelay.end())
                 {
                     connman.PushMessage(pfrom, msgMaker.Make(nSendFlags, NetMsgType::TX, mi->second));
                     push = true;
                 }
                 if (!push)
                 {
                     vNotFound.push_back(inv);
                 }
             }
             else if (inv.type == MSG_STX)
             {
                 // Send stream from relay memory
                 bool push = false;
                 int nSendFlags = 0;
                 CServiceTransaction stx;
                 if(g_stxmempool->lookup(inv.hash, stx))
                 {
                     connman.PushMessage(pfrom, msgMaker.Make(nSendFlags, NetMsgType::STX, stx));
                     pfrom->filterServiceDataKnown.insert(inv.hash);
                     push = true;
                 }
                 if (!push)
                 {
                     vNotFound.push_back(inv);
                 }
             }
             // Track requests for our stuff.
             GetMainSignals().Inventory(inv.hash);
             if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
             {
                 break;
             }
         }
     }
     pfrom->vRecvGetData.erase(pfrom->vRecvGetData.begin(), it);

     if (!vNotFound.empty())
     {
         // Let the peer know that we didn't find what it asked for, so it
         // doesn't have to wait around forever. Currently only SPV clients
         // actually care about this message: it's needed when they are
         // recursively walking the dependencies of relevant unconfirmed
         // transactions. SPV clients want to do that because they want to know
         // about (and store and rebroadcast and risk analyze) the dependencies
         // of transactions relevant to them, without having to download the
         // entire memory pool.
         connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::NOTFOUND, vNotFound));
     }
}

void RegisterNodeSignals(CNodeSignals &nodeSignals) {
    nodeSignals.ProcessMessages.connect(&ProcessMessages);
    nodeSignals.SendMessages.connect(&SendMessages);
    nodeSignals.InitializeNode.connect(&InitializeNode);
    nodeSignals.FinalizeNode.connect(&FinalizeNode);
}

void UnregisterNodeSignals(CNodeSignals &nodeSignals) {
    nodeSignals.ProcessMessages.disconnect(&ProcessMessages);
    nodeSignals.SendMessages.disconnect(&SendMessages);
    nodeSignals.InitializeNode.disconnect(&InitializeNode);
    nodeSignals.FinalizeNode.disconnect(&FinalizeNode);
}


bool static ProcessMessage(CNode* pfrom, std::string strCommand, CDataStream& vRecv, int64_t nTimeReceived, CConnman &connman, const std::atomic<bool> &interruptMsgProc)
{
    const CNetworkTemplate& chainparams = pnetMan->getActivePaymentNetwork();
    RandAddSeedPerfmon();
    LogPrint("net","received: %s (%u bytes) peer=%d\n", SanitizeString(strCommand), vRecv.size(), pfrom->id);
    if (gArgs.IsArgSet("-dropmessagestest") && GetRand(atoi(gArgs.GetArg("-dropmessagestest", "0"))) == 0)
    {
        LogPrint("net", "dropmessagestest DROPPING RECV MESSAGE \n");
        return true;
    }


    if (!(pfrom->GetLocalServices() & NODE_BLOOM) &&
              (strCommand == NetMsgType::FILTERLOAD ||
               strCommand == NetMsgType::FILTERADD ||
               strCommand == NetMsgType::FILTERCLEAR))
    {
        if (pfrom->nVersion >= NO_BLOOM_VERSION)
        {
            Misbehaving(pfrom->GetId(), 100, "no-bloom-version");
            return false;
        }
        else if (gArgs.GetBoolArg("-enforcenodebloom", false))
        {
            pfrom->fDisconnect = true;
            return false;
        }
    }


    if (strCommand == NetMsgType::VERSION)
    {
        // Each connection can only send one version message
        if (pfrom->nVersion != 0)
        {
            connman.PushMessage(pfrom, CNetMsgMaker(MIN_PROTO_VERSION).Make(NetMsgType::REJECT, strCommand, REJECT_DUPLICATE, std::string("Duplicate version message")));
            LOCK(cs_main);
            Misbehaving(pfrom, 1, "multiple-version");
            return false;
        }

        int64_t nTime;
        CAddress addrMe;
        CAddress addrFrom;
        uint64_t nNonce = 1;
        uint64_t nServiceInt;
        ServiceFlags nServices;
        int nVersion;
        int nSendVersion;
        std::string strSubVer;
        std::string cleanSubVer;
        int nStartingHeight = -1;
        bool fRelay = true;

        vRecv >> nVersion >> nServiceInt >> nTime >> addrMe;
        nSendVersion = std::min(nVersion, PROTOCOL_VERSION);
        nServices = ServiceFlags(nServiceInt);
        if (!pfrom->fInbound) {
            connman.SetServices(pfrom->addr, nServices);
        }
        if (pfrom->nServicesExpected & ~nServices) {
            LogPrintf("peer=%d does not offer the expected services (%08x offered, %08x expected); disconnecting\n",
                     pfrom->id, nServices, pfrom->nServicesExpected);
            connman.PushMessage(
                pfrom,
                CNetMsgMaker(MIN_PROTO_VERSION).Make(NetMsgType::REJECT, strCommand, REJECT_NONSTANDARD,
                          strprintf("Expected to offer services %08x", pfrom->nServicesExpected)));
            pfrom->fDisconnect = true;
            return false;
        }

        if (nVersion < MIN_PROTO_VERSION) {
            // disconnect from peers older than this proto version
            LogPrintf("peer=%d using obsolete version %i; disconnecting\n",
                      pfrom->id, nVersion);
            connman.PushMessage(pfrom,
                CNetMsgMaker(MIN_PROTO_VERSION).Make(NetMsgType::REJECT, strCommand, REJECT_OBSOLETE,
                          strprintf("Version must be %d or greater", MIN_PROTO_VERSION)));
            pfrom->fDisconnect = true;
            return false;
        }

        if (!vRecv.empty()) {
            vRecv >> addrFrom >> nNonce;
        }
        if (!vRecv.empty()) {
            vRecv >> LIMITED_STRING(strSubVer, MAX_SUBVERSION_LENGTH);
            cleanSubVer = SanitizeString(strSubVer);
        }
        if (!vRecv.empty()) {
            vRecv >> nStartingHeight;
        }
        if (!vRecv.empty()) {
            vRecv >> fRelay;
        }
        // Disconnect if we connected to ourself
        if (pfrom->fInbound && !connman.CheckIncomingNonce(nNonce)) {
            LogPrintf("connected to self at %s, disconnecting\n",
                      pfrom->addr.ToString());
            pfrom->fDisconnect = true;
            return true;
        }

        if (pfrom->fInbound && addrMe.IsRoutable()) {
            SeenLocal(addrMe);
        }

        // Be shy and don't send version until we hear
        if (pfrom->fInbound) {
            PushNodeVersion(pfrom, connman, GetAdjustedTime());
        }

        connman.PushMessage(pfrom, CNetMsgMaker(MIN_PROTO_VERSION).Make(NetMsgType::VERACK));

        pfrom->nServices = nServices;
        pfrom->SetAddrLocal(addrMe);
        {
            LOCK(pfrom->cs_SubVer);
            pfrom->strSubVer = strSubVer;
            pfrom->cleanSubVer = cleanSubVer;
        }
        pfrom->nStartingHeight = nStartingHeight;
        pfrom->fClient = !(nServices & NODE_NETWORK);
        {
            LOCK(pfrom->cs_filter);
            // set to true after we get the first filter* message
            pfrom->fRelayTxes = fRelay;
        }

        // Change version
        pfrom->SetSendVersion(nSendVersion);
        pfrom->nVersion = nVersion;

        // Potentially mark this peer as a preferred download peer.
        {
            LOCK(cs_main);
            UpdatePreferredDownload(pfrom, State(pfrom->GetId()));
        }

        if (!pfrom->fInbound) {
            // Advertise our address
            if (fListen && !pnetMan->getChainActive()->IsInitialBlockDownload()) {
                CAddress addr =
                    GetLocalAddress(&pfrom->addr, pfrom->GetLocalServices());
                FastRandomContext insecure_rand;
                if (addr.IsRoutable()) {
                    LogPrint("net",
                             "ProcessMessages: advertising address %s\n",
                             addr.ToString());
                    pfrom->PushAddress(addr, insecure_rand);
                } else if (IsPeerAddrLocalGood(pfrom)) {
                    addr.SetIP(addrMe);
                    LogPrintf("ProcessMessages: advertising address %s\n", addr.ToString());
                    pfrom->PushAddress(addr, insecure_rand);
                }
            }

            // Get recent addresses
            if (pfrom->fOneShot || connman.GetAddressCount() < 1000)
            {
                connman.PushMessage(pfrom, CNetMsgMaker(nSendVersion).Make(NetMsgType::GETADDR));
                pfrom->fGetAddr = true;
            }
            connman.MarkAddressGood(pfrom->addr);
        }

        std::string remoteAddr;
        if (fLogIPs) {
            remoteAddr = ", peeraddr=" + pfrom->addr.ToString();
        }

        LogPrintf("receive version message: [%s] %s: version %d, blocks=%d, "
                  "us=%s, peer=%d%s\n",
                  pfrom->addr.ToString().c_str(), cleanSubVer, pfrom->nVersion,
                  pfrom->nStartingHeight, addrMe.ToString(), pfrom->id,
                  remoteAddr);

        int64_t nTimeOffset = nTime - GetTime();
        pfrom->nTimeOffset = nTimeOffset;
        AddTimeData(pfrom->addr, nTimeOffset);

        // Feeler connections exist only to verify if address is online.
        if (pfrom->fFeeler) {
            assert(pfrom->fInbound == false);
            pfrom->fDisconnect = true;
        }
        return true;
    }


    else if (pfrom->nVersion == 0)
    {
        // Must have a version message before anything else
        LOCK(cs_main);
        Misbehaving(pfrom, 1, "missing-version");
        return false;
    }

    const CNetMsgMaker msgMaker(pfrom->GetSendVersion());


    if (strCommand == NetMsgType::VERACK)
    {
        pfrom->SetRecvVersion(std::min(pfrom->nVersion.load(), PROTOCOL_VERSION));

        if (!pfrom->fInbound)
        {
            // Mark this node as currently connected, so we update its timestamp
            // later.
            LOCK(cs_main);
            State(pfrom->GetId())->fCurrentlyConnected = true;
        }

        if (pfrom->nVersion >= SENDHEADERS_VERSION)
        {
            // Tell our peer we prefer to receive headers rather than inv's
            // We send this to non-NODE NETWORK peers as well, because even
            // non-NODE NETWORK peers can announce blocks (such as pruning
            // nodes)
            connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::SENDHEADERS));
        }
        pfrom->fSuccessfullyConnected = true;
    }

/*
    else if (!pfrom->fSuccessfullyConnected) {
        // Must have a verack message before anything else
        LOCK(cs_main);
        Misbehaving(pfrom, 1, "missing-verack");
        return false;
    }
*/

    else if (strCommand == NetMsgType::ADDR)
    {
        std::vector<CAddress> vAddr;
        vRecv >> vAddr;

        // Don't want addr from older versions unless seeding
        if (connman.GetAddressCount() > 1000)
        {
            return true;
        }
        if (vAddr.size() > 1000)
        {
            LOCK(cs_main);
            Misbehaving(pfrom, 20, "oversized-addr");
            return error("message addr size() = %u", vAddr.size());
        }

        // Store the new addresses
        std::vector<CAddress> vAddrOk;
        int64_t nNow = GetAdjustedTime();
        int64_t nSince = nNow - 10 * 60;
        for (CAddress &addr : vAddr) {

            if ((addr.nServices & REQUIRED_SERVICES) != REQUIRED_SERVICES) {
                continue;
            }

            if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60) {
                addr.nTime = nNow - 5 * 24 * 60 * 60;
            }
            pfrom->AddAddressKnown(addr);
            bool fReachable = IsReachable(addr);
            if (addr.nTime > nSince && !pfrom->fGetAddr && vAddr.size() <= 10 &&
                addr.IsRoutable()) {
                // Relay to a limited number of other nodes
                RelayAddress(addr, fReachable, connman);
            }
            // Do not store addresses outside our network
            if (fReachable) {
                vAddrOk.push_back(addr);
            }
        }
        connman.AddNewAddresses(vAddrOk, pfrom->addr, 2 * 60 * 60);
        if (vAddr.size() < 1000) {
            pfrom->fGetAddr = false;
        }
        if (pfrom->fOneShot) {
            pfrom->fDisconnect = true;
        }
    }

    else if (strCommand == NetMsgType::SENDHEADERS)
    {
        LOCK(cs_main);
        State(pfrom->GetId())->fPreferHeaders = true;
    }


    else if (strCommand == NetMsgType::INV)
    {
        std::vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            LOCK(cs_main);
            Misbehaving(pfrom, 20, "oversized-inv");
            return error("message inv size() = %u", vInv.size());
        }

        bool fBlocksOnly = !fRelayTxes;

        // Allow whitelisted peers to send data other than blocks in blocks only
        // mode if whitelistrelay is true
        if (pfrom->fWhitelisted && gArgs.GetBoolArg("-whitelistrelay", DEFAULT_WHITELISTRELAY)) 
        {
            fBlocksOnly = false;
        }

        LOCK(cs_main);

        uint32_t nFetchFlags = GetFetchFlags(pfrom, pnetMan->getChainActive()->chainActive.Tip(), chainparams.GetConsensus());

        std::vector<CInv> vToFetch;

        for (size_t nInv = 0; nInv < vInv.size(); nInv++)
        {
            CInv &inv = vInv[nInv];

            if (interruptMsgProc)
            {
                return true;
            }

            bool fAlreadyHave = AlreadyHave(inv);
            LogPrintf("got inv: %s  %s peer=%d\n", inv.ToString(),
                     fAlreadyHave ? "have" : "new", pfrom->id);

            if (inv.type == MSG_TX || inv.type == MSG_STX)
            {
                inv.type |= nFetchFlags;
            }
            if (inv.type == MSG_BLOCK)
            {
                UpdateBlockAvailability(pfrom->GetId(), inv.hash);
                if (!fAlreadyHave && !fImporting && !fReindex &&
                    !mapBlocksInFlight.count(inv.hash)) {
                    // We used to request the full block here, but since
                    // headers-announcements are now the primary method of
                    // announcement on the network, and since, in the case that
                    // a node fell back to inv we probably have a reorg which we
                    // should get the headers for first, we now only provide a
                    // getheaders response here. When we receive the headers, we
                    // will then ask for the blocks we need.
                    connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::GETHEADERS, pnetMan->getChainActive()->chainActive.GetLocator(pnetMan->getChainActive()->pindexBestHeader), inv.hash));
                    CNodeState *nodestate = State(pfrom->GetId());
                    if (CanDirectFetch(chainparams.GetConsensus()) && nodestate->nBlocksInFlight < MAX_BLOCKS_IN_TRANSIT_PER_PEER) 
                    {
                        vToFetch.push_back(inv);
                        // Mark block as in flight already, even though the actual "getdata" message only goes out
                        // later (within the same cs_main lock, though).
                        MarkBlockAsInFlight(pfrom->GetId(), inv.hash, chainparams.GetConsensus());
                    }
                    LogPrintf("getheaders (%d) %s to peer=%d\n", pnetMan->getChainActive()->pindexBestHeader->nHeight, inv.hash.ToString(), pfrom->id);
                }
            }
            else
            {
                pfrom->AddInventoryKnown(inv);
                if (fBlocksOnly)
                {
                    LogPrintf("transaction (%s) inv sent in violation of protocol peer=%d\n", inv.hash.ToString(), pfrom->id);
                }
                else if (!fAlreadyHave && !fImporting && !fReindex && !pnetMan->getChainActive()->IsInitialBlockDownload())
                {
                    pfrom->AskFor(inv);
                }
            }

            // Track requests for our stuff
            GetMainSignals().Inventory(inv.hash);
        }

        if (!vToFetch.empty())
        {
            connman.PushMessage(pfrom,msgMaker.Make(NetMsgType::GETDATA, vToFetch));
        }
    }


    else if (strCommand == NetMsgType::GETDATA)
    {
        std::vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            LOCK(cs_main);
            Misbehaving(pfrom, 20, "too-many-inv");
            return error("message getdata size() = %u", vInv.size());
        }

        LogPrintf("received getdata (%u invsz) peer=%d\n", vInv.size(), pfrom->id);

        if (vInv.size() > 0)
        {
            LogPrintf("received getdata for: %s peer=%d\n", vInv[0].ToString(), pfrom->id);
        }

        pfrom->vRecvGetData.insert(pfrom->vRecvGetData.end(), vInv.begin(),vInv.end());
        ProcessGetData(pfrom, connman, chainparams.GetConsensus(), interruptMsgProc);
    }


    else if (strCommand == NetMsgType::GETBLOCKS)
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        // We might have announced the currently-being-connected tip using a
        // compact block, which resulted in the peer sending a getblocks
        // request, which we would otherwise respond to without the new block.
        // To avoid this situation we simply verify that we are on our best
        // known chain now. This is super overkill, but we handle it better
        // for getheaders requests, and there are no known nodes which support
        // compact blocks but still use getblocks to request blocks.
        {
            std::shared_ptr<const CBlock> a_recent_block;
            {
                LOCK(cs_most_recent_block);
                a_recent_block = most_recent_block;
            }
            CValidationState dummy;
            ActivateBestChain(dummy, pnetMan->getActivePaymentNetwork(), a_recent_block);
        }

        LOCK(cs_main);

        // Find the last block the caller has in the main chain
        const CBlockIndex *pindex = pnetMan->getChainActive()->FindForkInGlobalIndex(pnetMan->getChainActive()->chainActive, locator);

        // Send the rest of the chain
        if (pindex) {
            pindex = pnetMan->getChainActive()->chainActive.Next(pindex);
        }
        int nLimit = 500;
        LogPrintf("getblocks %d to %s limit %d from peer=%d\n",
                 (pindex ? pindex->nHeight : -1),
                 hashStop.IsNull() ? "end" : hashStop.ToString(), nLimit,
                 pfrom->id);
        for (; pindex; pindex = pnetMan->getChainActive()->chainActive.Next(pindex))
        {
            if (pindex->GetBlockHash() == hashStop) {
                LogPrintf("  getblocks stopping at %d %s\n",
                         pindex->nHeight, pindex->GetBlockHash().ToString());
                break;
            }
            pfrom->PushInventory(CInv(MSG_BLOCK, pindex->GetBlockHash()));
            if (--nLimit <= 0) {
                // When this block is requested, we'll send an inv that'll
                // trigger the peer to getblocks the next batch of inventory.
                LogPrint("net","  getblocks stopping at limit %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                pfrom->hashContinue = pindex->GetBlockHash();
                break;
            }
        }
    }

    else if (strCommand == NetMsgType::GETHEADERS)
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        LOCK(cs_main);

        if (pnetMan->getChainActive()->IsInitialBlockDownload() && !pfrom->fWhitelisted)
        {
            LogPrintf("Ignoring getheaders from peer=%d because node is in initial block download\n",pfrom->id);
            return true;
        }

        CNodeState *nodestate = State(pfrom->GetId());
        CBlockIndex *pindex = nullptr;
        if (locator.IsNull())
        {
            // If locator is null, return the hashStop block
            BlockMap::iterator mi = pnetMan->getChainActive()->mapBlockIndex.find(hashStop);
            if (mi == pnetMan->getChainActive()->mapBlockIndex.end())
            {
                return true;
            }
            pindex = (*mi).second;
        }
        else
        {
            // Find the last block the caller has in the main chain
            pindex = pnetMan->getChainActive()->FindForkInGlobalIndex(pnetMan->getChainActive()->chainActive, locator);
            if (pindex)
            {
                pindex = pnetMan->getChainActive()->chainActive.Next(pindex);
            }
        }

        // we must use CBlocks, as CBlockHeaders won't include the 0x00 nTx
        // count at the end
        std::vector<CBlock> vHeaders;
        int nLimit = MAX_HEADERS_RESULTS;
        LogPrintf("getheaders %d to %s from peer=%d\n",
                 (pindex ? pindex->nHeight : -1),
                 hashStop.IsNull() ? "end" : hashStop.ToString(), pfrom->id);
        for (; pindex; pindex = pnetMan->getChainActive()->chainActive.Next(pindex))
        {
            vHeaders.push_back(pindex->GetBlockHeader());
            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
            {
                break;
            }
        }
        // pindex can be nullptr either if we sent chainActive.Tip() OR
        // if our peer has chainActive.Tip() (and thus we are sending an empty
        // headers message). In both cases it's safe to update
        // pindexBestHeaderSent to be our tip.
        //
        // It is important that we simply reset the BestHeaderSent value here,
        // and not max(BestHeaderSent, newHeaderSent). We might have announced
        // the currently-being-connected tip using a compact block, which
        // resulted in the peer sending a headers request, which we respond to
        // without the new block. By resetting the BestHeaderSent, we ensure we
        // will re-announce the new block via headers (or compact blocks again)
        // in the SendMessages logic.
        nodestate->pindexBestHeaderSent = pindex ? pindex : pnetMan->getChainActive()->chainActive.Tip();
        connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::HEADERS, vHeaders));
    }

    else if (strCommand == NetMsgType::TX)
    {
        // Stop processing the transaction early if
        // We are in blocks only mode and peer is either not whitelisted or
        // whitelistrelay is off
        if (!fRelayTxes && (!pfrom->fWhitelisted || !gArgs.GetBoolArg("-whitelistrelay", DEFAULT_WHITELISTRELAY)))
        {
            LogPrintf("transaction sent in violation of protocol peer=%d\n", pfrom->id);
            return true;
        }

        std::deque<COutPoint> vWorkQueue;
        std::vector<uint256> vEraseQueue;
        CTransaction tx;
        vRecv >> tx;
        const CTransactionRef ptx = std::make_shared<CTransaction>(tx);

        CInv inv(MSG_TX, tx.GetId());
        pfrom->AddInventoryKnown(inv);

        LOCK(cs_main);

        bool fMissingInputs = false;
        CValidationState state;

        pfrom->setAskFor.erase(inv.hash);
        mapAlreadyAskedFor.erase(inv.hash);

        if (!AlreadyHave(inv) && AcceptToMemoryPool(mempool, state, ptx, true, &fMissingInputs))
        {
            mempool.check(pnetMan->getChainActive()->pcoinsTip.get());
            RelayTransaction(tx, connman);
            for (size_t i = 0; i < tx.vout.size(); i++)
            {
                vWorkQueue.emplace_back(inv.hash, i);
            }

            pfrom->nLastTXTime = GetTime();

            LogPrint("mempool", "AcceptToMemoryPool: peer=%d: accepted %s "
                                     "(poolsz %u txn, %u kB)\n",
                     pfrom->id, tx.GetId().ToString(), mempool.size(),
                     mempool.DynamicMemoryUsage() / 1000);

            // Recursively process any orphan transactions that depended on this
            // one
            std::set<NodeId> setMisbehaving;
            while (!vWorkQueue.empty())
            {
                auto itByPrev = mapOrphanTransactionsByPrev.find(vWorkQueue[0].hash);
                vWorkQueue.pop_front();
                if (itByPrev == mapOrphanTransactionsByPrev.end())
                {
                    continue;
                }
                for (auto mi = itByPrev->second.begin(); mi != itByPrev->second.end(); ++mi)
                {
                    const uint256& orphanHash = *mi;
                    const CTransaction orphanTx = mapOrphanTransactions[orphanHash].tx;
                    const CTransactionRef &porphanTx = std::make_shared<CTransaction>(orphanTx);
                    const uint256 &orphanId = orphanTx.GetId();
                    NodeId fromPeer = mapOrphanTransactions[orphanHash].fromPeer;

                    bool fMissingInputs2 = false;
                    // Use a dummy CValidationState so someone can't setup nodes
                    // to counter-DoS based on orphan resolution (that is,
                    // feeding people an invalid transaction based on LegitTxX
                    // in order to get anyone relaying LegitTxX banned)
                    CValidationState stateDummy;

                    if (setMisbehaving.count(fromPeer)) {
                        continue;
                    }
                    if (AcceptToMemoryPool(mempool, stateDummy, porphanTx, true, &fMissingInputs2))
                    {
                        LogPrintf("   accepted orphan tx %s\n", orphanId.ToString());
                        RelayTransaction(orphanTx, connman);
                        for (size_t i = 0; i < orphanTx.vout.size(); i++) {
                            vWorkQueue.emplace_back(orphanId, i);
                        }
                        vEraseQueue.push_back(orphanId);
                    } else if (!fMissingInputs2) {
                        int nDos = 0;
                        if (stateDummy.IsInvalid(nDos) && nDos > 0) {
                            // Punish peer that gave us an invalid orphan tx
                            Misbehaving(fromPeer, nDos, "invalid-orphan-tx");
                            setMisbehaving.insert(fromPeer);
                            LogPrintf("   invalid orphan tx %s\n", orphanId.ToString());
                        }
                        // Has inputs but not accepted to mempool
                        // Probably non-standard or insufficient fee/priority
                        LogPrintf("   removed orphan tx %s\n",orphanId.ToString());
                        vEraseQueue.push_back(orphanId);
                        if (!stateDummy.CorruptionPossible()) {
                            // Do not use rejection cache for witness
                            // transactions or witness-stripped transactions, as
                            // they can have been malleated. See
                            // https://github.com/bitcoin/bitcoin/issues/8279
                            // for details.
                            assert(recentRejects);
                            recentRejects->insert(orphanId);
                        }
                    }
                    mempool.check(pnetMan->getChainActive()->pcoinsTip.get());
                }
            }

            for (uint256 hash : vEraseQueue)
            {
                EraseOrphanTx(hash);
            }
        }
        else if (fMissingInputs)
        {
            // It may be the case that the orphans parents have all been
            // rejected.
            bool fRejectedParents = false;
            for (const CTxIn &txin : tx.vin)
            {
                if (recentRejects->contains(txin.prevout.hash))
                {
                    fRejectedParents = true;
                    break;
                }
            }
            if (!fRejectedParents)
            {
                uint32_t nFetchFlags = GetFetchFlags(pfrom, pnetMan->getChainActive()->chainActive.Tip(), chainparams.GetConsensus());
                for (const CTxIn &txin : tx.vin)
                {
                    CInv _inv(MSG_TX | nFetchFlags, txin.prevout.hash);
                    pfrom->AddInventoryKnown(_inv);
                    if (!AlreadyHave(_inv))
                    {
                        pfrom->AskFor(_inv);
                    }
                }
                AddOrphanTx(tx, pfrom->GetId());

                // DoS prevention: do not allow mapOrphanTransactions to grow
                // unbounded
                unsigned int nMaxOrphanTx = (unsigned int)std::max(
                    int64_t(0),
                    gArgs.GetArg("-maxorphantx",
                                 DEFAULT_MAX_ORPHAN_TRANSACTIONS));
                unsigned int nEvicted = LimitOrphanTxSize(nMaxOrphanTx);
                if (nEvicted > 0) {
                    LogPrintf("mapOrphan overflow, removed %u tx\n", nEvicted);
                }
            } else {
                LogPrintf("not keeping orphan with rejected parents %s\n", tx.GetId().ToString());
                // We will continue to reject this tx since it has rejected
                // parents so avoid re-requesting it from other peers.
                recentRejects->insert(tx.GetId());
            }
        }
        else
        {
            if (!state.CorruptionPossible()) {
                // Do not use rejection cache for witness transactions or
                // witness-stripped transactions, as they can have been
                // malleated. See https://github.com/bitcoin/bitcoin/issues/8279
                // for details.
                assert(recentRejects);
                recentRejects->insert(tx.GetId());
            }

            if (pfrom->fWhitelisted && gArgs.GetBoolArg("-whitelistforcerelay", DEFAULT_WHITELISTFORCERELAY))
            {
                // Always relay transactions received from whitelisted peers,
                // even if they were already in the mempool or rejected from it
                // due to policy, allowing the node to function as a gateway for
                // nodes hidden behind it.
                //
                // Never relay transactions that we would assign a non-zero DoS
                // score for, as we expect peers to do the same with us in that
                // case.
                int nDoS = 0;
                if (!state.IsInvalid(nDoS) || nDoS == 0)
                {
                    LogPrint("net", "Force relaying tx %s from whitelisted peer=%d\n", tx.GetId().ToString(), pfrom->id);
                    RelayTransaction(tx, connman);
                }
                else
                {
                    LogPrint("net", "Not relaying invalid transaction %s from "
                              "whitelisted peer=%d (%s)\n",
                              tx.GetId().ToString(), pfrom->id,
                              FormatStateMessage(state));
                }
            }
        }
        int nDoS = 0;
        if (state.IsInvalid(nDoS)) {
            LogPrintf("%s from peer=%d was not accepted: %s\n", tx.GetHash().ToString(), pfrom->id, FormatStateMessage(state));
            // Never send AcceptToMemoryPool's internal codes over P2P.
            if (state.GetRejectCode() > 0 &&
                state.GetRejectCode() < REJECT_INTERNAL)
            {
                connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::REJECT, strCommand,
                                  uint8_t(state.GetRejectCode()),
                                  state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH),inv.hash));
            }
            if (nDoS > 0)
            {
                Misbehaving(pfrom, nDoS, state.GetRejectReason());
            }
        }
    }

    else if (strCommand == NetMsgType::STX)
    {
        CServiceTransaction pstx;
        vRecv >> pstx;
        CTransaction tx;
        uint256 blockHashOfTx;
        if(pstx.paymentReferenceHash.IsNull())
        {
            return error("invalid service transaction with hash %s recieved", pstx.GetHash().GetHex().c_str());
        }
        g_stxmempool->add(pstx.GetHash(), pstx);
        if (GetTransaction(pstx.paymentReferenceHash, tx, pnetMan->getActivePaymentNetwork()->GetConsensus(), blockHashOfTx))
        {
            //if we can get the transaction we have already processed it so it is safe to call CheckTransactionANS here
            CValidationState state;
            if(CheckServiceTransaction(pstx, tx, state))
            {                
                ProcessServiceCommand(pstx, tx, state);
                RelayServiceTransaction(pstx, connman);
            }
            else
            {
                {
                    LOCK(cs_pendstx);
                    // we failed so add to a pending map for now
                    std::map<uint256, int64_t>::const_iterator it = pendingStx.find(pstx.GetHash());
                    if (it == pendingStx.end())
                    {
                        int64_t nNow = GetTimeMicros();
                        pendingStx.insert(std::make_pair(pstx.GetHash(), nNow));
                    }
                }
                int nDoS = 0;
                if (state.IsInvalid(nDoS))
                {
                    LogPrintf("%s from peer=%d was not accepted: %s\n", pstx.GetHash().ToString().c_str(), pfrom->id, FormatStateMessage(state));
                    // Never send AcceptToMemoryPool's internal codes over P2P.
                    if (state.GetRejectCode() > 0 && state.GetRejectCode() < REJECT_INTERNAL)
                    {
                        connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::REJECT, strCommand,
                                          uint8_t(state.GetRejectCode()),
                                          state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH),pstx.GetHash()));
                    }
                    if (nDoS > 0)
                    {
                        Misbehaving(pfrom, nDoS, state.GetRejectReason());
                    }
                }
            }
        }
        else
        {
            // do nothing, we dont have the payment tx so we wont accept the service tx for it
        }
    }

    // Ignore headers received while importing
    else if (strCommand == NetMsgType::HEADERS && !fImporting && !fReindex)
    {
        std::vector<CBlockHeader> headers;

        // Bypass the normal CBlock deserialization, as we don't want to risk deserializing 2000 full blocks.
        unsigned int nCount = ReadCompactSize(vRecv);
        if (nCount > MAX_HEADERS_RESULTS) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 20, "too-many-headers");
            return error("headers message size = %u", nCount);
        }
        headers.resize(nCount);
        for (unsigned int n = 0; n < nCount; n++) {
            vRecv >> headers[n];
            ReadCompactSize(vRecv); // ignore tx count; assume it is 0.
            ReadCompactSize(vRecv); // ignore empty vchBlockSig
        }

        LOCK(cs_main);

        if (nCount == 0) 
        {
            // Nothing interesting. Stop asking this peers for more headers.
            return true;
        }

        CBlockIndex *pindexLast = NULL;
        for(const CBlockHeader& header : headers) {
            CValidationState state;
            if (pindexLast != NULL && header.hashPrevBlock != pindexLast->GetBlockHash()) {
                Misbehaving(pfrom->GetId(), 20, "disconnected-header");
                return error("non-continuous headers sequence");
            }
            if (!AcceptBlockHeader(header, state, chainparams, &pindexLast)) {
                int nDoS;
                if (state.IsInvalid(nDoS)) {
                    if (nDoS > 0)
                        Misbehaving(pfrom->GetId(), nDoS, state.GetRejectReason());
                    return error("invalid header received");
                }
            }
        }

        if (pindexLast)
            UpdateBlockAvailability(pfrom->GetId(), pindexLast->GetBlockHash());

        if (nCount == MAX_HEADERS_RESULTS && pindexLast) 
        {
            // Headers message had its maximum size; the peer may have more headers.
            // TODO: optimize: if pindexLast is an ancestor of chainActive.Tip or pindexBestHeader, continue
            // from there instead.
            LogPrint("net", "more getheaders (%d) to end to peer=%d (startheight:%d)\n", pindexLast->nHeight, pfrom->id, pfrom->nStartingHeight);
            connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::GETHEADERS, pnetMan->getChainActive()->chainActive.GetLocator(pindexLast), uint256()));
        }

        bool fCanDirectFetch = CanDirectFetch(chainparams.GetConsensus());
        CNodeState *nodestate = State(pfrom->GetId());
        // If this set of headers is valid and ends in a block with at least as
        // much work as our tip, download as much as possible.
        if (fCanDirectFetch && pindexLast->IsValid(BLOCK_VALID_TREE) && pnetMan->getChainActive()->chainActive.Tip()->nChainWork <= pindexLast->nChainWork) {
            std::vector<CBlockIndex *> vToFetch;
            CBlockIndex *pindexWalk = pindexLast;
            // Calculate all the blocks we'd need to switch to pindexLast, up to a limit.
            while (pindexWalk && !pnetMan->getChainActive()->chainActive.Contains(pindexWalk) && vToFetch.size() <= MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
                if (!(pindexWalk->nStatus & BLOCK_HAVE_DATA) &&
                        !mapBlocksInFlight.count(pindexWalk->GetBlockHash())) {
                    // We don't have this block, and it's not yet in flight.
                    vToFetch.push_back(pindexWalk);
                }
                pindexWalk = pindexWalk->pprev;
            }
            // If pindexWalk still isn't on our main chain, we're looking at a
            // very large reorg at a time we think we're close to caught up to
            // the main chain -- this shouldn't really happen.  Bail out on the
            // direct fetch and rely on parallel download instead.
            if (!pnetMan->getChainActive()->chainActive.Contains(pindexWalk))
            {
                LogPrint("net", "Large reorg, won't direct fetch to %s (%d)\n",
                        pindexLast->GetBlockHash().ToString(),
                        pindexLast->nHeight);
            } 
            else 
            {
                std::vector<CInv> vGetData;
                // Download as much as possible, from earliest to latest.
                BOOST_REVERSE_FOREACH(CBlockIndex *pindex, vToFetch) {
                    if (nodestate->nBlocksInFlight >= MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
                        // Can't download any more from this peer
                        break;
                    }
                    vGetData.push_back(CInv(MSG_BLOCK, pindex->GetBlockHash()));
                    MarkBlockAsInFlight(pfrom->GetId(), pindex->GetBlockHash(), chainparams.GetConsensus(), pindex);
                    LogPrint("net", "Requesting block %s from  peer=%d\n",
                            pindex->GetBlockHash().ToString(), pfrom->id);
                }
                if (vGetData.size() > 1) {
                    LogPrint("net", "Downloading blocks toward %s (%d) via headers direct fetch\n",
                            pindexLast->GetBlockHash().ToString(), pindexLast->nHeight);
                }
                if (vGetData.size() > 0) 
                {
                    connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::GETDATA, vGetData));
                }
            }
        }
        CheckBlockIndex(chainparams.GetConsensus());
    }

    else if (strCommand == NetMsgType::BLOCK && !fImporting && !fReindex) // Ignore blocks received while importing
    {
        std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
        vRecv >> *pblock;

        LogPrint("net","received block %s peer=%d\n", pblock->GetHash().ToString(), pfrom->id);

        // Process all blocks from whitelisted peers, even if not requested,
        // unless we're still syncing with the network. Such an unrequested
        // block may still be processed, subject to the conditions in
        // AcceptBlock().
        bool forceProcessing = pfrom->fWhitelisted && !pnetMan->getChainActive()->IsInitialBlockDownload();
        const uint256 hash(pblock->GetHash());
        {
            LOCK(cs_main);
            // Also always process if we requested the block explicitly, as we
            // may need it even though it is not a candidate for a new best tip.
            forceProcessing |= MarkBlockAsReceived(hash);
            // mapBlockSource is only used for sending reject messages and DoS
            // scores, so the race between here and cs_main in ProcessNewBlock
            // is fine.
            mapBlockSource.emplace(hash, std::make_pair(pfrom->GetId(), true));
        }
        CValidationState state;
        ProcessNewBlock(state, chainparams, pfrom, pblock, forceProcessing, NULL);
        int nDoS;
        if (state.IsInvalid(nDoS))
        {
            assert (state.GetRejectCode() < REJECT_INTERNAL); // Blocks are never rejected with internal reject codes
            connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::REJECT, strCommand, (unsigned char)state.GetRejectCode(),
                               state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH), hash));
            if (nDoS > 0)
            {
                LOCK(cs_main);
                Misbehaving(pfrom->GetId(), nDoS, "invalid-blk");
            }
        }
    }

    // This asymmetric behavior for inbound and outbound connections was introduced
    // to prevent a fingerprinting attack: an attacker can send specific fake addresses
    // to users' AddrMan and later request them by sending getaddr messages.
    // Making nodes which are behind NAT and can only make outgoing connections ignore
    // the getaddr message mitigates the attack.
    else if ((strCommand == NetMsgType::GETADDR) && (pfrom->fInbound))
    {
        // This asymmetric behavior for inbound and outbound connections was
        // introduced to prevent a fingerprinting attack: an attacker can send
        // specific fake addresses to users' AddrMan and later request them by
        // sending getaddr messages. Making nodes which are behind NAT and can
        // only make outgoing connections ignore the getaddr message mitigates
        // the attack.
        if (!pfrom->fInbound)
        {
            LogPrintf("Ignoring \"getaddr\" from outbound connection. peer=%d\n", pfrom->id);
            return true;
        }

        // Only send one GetAddr response per connection to reduce resource
        // waste and discourage addr stamping of INV announcements.
        if (pfrom->fSentAddr)
        {
            LogPrintf("Ignoring repeated \"getaddr\". peer=%d\n",pfrom->id);
            return true;
        }
        pfrom->fSentAddr = true;

        pfrom->vAddrToSend.clear();
        std::vector<CAddress> vAddr = connman.GetAddresses();
        FastRandomContext insecure_rand;
        for (const CAddress &addr : vAddr) {
            pfrom->PushAddress(addr, insecure_rand);
        }
    }


    else if (strCommand == NetMsgType::MEMPOOL)
    {
        std::vector<uint256> vtxid;
        mempool.queryHashes(vtxid);
        std::vector<CInv> vInv;
        BOOST_FOREACH (uint256 &hash, vtxid)
        {
            CInv inv(MSG_TX, hash);
            if (pfrom->pfilter)
            {
                CTxMemPoolEntry txe;
                bool fInMemPool = mempool.lookup(hash, txe);
                if (!fInMemPool)
                    continue; // another thread removed since queryHashes, maybe...
                if (!pfrom->pfilter->IsRelevantAndUpdate(txe.GetTx()))
                    continue;
            }
            vInv.push_back(inv);
            if (vInv.size() == MAX_INV_SZ)
            {
                connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::INV, vInv));
                vInv.clear();
            }
        }
        if (vInv.size() > 0)
        {
            connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::INV, vInv));
        }
    }


    else if (strCommand == NetMsgType::PING)
    {
        {
            uint64_t nonce = 0;
            vRecv >> nonce;
            // Echo the message back with the nonce. This allows for two useful features:
            //
            // 1) A remote node can quickly check if the connection is operational
            // 2) Remote nodes can measure the latency of the network thread. If this node
            //    is overloaded it won't respond to pings quickly and the remote node can
            //    avoid sending us more work, like chain download requests.
            //
            // The nonce stops the remote getting confused between different pings: without
            // it, if the remote node sends a ping once per second and this node takes 5
            // seconds to respond to each, the 5th ping the remote sends would appear to
            // return very quickly.
            connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::PONG, nonce));
        }
    }


    else if (strCommand == NetMsgType::PONG)
    {
        int64_t pingUsecEnd = nTimeReceived;
        uint64_t nonce = 0;
        size_t nAvail = vRecv.in_avail();
        bool bPingFinished = false;
        std::string sProblem;

        if (nAvail >= sizeof(nonce)) {
            vRecv >> nonce;

            // Only process pong message if there is an outstanding ping (old ping without nonce should never pong)
            if (pfrom->nPingNonceSent != 0) {
                if (nonce == pfrom->nPingNonceSent) {
                    // Matching pong received, this ping is no longer outstanding
                    bPingFinished = true;
                    int64_t pingUsecTime = pingUsecEnd - pfrom->nPingUsecStart;
                    if (pingUsecTime > 0)
                    {
                        // Successful ping time measurement, replace previous
                        pfrom->nPingUsecTime = pingUsecTime;
                        pfrom->nMinPingUsecTime = std::min(pfrom->nMinPingUsecTime.load(), pingUsecTime);
                    }
                    else
                    {
                        // This should never happen
                        sProblem = "Timing mishap";
                    }
                }
                else
                {
                    // Nonce mismatches are normal when pings are overlapping
                    sProblem = "Nonce mismatch";
                    if (nonce == 0) {
                        // This is most likely a bug in another implementation somewhere; cancel this ping
                        bPingFinished = true;
                        sProblem = "Nonce zero";
                    }
                }
            }
            else
            {
                sProblem = "Unsolicited pong without ping";
            }
        }
        else
        {
            // This is most likely a bug in another implementation somewhere; cancel this ping
            bPingFinished = true;
            sProblem = "Short payload";
        }

        if (!(sProblem.empty()))
        {
            LogPrint("net", "pong peer=%d: %s, %x expected, %x received, %u bytes\n",
                pfrom->id,
                sProblem,
                pfrom->nPingNonceSent,
                nonce,
                nAvail);
        }
        if (bPingFinished) {
            pfrom->nPingNonceSent = 0;
        }
    }

    else if (strCommand == NetMsgType::FILTERLOAD)
    {
        CBloomFilter filter;
        vRecv >> filter;

        if (!filter.IsWithinSizeConstraints())
            // There is no excuse for sending a too-large filter
            Misbehaving(pfrom->GetId(), 100, "oversized-bloom-filter");
        else
        {
            LOCK(pfrom->cs_filter);
            delete pfrom->pfilter;
            pfrom->pfilter = new CBloomFilter(filter);
            pfrom->pfilter->UpdateEmptyFull();
        }
        pfrom->fRelayTxes = true;
    }


    else if (strCommand == NetMsgType::FILTERADD)
    {
        std::vector<unsigned char> vData;
        vRecv >> vData;

        // Nodes must NEVER send a data item > 520 bytes (the max size for a script data object,
        // and thus, the maximum size any matched object can have) in a filteradd message
        if (vData.size() > MAX_SCRIPT_ELEMENT_SIZE)
        {
            Misbehaving(pfrom->GetId(), 100, "invalid-filteradd");
        } else {
            LOCK(pfrom->cs_filter);
            if (pfrom->pfilter)
                pfrom->pfilter->insert(vData);
            else
                Misbehaving(pfrom->GetId(), 100, "invalid-filteradd");
        }
    }


    else if (strCommand == NetMsgType::FILTERCLEAR)
    {
        LOCK(pfrom->cs_filter);
        delete pfrom->pfilter;
        pfrom->pfilter = new CBloomFilter();
        pfrom->fRelayTxes = true;
    }


    else if (strCommand == NetMsgType::REJECT)
    {
        if (fDebug)
        {
            try {
                std::string strMsg; unsigned char ccode; std::string strReason;
                vRecv >> LIMITED_STRING(strMsg, CMessageHeader::COMMAND_SIZE) >> ccode >> LIMITED_STRING(strReason, MAX_REJECT_MESSAGE_LENGTH);

                std::ostringstream ss;
                ss << strMsg << " code " << itostr(ccode) << ": " << strReason;

                if (strMsg == NetMsgType::BLOCK || strMsg == NetMsgType::TX)
                {
                    uint256 hash;
                    vRecv >> hash;
                    ss << ": hash " << hash.ToString();
                }
                LogPrint("net", "Reject %s\n", SanitizeString(ss.str()));
            }
            catch (const std::ios_base::failure&)
            {
                // Avoid feedback loops by preventing reject messages from triggering a new reject message.
                LogPrint("net", "Unparseable reject message received\n");
            }
        }
    }

    else
    {
        // Ignore unknown commands for extensibility
        LogPrint("net", "Unknown command \"%s\" from peer=%d\n", SanitizeString(strCommand), pfrom->id);
    }



    return true;
}

bool ProcessMessages(CNode *pfrom, CConnman &connman, const std::atomic<bool> &interruptMsgProc)
{
    //
    // Message format
    //  (4) message start
    //  (12) command
    //  (4) size
    //  (4) checksum
    //  (x) data
    //
    bool fMoreWork = false;

    if (!pfrom->vRecvGetData.empty())
    {
        ProcessGetData(pfrom, connman, pnetMan->getActivePaymentNetwork()->GetConsensus(), interruptMsgProc);
    }

    if (pfrom->fDisconnect)
    {
        return false;
    }

    // this maintains the order of responses
    if (!pfrom->vRecvGetData.empty())
    {
        return true;
    }

    // Don't bother if send buffer is too full to respond anyway
    if (pfrom->fPauseSend)
    {
        return false;
    }

    std::list<CNetMessage> msgs;
    {
        LOCK(pfrom->cs_vProcessMsg);
        if (pfrom->vProcessMsg.empty()) {
            return false;
        }
        // Just take one message
        msgs.splice(msgs.begin(), pfrom->vProcessMsg,
                    pfrom->vProcessMsg.begin());
        pfrom->nProcessQueueSize -=
            msgs.front().vRecv.size() + CMessageHeader::HEADER_SIZE;
        pfrom->fPauseRecv =
            pfrom->nProcessQueueSize > connman.GetReceiveFloodSize();
        fMoreWork = !pfrom->vProcessMsg.empty();
    }
    CNetMessage &msg(msgs.front());

    msg.SetVersion(pfrom->GetRecvVersion());

    // Scan for message start
    if (memcmp(std::begin(msg.hdr.pchMessageStart),
               std::begin(pnetMan->getActivePaymentNetwork()->MessageStart()),
               CMessageHeader::MESSAGE_START_SIZE) != 0)
    {
        LogPrintf("PROCESSMESSAGE: INVALID MESSAGESTART %s peer=%d\n",
                  SanitizeString(msg.hdr.GetCommand()), pfrom->id);
        pfrom->fDisconnect = true;
        return false;
    }

    // Read header
    CMessageHeader &hdr = msg.hdr;
    if (!hdr.IsValid(pnetMan->getActivePaymentNetwork()->MessageStart()))
    {
        LogPrintf("PROCESSMESSAGE: ERRORS IN HEADER %s peer=%d\n",
                  SanitizeString(hdr.GetCommand()), pfrom->id);
        return fMoreWork;
    }
    std::string strCommand = hdr.GetCommand();

    // Message size
    unsigned int nMessageSize = hdr.nMessageSize;

    // Checksum
    CDataStream &vRecv = msg.vRecv;
    const uint256 &hash = msg.GetMessageHash();
    if (memcmp(hash.begin(), hdr.pchChecksum, CMessageHeader::CHECKSUM_SIZE) !=
        0) {
        LogPrintf(
            "%s(%s, %u bytes): CHECKSUM ERROR expected %s was %s\n", __func__,
            SanitizeString(strCommand), nMessageSize,
            HexStr(hash.begin(), hash.begin() + CMessageHeader::CHECKSUM_SIZE),
            HexStr(hdr.pchChecksum,
                   hdr.pchChecksum + CMessageHeader::CHECKSUM_SIZE));
        return fMoreWork;
    }

    // Process message
    bool fRet = false;
    try
    {
        fRet = ProcessMessage(pfrom, strCommand, vRecv, msg.nTime, connman, interruptMsgProc);
        if (interruptMsgProc)
        {
            return false;
        }
        if (!pfrom->vRecvGetData.empty())
        {
            fMoreWork = true;
        }
    }
    catch (const std::ios_base::failure &e)
    {
        connman.PushMessage(pfrom, CNetMsgMaker(MIN_PROTO_VERSION).Make(NetMsgType::REJECT, strCommand, REJECT_MALFORMED, std::string("error parsing message")));
        if (strstr(e.what(), "end of data"))
        {
            // Allow exceptions from under-length message on vRecv
            LogPrintf(
                "%s(%s, %u bytes): Exception '%s' caught, normally caused by a "
                "message being shorter than its stated length\n",
                __func__, SanitizeString(strCommand), nMessageSize, e.what());
        }
        else if (strstr(e.what(), "size too large"))
        {
            // Allow exceptions from over-long size
            LogPrintf("%s(%s, %u bytes): Exception '%s' caught\n", __func__,
                      SanitizeString(strCommand), nMessageSize, e.what());
        }
        else if (strstr(e.what(), "non-canonical ReadCompactSize()"))
        {
            // Allow exceptions from non-canonical encoding
            LogPrintf("%s(%s, %u bytes): Exception '%s' caught\n", __func__,
                      SanitizeString(strCommand), nMessageSize, e.what());
        }
        else
        {
            PrintExceptionContinue(&e, "ProcessMessages()");
        }
    }
    catch (const std::exception &e)
    {
        PrintExceptionContinue(&e, "ProcessMessages()");
    }
    catch (...)
    {
        PrintExceptionContinue(nullptr, "ProcessMessages()");
    }

    if (!fRet)
    {
        LogPrintf("%s(%s, %u bytes) FAILED peer=%d\n", __func__, SanitizeString(strCommand), nMessageSize, pfrom->id);
    }

    LOCK(cs_main);
    SendRejectsAndCheckIfBanned(pfrom, connman);

    return fMoreWork;
}

bool SendMessages(CNode *pto, CConnman &connman, const std::atomic<bool> &interruptMsgProc)
{
    const Consensus::Params &consensusParams = pnetMan->getActivePaymentNetwork()->GetConsensus();

    // Don't send anything until the version handshake is complete
    if (!pto->fSuccessfullyConnected || pto->fDisconnect) {
        return true;
    }

    // If we get here, the outgoing message serialization version is set and
    // can't change.
    const CNetMsgMaker msgMaker(pto->GetSendVersion());

    //
    // Message: ping
    //
    bool pingSend = false;
    if (pto->fPingQueued) {
        // RPC ping request by user
        pingSend = true;
    }
    if (pto->nPingNonceSent == 0 &&
        pto->nPingUsecStart + PING_INTERVAL * 1000000 < GetTimeMicros()) {
        // Ping automatically sent as a latency probe & keepalive.
        pingSend = true;
    }
    if (pingSend)
    {
        uint64_t nonce = 0;
        while (nonce == 0)
        {
            GetRandBytes((uint8_t *)&nonce, sizeof(nonce));
        }
        pto->fPingQueued = false;
        pto->nPingUsecStart = GetTimeMicros();
        pto->nPingNonceSent = nonce;
        connman.PushMessage(pto, msgMaker.Make(NetMsgType::PING, nonce));
    }

    // Acquire cs_main for IsInitialBlockDownload() and CNodeState()
    TRY_LOCK(cs_main, lockMain);
    if (!lockMain) {
        return true;
    }

    if (SendRejectsAndCheckIfBanned(pto, connman))
    {
        return true;
    }
    CNodeState &state = *State(pto->GetId());

    // Address refresh broadcast
    int64_t nNow = GetTimeMicros();
    if (!pnetMan->getChainActive()->IsInitialBlockDownload() && pto->nNextLocalAddrSend < nNow)
    {
        AdvertiseLocal(pto);
        pto->nNextLocalAddrSend = PoissonNextSend(nNow, AVG_LOCAL_ADDRESS_BROADCAST_INTERVAL);
    }

    //
    // Message: addr
    //
    if (pto->nNextAddrSend < nNow) {
        pto->nNextAddrSend =
            PoissonNextSend(nNow, AVG_ADDRESS_BROADCAST_INTERVAL);
        std::vector<CAddress> vAddr;
        vAddr.reserve(pto->vAddrToSend.size());
        for (const CAddress &addr : pto->vAddrToSend) {
            if (!pto->addrKnown.contains(addr.GetKey())) {
                pto->addrKnown.insert(addr.GetKey());
                vAddr.push_back(addr);
                // receiver rejects addr messages larger than 1000
                if (vAddr.size() >= 1000) {
                    connman.PushMessage(pto,
                                        msgMaker.Make(NetMsgType::ADDR, vAddr));
                    vAddr.clear();
                }
            }
        }
        pto->vAddrToSend.clear();
        if (!vAddr.empty()) {
            connman.PushMessage(pto, msgMaker.Make(NetMsgType::ADDR, vAddr));
        }

        // we only send the big addr message once
        if (pto->vAddrToSend.capacity() > 40) {
            pto->vAddrToSend.shrink_to_fit();
        }
    }

    // Start block sync
    if (pnetMan->getChainActive()->pindexBestHeader == nullptr)
    {
        pnetMan->getChainActive()->pindexBestHeader = pnetMan->getChainActive()->chainActive.Tip();
    }

    // Download if this is a nice peer, or we have no nice peers and this one
    // might do.
    bool fFetch = state.fPreferredDownload || (nPreferredDownload == 0 && !pto->fClient && !pto->fOneShot);

    if (!state.fSyncStarted && !pto->fClient && !fImporting && !fReindex) {
        // Only actively request headers from a single peer, unless we're close
        // to today.
        if ((nSyncStarted == 0 && fFetch) || pnetMan->getChainActive()->pindexBestHeader->GetBlockTime() > GetAdjustedTime() - 24 * 60 * 60)
        {
            state.fSyncStarted = true;
            nSyncStarted++;
            const CBlockIndex *pindexStart = pnetMan->getChainActive()->pindexBestHeader;
            /**
             * If possible, start at the block preceding the currently best
             * known header. This ensures that we always get a non-empty list of
             * headers back as long as the peer is up-to-date. With a non-empty
             * response, we can initialise the peer's known best block. This
             * wouldn't be possible if we requested starting at pindexBestHeader
             * and got back an empty response.
             */
            if (pindexStart->pprev) {
                pindexStart = pindexStart->pprev;
            }

            LogPrint("net", "initial getheaders (%d) to peer=%d (startheight:%d)\n", pindexStart->nHeight, pto->id, pto->nStartingHeight);
            connman.PushMessage( pto, msgMaker.Make(NetMsgType::GETHEADERS, pnetMan->getChainActive()->chainActive.GetLocator(pindexStart), uint256()));
        }
    }

    // Resend wallet transactions that haven't gotten in a block yet
    // Except during reindex, importing and IBD, when old wallet transactions
    // become unconfirmed and spams other nodes.
    if (!fReindex && !fImporting && !pnetMan->getChainActive()->IsInitialBlockDownload())
    {
        GetMainSignals().Broadcast(nTimeBestReceived, &connman);
    }

    //
    // Try sending block announcements via headers
    //
    {
        // If we have less than MAX_BLOCKS_TO_ANNOUNCE in our list of block
        // hashes we're relaying, and our peer wants headers announcements, then
        // find the first header not yet known to our peer but would connect,
        // and send. If no header would connect, or if we have too many blocks,
        // or if the peer doesn't want headers, just add all to the inv queue.
        LOCK(pto->cs_inventory);
        std::vector<CBlock> vHeaders;
        bool fRevertToInv = ((!state.fPreferHeaders && ( pto->vBlockHashesToAnnounce.size() > 1)) || pto->vBlockHashesToAnnounce.size() > MAX_BLOCKS_TO_ANNOUNCE);
        // last header queued for delivery
        CBlockIndex *pBestIndex = nullptr;
        // ensure pindexBestKnownBlock is up-to-date
        ProcessBlockAvailability(pto->id);

        if (!fRevertToInv)
        {
            bool fFoundStartingHeader = false;
            // Try to find first header that our peer doesn't have, and then
            // send all headers past that one. If we come across an headers that
            // aren't on chainActive, give up.
            for (const uint256 &hash : pto->vBlockHashesToAnnounce) {
                BlockMap::iterator mi = pnetMan->getChainActive()->mapBlockIndex.find(hash);
                assert(mi != pnetMan->getChainActive()->mapBlockIndex.end());
                CBlockIndex *pindex = mi->second;
                if (pnetMan->getChainActive()->chainActive[pindex->nHeight] != pindex)
                {
                    // Bail out if we reorged away from this block
                    fRevertToInv = true;
                    break;
                }
                if (pBestIndex != nullptr && pindex->pprev != pBestIndex)
                {
                    // This means that the list of blocks to announce don't
                    // connect to each other. This shouldn't really be possible
                    // to hit during regular operation (because reorgs should
                    // take us to a chain that has some block not on the prior
                    // chain, which should be caught by the prior check), but
                    // one way this could happen is by using invalidateblock /
                    // reconsiderblock repeatedly on the tip, causing it to be
                    // added multiple times to vBlockHashesToAnnounce. Robustly
                    // deal with this rare situation by reverting to an inv.
                    fRevertToInv = true;
                    break;
                }
                pBestIndex = pindex;
                if (fFoundStartingHeader)
                {
                    // add this to the headers message
                    vHeaders.push_back(pindex->GetBlockHeader());
                }
                else if (PeerHasHeader(&state, pindex))
                {
                    // Keep looking for the first new block.
                    continue;
                }
                else if (pindex->pprev == nullptr || PeerHasHeader(&state, pindex->pprev))
                {
                    // Peer doesn't have this header but they do have the prior
                    // one.
                    // Start sending headers.
                    fFoundStartingHeader = true;
                    vHeaders.push_back(pindex->GetBlockHeader());
                }
                else
                {
                    // Peer doesn't have this header or the prior one --
                    // nothing will connect, so bail out.
                    fRevertToInv = true;
                    break;
                }
            }
        }
        if (!fRevertToInv && !vHeaders.empty())
        {
            if (state.fPreferHeaders)
            {
                if (vHeaders.size() > 1)
                {
                    LogPrint("net", "%s: %u headers, range (%s, %s), to peer=%d\n", __func__, vHeaders.size(),
                             vHeaders.front().GetHash().ToString(), vHeaders.back().GetHash().ToString(), pto->id);
                }
                else
                {
                    LogPrint("net", "%s: sending header %s to peer=%d\n", __func__, vHeaders.front().GetHash().ToString(), pto->id);
                }
                connman.PushMessage(pto, msgMaker.Make(NetMsgType::HEADERS, vHeaders));
                state.pindexBestHeaderSent = pBestIndex;
            }
            else
            {
                fRevertToInv = true;
            }
        }
        if (fRevertToInv)
        {
            // If falling back to using an inv, just try to inv the tip. The
            // last entry in vBlockHashesToAnnounce was our tip at some point in
            // the past.
            if (!pto->vBlockHashesToAnnounce.empty())
            {
                const uint256 &hashToAnnounce = pto->vBlockHashesToAnnounce.back();
                BlockMap::iterator mi = pnetMan->getChainActive()->mapBlockIndex.find(hashToAnnounce);
                assert(mi != pnetMan->getChainActive()->mapBlockIndex.end());
                CBlockIndex *pindex = mi->second;

                // Warn if we're announcing a block that is not on the main
                // chain. This should be very rare and could be optimized out.
                // Just log for now.
                if (pnetMan->getChainActive()->chainActive[pindex->nHeight] != pindex)
                {
                    LogPrint("net", "Announcing block %s not on main chain (tip=%s)\n",
                             hashToAnnounce.ToString(), pnetMan->getChainActive()->chainActive.Tip()->GetBlockHash().ToString());
                }

                // If the peer's chain has this block, don't inv it back.
                if (!PeerHasHeader(&state, pindex))
                {
                    pto->PushInventory(CInv(MSG_BLOCK, hashToAnnounce));
                    LogPrint("net", "%s: sending inv peer=%d hash=%s\n",
                             __func__, pto->id, hashToAnnounce.ToString());
                }
            }
        }
        pto->vBlockHashesToAnnounce.clear();
    }

    //
    // Message: inventory
    //   
    std::vector<CInv> vInv;
    {
        LOCK(pto->cs_inventory);
        vInv.reserve(std::max<size_t>(pto->vInventoryBlockToSend.size(), INVENTORY_BROADCAST_MAX));

        // Add blocks
        for (const uint256 &hash : pto->vInventoryBlockToSend)
        {
            vInv.push_back(CInv(MSG_BLOCK, hash));
            if (vInv.size() == MAX_INV_SZ)
            {
                connman.PushMessage(pto, msgMaker.Make(NetMsgType::INV, vInv));
                vInv.clear();
            }
        }
        pto->vInventoryBlockToSend.clear();

        // Check whether periodic sends should happen
        bool fSendTrickle = pto->fWhitelisted;
        if (pto->nNextInvSend < nNow)
        {
            fSendTrickle = true;
            // Use half the delay for outbound peers, as there is less privacy
            // concern for them.
            pto->nNextInvSend = PoissonNextSend(nNow, INVENTORY_BROADCAST_INTERVAL >> !pto->fInbound);
        }

        // Time to send but the peer has requested we not relay transactions.
        if (fSendTrickle)
        {
            LOCK(pto->cs_filter);
            if (!pto->fRelayTxes)
            {
                pto->setInventoryTxToSend.clear();
            }
        }

        // Determine transactions to relay
        if (fSendTrickle) 
        {
            // Produce a vector with all candidates for sending
            std::vector<std::set<uint256>::iterator> vInvTx;
            vInvTx.reserve(pto->setInventoryTxToSend.size());
            for (std::set<uint256>::iterator it = pto->setInventoryTxToSend.begin(); it != pto->setInventoryTxToSend.end(); it++)
            {
                vInvTx.push_back(it);
            }
            // No reason to drain out at many times the network's capacity,
            // especially since we have many peers and some will draw much
            // shorter delays.
            unsigned int nRelayedTransactions = 0;
            LOCK(pto->cs_filter);
            while (!vInvTx.empty() && nRelayedTransactions < INVENTORY_BROADCAST_MAX)
            {
                std::set<uint256>::iterator it = vInvTx.back();
                vInvTx.pop_back();
                uint256 hash = *it;
                // Remove it from the to-be-sent set
                pto->setInventoryTxToSend.erase(it);
                // Check if not in the filter already
                if (pto->filterInventoryKnown.contains(hash))
                {
                    continue;
                }
                // Not in the mempool anymore? don't bother sending it.
                if (!mempool.exists(hash))
                {
                    continue;
                }
                // Send
                vInv.push_back(CInv(MSG_TX, hash));
                nRelayedTransactions++;
                {
                    // Expire old relay messages
                    while (!vRelayExpiration.empty() && vRelayExpiration.front().first < nNow)
                    {
                        mapRelay.erase(vRelayExpiration.front().second);
                        vRelayExpiration.pop_front();
                    }
                }
                if (vInv.size() == MAX_INV_SZ)
                {
                    connman.PushMessage(pto, msgMaker.Make(NetMsgType::INV, vInv));
                    vInv.clear();
                }
                pto->filterInventoryKnown.insert(hash);
            }
        }

        if(!pto->setInventoryStxToSend.empty())
        {
            std::vector<std::set<uint256>::iterator> vInvStx;
            for (std::set<uint256>::iterator it = pto->setInventoryStxToSend.begin(); it != pto->setInventoryStxToSend.end(); it++) 
            {
                vInvStx.push_back(it);
            }
            uint64_t nRelayedTransactions = 0;
            while (!vInvStx.empty() && nRelayedTransactions < INVENTORY_BROADCAST_MAX)
            {
                std::set<uint256>::iterator it = vInvStx.back();
                vInvStx.pop_back();
                uint256 hash = *it;
                pto->setInventoryStxToSend.erase(it);
                if (pto->filterServiceDataKnown.contains(hash))
                {
                    continue;
                }
                vInv.push_back(CInv(MSG_STX, hash));
                nRelayedTransactions++;
                if (vInv.size() == MAX_INV_SZ)
                {
                    connman.PushMessage(pto, msgMaker.Make(NetMsgType::INV, vInv));
                    vInv.clear();
                }
            }

        }
    }
    if (!vInv.empty())
    {
        connman.PushMessage(pto, msgMaker.Make(NetMsgType::INV, vInv));
    }

    // Detect whether we're stalling
    nNow = GetTimeMicros();
    if (state.nStallingSince && state.nStallingSince < nNow - 1000000 * BLOCK_STALLING_TIMEOUT)
    {
        // Stalling only triggers when the block download window cannot move.
        // During normal steady state, the download window should be much larger
        // than the to-be-downloaded set of blocks, so disconnection should only
        // happen during initial block download.
        LogPrintf("Peer=%d is stalling block download, disconnecting\n", pto->id);
        pto->fDisconnect = true;
        return true;
    }
    // In case there is a block that has been in flight from this peer for 2 +
    // 0.5 * N times the block interval (with N the number of peers from which
    // we're downloading validated blocks), disconnect due to timeout. We
    // compensate for other peers to prevent killing off peers due to our own
    // downstream link being saturated. We only count validated in-flight blocks
    // so peers can't advertise non-existing block hashes to unreasonably
    // increase our timeout.
    if (state.vBlocksInFlight.size() > 0)
    {
        int64_t targetSpacing = consensusParams.nTargetSpacing;
        if(pnetMan->getChainActive()->chainActive.Tip()->GetMedianTimePast() > SERVICE_UPGRADE_HARDFORK)
        {
            targetSpacing = 150;
        }
        QueuedBlock &queuedBlock = state.vBlocksInFlight.front();
        int nOtherPeersWithValidatedDownloads = nPeersWithValidatedDownloads - (state.nBlocksInFlightValidHeaders > 0);
        if (nNow > state.nDownloadingSince +
                       targetSpacing * (BLOCK_DOWNLOAD_TIMEOUT_BASE +
                            BLOCK_DOWNLOAD_TIMEOUT_PER_PEER * nOtherPeersWithValidatedDownloads)) {
            LogPrintf("Timeout downloading block %s from peer=%d, "
                      "disconnecting\n",
                      queuedBlock.hash.ToString(), pto->id);
            pto->fDisconnect = true;
            return true;
        }
    }

    //
    // Message: getdata (blocks)
    //
    std::vector<CInv> vGetData;
    if (!pto->fClient && (fFetch || !pnetMan->getChainActive()->IsInitialBlockDownload()) && state.nBlocksInFlight < MAX_BLOCKS_IN_TRANSIT_PER_PEER)
    {
        std::vector<const CBlockIndex *> vToDownload;
        NodeId staller = -1;
        FindNextBlocksToDownload(pto->GetId(), MAX_BLOCKS_IN_TRANSIT_PER_PEER - state.nBlocksInFlight, vToDownload, staller);
        for (const CBlockIndex *pindex : vToDownload)
        {
            uint32_t nFetchFlags = GetFetchFlags(pto, pindex->pprev, consensusParams);
            vGetData.push_back(CInv(MSG_BLOCK | nFetchFlags, pindex->GetBlockHash()));
            MarkBlockAsInFlight(pto->GetId(), pindex->GetBlockHash(), consensusParams, pindex);
            LogPrint("net", "Requesting block %s (%d) peer=%d\n", pindex->GetBlockHash().ToString(), pindex->nHeight, pto->id);
        }
        if (state.nBlocksInFlight == 0 && staller != -1)
        {
            if (State(staller)->nStallingSince == 0)
            {
                State(staller)->nStallingSince = nNow;
                LogPrint("net", "Stall started peer=%d\n", staller);
            }
        }
    }

    //
    // Message: getdata (non-blocks)
    //
    while (!pto->mapAskFor.empty() && (*pto->mapAskFor.begin()).first <= nNow)
    {
        const CInv &inv = (*pto->mapAskFor.begin()).second;
        if (!AlreadyHave(inv))
        {
            if(!pto->filterServiceDataKnown.contains(inv.hash))
            {
                LogPrint("net", "Requesting %s peer=%d\n", inv.ToString(), pto->id);
                vGetData.push_back(inv);
                if (vGetData.size() >= 1000)
                {
                    connman.PushMessage(pto, msgMaker.Make(NetMsgType::GETDATA, vGetData));
                    vGetData.clear();
                }
            }
        }
        else
        {
            // If we're not going to ask, don't expect a response.
            pto->setAskFor.erase(inv.hash);
        }
        pto->mapAskFor.erase(pto->mapAskFor.begin());
    }
    if (!vGetData.empty())
    {
        connman.PushMessage(pto, msgMaker.Make(NetMsgType::GETDATA, vGetData));
    }
    return true;
}

class CNetProcessingCleanup {
public:
    CNetProcessingCleanup() {}
    ~CNetProcessingCleanup() {
        // orphan transactions
        mapOrphanTransactions.clear();
        mapOrphanTransactionsByPrev.clear();
    }
} instance_of_cnetprocessingcleanup;
