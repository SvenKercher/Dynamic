// Copyright (c) 2016-2018 Duality Blockchain Solutions Developers
// Copyright (c) 2014-2018 The Dash Core Developers
// Copyright (c) 2009-2018 The Bitcoin Developers
// Copyright (c) 2009-2018 Satoshi Nakamoto
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/dynamic-config.h"
#endif

#include "init.h"

#include "activedynode.h"
#include "addrman.h"
#include "amount.h"
#include "base58.h"
#include "chain.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "dynode-payments.h"
#include "dynode-sync.h"
#include "dynodeconfig.h"
#include "dynodeman.h"
#include "flat-database.h"
#include "governance.h"
#include "instantsend.h"
#include "httpserver.h"
#include "httprpc.h"
#include "key.h"
#include "validation.h"
#include "messagesigner.h"
#include "miner.h"
#include "net.h"
#include "netfulfilledman.h"
#include "net_processing.h"
#include "policy/policy.h"
#ifdef ENABLE_WALLET
#include "privatesend-client.h"
#endif // ENABLE_WALLET
#include "privatesend-server.h"
#include "psnotificationinterface.h"
#include "rpcregister.h"
#include "rpcserver.h"
#include "compat/sanity.h"
#include "script/standard.h"
#include "script/sigcache.h"
#include "scheduler.h"
#include "spork.h"
#include "timedata.h"
#include "txdb.h"
#include "txmempool.h"
#include "torcontrol.h"
#include "ui_interface.h"
#include "util.h"
#include "utilmoneystr.h"
#include "utilstrencodings.h"
#include "consensus/validation.h"
#include "validationinterface.h"
#ifdef ENABLE_WALLET
#include "wallet/db.h"
#include "keepass.h"
#include "wallet/wallet.h"
#include "wallet/walletdb.h"
#endif

#include <memory>
#include <stdint.h>
#include <stdio.h>

#ifndef WIN32
#include <signal.h>
#endif

#include <openssl/crypto.h>

#include <boost/bind.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/filesystem.hpp>
#include <boost/function.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/thread.hpp>

#if ENABLE_ZMQ
#include "zmq/zmqnotificationinterface.h"
#endif

extern void ThreadSendAlert(CConnman& connman);

bool fFeeEstimatesInitialized = false;
bool fRestartRequested = false;  // true: restart false: shutdown

static const bool DEFAULT_PROXYRANDOMIZE = true;
static const bool DEFAULT_REST_ENABLE = false;
static const bool DEFAULT_DISABLE_SAFEMODE = false;
static const bool DEFAULT_STOPAFTERBLOCKIMPORT = false;

std::unique_ptr<CConnman> g_connman;
std::unique_ptr<PeerLogicValidation> peerLogic;

#if ENABLE_ZMQ
static CZMQNotificationInterface* pzmqNotificationInterface = NULL;
#endif

static CPSNotificationInterface* ppsNotificationInterface = NULL;

#ifdef WIN32
// Win32 LevelDB doesn't use filedescriptors, and the ones used for
// accessing block files don't count towards the fd_set size limit
// anyway.
#define MIN_CORE_FILEDESCRIPTORS 0
#else
#define MIN_CORE_FILEDESCRIPTORS 150
#endif

/** Used to pass flags to the Bind() function */
enum BindFlags {
    BF_NONE         = 0,
    BF_EXPLICIT     = (1U << 0),
    BF_REPORT_ERROR = (1U << 1),
    BF_WHITELIST    = (1U << 2),
};

static const char* FEE_ESTIMATES_FILENAME="fee_estimates.dat";

//////////////////////////////////////////////////////////////////////////////
//
// Shutdown
//

//
// Thread management and startup/shutdown:
//
// The network-processing threads are all part of a thread group
// created by AppInit() or the Qt main() function.
//
// A clean exit happens when StartShutdown() or the SIGTERM
// signal handler sets fRequestShutdown, which triggers
// the DetectShutdownThread(), which interrupts the main thread group.
// DetectShutdownThread() then exits, which causes AppInit() to
// continue (it .joins the shutdown thread).
// Shutdown() is then
// called to clean up database connections, and stop other
// threads that should only be stopped after the main network-processing
// threads have exited.
//
// Note that if running -daemon the parent process returns from AppInit2
// before adding any threads to the threadGroup, so .join_all() returns
// immediately and the parent exits from main().
//
// Shutdown for Qt is very similar, only it uses a QTimer to detect
// fRequestShutdown getting set, and then does the normal Qt
// shutdown thing.
//

std::atomic<bool> fRequestShutdown(false);

void StartShutdown()
{
    fRequestShutdown = true;
}
bool ShutdownRequested()
{
    return fRequestShutdown || fRestartRequested;
}

class CCoinsViewErrorCatcher : public CCoinsViewBacked
{
public:
    CCoinsViewErrorCatcher(CCoinsView* view) : CCoinsViewBacked(view) {}
    bool GetCoin(const COutPoint &outpoint, Coin &coin) const override {
        try {
            return CCoinsViewBacked::GetCoin(outpoint, coin);
        } catch(const std::runtime_error& e) {
            uiInterface.ThreadSafeMessageBox(_("Error reading from database, shutting down."), "", CClientUIInterface::MSG_ERROR);
            LogPrintf("Error reading from database: %s\n", e.what());
            // Starting the shutdown sequence and returning false to the caller would be
            // interpreted as 'entry not found' (as opposed to unable to read data), and
            // could lead to invalid interpretation. Just exit immediately, as we can't
            // continue anyway, and all writes should be atomic.
            abort();
        }
    }
    // Writes do not need similar protection, as failure to write is handled by the caller.
};

static CCoinsViewErrorCatcher *pcoinscatcher = NULL;
static std::unique_ptr<ECCVerifyHandle> globalVerifyHandle;

void Interrupt(boost::thread_group& threadGroup)
{
    InterruptHTTPServer();
    InterruptHTTPRPC();
    InterruptRPC();
    InterruptREST();
    InterruptTorControl();
    if (g_connman)
        g_connman->Interrupt();
    threadGroup.interrupt_all();
}

/** Preparing steps before shutting down or restarting the wallet */
void PrepareShutdown()
{
    fRequestShutdown = true; // Needed when we shutdown the wallet
    fRestartRequested = true; // Needed when we restart the wallet

    LogPrintf("%s: In progress...\n", __func__);
    static CCriticalSection cs_Shutdown;
    TRY_LOCK(cs_Shutdown, lockShutdown);
    if (!lockShutdown)
        return;

    /// Note: Shutdown() must be able to handle cases in which AppInit2() failed part of the way,
    /// for example if the data directory was found to be locked.
    /// Be sure that anything that writes files or flushes caches only does this if the respective
    /// module was initialized.
    RenameThread("dynamic-shutoff");
    mempool.AddTransactionsUpdated(1);
    StopHTTPRPC();
    StopREST();
    StopRPC();
    StopHTTPServer();
#ifdef ENABLE_WALLET
    if (pwalletMain)
        pwalletMain->Flush(false);
#endif
    GenerateDynamics(false, 0, Params(), *g_connman);
    MapPort(false);
    UnregisterValidationInterface(peerLogic.get());
    peerLogic.reset();
    g_connman.reset();

    // STORE DATA CACHES INTO SERIALIZED DAT FILES
    CFlatDB<CDynodeMan> flatdb1("dncache.dat", "magicDynodeCache");
    flatdb1.Dump(dnodeman);
    CFlatDB<CDynodePayments> flatdb2("dnpayments.dat", "magicDynodePaymentsCache");
    flatdb2.Dump(dnpayments);
    CFlatDB<CGovernanceManager> flatdb3("governance.dat", "magicGovernanceCache");
    flatdb3.Dump(governance);
    CFlatDB<CNetFulfilledRequestManager> flatdb4("netfulfilled.dat", "magicFulfilledCache");
    flatdb4.Dump(netfulfilledman);

    UnregisterNodeSignals(GetNodeSignals());
    DumpMempool();

    if (fFeeEstimatesInitialized)
    {
        boost::filesystem::path est_path = GetDataDir() / FEE_ESTIMATES_FILENAME;
        CAutoFile est_fileout(fopen(est_path.string().c_str(), "wb"), SER_DISK, CLIENT_VERSION);
        if (!est_fileout.IsNull())
            mempool.WriteFeeEstimates(est_fileout);
        else
            LogPrintf("%s: Failed to write fee estimates to %s\n", __func__, est_path.string());
        fFeeEstimatesInitialized = false;
    }

    {
        LOCK(cs_main);
        if (pcoinsTip != NULL) {
            FlushStateToDisk();
        }
        delete pcoinsTip;
        pcoinsTip = NULL;
        delete pcoinscatcher;
        pcoinscatcher = NULL;
        delete pcoinsdbview;
        pcoinsdbview = NULL;
        delete pblocktree;
        pblocktree = NULL;
    }
#ifdef ENABLE_WALLET
    if (pwalletMain)
        pwalletMain->Flush(true);
#endif

#if ENABLE_ZMQ
    if (pzmqNotificationInterface) {
        UnregisterValidationInterface(pzmqNotificationInterface);
        delete pzmqNotificationInterface;
        pzmqNotificationInterface = NULL;
    }
#endif

    if (ppsNotificationInterface) {
        UnregisterValidationInterface(ppsNotificationInterface);
        delete ppsNotificationInterface;
        ppsNotificationInterface = NULL;
    }

#ifndef WIN32
    try {
        boost::filesystem::remove(GetPidFile());
    } catch (const boost::filesystem::filesystem_error& e) {
        LogPrintf("%s: Unable to remove pidfile: %s\n", __func__, e.what());
    }
#endif
    UnregisterAllValidationInterfaces();
}

/**
* Shutdown is split into 2 parts:
* Part 1: shut down everything but the main wallet instance (done in PrepareShutdown() )
* Part 2: delete wallet instance
*
* In case of a restart PrepareShutdown() was already called before, but this method here gets
* called implicitly when the parent object is deleted. In this case we have to skip the
* PrepareShutdown() part because it was already executed and just delete the wallet instance.
*/
void Shutdown()
{
    // Shutdown part 1: prepare shutdown
    if(!fRestartRequested){
        PrepareShutdown();
    }
   // Shutdown part 2: Stop TOR thread and delete wallet instance
    StopTorControl();
#ifdef ENABLE_WALLET
    delete pwalletMain;
    pwalletMain = NULL;
#endif
    globalVerifyHandle.reset();
    ECC_Stop();
    LogPrintf("%s: done\n", __func__);
}

/**
 * Signal handlers are very limited in what they are allowed to do, so:
 */
void HandleSIGTERM(int)
{
    fRequestShutdown = true;
}

void HandleSIGHUP(int)
{
    fReopenDebugLog = true;
}

bool static Bind(CConnman& connman, const CService &addr, unsigned int flags) {
    if (!(flags & BF_EXPLICIT) && IsLimited(addr))
        return false;
    std::string strError;
    if (!connman.BindListenPort(addr, strError, (flags & BF_WHITELIST) != 0)) {
        if (flags & BF_REPORT_ERROR)
            return InitError(strError);
        return false;
    }
    return true;
}

void OnRPCStopped()
{
    cvBlockChange.notify_all();
    LogPrint("rpc", "RPC stopped.\n");
}

void OnRPCPreCommand(const CRPCCommand& cmd)
{
    // Observe safe mode
    std::string strWarning = GetWarnings("rpc");
    if (strWarning != "" && !GetBoolArg("-disablesafemode", DEFAULT_DISABLE_SAFEMODE) &&
        !cmd.okSafeMode)
        throw JSONRPCError(RPC_FORBIDDEN_BY_SAFE_MODE, std::string("Safe mode: ") + strWarning);
}

std::string HelpMessage(HelpMessageMode mode)
{
    const bool showDebug = GetBoolArg("-help-debug", false);

    // When adding new options to the categories, please keep and ensure alphabetical ordering.
    // Do not translate _(...) -help-debug options, Many technical terms, and only a very small audience, so is unnecessary stress to translators.
    std::string strUsage = HelpMessageGroup(_("Options:"));
    strUsage += HelpMessageOpt("-?", _("Print this help message and exit"));
    strUsage += HelpMessageOpt("-version", _("Print version and exit"));
    strUsage += HelpMessageOpt("-alerts", strprintf(_("Receive and display P2P network alerts (default: %u)"), DEFAULT_ALERTS));
    strUsage += HelpMessageOpt("-alertnotify=<cmd>", _("Execute command when a relevant alert is received or we see a really long fork (%s in cmd is replaced by message)"));
    strUsage += HelpMessageOpt("-blocknotify=<cmd>", _("Execute command when the best block changes (%s in cmd is replaced by block hash)"));
    if (showDebug)
        strUsage += HelpMessageOpt("-blocksonly", strprintf(_("Whether to operate in a blocks only mode (default: %u)"), DEFAULT_BLOCKSONLY));
    strUsage += HelpMessageOpt("-checkblocks=<n>", strprintf(_("How many blocks to check at startup (default: %u, 0 = all)"), DEFAULT_CHECKBLOCKS));
    strUsage += HelpMessageOpt("-checklevel=<n>", strprintf(_("How thorough the block verification of -checkblocks is (0-4, default: %u)"), DEFAULT_CHECKLEVEL));
    strUsage += HelpMessageOpt("-conf=<file>", strprintf(_("Specify configuration file (default: %s)"), DYNAMIC_CONF_FILENAME));
    if (mode == HMM_DYNAMICD)
    {
#if HAVE_DECL_DAEMON
        strUsage += HelpMessageOpt("-daemon", _("Run in the background as a daemon and accept commands"));
#endif
    }
    strUsage += HelpMessageOpt("-datadir=<dir>", _("Specify data directory"));
    strUsage += HelpMessageOpt("-dbcache=<n>", strprintf(_("Set database cache size in megabytes (%d to %d, default: %d)"), nMinDbCache, nMaxDbCache, nDefaultDbCache));
    strUsage += HelpMessageOpt("-feefilter", strprintf(_("Tell other nodes to filter invs to us by our mempool min fee (default: %u)"), DEFAULT_FEEFILTER));
    strUsage += HelpMessageOpt("-loadblock=<file>", _("Imports blocks from external blk000??.dat file on startup"));
    strUsage += HelpMessageOpt("-maxorphantx=<n>", strprintf(_("Keep at most <n> unconnectable transactions in memory (default: %u)"), DEFAULT_MAX_ORPHAN_TRANSACTIONS));
    strUsage += HelpMessageOpt("-maxmempool=<n>", strprintf(_("Keep the transaction memory pool below <n> megabytes (default: %u)"), DEFAULT_MAX_MEMPOOL_SIZE));
    strUsage += HelpMessageOpt("-mempoolexpiry=<n>", strprintf(_("Do not keep transactions in the mempool longer than <n> hours (default: %u)"), DEFAULT_MEMPOOL_EXPIRY));
    strUsage += HelpMessageOpt("-par=<n>", strprintf(_("Set the number of script verification threads (%u to %d, 0 = auto, <0 = leave that many cores free, default: %d)"),
        -GetNumCores(), MAX_SCRIPTCHECK_THREADS, DEFAULT_SCRIPTCHECK_THREADS));
#ifndef WIN32
    strUsage += HelpMessageOpt("-pid=<file>", strprintf(_("Specify pid file (default: %s)"), DYNAMIC_PID_FILENAME));
#endif
    strUsage += HelpMessageOpt("-prune=<n>", strprintf(_("Reduce storage requirements by pruning (deleting) old blocks. This mode is incompatible with -txindex and -rescan. "
            "Warning: Reverting this setting requires re-downloading the entire blockchain. "
            "(default: 0 = disable pruning blocks, >%u = target size in MiB to use for block files)"), MIN_DISK_SPACE_FOR_BLOCK_FILES / 1024 / 1024));
    strUsage += HelpMessageOpt("-reindex-chainstate", _("Rebuild chain state from the currently indexed blocks"));
    strUsage += HelpMessageOpt("-reindex", _("Rebuild chain state and block index from the blk*.dat files on disk"));
#ifndef WIN32
    strUsage += HelpMessageOpt("-sysperms", _("Create new files with system default permissions, instead of umask 077 (only effective with disabled wallet functionality)"));
#endif
    strUsage += HelpMessageOpt("-txindex", strprintf(_("Maintain a full transaction index, used by the getrawtransaction rpc call (default: %u)"), DEFAULT_TXINDEX));

    strUsage += HelpMessageOpt("-addressindex", strprintf(_("Maintain a full address index, used to query for the balance, txids and unspent outputs for addresses (default: %u)"), DEFAULT_ADDRESSINDEX));
    strUsage += HelpMessageOpt("-timestampindex", strprintf(_("Maintain a timestamp index for block hashes, used to query blocks hashes by a range of timestamps (default: %u)"), DEFAULT_TIMESTAMPINDEX));
    strUsage += HelpMessageOpt("-spentindex", strprintf(_("Maintain a full spent index, used to query the spending txid and input index for an outpoint (default: %u)"), DEFAULT_SPENTINDEX));

    strUsage += HelpMessageGroup(_("Connection options:"));
    strUsage += HelpMessageOpt("-addnode=<ip>", _("Add a node to connect to and attempt to keep the connection open"));
    strUsage += HelpMessageOpt("-banscore=<n>", strprintf(_("Threshold for disconnecting misbehaving peers (default: %u)"), DEFAULT_BANSCORE_THRESHOLD));
    strUsage += HelpMessageOpt("-bantime=<n>", strprintf(_("Number of seconds to keep misbehaving peers from reconnecting (default: %u)"), DEFAULT_MISBEHAVING_BANTIME));
    strUsage += HelpMessageOpt("-bind=<addr>", _("Bind to given address and always listen on it. Use [host]:port notation for IPv6"));
    strUsage += HelpMessageOpt("-connect=<ip>", _("Connect only to the specified node(s); -noconnect or -connect=0 alone to disable automatic connections"));
    strUsage += HelpMessageOpt("-discover", _("Discover own IP addresses (default: 1 when listening and no -externalip or -proxy)"));
    strUsage += HelpMessageOpt("-dns", _("Allow DNS lookups for -addnode, -seednode and -connect") + " " + strprintf(_("(default: %u)"), DEFAULT_NAME_LOOKUP));
    strUsage += HelpMessageOpt("-dnsseed", _("Query for peer addresses via DNS lookup, if low on addresses (default: 1 unless -connect/-noconnect)"));
    strUsage += HelpMessageOpt("-externalip=<ip>", _("Specify your own public address"));
    strUsage += HelpMessageOpt("-forcednsseed", strprintf(_("Always query for peer addresses via DNS lookup (default: %u)"), DEFAULT_FORCEDNSSEED));
    strUsage += HelpMessageOpt("-listen", _("Accept connections from outside (default: 1 if no -proxy or -connect/-noconnect)"));
    strUsage += HelpMessageOpt("-listenonion", strprintf(_("Automatically create Tor hidden service (default: %d)"), DEFAULT_LISTEN_ONION));
    strUsage += HelpMessageOpt("-maxconnections=<n>", strprintf(_("Maintain at most <n> connections to peers (temporary service connections excluded) (default: %u)"), DEFAULT_MAX_PEER_CONNECTIONS));
    strUsage += HelpMessageOpt("-maxreceivebuffer=<n>", strprintf(_("Maximum per-connection receive buffer, <n>*1000 bytes (default: %u)"), DEFAULT_MAXRECEIVEBUFFER));
    strUsage += HelpMessageOpt("-maxsenFdbuffer=<n>", strprintf(_("Maximum per-connection send buffer, <n>*1000 bytes (default: %u)"), DEFAULT_MAXSENDBUFFER));
    strUsage += HelpMessageOpt("-maxtimeadjustment", strprintf(_("Maximum allowed median peer time offset adjustment. Local perspective of time may be influenced by peers forward or backward by this amount. (default: %u seconds)"), DEFAULT_MAX_TIME_ADJUSTMENT));
    strUsage += HelpMessageOpt("-onion=<ip:port>", strprintf(_("Use separate SOCKS5 proxy to reach peers via Tor hidden services (default: %s)"), "-proxy"));
    strUsage += HelpMessageOpt("-onlynet=<net>", _("Only connect to nodes in network <net> (ipv4, ipv6 or onion)"));
    strUsage += HelpMessageOpt("-permitbaremultisig", strprintf(_("Relay non-P2SH multisig (default: %u)"), DEFAULT_PERMIT_BAREMULTISIG));
    strUsage += HelpMessageOpt("-peerbloomfilters", strprintf(_("Support filtering of blocks and transaction with bloom filters (default: %u)"), DEFAULT_PEERBLOOMFILTERS));
    strUsage += HelpMessageOpt("-port=<port>", strprintf(_("Listen for connections on <port> (default: %u or testnet: %u)"), DEFAULT_P2P_PORT, DEFAULT_P2P_PORT + 100));
    strUsage += HelpMessageOpt("-proxy=<ip:port>", _("Connect through SOCKS5 proxy"));
    strUsage += HelpMessageOpt("-proxyrandomize", strprintf(_("Randomize credentials for every proxy connection. This enables Tor stream isolation (default: %u)"), DEFAULT_PROXYRANDOMIZE));
    strUsage += HelpMessageOpt("-seednode=<ip>", _("Connect to a node to retrieve peer addresses, and disconnect"));
    strUsage += HelpMessageOpt("-timeout=<n>", strprintf(_("Specify connection timeout in milliseconds (minimum: 1, default: %d)"), DEFAULT_CONNECT_TIMEOUT));
    strUsage += HelpMessageOpt("-torcontrol=<ip>:<port>", strprintf(_("Tor control port to use if onion listening enabled (default: %s)"), DEFAULT_TOR_CONTROL));
    strUsage += HelpMessageOpt("-torpassword=<pass>", _("Tor control port password (default: empty)"));
#ifdef USE_UPNP
#if USE_UPNP
    strUsage += HelpMessageOpt("-upnp", _("Use UPnP to map the listening port (default: 1 when listening and no -proxy)"));
#else
    strUsage += HelpMessageOpt("-upnp", strprintf(_("Use UPnP to map the listening port (default: %u)"), 0));
#endif
#endif
    strUsage += HelpMessageOpt("-whitebind=<addr>", _("Bind to given address and whitelist peers connecting to it. Use [host]:port notation for IPv6"));
    strUsage += HelpMessageOpt("-whitelist=<netmask>", _("Whitelist peers connecting from the given netmask or IP address. Can be specified multiple times.") +
        " " + _("Whitelisted peers cannot be DoS banned and their transactions are always relayed, even if they are already in the mempool, useful e.g. for a gateway"));
    strUsage += HelpMessageOpt("-whitelistrelay", strprintf(_("Accept relayed transactions received from whitelisted peers even when not relaying transactions (default: %d)"), DEFAULT_WHITELISTRELAY));
    strUsage += HelpMessageOpt("-whitelistforcerelay", strprintf(_("Force relay of transactions from whitelisted peers even they violate local relay policy (default: %d)"), DEFAULT_WHITELISTFORCERELAY));
    strUsage += HelpMessageOpt("-maxuploadtarget=<n>", strprintf(_("Tries to keep outbound traffic under the given target (in MiB per 24h), 0 = no limit (default: %d)"), DEFAULT_MAX_UPLOAD_TARGET));

#ifdef ENABLE_WALLET
    strUsage += CWallet::GetWalletHelpString(showDebug);
    if (mode == HMM_DYNAMIC_QT)
        strUsage += HelpMessageOpt("-windowtitle=<name>", _("Wallet window title"));
#endif

#if ENABLE_ZMQ
    strUsage += HelpMessageGroup(_("ZeroMQ notification options:"));
    strUsage += HelpMessageOpt("-zmqpubhashblock=<address>", _("Enable publish hash block in <address>"));
    strUsage += HelpMessageOpt("-zmqpubhashtx=<address>", _("Enable publish hash transaction in <address>"));
    strUsage += HelpMessageOpt("-zmqpubhashtxlock=<address>", _("Enable publish hash transaction (locked via InstantSend) in <address>"));
    strUsage += HelpMessageOpt("-zmqpubrawblock=<address>", _("Enable publish raw block in <address>"));
    strUsage += HelpMessageOpt("-zmqpubrawtx=<address>", _("Enable publish raw transaction in <address>"));
    strUsage += HelpMessageOpt("-zmqpubrawtxlock=<address>", _("Enable publish raw transaction (locked via InstantSend) in <address>"));
#endif

    strUsage += HelpMessageGroup(_("Debugging/Testing options:"));
    strUsage += HelpMessageOpt("-uacomment=<cmt>", _("Append comment to the user agent string"));
    if (showDebug)
    {
        strUsage += HelpMessageOpt("-checkblockindex", strprintf("Do a full consistency check for mapBlockIndex, setBlockIndexCandidates, chainActive and mapBlocksUnlinked occasionally. Also sets -checkmempool (default: %u)", Params(CBaseChainParams::MAIN).DefaultConsistencyChecks()));
        strUsage += HelpMessageOpt("-checkmempool=<n>", strprintf("Run checks every <n> transactions (default: %u)", Params(CBaseChainParams::MAIN).DefaultConsistencyChecks()));
        strUsage += HelpMessageOpt("-checkpoints", strprintf("Disable expensive verification for known chain history (default: %u)", DEFAULT_CHECKPOINTS_ENABLED));
        strUsage += HelpMessageOpt("-disablesafemode", strprintf("Disable safemode, override a real safe mode event (default: %u)", DEFAULT_DISABLE_SAFEMODE));
        strUsage += HelpMessageOpt("-testsafemode", strprintf("Force safe mode (default: %u)", DEFAULT_TESTSAFEMODE));
        strUsage += HelpMessageOpt("-dropmessagestest=<n>", "Randomly drop 1 of every <n> network messages");
        strUsage += HelpMessageOpt("-fuzzmessagestest=<n>", "Randomly fuzz 1 of every <n> network messages");
        strUsage += HelpMessageOpt("-stopafterblockimport", strprintf("Stop running after importing blocks from disk (default: %u)", DEFAULT_STOPAFTERBLOCKIMPORT));
        strUsage += HelpMessageOpt("-limitancestorcount=<n>", strprintf("Do not accept transactions if number of in-mempool ancestors is <n> or more (default: %u)", DEFAULT_ANCESTOR_LIMIT));
        strUsage += HelpMessageOpt("-limitancestorsize=<n>", strprintf("Do not accept transactions whose size with all in-mempool ancestors exceeds <n> kilobytes (default: %u)", DEFAULT_ANCESTOR_SIZE_LIMIT));
        strUsage += HelpMessageOpt("-limitdescendantcount=<n>", strprintf("Do not accept transactions if any ancestor would have <n> or more in-mempool descendants (default: %u)", DEFAULT_DESCENDANT_LIMIT));
        strUsage += HelpMessageOpt("-limitdescendantsize=<n>", strprintf("Do not accept transactions if any ancestor would have more than <n> kilobytes of in-mempool descendants (default: %u).", DEFAULT_DESCENDANT_SIZE_LIMIT));
    }
    std::string debugCategories = "addrman, alert, bench, coindb, db, http, leveldb, libevent, lock, mempool, mempoolrej, net, proxy, prune, rand, reindex, rpc, selectcoins, tor, zmq, "
                             "Dynamic (or specifically: gobject, instantsend, keepass, dynode, dnpayments, dnsync, privatesend, spork)"; // Don't translate these and qt below
    if (mode == HMM_DYNAMIC_QT)
        debugCategories += ", qt";
    strUsage += HelpMessageOpt("-debug=<category>", strprintf(_("Output debugging information (default: %u, supplying <category> is optional)"), 0) + ". " +
        _("If <category> is not supplied or if <category> = 1, output all debugging information.") + _("<category> can be:") + " " + debugCategories + ".");
    if (showDebug)
        strUsage += HelpMessageOpt("-nodebug", "Turn off debugging messages, same as -debug=0");
    strUsage += HelpMessageOpt("-gen", strprintf(_("Generate coins (default: %u)"), DEFAULT_GENERATE));
    strUsage += HelpMessageOpt("-genproclimit=<n>", strprintf(_("Set the number of threads for coin generation if enabled (-1 = all cores, default: %d)"), DEFAULT_GENERATE_THREADS));
    strUsage += HelpMessageOpt("-help-debug", _("Show all debugging options (usage: --help -help-debug)"));
    strUsage += HelpMessageOpt("-logips", strprintf(_("Include IP addresses in debug output (default: %u)"), DEFAULT_LOGIPS));
    strUsage += HelpMessageOpt("-logtimestamps", strprintf(_("Prepend debug output with timestamp (default: %u)"), DEFAULT_LOGTIMESTAMPS));
    if (showDebug)
    {
        strUsage += HelpMessageOpt("-logtimemicros", strprintf("Add microsecond precision to debug timestamps (default: %u)", DEFAULT_LOGTIMEMICROS));
        strUsage += HelpMessageOpt("-logthreadnames", strprintf("Add thread names to debug messages (default: %u)", DEFAULT_LOGTHREADNAMES));
        strUsage += HelpMessageOpt("-mocktime=<n>", "Replace actual time with <n> seconds since epoch (default: 0)");
        strUsage += HelpMessageOpt("-limitfreerelay=<n>", strprintf("Continuously rate-limit free transactions to <n>*1000 bytes per minute (default: %u)", DEFAULT_LIMITFREERELAY));
        strUsage += HelpMessageOpt("-relaypriority", strprintf("Require high priority for relaying free or low-fee transactions (default: %u)", DEFAULT_RELAYPRIORITY));
        strUsage += HelpMessageOpt("-maxsigcachesize=<n>", strprintf("Limit size of signature cache to <n> MiB (default: %u)", DEFAULT_MAX_SIG_CACHE_SIZE));
    }
    strUsage += HelpMessageOpt("-minrelaytxfee=<amt>", strprintf(_("Fees (in %s/kB) smaller than this are considered zero fee for relaying, mining and transaction creation (default: %s)"),
        CURRENCY_UNIT, FormatMoney(DEFAULT_MIN_RELAY_TX_FEE)));
    strUsage += HelpMessageOpt("-maxtxfee=<amt>", strprintf(_("Maximum total fees (in %s) to use in a single wallet transaction or raw transaction; setting this too low may abort large transactions (default: %s)"),
        CURRENCY_UNIT, FormatMoney(DEFAULT_TRANSACTION_MAXFEE)));
    strUsage += HelpMessageOpt("-printtoconsole", _("Send trace/debug info to console instead of debug.log file"));
    strUsage += HelpMessageOpt("-printtodebuglog", strprintf(_("Send trace/debug info to debug.log file (default: %u)"), 1));
    if (showDebug)
    {
        strUsage += HelpMessageOpt("-printpriority", strprintf("Log transaction priority and fee per kB when mining blocks (default: %u)", DEFAULT_PRINTPRIORITY));
    }
    strUsage += HelpMessageOpt("-shrinkdebugfile", _("Shrink debug.log file on client startup (default: 1 when no -debug)"));
    AppendParamsHelpMessages(strUsage, showDebug);
    strUsage += HelpMessageOpt("-litemode=<n>", strprintf(_("Disable all Dynamic specific functionality (Dynodes, PrivateSend, InstantSend, Governance) (0-1, default: %u)"), 0));

    strUsage += HelpMessageGroup(_("Dynode options:"));
    strUsage += HelpMessageOpt("-dynode=<n>", strprintf(_("Enable the client to act as a Dynode (0-1, default: %u)"), 0));
    strUsage += HelpMessageOpt("-dnconf=<file>", strprintf(_("Specify Dynode configuration file (default: %s)"), "dynode.conf"));
    strUsage += HelpMessageOpt("-dnconflock=<n>", strprintf(_("Lock Dynodes from Dynode configuration file (default: %u)"), 1));
    strUsage += HelpMessageOpt("-dynodepairingkey=<n>", _("Set the Dynode private key"));

#ifdef ENABLE_WALLET
    strUsage += HelpMessageGroup(_("PrivateSend options:"));
    strUsage += HelpMessageOpt("-enableprivatesend=<n>", strprintf(_("Enable use of automated PrivateSend for funds stored in this wallet (0-1, default: %u)"), 0));
    strUsage += HelpMessageOpt("-privatesendmultisession=<n>", strprintf(_("Enable multiple PrivateSend mixing sessions per block, experimental (0-1, default: %u)"), DEFAULT_PRIVATESEND_MULTISESSION));
    strUsage += HelpMessageOpt("-privatesendrounds=<n>", strprintf(_("Use N separate Dynodes for each denominated input to mix funds (2-16, default: %u)"), DEFAULT_PRIVATESEND_ROUNDS));
    strUsage += HelpMessageOpt("-privatesendamount=<n>", strprintf(_("Keep N DYN anonymized (default: %u)"), DEFAULT_PRIVATESEND_AMOUNT));
    strUsage += HelpMessageOpt("-liquidityprovider=<n>", strprintf(_("Provide liquidity to PrivateSend by infrequently mixing coins on a continual basis (0-100, default: %u, 1=very frequent, high fees, 100=very infrequent, low fees)"), DEFAULT_PRIVATESEND_LIQUIDITY));
#endif // ENABLE_WALLET

    strUsage += HelpMessageGroup(_("InstantSend options:"));
    strUsage += HelpMessageOpt("-enableinstantsend=<n>", strprintf(_("Enable InstantSend, show confirmations for locked transactions (0-1, default: %u)"), 1));
    strUsage += HelpMessageOpt("-instantsenddepth=<n>", strprintf(_("Show N confirmations for a successfully locked transaction (0-9999, default: %u)"), DEFAULT_INSTANTSEND_DEPTH));
    strUsage += HelpMessageOpt("-instantsendnotify=<cmd>", _("Execute command when a wallet InstantSend transaction is successfully locked (%s in cmd is replaced by TxID)"));


    strUsage += HelpMessageGroup(_("Node relay options:"));
    if (showDebug)
        strUsage += HelpMessageOpt("-acceptnonstdtxn", strprintf("Relay and mine \"non-standard\" transactions (%sdefault: %u)", "testnet/regtest only; ", !Params(CBaseChainParams::TESTNET).RequireStandard()));
    strUsage += HelpMessageOpt("-bytespersigop", strprintf(_("Minimum bytes per sigop in transactions we relay and mine (default: %u)"), DEFAULT_BYTES_PER_SIGOP));
    strUsage += HelpMessageOpt("-datacarrier", strprintf(_("Relay and mine data carrier transactions (default: %u)"), DEFAULT_ACCEPT_DATACARRIER));
    strUsage += HelpMessageOpt("-datacarriersize", strprintf(_("Maximum size of data in data carrier transactions we relay and mine (default: %u)"), MAX_OP_RETURN_RELAY));
    strUsage += HelpMessageOpt("-mempoolreplacement", strprintf(_("Enable transaction replacement in the memory pool (default: %u)"), DEFAULT_ENABLE_REPLACEMENT));

    strUsage += HelpMessageGroup(_("Block creation options:"));
    strUsage += HelpMessageOpt("-blockminsize=<n>", strprintf(_("Set minimum block size in bytes (default: %u)"), DEFAULT_BLOCK_MIN_SIZE));
    strUsage += HelpMessageOpt("-blockmaxsize=<n>", strprintf(_("Set maximum block size in bytes (default: %d)"), DEFAULT_BLOCK_MAX_SIZE));
    strUsage += HelpMessageOpt("-blockprioritysize=<n>", strprintf(_("Set maximum size of high-priority/low-fee transactions in bytes (default: %d)"), DEFAULT_BLOCK_PRIORITY_SIZE));
    if (showDebug)
        strUsage += HelpMessageOpt("-blockversion=<n>", "Override block version to test forking scenarios");

    strUsage += HelpMessageGroup(_("RPC server options:"));
    strUsage += HelpMessageOpt("-server", _("Accept command line and JSON-RPC commands"));
    strUsage += HelpMessageOpt("-rest", strprintf(_("Accept public REST requests (default: %u)"), DEFAULT_REST_ENABLE));
    strUsage += HelpMessageOpt("-rpcbind=<addr>", _("Bind to given address to listen for JSON-RPC connections. Use [host]:port notation for IPv6. This option can be specified multiple times (default: bind to all interfaces)"));
    strUsage += HelpMessageOpt("-rpccookiefile=<loc>", _("Location of the auth cookie (default: data dir)"));
    strUsage += HelpMessageOpt("-rpcuser=<user>", _("Username for JSON-RPC connections"));
    strUsage += HelpMessageOpt("-rpcpassword=<pw>", _("Password for JSON-RPC connections"));
    strUsage += HelpMessageOpt("-rpcauth=<userpw>", _("Username and hashed password for JSON-RPC connections. The field <userpw> comes in the format: <USERNAME>:<SALT>$<HASH>. A canonical python script is included in share/rpcuser. This option can be specified multiple times"));
    strUsage += HelpMessageOpt("-rpcport=<port>", strprintf(_("Listen for JSON-RPC connections on <port> (default: %u or testnet: %u)"), BaseParams(CBaseChainParams::MAIN).RPCPort(), BaseParams(CBaseChainParams::TESTNET).RPCPort()));
    strUsage += HelpMessageOpt("-rpcallowip=<ip>", _("Allow JSON-RPC connections from specified source. Valid for <ip> are a single IP (e.g. 1.2.3.4), a network/netmask (e.g. 1.2.3.4/255.255.255.0) or a network/CIDR (e.g. 1.2.3.4/24). This option can be specified multiple times"));
    strUsage += HelpMessageOpt("-rpcthreads=<n>", strprintf(_("Set the number of threads to service RPC calls (default: %d)"), DEFAULT_HTTP_THREADS));
    if (showDebug) {
        strUsage += HelpMessageOpt("-rpcworkqueue=<n>", strprintf("Set the depth of the work queue to service RPC calls (default: %d)", DEFAULT_HTTP_WORKQUEUE));
        strUsage += HelpMessageOpt("-rpcservertimeout=<n>", strprintf("Timeout during HTTP requests (default: %d)", DEFAULT_HTTP_SERVER_TIMEOUT));
    }

    return strUsage;
}

std::string LicenseInfo()
{
    return FormatParagraph(strprintf(_("Copyright (C) 2016-%i Duality Blockchain Solutions Developers"), COPYRIGHT_YEAR)) + "\n" +
           "\n" +
           FormatParagraph(strprintf(_("Copyright (C) 2009-%i The Bitcoin Core Developers"), COPYRIGHT_YEAR)) + "\n" +
           "\n" +
           FormatParagraph(strprintf(_("Copyright (C) 2014-%i The Dash Developers"), COPYRIGHT_YEAR)) + "\n" +
           "\n" +
           FormatParagraph(_("This is experimental software.")) + "\n" +
           "\n" +
           FormatParagraph(_("Distributed under the MIT software license, see the accompanying file COPYING or <http://www.opensource.org/licenses/mit-license.php>.")) + "\n" +
           "\n" +
           FormatParagraph(_("This product includes software developed by the OpenSSL Project for use in the OpenSSL Toolkit <https://www.openssl.org/> and cryptographic software written by Eric Young and UPnP software written by Thomas Bernard.")) +
           "\n";
}

static void BlockNotifyCallback(bool initialSync, const CBlockIndex *pBlockIndex)
{
    if (initialSync || !pBlockIndex)
        return;

    std::string strCmd = GetArg("-blocknotify", "");

    boost::replace_all(strCmd, "%s", pBlockIndex->GetBlockHash().GetHex());
    boost::thread t(runCommand, strCmd); // thread runs free
}

static bool fHaveGenesis = false;
static boost::mutex cs_GenesisWait;
static CConditionVariable condvar_GenesisWait;

static void BlockNotifyGenesisWait(bool, const CBlockIndex *pBlockIndex)
{
    if (pBlockIndex != NULL) {
        {
            boost::unique_lock<boost::mutex> lock_GenesisWait(cs_GenesisWait);
            fHaveGenesis = true;
        }
        condvar_GenesisWait.notify_all();
    }
}

struct CImportingNow
{
    CImportingNow() {
        assert(fImporting == false);
        fImporting = true;
    }

    ~CImportingNow() {
        assert(fImporting == true);
        fImporting = false;
    }
};


// If we're using -prune with -reindex, then delete block files that will be ignored by the
// reindex.  Since reindexing works by starting at block file 0 and looping until a blockfile
// is missing, do the same here to delete any later block files after a gap.  Also delete all
// rev files since they'll be rewritten by the reindex anyway.  This ensures that vinfoBlockFile
// is in sync with what's actually on disk by the time we start downloading, so that pruning
// works correctly.
void CleanupBlockRevFiles()
{
    using namespace boost::filesystem;
    std::map<std::string, path> mapBlockFiles;

    // Glob all blk?????.dat and rev?????.dat files from the blocks directory.
    // Remove the rev files immediately and insert the blk file paths into an
    // ordered map keyed by block file index.
    LogPrintf("Removing unusable blk?????.dat and rev?????.dat files for -reindex with -prune\n");
    path blocksdir = GetDataDir() / "blocks";
    for (directory_iterator it(blocksdir); it != directory_iterator(); it++) {
        if (is_regular_file(*it) &&
            it->path().filename().string().length() == 12 &&
            it->path().filename().string().substr(8,4) == ".dat")
        {
            if (it->path().filename().string().substr(0,3) == "blk")
                mapBlockFiles[it->path().filename().string().substr(3,5)] = it->path();
            else if (it->path().filename().string().substr(0,3) == "rev")
                remove(it->path());
        }
    }

    // Remove all block files that aren't part of a contiguous set starting at
    // zero by walking the ordered map (keys are block file indices) by
    // keeping a separate counter.  Once we hit a gap (or if 0 doesn't exist)
    // start removing block files.
    int nContigCounter = 0;
    BOOST_FOREACH(const PAIRTYPE(std::string, path)& item, mapBlockFiles) {
        if (atoi(item.first) == nContigCounter) {
            nContigCounter++;
            continue;
        }
        remove(item.second);
    }
}

void ThreadImport(std::vector<boost::filesystem::path> vImportFiles)
{
    const CChainParams& chainparams = Params();
    RenameThread("dynamic-loadblk");

    {
    CImportingNow imp;

    // -reindex
    if (fReindex) {
        int nFile = 0;
        while (true) {
            CDiskBlockPos pos(nFile, 0);
            if (!boost::filesystem::exists(GetBlockPosFilename(pos, "blk")))
                break; // No block files left to reindex
            FILE *file = OpenBlockFile(pos, true);
            if (!file)
                break; // This error is logged in OpenBlockFile
            LogPrintf("Reindexing block file blk%05u.dat...\n", (unsigned int)nFile);
            LoadExternalBlockFile(chainparams, file, &pos);
            nFile++;
        }
        pblocktree->WriteReindexing(false);
        fReindex = false;
        LogPrintf("Reindexing finished\n");
        // To avoid ending up in a situation without genesis block, re-try initializing (no-op if reindexing worked):
        InitBlockIndex(chainparams);
    }

    // hardcoded $DATADIR/bootstrap.dat
    boost::filesystem::path pathBootstrap = GetDataDir() / "bootstrap.dat";
    if (boost::filesystem::exists(pathBootstrap)) {
        FILE *file = fopen(pathBootstrap.string().c_str(), "rb");
        if (file) {
            boost::filesystem::path pathBootstrapOld = GetDataDir() / "bootstrap.dat.old";
            LogPrintf("Importing bootstrap.dat...\n");
            LoadExternalBlockFile(chainparams, file);
            RenameOver(pathBootstrap, pathBootstrapOld);
        } else {
            LogPrintf("Warning: Could not open bootstrap file %s\n", pathBootstrap.string());
        }
    }

    // -loadblock=
    BOOST_FOREACH(const boost::filesystem::path& path, vImportFiles) {
        FILE *file = fopen(path.string().c_str(), "rb");
        if (file) {
            LogPrintf("Importing blocks file %s...\n", path.string());
            LoadExternalBlockFile(chainparams, file);
        } else {
            LogPrintf("Warning: Could not open blocks file %s\n", path.string());
        }
    }

    // scan for better chains in the block chain database, that are not yet connected in the active best chain
    CValidationState state;
    if (!ActivateBestChain(state, chainparams)) {
        LogPrintf("Failed to connect best block");
        StartShutdown();
    }

    if (GetBoolArg("-stopafterblockimport", DEFAULT_STOPAFTERBLOCKIMPORT)) {
        LogPrintf("Stopping after block import\n");
        StartShutdown();
    }
    } // End scope of CImportingNow
    LoadMempool();
}

/** Sanity checks
 *  Ensure that Dynamic is running in a usable environment with all
 *  necessary library support.
 */
bool InitSanityCheck(void)
{
    if(!ECC_InitSanityCheck()) {
        InitError("Elliptic curve cryptography sanity check failure. Aborting.");
        return false;
    }
    if (!glibc_sanity_test() || !glibcxx_sanity_test())
        return false;

    return true;
}

bool AppInitServers(boost::thread_group& threadGroup)
{
    RPCServer::OnStopped(&OnRPCStopped);
    RPCServer::OnPreCommand(&OnRPCPreCommand);
    if (!InitHTTPServer())
        return false;
    if (!StartRPC())
        return false;
    if (!StartHTTPRPC())
        return false;
    if (GetBoolArg("-rest", DEFAULT_REST_ENABLE) && !StartREST())
        return false;
    if (!StartHTTPServer())
        return false;
    return true;
}

// Parameter interaction based on rules
void InitParameterInteraction()
{
    // when specifying an explicit binding address, you want to listen on it
    // even when -connect or -proxy is specified
    if (mapArgs.count("-bind")) {
        if (SoftSetBoolArg("-listen", true))
            LogPrintf("%s: parameter interaction: -bind set -> setting -listen=1\n", __func__);
    }
    if (mapArgs.count("-whitebind")) {
        if (SoftSetBoolArg("-listen", true))
            LogPrintf("%s: parameter interaction: -whitebind set -> setting -listen=1\n", __func__);
    }

    if (GetBoolArg("-dynode", false)) {
        // Dynodes must accept connections from outside
        if (SoftSetBoolArg("-listen", true))
            LogPrintf("%s: parameter interaction: -dynode=1 -> setting -listen=1\n", __func__);
    }

    if (mapArgs.count("-connect") && mapMultiArgs["-connect"].size() > 0) {
        // when only connecting to trusted nodes, do not seed via DNS, or listen by default
        if (SoftSetBoolArg("-dnsseed", false))
            LogPrintf("%s: parameter interaction: -connect set -> setting -dnsseed=0\n", __func__);
        if (SoftSetBoolArg("-listen", false))
            LogPrintf("%s: parameter interaction: -connect set -> setting -listen=0\n", __func__);
    }

    if (mapArgs.count("-proxy")) {
        // to protect privacy, do not listen by default if a default proxy server is specified
        if (SoftSetBoolArg("-listen", false))
            LogPrintf("%s: parameter interaction: -proxy set -> setting -listen=0\n", __func__);
        // to protect privacy, do not use UPNP when a proxy is set. The user may still specify -listen=1
        // to listen locally, so don't rely on this happening through -listen below.
        if (SoftSetBoolArg("-upnp", false))
            LogPrintf("%s: parameter interaction: -proxy set -> setting -upnp=0\n", __func__);
        // to protect privacy, do not discover addresses by default
        if (SoftSetBoolArg("-discover", false))
            LogPrintf("%s: parameter interaction: -proxy set -> setting -discover=0\n", __func__);
    }

    if (!GetBoolArg("-listen", DEFAULT_LISTEN)) {
        // do not map ports or try to retrieve public IP when not listening (pointless)
        if (SoftSetBoolArg("-upnp", false))
            LogPrintf("%s: parameter interaction: -listen=0 -> setting -upnp=0\n", __func__);
        if (SoftSetBoolArg("-discover", false))
            LogPrintf("%s: parameter interaction: -listen=0 -> setting -discover=0\n", __func__);
        if (SoftSetBoolArg("-listenonion", false))
            LogPrintf("%s: parameter interaction: -listen=0 -> setting -listenonion=0\n", __func__);
    }

    if (mapArgs.count("-externalip")) {
        // if an explicit public IP is specified, do not try to find others
        if (SoftSetBoolArg("-discover", false))
            LogPrintf("%s: parameter interaction: -externalip set -> setting -discover=0\n", __func__);
    }

    // disable whitelistrelay in blocksonly mode
    if (GetBoolArg("-whitelistforcerelay", DEFAULT_WHITELISTFORCERELAY)) {
        if (SoftSetBoolArg("-whitelistrelay", true))
            LogPrintf("%s: parameter interaction: -whitelistforcerelay=1 -> setting -whitelistrelay=1\n", __func__);
    }

    if(!GetBoolArg("-enableinstantsend", fEnableInstantSend)){
        if (SoftSetArg("-instantsenddepth", 0))
            LogPrintf("%s: parameter interaction: -enableinstantsend=false -> setting -nInstantSendDepth=0\n", __func__);
    }

#ifdef ENABLE_WALLET
    int nLiqProvTmp = GetArg("-liquidityprovider", DEFAULT_PRIVATESEND_LIQUIDITY);
    if (nLiqProvTmp > 0) {
        mapArgs["-enableprivatesend"] = "1";
        LogPrintf("%s: parameter interaction: -liquidityprovider=%d -> setting -enableprivatesend=1\n", __func__, nLiqProvTmp);
        mapArgs["-privatesendamount"] = "999999";
        LogPrintf("%s: parameter interaction: -liquidityprovider=%d -> setting -privatesendamount=999999\n", __func__, nLiqProvTmp);
        mapArgs["-privatesendmultisession"] = "0";
        LogPrintf("%s: parameter interaction: -liquidityprovider=%d -> setting -privatesendmultisession=0\n", __func__, nLiqProvTmp);
    }
    if (mapArgs.count("-hdseed") && IsHex(GetArg("-hdseed", "not hex")) && (mapArgs.count("-mnemonic") || mapArgs.count("-mnemonicpassphrase"))) {
        mapArgs.erase("-mnemonic");
        mapArgs.erase("-mnemonicpassphrase");
        LogPrintf("%s: parameter interaction: can't use -hdseed and -mnemonic/-mnemonicpassphrase together, will prefer -seed\n", __func__);
    }
#endif // ENABLE_WALLET
    // Make sure additional indexes are recalculated correctly in VerifyDB
    // (we must reconnect blocks whenever we disconnect them for these indexes to work)
    bool fAdditionalIndexes =
         GetBoolArg("-addressindex", DEFAULT_ADDRESSINDEX) ||
        GetBoolArg("-spentindex", DEFAULT_SPENTINDEX) ||
        GetBoolArg("-timestampindex", DEFAULT_TIMESTAMPINDEX);

    if (fAdditionalIndexes && GetArg("-checklevel", DEFAULT_CHECKLEVEL) < 4) {
        mapArgs["-checklevel"] = "4";
        LogPrintf("%s: parameter interaction: additional indexes -> setting -checklevel=4\n", __func__);
    }
}

static std::string ResolveErrMsg(const char * const optname, const std::string& strBind)
{
    return strprintf(_("Cannot resolve -%s address: '%s'"), optname, strBind);
}

void InitLogging()
{
    fPrintToConsole = GetBoolArg("-printtoconsole", false);
    fPrintToDebugLog = GetBoolArg("-printtodebuglog", true) && !fPrintToConsole;
    fLogTimestamps = GetBoolArg("-logtimestamps", DEFAULT_LOGTIMESTAMPS);
    fLogTimeMicros = GetBoolArg("-logtimemicros", DEFAULT_LOGTIMEMICROS);
    fLogThreadNames = GetBoolArg("-logthreadnames", DEFAULT_LOGTHREADNAMES);
    fLogIPs = GetBoolArg("-logips", DEFAULT_LOGIPS);

    LogPrintf("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
    LogPrintf("Dynamic version %s\n", FormatFullVersion());
}

/** Initialize Dynamic.
 *  @pre Parameters should be parsed and config file should be read.
 */
bool AppInit2(boost::thread_group& threadGroup, CScheduler& scheduler)
{
    // ********************************************************* Step 1: setup
#ifdef _MSC_VER
    // Turn off Microsoft heap dump noise
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, CreateFileA("NUL", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, 0));
#endif
#if _MSC_VER >= 1400
    // Disable confusing "helpful" text message on abort, Ctrl-C
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#ifdef WIN32
    // Enable Data Execution Prevention (DEP)
    // Minimum supported OS versions: WinXP SP3, WinVista >= SP1, Win Server 2008
    // A failure is non-critical and needs no further attention!
#ifndef PROCESS_DEP_ENABLE
    // We define this here, because GCCs winbase.h limits this to _WIN32_WINNT >= 0x0601 (Windows 7),
    // which is not correct. Can be removed, when GCCs winbase.h is fixed!
#define PROCESS_DEP_ENABLE 0x00000001
#endif
    typedef BOOL (WINAPI *PSETPROCDEPPOL)(DWORD);
    PSETPROCDEPPOL setProcDEPPol = (PSETPROCDEPPOL)GetProcAddress(GetModuleHandleA("Kernel32.dll"), "SetProcessDEPPolicy");
    if (setProcDEPPol != NULL) setProcDEPPol(PROCESS_DEP_ENABLE);
#endif

    if (!SetupNetworking())
        return InitError("Initializing networking failed");

#ifndef WIN32
    if (!GetBoolArg("-sysperms", false)) {
        umask(077);
    }

    // Clean shutdown on SIGTERM
    struct sigaction sa;
    sa.sa_handler = HandleSIGTERM;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    // Reopen debug.log on SIGHUP
    struct sigaction sa_hup;
    sa_hup.sa_handler = HandleSIGHUP;
    sigemptyset(&sa_hup.sa_mask);
    sa_hup.sa_flags = 0;
    sigaction(SIGHUP, &sa_hup, NULL);

    // Ignore SIGPIPE, otherwise it will bring the daemon down if the client closes unexpectedly
    signal(SIGPIPE, SIG_IGN);
#endif

    // ********************************************************* Step 2: parameter interactions
    const CChainParams& chainparams = Params();

    // also see: InitParameterInteraction()

    // if using block pruning, then disallow txindex
    if (GetArg("-prune", 0)) {
        if (GetBoolArg("-txindex", DEFAULT_TXINDEX))
            return InitError(_("Prune mode is incompatible with -txindex."));
    }

    // Make sure enough file descriptors are available
    int nBind = std::max(
                (mapMultiArgs.count("-bind") ? mapMultiArgs.at("-bind").size() : 0) +
                (mapMultiArgs.count("-whitebind") ? mapMultiArgs.at("-whitebind").size() : 0), size_t(1));
    int nUserMaxConnections = GetArg("-maxconnections", DEFAULT_MAX_PEER_CONNECTIONS);
    int nMaxConnections = std::max(nUserMaxConnections, 0);

    // Trim requested connection counts, to fit into system limitations
    nMaxConnections = std::max(std::min(nMaxConnections, (int)(FD_SETSIZE - nBind - MIN_CORE_FILEDESCRIPTORS)), 0);
    int nFD = RaiseFileDescriptorLimit(nMaxConnections + MIN_CORE_FILEDESCRIPTORS);
    if (nFD < MIN_CORE_FILEDESCRIPTORS)
        return InitError(_("Not enough file descriptors available."));
    nMaxConnections = std::min(nFD - MIN_CORE_FILEDESCRIPTORS, nMaxConnections);

    if (nMaxConnections < nUserMaxConnections)
        InitWarning(strprintf(_("Reducing -maxconnections from %d to %d, because of system limitations."), nUserMaxConnections, nMaxConnections));

    // ********************************************************* Step 3: parameter-to-internal-flags

    fDebug = !mapMultiArgs["-debug"].empty();
    // Special-case: if -debug=0/-nodebug is set, turn off debugging messages
    const std::vector<std::string>& categories = mapMultiArgs["-debug"];
    if (GetBoolArg("-nodebug", false) || find(categories.begin(), categories.end(), std::string("0")) != categories.end())
        fDebug = false;

    // Check for -debugnet
    if (GetBoolArg("-debugnet", false))
        InitWarning(_("Unsupported argument -debugnet ignored, use -debug=net."));
    // Check for -socks - as this is a privacy risk to continue, exit here
    if (mapArgs.count("-socks"))
        return InitError(_("Unsupported argument -socks found. Setting SOCKS version isn't possible anymore, only SOCKS5 proxies are supported."));
    // Check for -tor - as this is a privacy risk to continue, exit here
    if (GetBoolArg("-tor", false))
        return InitError(_("Unsupported argument -tor found, use -onion."));

    if (GetBoolArg("-benchmark", false))
        InitWarning(_("Unsupported argument -benchmark ignored, use -debug=bench."));

    if (GetBoolArg("-whitelistalwaysrelay", false))
        InitWarning(_("Unsupported argument -whitelistalwaysrelay ignored, use -whitelistrelay and/or -whitelistforcerelay."));

    // Checkmempool and checkblockindex default to true in regtest mode
    int ratio = std::min<int>(std::max<int>(GetArg("-checkmempool", chainparams.DefaultConsistencyChecks() ? 1 : 0), 0), 1000000);
    if (ratio != 0) {
        mempool.setSanityCheck(1.0 / ratio);
    }
    fCheckBlockIndex = GetBoolArg("-checkblockindex", chainparams.DefaultConsistencyChecks());
    fCheckpointsEnabled = GetBoolArg("-checkpoints", DEFAULT_CHECKPOINTS_ENABLED);

    // mempool limits
    int64_t nMempoolSizeMax = GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000;
    int64_t nMempoolSizeMin = GetArg("-limitdescendantsize", DEFAULT_DESCENDANT_SIZE_LIMIT) * 1000 * 40;
    if (nMempoolSizeMax < 0 || nMempoolSizeMax < nMempoolSizeMin)
        return InitError(strprintf(_("-maxmempool must be at least %d MB"), std::ceil(nMempoolSizeMin / 1000000.0)));

    // -par=0 means autodetect, but nScriptCheckThreads==0 means no concurrency
    nScriptCheckThreads = GetArg("-par", DEFAULT_SCRIPTCHECK_THREADS);
    if (nScriptCheckThreads <= 0)
        nScriptCheckThreads += GetNumCores();
    if (nScriptCheckThreads <= 1)
        nScriptCheckThreads = 0;
    else if (nScriptCheckThreads > MAX_SCRIPTCHECK_THREADS)
        nScriptCheckThreads = MAX_SCRIPTCHECK_THREADS;

    fServer = GetBoolArg("-server", false);

    // block pruning; get the amount of disk space (in MiB) to allot for block & undo files
    int64_t nSignedPruneTarget = GetArg("-prune", 0) * 1024 * 1024;
    if (nSignedPruneTarget < 0) {
        return InitError(_("Prune cannot be configured with a negative value."));
    }
    nPruneTarget = (uint64_t) nSignedPruneTarget;
    if (nPruneTarget) {
        if (nPruneTarget < MIN_DISK_SPACE_FOR_BLOCK_FILES) {
            return InitError(strprintf(_("Prune configured below the minimum of %d MiB.  Please use a higher number."), MIN_DISK_SPACE_FOR_BLOCK_FILES / 1024 / 1024));
        }
        LogPrintf("Prune configured to target %uMiB on disk for block and undo files.\n", nPruneTarget / 1024 / 1024);
        fPruneMode = true;
    }

    RegisterAllCoreRPCCommands(tableRPC);
#ifdef ENABLE_WALLET
    RegisterWalletRPCCommands(tableRPC);
#endif

    nConnectTimeout = GetArg("-timeout", DEFAULT_CONNECT_TIMEOUT);
    if (nConnectTimeout <= 0)
        nConnectTimeout = DEFAULT_CONNECT_TIMEOUT;

    // Fee-per-kilobyte amount considered the same as "free"
    // If you are mining, be careful setting this:
    // if you set it to zero then
    // a transaction spammer can cheaply fill blocks using
    // 1-satoshi-fee transactions. It should be set above the real
    // cost to you of processing a transaction.
    if (mapArgs.count("-minrelaytxfee"))
    {
        CAmount n = 0;
        if (!ParseMoney(mapArgs["-minrelaytxfee"], n))
            return InitError(AmountErrMsg("minrelaytxfee", mapArgs["-minrelaytxfee"]));
        // High fee check is done afterward in CWallet::ParameterInteraction()
        ::minRelayTxFee = CFeeRate(n);
    }

    fRequireStandard = !GetBoolArg("-acceptnonstdtxn", !Params().RequireStandard());
    if (Params().RequireStandard() && !fRequireStandard)
        return InitError(strprintf("acceptnonstdtxn is not currently supported for %s chain", chainparams.NetworkIDString()));
    nBytesPerSigOp = GetArg("-bytespersigop", nBytesPerSigOp);

#ifdef ENABLE_WALLET
    if (!CWallet::ParameterInteraction())
        return false;
#endif // ENABLE_WALLET

    fIsBareMultisigStd = GetBoolArg("-permitbaremultisig", DEFAULT_PERMIT_BAREMULTISIG);
    fAcceptDatacarrier = GetBoolArg("-datacarrier", DEFAULT_ACCEPT_DATACARRIER);
    nMaxDatacarrierBytes = GetArg("-datacarriersize", nMaxDatacarrierBytes);

    fAlerts = GetBoolArg("-alerts", DEFAULT_ALERTS);

    // Option to startup with mocktime set (used for regression testing):
    SetMockTime(GetArg("-mocktime", 0)); // SetMockTime(0) is a no-op

    ServiceFlags nLocalServices = NODE_NETWORK;
    ServiceFlags nRelevantServices = NODE_NETWORK;

    if (GetBoolArg("-peerbloomfilters", DEFAULT_PEERBLOOMFILTERS))
        nLocalServices = ServiceFlags(nLocalServices | NODE_BLOOM);

    fEnableReplacement = GetBoolArg("-mempoolreplacement", DEFAULT_ENABLE_REPLACEMENT);
    if ((!fEnableReplacement) && mapArgs.count("-mempoolreplacement")) {
        // Minimal effort at forwards compatibility
        std::string strReplacementModeList = GetArg("-mempoolreplacement", "");  // default is impossible
        std::vector<std::string> vstrReplacementModes;
        boost::split(vstrReplacementModes, strReplacementModeList, boost::is_any_of(","));
        fEnableReplacement = (std::find(vstrReplacementModes.begin(), vstrReplacementModes.end(), "fee") != vstrReplacementModes.end());
    }

    // ********************************************************* Step 4: application initialization: dir lock, daemonize, pidfile, debug log, seed insecure_rand()

    // Initialize elliptic curve code
    ECC_Start();
    globalVerifyHandle.reset(new ECCVerifyHandle());

    // Sanity check
    if (!InitSanityCheck())
        return InitError(_("Initialization sanity check failed. Dynamic is shutting down."));

    std::string strDataDir = GetDataDir().string();

    // Make sure only a single Dynamic process is using the data directory.
    boost::filesystem::path pathLockFile = GetDataDir() / ".lock";
    FILE* file = fopen(pathLockFile.string().c_str(), "a"); // empty lock file; created if it doesn't exist.
    if (file) fclose(file);

    try {
        static boost::interprocess::file_lock lock(pathLockFile.string().c_str());
        // Wait maximum 10 seconds if an old wallet is still running. Avoids lockup during restart
        if (!lock.timed_lock(boost::get_system_time() + boost::posix_time::seconds(10)))
            return InitError(strprintf(_("Cannot obtain a lock on data directory %s. Dynamic is probably already running."), strDataDir));
    } catch(const boost::interprocess::interprocess_exception& e) {
        return InitError(strprintf(_("Cannot obtain a lock on data directory %s. Dynamic is probably already running.") + " %s.", strDataDir, e.what()));
    }

#ifndef WIN32
    CreatePidFile(GetPidFile(), getpid());
#endif
    if (GetBoolArg("-shrinkdebugfile", !fDebug))
        ShrinkDebugFile();

    if (fPrintToDebugLog)
        OpenDebugLog();

#ifdef ENABLE_WALLET
    LogPrintf("Using BerkeleyDB version %s\n", DbEnv::version(0, 0, 0));
#endif
    if (!fLogTimestamps)
        LogPrintf("Startup time: %s\n", DateTimeStrFormat("%Y-%m-%d %H:%M:%S", GetTime()));
    LogPrintf("Default data directory %s\n", GetDefaultDataDir().string());
    LogPrintf("Using data directory %s\n", strDataDir);
    LogPrintf("Using config file %s\n", GetConfigFile(GetArg("-conf", DYNAMIC_CONF_FILENAME)).string());
    LogPrintf("Using at most %i connections (%i file descriptors available)\n", nMaxConnections, nFD);
    std::ostringstream strErrors;

    LogPrintf("Using %u threads for script verification\n", nScriptCheckThreads);
    if (nScriptCheckThreads) {
        for (int i=0; i<nScriptCheckThreads-1; i++)
            threadGroup.create_thread(&ThreadScriptCheck);
    }
    if (mapArgs.count("-sporkkey")) // spork priv key
    {
        if (!sporkManager.SetPrivKey(GetArg("-sporkkey", "")))
            return InitError(_("Unable to sign spork message, wrong key?"));
    }

    // Start the lightweight task scheduler thread
    CScheduler::Function serviceLoop = boost::bind(&CScheduler::serviceQueue, &scheduler);
    threadGroup.create_thread(boost::bind(&TraceThread<CScheduler::Function>, "scheduler", serviceLoop));

    /* Start the RPC server already.  It will be started in "warmup" mode
     * and not really process calls already (but it will signify connections
     * that the server is there and will be ready later).  Warmup mode will
     * be disabled when initialisation is finished.
     */
    if (fServer)
    {
        uiInterface.InitMessage.connect(SetRPCWarmupStatus);
        if (!AppInitServers(threadGroup))
            return InitError(_("Unable to start HTTP server. See debug log for details."));
    }

    int64_t nStart;

    // ********************************************************* Step 5: Backup wallet and verify wallet database integrity
#ifdef ENABLE_WALLET
    if (!CWallet::InitAutoBackup())
        return false;

    if (!CWallet::Verify())
        return false;

    // Initialize KeePass Integration
    keePassInt.init();
#endif // ENABLE_WALLET
    // ********************************************************* Step 6: network initialization

    assert(!g_connman);
    g_connman = std::unique_ptr<CConnman>(new CConnman(GetRand(std::numeric_limits<uint64_t>::max()), GetRand(std::numeric_limits<uint64_t>::max())));
    CConnman& connman = *g_connman;

    peerLogic.reset(new PeerLogicValidation(&connman));
    RegisterValidationInterface(peerLogic.get());
    RegisterNodeSignals(GetNodeSignals());

    // sanitize comments per BIP-0014, format user agent and check total size
    std::vector<std::string> uacomments;
    BOOST_FOREACH(std::string cmt, mapMultiArgs["-uacomment"])
    {
        if (cmt != SanitizeString(cmt, SAFE_CHARS_UA_COMMENT))
            return InitError(strprintf(_("User Agent comment (%s) contains unsafe characters."), cmt));
        uacomments.push_back(SanitizeString(cmt, SAFE_CHARS_UA_COMMENT));
    }
    strSubVersion = FormatSubVersion(CLIENT_NAME, CLIENT_VERSION, uacomments);
    if (strSubVersion.size() > MAX_SUBVERSION_LENGTH) {
        return InitError(strprintf(_("Total length of network version string (%i) exceeds maximum length (%i). Reduce the number or size of uacomments."),
            strSubVersion.size(), MAX_SUBVERSION_LENGTH));
    }

    if (mapArgs.count("-onlynet")) {
        std::set<enum Network> nets;
        BOOST_FOREACH(const std::string& snet, mapMultiArgs["-onlynet"]) {
            enum Network net = ParseNetwork(snet);
            if (net == NET_UNROUTABLE)
                return InitError(strprintf(_("Unknown network specified in -onlynet: '%s'"), snet));
            nets.insert(net);
        }
        for (int n = 0; n < NET_MAX; n++) {
            enum Network net = (enum Network)n;
            if (!nets.count(net))
                SetLimited(net);
        }
    }

    if (mapArgs.count("-whitelist")) {
        BOOST_FOREACH(const std::string& net, mapMultiArgs["-whitelist"]) {
            CSubNet subnet;
            LookupSubNet(net.c_str(), subnet);
            if (!subnet.IsValid())
                return InitError(strprintf(_("Invalid netmask specified in -whitelist: '%s'"), net));
            connman.AddWhitelistedRange(subnet);
        }
    }

    bool proxyRandomize = GetBoolArg("-proxyrandomize", DEFAULT_PROXYRANDOMIZE);
    // -proxy sets a proxy for all outgoing network traffic
    // -noproxy (or -proxy=0) as well as the empty string can be used to not set a proxy, this is the default
    std::string proxyArg = GetArg("-proxy", "");
    SetLimited(NET_TOR);
    if (proxyArg != "" && proxyArg != "0") {
        CService resolved(LookupNumeric(proxyArg.c_str(), 9050));
        proxyType addrProxy = proxyType(resolved, proxyRandomize);
        if (!addrProxy.IsValid())
            return InitError(strprintf(_("Invalid -proxy address: '%s'"), proxyArg));

        SetProxy(NET_IPV4, addrProxy);
        SetProxy(NET_IPV6, addrProxy);
        SetProxy(NET_TOR, addrProxy);
        SetNameProxy(addrProxy);
        SetLimited(NET_TOR, false); // by default, -proxy sets onion as reachable, unless -noonion later
    }

    // -onion can be used to set only a proxy for .onion, or override normal proxy for .onion addresses
    // -noonion (or -onion=0) disables connecting to .onion entirely
    // An empty string is used to not override the onion proxy (in which case it defaults to -proxy set above, or none)
    std::string onionArg = GetArg("-onion", "");
    if (onionArg != "") {
        if (onionArg == "0") { // Handle -noonion/-onion=0
            SetLimited(NET_TOR); // set onions as unreachable
        } else {
            CService resolved(LookupNumeric(onionArg.c_str(), 9050));
            proxyType addrOnion = proxyType(resolved, proxyRandomize);
            if (!addrOnion.IsValid())
                return InitError(strprintf(_("Invalid -onion address: '%s'"), onionArg));
            SetProxy(NET_TOR, addrOnion);
            SetLimited(NET_TOR, false);
        }
    }

    // see Step 2: parameter interactions for more information about these
    fListen = GetBoolArg("-listen", DEFAULT_LISTEN);
    fDiscover = GetBoolArg("-discover", true);
    fNameLookup = GetBoolArg("-dns", DEFAULT_NAME_LOOKUP);
    fRelayTxes = !GetBoolArg("-blocksonly", DEFAULT_BLOCKSONLY);

    if (fListen) {
        bool fBound = false;
        if (mapArgs.count("-bind") || mapArgs.count("-whitebind")) {
            BOOST_FOREACH(const std::string& strBind, mapMultiArgs["-bind"]) {
                CService addrBind;
                if (!Lookup(strBind.c_str(), addrBind, GetListenPort(), false))
                    return InitError(ResolveErrMsg("bind", strBind));
                fBound |= Bind(connman, addrBind, (BF_EXPLICIT | BF_REPORT_ERROR));
            }
            BOOST_FOREACH(const std::string& strBind, mapMultiArgs["-whitebind"]) {
                CService addrBind;
                if (!Lookup(strBind.c_str(), addrBind, 0, false))
                    return InitError(ResolveErrMsg("whitebind", strBind));
                if (addrBind.GetPort() == 0)
                    return InitError(strprintf(_("Need to specify a port with -whitebind: '%s'"), strBind));
                fBound |= Bind(connman, addrBind, (BF_EXPLICIT | BF_REPORT_ERROR | BF_WHITELIST));
            }
        }
        else {
            struct in_addr inaddr_any;
            inaddr_any.s_addr = INADDR_ANY;
            fBound |= Bind(connman, CService(in6addr_any, GetListenPort()), BF_NONE);
            fBound |= Bind(connman, CService(inaddr_any, GetListenPort()), !fBound ? BF_REPORT_ERROR : BF_NONE);
        }
        if (!fBound)
            return InitError(_("Failed to listen on any port. Use -listen=0 if you want this."));
    }

    if (mapArgs.count("-externalip")) {
        BOOST_FOREACH(const std::string& strAddr, mapMultiArgs["-externalip"]) {
            CService addrLocal;
            if (Lookup(strAddr.c_str(), addrLocal, GetListenPort(), fNameLookup) && addrLocal.IsValid())
                AddLocal(addrLocal, LOCAL_MANUAL);
            else
                return InitError(ResolveErrMsg("externalip", strAddr));
        }
    }

    BOOST_FOREACH(const std::string& strDest, mapMultiArgs["-seednode"])
        connman.AddOneShot(strDest);

#if ENABLE_ZMQ
    pzmqNotificationInterface = CZMQNotificationInterface::CreateWithArguments(mapArgs);

    if (pzmqNotificationInterface) {
        RegisterValidationInterface(pzmqNotificationInterface);
    }
#endif

    ppsNotificationInterface = new CPSNotificationInterface(connman);
    RegisterValidationInterface(ppsNotificationInterface);

    uint64_t nMaxOutboundLimit = 0; //unlimited unless -maxuploadtarget is set
    uint64_t nMaxOutboundTimeframe = MAX_UPLOAD_TIMEFRAME;

    if (mapArgs.count("-maxuploadtarget")) {
        nMaxOutboundLimit = GetArg("-maxuploadtarget", DEFAULT_MAX_UPLOAD_TARGET)*1024*1024;
    }

    // ********************************************************* Step 7: load block chain

    fReindex = GetBoolArg("-reindex", false);
    bool fReindexChainState = GetBoolArg("-reindex-chainstate", false);

    // Upgrading to 0.8; hard-link the old blknnnn.dat files into /blocks/
    boost::filesystem::path blocksDir = GetDataDir() / "blocks";
    if (!boost::filesystem::exists(blocksDir))
    {
        boost::filesystem::create_directories(blocksDir);
        bool linked = false;
        for (unsigned int i = 1; i < 10000; i++) {
            boost::filesystem::path source = GetDataDir() / strprintf("blk%04u.dat", i);
            if (!boost::filesystem::exists(source)) break;
            boost::filesystem::path dest = blocksDir / strprintf("blk%05u.dat", i-1);
            try {
                boost::filesystem::create_hard_link(source, dest);
                LogPrintf("Hardlinked %s -> %s\n", source.string(), dest.string());
                linked = true;
            } catch (const boost::filesystem::filesystem_error& e) {
                // Note: hardlink creation failing is not a disaster, it just means
                // blocks will get re-downloaded from peers.
                LogPrintf("Error hardlinking blk%04u.dat: %s\n", i, e.what());
                break;
            }
        }
        if (linked)
        {
            fReindex = true;
        }
    }

    // cache size calculations
    int64_t nTotalCache = (GetArg("-dbcache", nDefaultDbCache) << 20);
    nTotalCache = std::max(nTotalCache, nMinDbCache << 20); // total cache cannot be less than nMinDbCache
    nTotalCache = std::min(nTotalCache, nMaxDbCache << 20); // total cache cannot be greated than nMaxDbcache
    int64_t nBlockTreeDBCache = nTotalCache / 8;
    nBlockTreeDBCache = std::min(nBlockTreeDBCache, (GetBoolArg("-txindex", DEFAULT_TXINDEX) ? nMaxBlockDBAndTxIndexCache : nMaxBlockDBCache) << 20);
    nTotalCache -= nBlockTreeDBCache;
    int64_t nCoinDBCache = std::min(nTotalCache / 2, (nTotalCache / 4) + (1 << 23)); // use 25%-50% of the remainder for disk cache
    nCoinDBCache = std::min(nCoinDBCache, nMaxCoinsDBCache << 20); // cap total coins db cache
    nTotalCache -= nCoinDBCache;
    nCoinCacheUsage = nTotalCache; // the rest goes to in-memory cache
    nMempoolSizeMax = GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000;
    LogPrintf("Cache configuration:\n");
    LogPrintf("* Using %.1fMiB for block index database\n", nBlockTreeDBCache * (1.0 / 1024 / 1024));
    LogPrintf("* Using %.1fMiB for chain state database\n", nCoinDBCache * (1.0 / 1024 / 1024));
    LogPrintf("* Using %.1fMiB for in-memory UTXO set (plus up to %.1fMiB of unused mempool space)\n", nCoinCacheUsage * (1.0 / 1024 / 1024), nMempoolSizeMax * (1.0 / 1024 / 1024));

    bool fLoaded = false;
    while (!fLoaded && !fRequestShutdown) {
        bool fReset = fReindex;
        std::string strLoadError;

        uiInterface.InitMessage(_("Loading block index..."));

        nStart = GetTimeMillis();
        do {
            try {
                UnloadBlockIndex();
                delete pcoinsTip;
                delete pcoinsdbview;
                delete pcoinscatcher;
                delete pblocktree;

                pblocktree = new CBlockTreeDB(nBlockTreeDBCache, false, fReindex);
                pcoinsdbview = new CCoinsViewDB(nCoinDBCache, false, fReindex || fReindexChainState);
                pcoinscatcher = new CCoinsViewErrorCatcher(pcoinsdbview);
                pcoinsTip = new CCoinsViewCache(pcoinscatcher);

                if (fReindex) {
                    pblocktree->WriteReindexing(true);
                    //If we're reindexing in prune mode, wipe away unusable block files and all undo data files
                    if (fPruneMode)
                        CleanupBlockRevFiles();
                } else {
                    // If necessary, upgrade from older database format.
                    if (!pcoinsdbview->Upgrade()) {
                        strLoadError = _("Error upgrading chainstate database");
                        break;
                    }
                }
                if (fRequestShutdown) break;

                if (!LoadBlockIndex(chainparams)) {
                    strLoadError = _("Error loading block database");
                    break;
                }

                // If the loaded chain has a wrong genesis, bail out immediately
                // (we're likely using a testnet datadir, or the other way around).
                if (!mapBlockIndex.empty() && mapBlockIndex.count(chainparams.GetConsensus().hashGenesisBlock) == 0)
                    return InitError(_("Incorrect or no genesis block found. Wrong datadir for network?"));

                // Initialize the block index (no-op if non-empty database was already loaded)
                if (!InitBlockIndex(chainparams)) {
                    strLoadError = _("Error initializing block database");
                    break;
                }

                // Check for changed -txindex state
                if (fTxIndex != GetBoolArg("-txindex", DEFAULT_TXINDEX)) {
                    strLoadError = _("You need to rebuild the database using -reindex-chainstate to change -txindex");
                    break;
                }

                // Check for changed -prune state.  What we are concerned about is a user who has pruned blocks
                // in the past, but is now trying to run unpruned.
                if (fHavePruned && !fPruneMode) {
                    strLoadError = _("You need to rebuild the database using -reindex to go back to unpruned mode.  This will redownload the entire blockchain");
                    break;
                }

                uiInterface.InitMessage(_("Verifying blocks..."));
                if (fHavePruned && GetArg("-checkblocks", DEFAULT_CHECKBLOCKS) > MIN_BLOCKS_TO_KEEP) {
                    LogPrintf("Prune: pruned datadir may not have more than %d blocks; only checking available blocks",
                        MIN_BLOCKS_TO_KEEP);
                }

                {
                    LOCK(cs_main);
                    CBlockIndex* tip = chainActive.Tip();
                    if (tip && tip->nTime > GetAdjustedTime() + 2 * 60 * 60) {
                        strLoadError = _("The block database contains a block which appears to be from the future. "
                                "This may be due to your computer's date and time being set incorrectly. "
                                "Only rebuild the block database if you are sure that your computer's date and time are correct");
                        break;
                    }
                }

                if (!CVerifyDB().VerifyDB(chainparams, pcoinsdbview, GetArg("-checklevel", DEFAULT_CHECKLEVEL),
                              GetArg("-checkblocks", DEFAULT_CHECKBLOCKS))) {
                    strLoadError = _("Corrupted block database detected");
                    break;
                }
            } catch (const std::exception& e) {
                if (fDebug) LogPrintf("%s\n", e.what());
                strLoadError = _("Error opening block database");
                break;
            }

            fLoaded = true;
        } while(false);

        if (!fLoaded && !fRequestShutdown) {
            // first suggest a reindex
            if (!fReset) {
                bool fRet = uiInterface.ThreadSafeQuestion(
                    strLoadError + ".\n\n" + _("Do you want to rebuild the block database now?"),
                    strLoadError + ".\nPlease restart with -reindex or -reindex-chainstate to recover.",
                    "", CClientUIInterface::MSG_ERROR | CClientUIInterface::BTN_ABORT);
                if (fRet) {
                    fReindex = true;
                    fRequestShutdown = false;
                } else {
                    LogPrintf("Aborted block database rebuild. Exiting.\n");
                    return false;
                }
            } else {
                return InitError(strLoadError);
            }
        }
    }

    // As LoadBlockIndex can take several minutes, it's possible the user
    // requested to kill the GUI during the last operation. If so, exit.
    // As the program has not fully started yet, Shutdown() is possibly overkill.
    if (fRequestShutdown)
    {
        LogPrintf("Shutdown requested. Exiting.\n");
        return false;
    }
    LogPrintf(" block index %15dms\n", GetTimeMillis() - nStart);

    boost::filesystem::path est_path = GetDataDir() / FEE_ESTIMATES_FILENAME;
    CAutoFile est_filein(fopen(est_path.string().c_str(), "rb"), SER_DISK, CLIENT_VERSION);
    // Allowed to fail as this file IS missing on first startup.
    if (!est_filein.IsNull())
        mempool.ReadFeeEstimates(est_filein);
    fFeeEstimatesInitialized = true;

    // ********************************************************* Step 8: load wallet
#ifdef ENABLE_WALLET
    if (!CWallet::InitLoadWallet())
        return false;
#else
    LogPrintf("No wallet support compiled in!\n");
#endif

    // ********************************************************* Step 9: data directory maintenance

    // if pruning, unset the service bit and perform the initial blockstore prune
    // after any wallet rescanning has taken place.
    if (fPruneMode) {
        LogPrintf("Unsetting NODE_NETWORK on prune mode\n");
        nLocalServices = ServiceFlags(nLocalServices & ~NODE_NETWORK);
        if (!fReindex && chainActive.Tip() != NULL) {
            uiInterface.InitMessage(_("Pruning blockstore..."));
            PruneAndFlush();
        }
    }

    // ********************************************************* Step 10: import blocks

    if (!CheckDiskSpace())
        return false;

    // Either install a handler to notify us when genesis activates, or set fHaveGenesis directly.
    // No locking, as this happens before any background thread is started.
    if (chainActive.Tip() == NULL) {
        uiInterface.NotifyBlockTip.connect(BlockNotifyGenesisWait);
    } else {
        fHaveGenesis = true;
    }

    if (mapArgs.count("-blocknotify"))
        uiInterface.NotifyBlockTip.connect(BlockNotifyCallback);

    std::vector<boost::filesystem::path> vImportFiles;
    if (mapArgs.count("-loadblock"))
    {
        BOOST_FOREACH(const std::string& strFile, mapMultiArgs["-loadblock"])
            vImportFiles.push_back(strFile);
    }
    threadGroup.create_thread(boost::bind(&ThreadImport, vImportFiles));
    // Wait for genesis block to be processed
    {
        boost::unique_lock<boost::mutex> lock(cs_GenesisWait);
        while (!fHaveGenesis) {
            condvar_GenesisWait.wait(lock);
        }
    uiInterface.NotifyBlockTip.disconnect(BlockNotifyGenesisWait);
    }

    // ********************************************************* Step 11a: setup PrivateSend
    fDyNode = GetBoolArg("-dynode", false);
    // TODO: dynode should have no wallet

    if((fDyNode || dynodeConfig.getCount() > -1) && fTxIndex == false) {
        return InitError("Enabling Dynode support requires turning on transaction indexing."
                  "Please add txindex=1 to your configuration and start with -reindex");
    }

    if(fDyNode) {
        LogPrintf("DYNODE:\n");

        std::string strdynodepairingkey = GetArg("-dynodepairingkey", "");
        if(!strdynodepairingkey.empty()) {
            if(!CMessageSigner::GetKeysFromSecret(strdynodepairingkey, activeDynode.keyDynode, activeDynode.pubKeyDynode))
                return InitError(_("Invalid dynodepairingkey. Please see documenation."));

            LogPrintf("  pubKeyDynode: %s\n", CDynamicAddress(activeDynode.pubKeyDynode.GetID()).ToString());
        } else {
            return InitError(_("You must specify a dynodepairingkey in the configuration. Please see documentation for help."));
        }
    }

#ifdef ENABLE_WALLET
    LogPrintf("Using Dynode config file %s\n", GetDynodeConfigFile().string());

    if(GetBoolArg("-dnconflock", true) && pwalletMain && (dynodeConfig.getCount() > 0)) {
        LOCK(pwalletMain->cs_wallet);
        LogPrintf("Locking Dynodes:\n");
        uint256 dnTxHash;
        int outputIndex;
        BOOST_FOREACH(CDynodeConfig::CDynodeEntry dne, dynodeConfig.getEntries()) {
            dnTxHash.SetHex(dne.getTxHash());
            outputIndex = boost::lexical_cast<unsigned int>(dne.getOutputIndex());
            COutPoint outpoint = COutPoint(dnTxHash, outputIndex);
            // don't lock non-spendable outpoint (i.e. it's already spent or it's not from this wallet at all)
            if(pwalletMain->IsMine(CTxIn(outpoint)) != ISMINE_SPENDABLE) {
                LogPrintf("  %s %s - IS NOT SPENDABLE, was not locked\n", dne.getTxHash(), dne.getOutputIndex());
                continue;
            }
            pwalletMain->LockCoin(outpoint);
            LogPrintf("  %s %s - locked successfully\n", dne.getTxHash(), dne.getOutputIndex());
        }
    }


    privateSendClient.nLiquidityProvider = std::min(std::max((int)GetArg("-liquidityprovider", DEFAULT_PRIVATESEND_LIQUIDITY), 0), 100);
    if(privateSendClient.nLiquidityProvider) {
        // special case for liquidity providers only, normal clients should use default value
        privateSendClient.SetMinBlocksToWait(privateSendClient.nLiquidityProvider * 15);
    }

    privateSendClient.fEnablePrivateSend = GetBoolArg("-enableprivatesend", false);
    privateSendClient.fPrivateSendMultiSession = GetBoolArg("-privatesendmultisession", DEFAULT_PRIVATESEND_MULTISESSION);
    privateSendClient.nPrivateSendRounds = std::min(std::max((int)GetArg("-privatesendrounds", DEFAULT_PRIVATESEND_ROUNDS), 2), 16);
    privateSendClient.nPrivateSendAmount = std::min(std::max((int)GetArg("-privatesendamount", DEFAULT_PRIVATESEND_AMOUNT), 2), 999999);
#endif // ENABLE_WALLET

    fEnableInstantSend = GetBoolArg("-enableinstantsend", 1);
    nInstantSendDepth = GetArg("-instantsenddepth", DEFAULT_INSTANTSEND_DEPTH);
    nInstantSendDepth = std::min(std::max(nInstantSendDepth, 0), 60);

    //lite mode disables all Dynode and Privatesend related functionality
    fLiteMode = GetBoolArg("-litemode", false);
    if(fDyNode && fLiteMode){
        return InitError("You can not start a Dynode in litemode");
    }

    LogPrintf("fLiteMode %d\n", fLiteMode);
    LogPrintf("nInstantSendDepth %d\n", nInstantSendDepth);
#ifdef ENABLE_WALLET
    LogPrintf("PrivateSend rounds %d\n", privateSendClient.nPrivateSendRounds);
    LogPrintf("PrivateSend amount %d\n", privateSendClient.nPrivateSendAmount);
#endif // ENABLE_WALLET

    CPrivateSend::InitStandardDenominations();

    // ********************************************************* Step 11b: Load cache data

    // LOAD SERIALIZED DAT FILES INTO DATA CACHES FOR INTERNAL USE

    boost::filesystem::path pathDB = GetDataDir();
    std::string strDBName;

    strDBName = "dncache.dat";
    uiInterface.InitMessage(_("Loading Dynode cache..."));
    CFlatDB<CDynodeMan> flatdb1(strDBName, "magicDynodeCache");
    if(!flatdb1.Load(dnodeman)) {
        return InitError(_("Failed to load Dynode cache from") + "\n" + (pathDB / strDBName).string());
    }

    if(dnodeman.size()) {
        strDBName = "dnpayments.dat";
        uiInterface.InitMessage(_("Loading Dynode payment cache..."));
        CFlatDB<CDynodePayments> flatdb2(strDBName, "magicDynodePaymentsCache");
        if(!flatdb2.Load(dnpayments)) {
            return InitError(_("Failed to load Dynode payments cache from") + "\n" + (pathDB / strDBName).string());
        }

        strDBName = "governance.dat";
        uiInterface.InitMessage(_("Loading governance cache..."));
        CFlatDB<CGovernanceManager> flatdb3(strDBName, "magicGovernanceCache");
        if(!flatdb3.Load(governance)) {
            return InitError(_("Failed to load governance cache from") + "\n" + (pathDB / strDBName).string());
        }
        governance.InitOnLoad();
    } else {
        uiInterface.InitMessage(_("Dynode cache is empty, skipping payments and governance cache..."));
    }
    governance.InitOnLoad();

    strDBName = "netfulfilled.dat";
    uiInterface.InitMessage(_("Loading fullfiled requests cache..."));
    CFlatDB<CNetFulfilledRequestManager> flatdb4(strDBName, "magicFulfilledCache");
    if(!flatdb4.Load(netfulfilledman)) {
        return InitError(_("Failed to load fulfilled requests cache from") + "\n" + (pathDB / strDBName).string());
    }

    // ********************************************************* Step 11c: update block tip in Dynamic modules

    // force UpdatedBlockTip to initialize nCachedBlockHeight for PS, DN payments and budgets
    // but don't call it directly to prevent triggering of other listeners like zmq etc.
    // GetMainSignals().UpdatedBlockTip(chainActive.Tip());
    ppsNotificationInterface->InitializeCurrentBlockTip();

    // ********************************************************* Step 11d: start dynamic-ps-<smth> threads

    threadGroup.create_thread(boost::bind(&ThreadCheckPrivateSend, boost::ref(*g_connman)));
    if (fDyNode)
        threadGroup.create_thread(boost::bind(&ThreadCheckPrivateSendServer, boost::ref(*g_connman)));
#ifdef ENABLE_WALLET
     else
        threadGroup.create_thread(boost::bind(&ThreadCheckPrivateSendClient, boost::ref(*g_connman)));
#endif // ENABLE_WALLET

    // ********************************************************* Step 12: start node


    if (!strErrors.str().empty())
        return InitError(strErrors.str());

    RandAddSeedPerfmon();

    //// debug print
    LogPrintf("mapBlockIndex.size() = %u\n",   mapBlockIndex.size());
    LogPrintf("chainActive.Height() = %d\n",   chainActive.Height());

    if (GetBoolArg("-listenonion", DEFAULT_LISTEN_ONION))
        StartTorControl(threadGroup, scheduler);

    Discover(threadGroup);

    // Map ports with UPnP
    MapPort(GetBoolArg("-upnp", DEFAULT_UPNP));

    std::string strNodeError;
    CConnman::Options connOptions;
    connOptions.nLocalServices = nLocalServices;
    connOptions.nRelevantServices = nRelevantServices;
    connOptions.nMaxConnections = nMaxConnections;
    connOptions.nMaxOutbound = std::min(MAX_OUTBOUND_CONNECTIONS, connOptions.nMaxConnections);
    connOptions.nMaxFeeler = 1;
    connOptions.nBestHeight = chainActive.Height();
    connOptions.uiInterface = &uiInterface;
    connOptions.nSendBufferMaxSize = 1000*GetArg("-maxsendbuffer", DEFAULT_MAXSENDBUFFER);
    connOptions.nReceiveFloodSize = 1000*GetArg("-maxreceivebuffer", DEFAULT_MAXRECEIVEBUFFER);

    connOptions.nMaxOutboundTimeframe = nMaxOutboundTimeframe;
    connOptions.nMaxOutboundLimit = nMaxOutboundLimit;

    if (!connman.Start(scheduler, strNodeError, connOptions))
        return InitError(strNodeError);

    // Generate coins in the background
    GenerateDynamics(GetBoolArg("-gen", DEFAULT_GENERATE), GetArg("-genproclimit", DEFAULT_GENERATE_THREADS), chainparams, connman);

    // ********************************************************* Step 13: finished

    SetRPCWarmupFinished();
    uiInterface.InitMessage(_("Done loading"));

#ifdef ENABLE_WALLET
    if (pwalletMain)
        pwalletMain->postInitProcess(threadGroup);
#endif

    threadGroup.create_thread(boost::bind(&ThreadSendAlert, boost::ref(connman)));

    return !fRequestShutdown;
}
